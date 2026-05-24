//! xkb keysym → Windows VK code. Ports the lookup in
//! `src/input/keysym_map.h` (only `keysym_to_vkey`; the `keysym_to_keycode`
//! mapping to `input::KeyCode` was unused outside the hotkey path that now
//! works directly off the VK code).

// Letters: XKB_KEY_a..z = 0x61..0x7A, A..Z = 0x41..0x5A.
const XKB_KEY_A: u32 = 0x0041;
const XKB_KEY_Z: u32 = 0x005A;
const XKB_KEY_LC_A: u32 = 0x0061;
const XKB_KEY_LC_Z: u32 = 0x007A;
const XKB_KEY_0: u32 = 0x0030;
const XKB_KEY_9: u32 = 0x0039;
const XKB_KEY_F1: u32 = 0xFFBE;
const XKB_KEY_F12: u32 = 0xFFC9;

pub fn keysym_to_vkey(sym: u32) -> i32 {
    if (XKB_KEY_LC_A..=XKB_KEY_LC_Z).contains(&sym) {
        return (b'A' as u32 + (sym - XKB_KEY_LC_A)) as i32;
    }
    if (XKB_KEY_A..=XKB_KEY_Z).contains(&sym) {
        return sym as i32;
    }
    if (XKB_KEY_0..=XKB_KEY_9).contains(&sym) {
        return sym as i32;
    }
    if (XKB_KEY_F1..=XKB_KEY_F12).contains(&sym) {
        return 0x70 + (sym - XKB_KEY_F1) as i32;
    }

    match sym {
        0xFF0D => 0x0D,          // Return
        0xFF1B => 0x1B,          // Escape
        0xFF09 | 0xFE20 => 0x09, // Tab / ISO_Left_Tab
        0xFF08 => 0x08,          // BackSpace
        0x0020 => 0x20,          // space
        0xFF51 => 0x25,          // Left
        0xFF52 => 0x26,          // Up
        0xFF53 => 0x27,          // Right
        0xFF54 => 0x28,          // Down
        0xFF50 => 0x24,          // Home
        0xFF57 => 0x23,          // End
        0xFF55 => 0x21,          // Page_Up
        0xFF56 => 0x22,          // Page_Down
        0xFFFF => 0x2E,          // Delete
        0xFF63 => 0x2D,          // Insert
        // OEM punctuation. Required so Chromium can derive event.key (e.g.
        // '>' from Shift+Period) for DOM keydown handlers; without a VK
        // here, jellyfin-web shortcuts like '<' / '>' never match.
        0x003B | 0x003A => 0xBA, // semicolon / colon
        0x003D | 0x002B => 0xBB, // equal / plus
        0x002C | 0x003C => 0xBC, // comma / less
        0x002D | 0x005F => 0xBD, // minus / underscore
        0x002E | 0x003E => 0xBE, // period / greater
        0x002F | 0x003F => 0xBF, // slash / question
        0x0060 | 0x007E => 0xC0, // grave / asciitilde
        0x005B | 0x007B => 0xDB, // bracketleft / braceleft
        0x005C | 0x007C => 0xDC, // backslash / bar
        0x005D | 0x007D => 0xDD, // bracketright / braceright
        0x0027 | 0x0022 => 0xDE, // apostrophe / quotedbl
        _ => 0,
    }
}
