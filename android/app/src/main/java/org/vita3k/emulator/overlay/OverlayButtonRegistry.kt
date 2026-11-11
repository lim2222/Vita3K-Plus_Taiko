package org.vita3k.emulator.overlay

object OverlayButtonRegistry {
    // 顺序即 bit 位索引,新增按钮永远加在最后,不要中途插入/删除,否则旧存档位错乱
    val ids: List<Int> = listOf(
        ButtonType.BUTTON_CROSS,      // bit 0
        ButtonType.BUTTON_CIRCLE,     // bit 1
        ButtonType.BUTTON_SQUARE,     // bit 2
        ButtonType.BUTTON_TRIANGLE,   // bit 3
        ButtonType.BUTTON_SELECT,     // bit 4
        ButtonType.BUTTON_START,      // bit 5
        ButtonType.BUTTON_PS,         // bit 6
        ButtonType.DPAD_UP,           // bit 7 (整个方向键当一个开关)
        ButtonType.STICK_LEFT,        // bit 8
        ButtonType.STICK_RIGHT,       // bit 9
        ButtonType.TRIGGER_L,         // bit 10
        ButtonType.TRIGGER_R,         // bit 11
        ButtonType.TRIGGER_L2,        // bit 12
        ButtonType.TRIGGER_R2,        // bit 13
        ButtonType.TRIGGER_L3,        // bit 14
        ButtonType.TRIGGER_R3,        // bit 15
        ButtonType.BUTTON_TOUCH_SWITCH, // bit 16
        ButtonType.BUTTON_TOUCH_HIDE,   // bit 17
        ButtonType.TATACON,             // bit 18 — 之后加鼓时用
        ButtonType.BUTTON_FAST_FORWARD, // bit 19 — 快进工具键
        ButtonType.BUTTON_SLOW_MOTION    // bit 20 — 慢放工具键
    )

    fun bitFor(legacyId: Int): Long {
        val idx = ids.indexOf(legacyId)
        return if (idx < 0) 0L else (1L shl idx)
    }

    // 默认全开(不含鼓、快进和慢放: 都是新功能,默认关闭避免打扰现有用户)
    fun defaultMask(): Long =
        ids.fold(0L) { acc, id ->
            if (id == ButtonType.TATACON || id == ButtonType.BUTTON_FAST_FORWARD
                || id == ButtonType.BUTTON_SLOW_MOTION) acc
            else acc or bitFor(id)
        }
}
