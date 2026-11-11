// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include <renderer/background_player.h>
#include <util/log.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <memory>
#include <vector>

namespace renderer {

// Upper bound of the scaled output, matching the size of the screen
// renderers' background staging buffers: bigger videos are scaled down to
// keep the per-frame upload cheap.
static constexpr uint32_t MAX_BACKGROUND_WIDTH = 1024;
static constexpr uint32_t MAX_BACKGROUND_HEIGHT = 720;

// Safety valve against a broken stream declaring absurd timestamps, same
// limit as PPSSPP's BackgroundPlayer.
static constexpr int MAX_FRAMES_PER_UPDATE = 100;

// After this much wall time ahead of the video, re-anchor the playback
// instead of fast-forwarding through the whole file (app pause, decode
// hiccup), like PPSSPP does.
static constexpr double REANCHOR_THRESHOLD = 1.0;
static constexpr double REANCHOR_FRAME_PTS = 0.1;

struct BackgroundPlayer::Context {
    std::string path;

    AVFormatContext *fmt = nullptr;
    AVCodecContext *dec = nullptr;
    AVPacket *packet = nullptr;
    AVFrame *frame = nullptr;
    SwsContext *sws = nullptr;

    int video_stream_index = -1;
    double video_time_base = 0.0;

    // scaled, tightly packed RGBA output
    std::vector<uint8_t> rgba;
    uint32_t out_width = 0;
    uint32_t out_height = 0;
    uint64_t serial = 0;

    // wall-clock pacing state, ported from PPSSPP's BackgroundPlayer
    double start_time = 0.0; // wall clock of the loop start, 0 = not started yet
    double next_frame_pts = 0.0; // pts of the last decoded frame
    bool has_frame = false;
    bool draining = false; // demuxer hit the end, draining the decoder

