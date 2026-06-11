//! Shared xkb modifier-state → CEF `EVENTFLAG_*` translation, used by both
//! the X11 and Wayland input backends (their xkb state is queried identically).

use jfn_platform_abi::event_flags::{
    EVENTFLAG_ALT_DOWN, EVENTFLAG_CONTROL_DOWN, EVENTFLAG_SHIFT_DOWN,
};
use xkbcommon::xkb;

/// Map the effective xkb modifier state to CEF event-flag bits.
pub fn to_cef_mods(st: &xkb::State) -> u32 {
    let mut m = 0u32;
    if st.mod_name_is_active(xkb::MOD_NAME_SHIFT, xkb::STATE_MODS_EFFECTIVE) {
        m |= EVENTFLAG_SHIFT_DOWN;
    }
    if st.mod_name_is_active(xkb::MOD_NAME_CTRL, xkb::STATE_MODS_EFFECTIVE) {
        m |= EVENTFLAG_CONTROL_DOWN;
    }
    if st.mod_name_is_active(xkb::MOD_NAME_ALT, xkb::STATE_MODS_EFFECTIVE) {
        m |= EVENTFLAG_ALT_DOWN;
    }
    m
}
