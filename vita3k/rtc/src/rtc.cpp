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

#include <rtc/rtc.h>

#include <util/log.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <numeric>

namespace {
// Guest clock model:
//     guest_time = scaled_elapsed + (host_now - anchor_host) * num / den
// so the guest can run faster (fast forward) or slower (slow motion) than wall
// time. Changing the scale (or pausing) folds the elapsed segment into
// scaled_elapsed first, keeping the clock monotonic across speed changes.
//
// While an emulator pause is active (guest threads suspended, audio stream
// paused) the guest-visible clock must stay frozen as well. Without this, the
// game resumes after a long pause with a system/RTC-time based timeline that
// is seconds to minutes in the future, while the frozen audio playback only
// continues from the pause point - the sound lags the picture by exactly the
// pause duration.
struct GuestClockState {
    std::uint64_t scaled_elapsed = 0;
    std::uint64_t anchor_host = 0;
    std::uint32_t scale_num = 1;
    std::uint32_t scale_den = 1;
    bool paused = false;
};

std::mutex rtc_clock_write_mutex;
std::atomic<std::uint32_t> rtc_clock_seq{ 0 };
std::atomic<std::uint64_t> rtc_scaled_elapsed{ 0 };
std::atomic<std::uint64_t> rtc_anchor_host{ 0 };
std::atomic<std::uint32_t> rtc_time_scale_num{ 1 };
std::atomic<std::uint32_t> rtc_time_scale_den{ 1 };
std::atomic<bool> rtc_clock_paused{ false };

std::uint64_t rtc_host_now_ticks() {
    const auto now = std::chrono::high_resolution_clock::now();
    return std::chrono::time_point_cast<VitaClocks>(now).time_since_epoch().count();
}

// Publish a consistent snapshot for the lock-free readers below. All callers
// must hold rtc_clock_write_mutex.
void rtc_clock_publish(const std::uint64_t scaled_elapsed, const std::uint64_t anchor_host,
    const std::uint32_t scale_num, const std::uint32_t scale_den, const bool paused) {
    const std::uint32_t seq = rtc_clock_seq.load(std::memory_order_relaxed);
    rtc_clock_seq.store(seq + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    rtc_scaled_elapsed.store(scaled_elapsed, std::memory_order_relaxed);
    rtc_anchor_host.store(anchor_host, std::memory_order_relaxed);
    rtc_time_scale_num.store(scale_num, std::memory_order_relaxed);
    rtc_time_scale_den.store(scale_den, std::memory_order_relaxed);
    rtc_clock_paused.store(paused, std::memory_order_relaxed);
    rtc_clock_seq.store(seq + 2, std::memory_order_release);
}

GuestClockState rtc_clock_snapshot() {
    for (;;) {
        const std::uint32_t before = rtc_clock_seq.load(std::memory_order_acquire);
        if (before & 1u)
            continue;
        GuestClockState state;
        state.scaled_elapsed = rtc_scaled_elapsed.load(std::memory_order_relaxed);
        state.anchor_host = rtc_anchor_host.load(std::memory_order_relaxed);
        state.scale_num = rtc_time_scale_num.load(std::memory_order_relaxed);
        state.scale_den = rtc_time_scale_den.load(std::memory_order_relaxed);
        state.paused = rtc_clock_paused.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (rtc_clock_seq.load(std::memory_order_relaxed) == before)
            return state;
    }
}

void rtc_clock_ensure_started() {
    if (rtc_anchor_host.load(std::memory_order_relaxed) != 0)
        return;
    const std::lock_guard<std::mutex> lock(rtc_clock_write_mutex);
    if (rtc_anchor_host.load(std::memory_order_relaxed) == 0)
        rtc_clock_publish(0, rtc_host_now_ticks(), 1, 1, false);
}

std::uint64_t rtc_clock_state_ticks(const GuestClockState &state) {
    if (state.paused)
        return state.scaled_elapsed;
    const std::uint64_t now = rtc_host_now_ticks();
    const std::uint64_t delta = now > state.anchor_host ? now - state.anchor_host : 0;
    return state.scaled_elapsed + delta * state.scale_num / state.scale_den;
}
} // namespace

void rtc_set_clock_paused(const bool paused) {
    rtc_clock_ensure_started();
    const std::lock_guard<std::mutex> lock(rtc_clock_write_mutex);

    const bool was_paused = rtc_clock_paused.load(std::memory_order_relaxed);
    if (paused == was_paused)
        return;

    const std::uint64_t now = rtc_host_now_ticks();
    const std::uint64_t anchor = rtc_anchor_host.load(std::memory_order_relaxed);
    const std::uint32_t scale_num = rtc_time_scale_num.load(std::memory_order_relaxed);
    const std::uint32_t scale_den = rtc_time_scale_den.load(std::memory_order_relaxed);
    std::uint64_t elapsed = rtc_scaled_elapsed.load(std::memory_order_relaxed);

    if (paused) {
        if (now > anchor)
            elapsed += (now - anchor) * scale_num / scale_den;
        rtc_clock_publish(elapsed, now, scale_num, scale_den, true);
    } else {
        const std::uint64_t pause_duration = now > anchor ? now - anchor : 0;
        rtc_clock_publish(elapsed, now, scale_num, scale_den, false);
        if (pause_duration != 0)
            LOG_INFO("[RTC] guest clock unfrozen after a {:.2f} s pause", static_cast<double>(pause_duration) / VITA_CLOCKS_PER_SEC);
    }
}

void rtc_set_time_scale(const int num, const int den) {
    const std::uint32_t requested_num = static_cast<std::uint32_t>(std::clamp(num, 1, 8));
    const std::uint32_t requested_den = static_cast<std::uint32_t>(std::clamp(den, 1, 8));
    const std::uint32_t divisor = std::gcd(requested_num, requested_den);
    const std::uint32_t scale_num = requested_num / divisor;
    const std::uint32_t scale_den = requested_den / divisor;

    rtc_clock_ensure_started();
    const std::lock_guard<std::mutex> lock(rtc_clock_write_mutex);

    const std::uint32_t old_num = rtc_time_scale_num.load(std::memory_order_relaxed);
    const std::uint32_t old_den = rtc_time_scale_den.load(std::memory_order_relaxed);
    if (old_num == scale_num && old_den == scale_den)
        return;

    const bool paused = rtc_clock_paused.load(std::memory_order_relaxed);
    const std::uint64_t now = rtc_host_now_ticks();
    const std::uint64_t anchor = rtc_anchor_host.load(std::memory_order_relaxed);
    std::uint64_t elapsed = rtc_scaled_elapsed.load(std::memory_order_relaxed);
    if (!paused && now > anchor)
        elapsed += (now - anchor) * old_num / old_den;
    rtc_clock_publish(elapsed, now, scale_num, scale_den, paused);
}

void rtc_reset_clock_state() {
    // The clock bookkeeping lives at process scope while a game can be
    // launched, closed and relaunched several times inside the same emulator
    // process. If a game is closed while paused, the frozen state would leak
    // into the next game: its guest clock never advances, time-based logic
    // stalls and the game can hang with no more frames presented. Start every
    // game with a clean, running clock at normal speed.
    const std::lock_guard<std::mutex> lock(rtc_clock_write_mutex);
    rtc_clock_publish(0, rtc_host_now_ticks(), 1, 1, false);
}

std::uint64_t rtc_guest_clock_ticks() {
    rtc_clock_ensure_started();
    return rtc_clock_state_ticks(rtc_clock_snapshot());
}

bool rtc_clock_is_paused() {
    return rtc_clock_paused.load(std::memory_order_relaxed);
}

int rtc_get_time_scale_num() {
    return static_cast<int>(rtc_time_scale_num.load(std::memory_order_relaxed));
}

int rtc_get_time_scale_den() {
    return static_cast<int>(rtc_time_scale_den.load(std::memory_order_relaxed));
}

static std::uint64_t rtc_ticks_since_epoch() {
    return rtc_guest_clock_ticks();
}

std::uint64_t rtc_base_ticks() {
    return RTC_OFFSET + std::time(nullptr) * VITA_CLOCKS_PER_SEC - rtc_ticks_since_epoch();
}

std::uint64_t rtc_get_ticks(uint64_t base_ticks) {
    return base_ticks + rtc_ticks_since_epoch();
}

// The following functions are from PPSSPP
// Copyright (c) 2012- PPSSPP Project.

void __RtcPspTimeToTm(tm *val, const SceDateTime *pt) {
    val->tm_year = pt->year - 1900;
    val->tm_mon = pt->month - 1;
    val->tm_mday = pt->day;
    val->tm_wday = -1;
    val->tm_yday = -1;
    val->tm_hour = pt->hour;
    val->tm_min = pt->minute;
    val->tm_sec = pt->second;
    val->tm_isdst = 0;
}

void __RtcTicksToPspTime(SceDateTime *t, std::uint64_t ticks) {
    int numYearAdd = 0;
    if (ticks < VITA_CLOCKS_PER_SEC) {
        t->year = 1;
        t->month = 1;
        t->day = 1;
        t->hour = 0;
        t->minute = 0;
        t->second = 0;
        t->microsecond = ticks;
        return;
    } else if (ticks < RTC_OFFSET) {
        // Need to get a year past 1970 for gmtime
        // Add enough 400 year to pass over 1970.
        numYearAdd = (int)((RTC_OFFSET - ticks) / RTC_400_YEAR_TICKS + 1);
        ticks += RTC_400_YEAR_TICKS * numYearAdd;
    }

    while (ticks >= RTC_OFFSET + RTC_400_YEAR_TICKS) {
        ticks -= RTC_400_YEAR_TICKS;
        --numYearAdd;
    }

    time_t time = (ticks - RTC_OFFSET) / VITA_CLOCKS_PER_SEC;
    t->microsecond = ticks % VITA_CLOCKS_PER_SEC;

    tm *local = gmtime(&time);
    if (!local) {
        LOG_ERROR("Date is too high/low to handle, pretending to work.");
        return;
    }

    t->year = local->tm_year + 1900 - numYearAdd * 400;
    t->month = local->tm_mon + 1;
    t->day = local->tm_mday;
    t->hour = local->tm_hour;
    t->minute = local->tm_min;
    t->second = local->tm_sec;
}

std::uint64_t __RtcPspTimeToTicks(const SceDateTime *pt) {
    tm local;
    __RtcPspTimeToTm(&local, pt);

    std::int64_t tickOffset = 0;
    while (local.tm_year < 70) {
        tickOffset -= RTC_400_YEAR_TICKS;
        local.tm_year += 400;
    }
    while (local.tm_year >= 470) {
        tickOffset += RTC_400_YEAR_TICKS;
        local.tm_year -= 400;
    }

    time_t seconds = rtc_timegm(&local);
    std::uint64_t result = RTC_OFFSET + (std::uint64_t)seconds * VITA_CLOCKS_PER_SEC;
    result += pt->microsecond;
    return result + tickOffset;
}
