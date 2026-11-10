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

#include "android_state.h"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_log.h>
#include <audio/state.h>
#include <config/state.h>
#include <ctrl/functions.h>
#include <display/state.h>
#include <jni.h>
#include <renderer/state.h>
#include <rtc/rtc.h>
#include <touch/functions.h>

#include <algorithm>
#include <string>

static int virtual_joystick_id = -1;
static SDL_Joystick *virtual_joystick = nullptr;

// Pushes the fast forward state into the emulator. The vblank thread is the
// source of truth: every tick it applies the effective speed to the guest clock
// and the audio producer, and shortens the vblank period. This function only
// requests a speed (and drops vsync so presenting does not cap it).
static void apply_fast_forward(EmuEnvState &emuenv, const bool enable, const int speed) {
    const int multiplier = enable ? std::clamp(speed, 2, 4) : 1;
    emuenv.display.fast_forward_speed.store(multiplier, std::memory_order_relaxed);
    if (multiplier == 1) {
        // Make disabling immediate instead of waiting for the next vblank (the
        // controller can also be detached while the emulator is paused).
        emuenv.audio.fast_forward_speed.store(1, std::memory_order_relaxed);
        rtc_set_time_scale(1);
    }
    if (emuenv.renderer)
        emuenv.renderer->set_vsync_state(multiplier == 1 ? emuenv.cfg.current_config.v_sync : false);
}

static void refresh_overlay_controllers() {
    auto *emuenv = get_emuenv();
    auto *controller = get_app_session_controller();
    if (!emuenv || !controller || !controller->has_active_session() || !SDL_WasInit(SDL_INIT_GAMEPAD))
        return;

    refresh_controllers(emuenv->ctrl, *emuenv);
}

void detach_overlay_virtual_controller() {
    if (virtual_joystick)
        SDL_CloseJoystick(virtual_joystick);
    if (virtual_joystick_id >= 0)
        SDL_DetachVirtualJoystick(virtual_joystick_id);
    virtual_joystick = nullptr;
    virtual_joystick_id = -1;
    // The overlay is going away (session end, controller connected, app to the
    // background), so never leave the emulator running at a multiplier.
    if (auto *emuenv = get_emuenv())
        apply_fast_forward(*emuenv, false, 1);
    refresh_overlay_controllers();
}

void attach_overlay_virtual_controller() {
    detach_overlay_virtual_controller();

    if (!SDL_WasInit(SDL_INIT_GAMEPAD))
        return;

    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = "Vita3K Virtual Controller";

    virtual_joystick_id = SDL_AttachVirtualJoystick(&desc);
    if (virtual_joystick_id == 0) {
        SDL_Log("Could not create overlay virtual controller: %s", SDL_GetError());
        return;
    }

    virtual_joystick = SDL_OpenJoystick(virtual_joystick_id);
    if (!virtual_joystick) {
        SDL_Log("Could not create virtual joystick: %s", SDL_GetError());
        detach_overlay_virtual_controller();
        return;
    }

    refresh_overlay_controllers();
}

extern "C" {

JNIEXPORT void JNICALL
Java_org_vita3k_emulator_overlay_InputOverlay_attachController(JNIEnv *env, jobject thiz) {
    attach_overlay_virtual_controller();
}

JNIEXPORT void JNICALL
Java_org_vita3k_emulator_overlay_InputOverlay_detachController(JNIEnv *env, jobject thiz) {
    detach_overlay_virtual_controller();
}

JNIEXPORT void JNICALL
Java_org_vita3k_emulator_overlay_InputOverlay_setAxis(JNIEnv *env, jobject thiz, jint axis, jshort value) {
    if (!virtual_joystick)
        return;

    SDL_SetJoystickVirtualAxis(virtual_joystick, axis, value);
}

JNIEXPORT void JNICALL
Java_org_vita3k_emulator_overlay_InputOverlay_setButton(JNIEnv *env, jobject thiz, jint button, jboolean value) {
    if (!virtual_joystick)
        return;

    if (button < 0)
        // l2/r2
        SDL_SetJoystickVirtualAxis(virtual_joystick, -button, value ? SDL_MAX_SINT16 : 0);
    else
        SDL_SetJoystickVirtualButton(virtual_joystick, button, value);
}

JNIEXPORT void JNICALL
Java_org_vita3k_emulator_overlay_InputOverlay_setFastForward(JNIEnv *env, jobject thiz, jboolean enable, jint speed) {
    auto *emuenv = get_emuenv();
    if (!emuenv)
        return;

    apply_fast_forward(*emuenv, enable != JNI_FALSE, static_cast<int>(speed));
}

JNIEXPORT void JNICALL
Java_org_vita3k_emulator_overlay_InputOverlay_setTouchState(JNIEnv *env, jobject thiz, jint mode) {
    auto *emuenv = get_emuenv();
    if (!emuenv)
        return;

    set_touch_mode(emuenv->touch, static_cast<int>(mode));
}

JNIEXPORT void JNICALL
Java_org_vita3k_emulator_overlay_InputOverlay_setDeviceNativeRotation(JNIEnv *env, jobject thiz, jint rotation) {
    auto *emuenv = get_emuenv();
    if (!emuenv || !emuenv->renderer)
        return;

    emuenv->renderer->set_device_native_rotation(rotation);
}

JNIEXPORT void JNICALL
Java_org_vita3k_emulator_Emulator_setDirectPad(JNIEnv *env, jclass, jint device_id, jstring name, jint vendor, jint product, jint crc, jint buttons, jfloatArray axes) {
    auto *emuenv = get_emuenv();
    if (!emuenv || !axes || env->GetArrayLength(axes) < 6)
        return;
    float values[6];
    env->GetFloatArrayRegion(axes, 0, 6, values);
    const char *chars = name ? env->GetStringUTFChars(name, nullptr) : nullptr;
    const std::string name_string = chars ? chars : "";
    if (chars)
        env->ReleaseStringUTFChars(name, chars);
    ctrl_set_direct_pad(*emuenv, device_id, name_string, static_cast<uint16_t>(vendor), static_cast<uint16_t>(product), static_cast<uint16_t>(crc), static_cast<uint32_t>(buttons), values);
}

JNIEXPORT void JNICALL
Java_org_vita3k_emulator_Emulator_removeDirectPad(JNIEnv *env, jclass, jint device_id) {
    auto *emuenv = get_emuenv();
    if (emuenv)
        ctrl_remove_direct_pad(*emuenv, device_id);
}

} // extern "C"