    ~Context() {
        sws_freeContext(sws);
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
    }
};

BackgroundPlayer::~BackgroundPlayer() {
    close();
}

bool BackgroundPlayer::open(const std::string &path) {
    std::lock_guard<std::mutex> lock(mutex);
    return open_locked(path);
}

bool BackgroundPlayer::open_locked(const std::string &path) {
    if (ctx && ctx->path == path)
        return true; // already looping this video

    close_locked();

    auto new_ctx = std::make_unique<Context>();
    new_ctx->path = path;

    if (avformat_open_input(&new_ctx->fmt, path.c_str(), nullptr, nullptr) < 0) {
        LOG_ERROR("BackgroundPlayer: failed to open {}", path);
        return false;
    }
    if (avformat_find_stream_info(new_ctx->fmt, nullptr) < 0) {
        LOG_ERROR("BackgroundPlayer: failed to read the stream info of {}", path);
        return false;
    }

    new_ctx->video_stream_index = av_find_best_stream(new_ctx->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (new_ctx->video_stream_index < 0) {
        LOG_ERROR("BackgroundPlayer: no video stream in {}", path);
        return false;
    }
    const AVStream *stream = new_ctx->fmt->streams[new_ctx->video_stream_index];

    const AVCodec *decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!decoder) {
        LOG_ERROR("BackgroundPlayer: unsupported video codec in {}", path);
        return false;
    }
    new_ctx->dec = avcodec_alloc_context3(decoder);
    if (!new_ctx->dec)
        return false;
    if (avcodec_parameters_to_context(new_ctx->dec, stream->codecpar) < 0
        || avcodec_open2(new_ctx->dec, decoder, nullptr) < 0) {
        LOG_ERROR("BackgroundPlayer: failed to open the video decoder for {}", path);
        return false;
    }

    new_ctx->video_time_base = av_q2d(stream->time_base);
    new_ctx->frame = av_frame_alloc();
    new_ctx->packet = av_packet_alloc();

    ctx = new_ctx.release();
    LOG_INFO("BackgroundPlayer: looping {}", path);
    return true;
}

void BackgroundPlayer::close() {
    std::lock_guard<std::mutex> lock(mutex);
    close_locked();
}

void BackgroundPlayer::close_locked() {
    delete ctx;
    ctx = nullptr;
}

bool BackgroundPlayer::is_loaded() const {
    std::lock_guard<std::mutex> lock(mutex);
    return ctx != nullptr;
}

void BackgroundPlayer::promote_frame_locked() {
    const int src_width = ctx->frame->width;
    const int src_height = ctx->frame->height;
    if (src_width <= 0 || src_height <= 0)
        return; // keep the previous frame

    if (ctx->out_width == 0) {
        // First frame: fix the output size inside the upload budget.
        const double scale = std::min(1.0,
            std::min(MAX_BACKGROUND_WIDTH / static_cast<double>(src_width),
                MAX_BACKGROUND_HEIGHT / static_cast<double>(src_height)));
        ctx->out_width = std::max<uint32_t>(1, static_cast<uint32_t>(src_width * scale));
        ctx->out_height = std::max<uint32_t>(1, static_cast<uint32_t>(src_height * scale));
        ctx->rgba.resize(static_cast<size_t>(ctx->out_width) * ctx->out_height * 4);
        LOG_INFO("BackgroundPlayer: scaling {}x{} down to {}x{}",
            src_width, src_height, ctx->out_width, ctx->out_height);
    }

    ctx->sws = sws_getCachedContext(ctx->sws, src_width, src_height,
        static_cast<AVPixelFormat>(ctx->frame->format),
        ctx->out_width, ctx->out_height, AV_PIX_FMT_RGBA,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!ctx->sws)
        return;

    uint8_t *dst_data[4] = { ctx->rgba.data(), nullptr, nullptr, nullptr };
    int dst_stride[4] = { static_cast<int>(ctx->out_width) * 4, 0, 0, 0 };
    sws_scale(ctx->sws, ctx->frame->data, ctx->frame->linesize, 0, src_height, dst_data, dst_stride);

    const int64_t pts = ctx->frame->best_effort_timestamp;
    ctx->next_frame_pts = pts == AV_NOPTS_VALUE ? 0.0 : pts * ctx->video_time_base;
    ctx->serial++;
    ctx->has_frame = true;
}

void BackgroundPlayer::rewind_locked() {
    ctx->draining = false;
    ctx->has_frame = false;
    ctx->start_time = 0.0; // re-anchored on the next update

    if (av_seek_frame(ctx->fmt, ctx->video_stream_index, 0, AVSEEK_FLAG_BACKWARD) == 0) {
        avcodec_flush_buffers(ctx->dec);
        return;
    }

    // Seeking is not supported by this stream: fall back to reopening the
    // file. This also handles a file removed while playing; reopening it then
    // fails and closes the player.
    LOG_WARN("BackgroundPlayer: seeking failed, reopening {}", ctx->path);
    const std::string path = ctx->path;
    close_locked();
    open_locked(path);
}

bool BackgroundPlayer::decode_next_frame_locked() {
    if (ctx->draining) {
        if (avcodec_receive_frame(ctx->dec, ctx->frame) == 0) {
            promote_frame_locked();
            return ctx->has_frame;
        }
        // the decoder is fully drained, loop back to the beginning
        rewind_locked();
        return false;
    }

    int read_result = 0;
    while ((read_result = av_read_frame(ctx->fmt, ctx->packet)) >= 0) {
        if (ctx->packet->stream_index != ctx->video_stream_index) {
            av_packet_unref(ctx->packet);
            continue;
        }

        const int send_result = avcodec_send_packet(ctx->dec, ctx->packet);
        av_packet_unref(ctx->packet);
        if (send_result < 0 && send_result != AVERROR(EAGAIN) && send_result != AVERROR_EOF)
            continue; // broken packet, skip it

        if (avcodec_receive_frame(ctx->dec, ctx->frame) == 0) {
            promote_frame_locked();
            return ctx->has_frame;
        }
        // EAGAIN: the decoder needs more packets before a frame pops out
    }

    if (read_result != AVERROR_EOF)
        LOG_WARN("BackgroundPlayer: demuxing failed, looping");

    // Demuxer end: drain the frames still buffered in the decoder, then loop.
    ctx->draining = true;
    avcodec_send_packet(ctx->dec, nullptr);
    return decode_next_frame_locked();
}

bool BackgroundPlayer::update(double now, const std::function<void(const Frame &)> &consume) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!ctx)
        return false;

    if (ctx->start_time == 0.0)
        ctx->start_time = now;
    double elapsed = now - ctx->start_time;

    // decode frames until one is available with a pts past `elapsed`, the same
    // pacing PPSSPP's BackgroundPlayer uses
    int safety = 0;
    while ((elapsed >= ctx->next_frame_pts || !ctx->has_frame) && safety++ < MAX_FRAMES_PER_UPDATE) {
        if (!decode_next_frame_locked())
            break;
        if (ctx->next_frame_pts < REANCHOR_FRAME_PTS && elapsed > REANCHOR_THRESHOLD) {
            ctx->start_time = now - ctx->next_frame_pts;
            elapsed = now - ctx->start_time;
        }
        if (elapsed < ctx->next_frame_pts)
            break;
    }

    if (!ctx) // the rewind fallback failed and closed the player
        return false;

    if (!ctx->has_frame)
        return false;

    const Frame frame{
        ctx->rgba.data(),
        ctx->out_width,
        ctx->out_height,
        ctx->serial
    };
    consume(frame);
    return true;
}

} // namespace renderer
