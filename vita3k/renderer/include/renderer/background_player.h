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

#pragma once

#include <cstdint>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>

namespace renderer {

// Wall clock in seconds, used to pace the animation background video: it
// must keep playing at its own rate regardless of the guest clock, so fast
// forward and slow motion do not affect it.
inline double wall_clock_now() {
    return std::chrono::duration_cast<std::chrono::duration<double>>(
        std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Muted looping video played behind the game screen, in the margins around
// it, ported from PPSSPP's BackgroundPlayer (Core/HW/BackgroundPlayer.cpp).
// Only the video stream is decoded and the frames are paced on the wall
// clock (not the guest clock), so fast forward and slow motion do not affect
// it. The FFmpeg context is fully hidden behind the private `Context` struct
// so this header does not pull the FFmpeg headers into every renderer TU.
class BackgroundPlayer {
public:
    // A decoded frame in tightly packed RGBA. `serial` changes whenever the
    // content changes, so consumers uploading into per-frame resources can
    // skip redundant uploads.
    struct Frame {
        const uint8_t *rgba;
        uint32_t width;
        uint32_t height;
        uint64_t serial;
    };

    BackgroundPlayer() = default;
    ~BackgroundPlayer();

    BackgroundPlayer(const BackgroundPlayer &) = delete;
    BackgroundPlayer &operator=(const BackgroundPlayer &) = delete;

    // Open the video at `path` (an absolute filesystem path) and loop it.
    // Re-opening the same path is a no-op, so this can be called on every
    // config apply. Returns false when the file cannot be opened or has no
    // decodable video stream; in that case the player stays closed.
    bool open(const std::string &path);

    void close();

    bool is_loaded() const;

    // Decode up to `now` (wall clock, in seconds) and hand the current frame
    // to `consume` while holding the player lock, making the pointer safe
    // against a concurrent open()/close() from another thread. Returns false
    // when no frame is available yet.
    bool update(double now, const std::function<void(const Frame &)> &consume);

private:
    struct Context;

    bool open_locked(const std::string &path);
    void close_locked();
    bool decode_next_frame_locked();
    void promote_frame_locked();
    void rewind_locked();

    mutable std::mutex mutex;
    Context *ctx = nullptr;
};

} // namespace renderer
