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

#include <audio/state.h>

#include <audio/impl/cubeb_audio.h>
#include <audio/impl/sdl_audio.h>

#include <util/log.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>

bool AudioState::init(const std::string &adapter_name) {
    // A session can end while a time scale is latched; never start the next game
    // with a stale multiplier before the display thread publishes its own.
    time_scale_num.store(1, std::memory_order_relaxed);
    time_scale_den.store(1, std::memory_order_relaxed);

    set_backend(adapter_name);
    if (!adapter)
        return false;

    return true;
}

void AudioState::stop_all_ports() {
    {
        const std::lock_guard<std::mutex> lock(mutex);
        for (auto &[_, port] : out_ports) {
            port->stopping = true;
        }
    }
    if (adapter)
        adapter->wake_all_ports();
}

void AudioState::deinit() {
    stop_all_ports();

    const std::lock_guard<std::mutex> lock(mutex);

    out_ports.clear();

    if (in_port.running) {
        SDL_DestroyAudioStream(static_cast<SDL_AudioStream *>(in_port.id));
        in_port.id = nullptr;
        in_port.running = false;
        in_port.len_bytes = 0;
    }
}

void AudioState::set_backend(const std::string &adapter_name) {
    if (adapter_name == this->audio_backend)
        return;

    // first delete all ports then delete the backend
    out_ports.clear();
    adapter.reset();
    if (adapter_name == "SDL") {
        adapter = std::make_unique<SDLAudioAdapter>(*this);
    } else if (adapter_name == "Cubeb") {
        adapter = std::make_unique<CubebAudioAdapter>(*this);
    } else {
        LOG_ERROR("Unknown audio adapter {}", adapter_name);
        return;
    }
    this->audio_backend = adapter_name;

    // lock the mutex to make sure nothing happens until the initialisation is done
    const std::lock_guard<std::mutex> lock(mutex);
    if (!adapter->init()) {
        adapter.reset();
    }
}

AudioOutPortPtr AudioState::open_port(int nb_channels, int freq, int nb_sample) {
    // Guard against a dead backend or a failed port open instead of crashing.
    if (!adapter)
        return nullptr;

    AudioOutPortPtr port = adapter->open_port(nb_channels, freq, nb_sample);
    if (!port)
        return nullptr;

    set_volume(*port, port->volume);
    return port;
}

void AudioState::audio_output(AudioOutPort &out_port, const void *buffer) {
    if (out_port.stopping)
        return;

    const int scale_num = std::clamp(time_scale_num.load(std::memory_order_relaxed), 1, 8);
    const int scale_den = std::clamp(time_scale_den.load(std::memory_order_relaxed), 1, 8);
    if (!buffer || (scale_num == scale_den) || out_port.len_bytes <= 0) {
        adapter->audio_output(out_port, buffer, out_port.len_bytes);
        return;
    }

    // Fast forward / slow motion: rescale the guest chunk so the host queue
    // drains at a different rate. The guest sound thread is paced by that
    // queue, so it advances at the same rate as the scaled guest clock. Linear
    // interpolation keeps the stream continuous; the pitch rises while fast
    // forwarding and drops while slow motion is active, which avoids adding a
    // time-stretching dependency.
    constexpr int sample_bytes = static_cast<int>(sizeof(int16_t));
    const int channels = (out_port.len > 0)
        ? std::clamp(out_port.len_bytes / (out_port.len * sample_bytes), 1, 2)
        : 2;
    const int frame_bytes = channels * sample_bytes;
    const int in_frames = out_port.len_bytes / frame_bytes;
    if (in_frames <= 1) {
        adapter->audio_output(out_port, buffer, out_port.len_bytes);
        return;
    }

    const int out_frames = std::max(1, static_cast<int>(static_cast<int64_t>(in_frames) * scale_den / scale_num));
    std::vector<int16_t> resampled(static_cast<size_t>(out_frames) * channels);
    const int16_t *in = static_cast<const int16_t *>(buffer);
    const double step = static_cast<double>(in_frames) / static_cast<double>(out_frames);
    for (int i = 0; i < out_frames; i++) {
        const double position = i * step;
        const int first = std::min(static_cast<int>(position), in_frames - 1);
        const int second = std::min(first + 1, in_frames - 1);
        const float frac = static_cast<float>(position - first);
        for (int c = 0; c < channels; c++) {
            const float a = in[first * channels + c];
            const float b = in[second * channels + c];
            resampled[static_cast<size_t>(i) * channels + c] = static_cast<int16_t>(a + (b - a) * frac);
        }
    }

    adapter->audio_output(out_port, resampled.data(), static_cast<int>(resampled.size() * sizeof(int16_t)));
}

void AudioState::set_volume(AudioOutPort &out_port, float volume) {
    out_port.volume = volume;
    adapter->set_volume(out_port, volume * global_volume);
}

void AudioState::set_global_volume(float volume) {
    global_volume = volume;
    //  Update adapter volume for each port.
    const std::lock_guard lock(mutex);
    for (const auto &[_, port] : out_ports) {
        adapter->set_volume(*port, port->volume * volume);
    }
}

void AudioState::switch_state(const bool pause) {
    adapter->switch_state(pause);
}

int AudioState::get_rest_sample(AudioOutPort &out_port) {
    const int rest = adapter->get_rest_sample(out_port);
    // A rescaled queue of `rest` host frames spans rest*num/den guest samples,
    // which is the unit the caller (sceAudioOutGetRestSample) asks for.
    const int scale_num = std::clamp(time_scale_num.load(std::memory_order_relaxed), 1, 8);
    const int scale_den = std::clamp(time_scale_den.load(std::memory_order_relaxed), 1, 8);
    if (scale_num == scale_den)
        return rest;
    return static_cast<int>(static_cast<int64_t>(rest) * scale_num / scale_den);
}

void AudioState::on_time_scale_changed(const int old_num, const int old_den, const int new_num, const int new_den) {
    // Only leaving slow motion needs the flush: fast forward's compressed
    // backlog already drains faster than real time, and entering slow motion
    // only leaves a normal-sized chunk in the queue.
    const bool was_slow_motion = old_den > old_num;
    const bool now_faster = static_cast<int64_t>(new_num) * old_den > static_cast<int64_t>(old_num) * new_den;
    if (!was_slow_motion || !now_faster)
        return;

    const std::lock_guard<std::mutex> lock(mutex);
    if (!adapter)
        return;
    for (auto &[_, port] : out_ports)
        adapter->flush_port(*port);
}

void AudioState::wake_all_ports() {
    const std::lock_guard<std::mutex> lock(mutex);
    for (auto &[_, port] : out_ports) {
        port->stopping = true;
    }
    if (adapter)
        adapter->wake_all_ports();
}