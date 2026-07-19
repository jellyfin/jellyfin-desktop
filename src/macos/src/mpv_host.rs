//! macOS [`MpvHost`]: pre-create environment (bundle marker, MoltenVK
//! heap workaround) and a VO wait loop that keeps the main CFRunLoop
//! serviced while mpv brings its window up.

use std::ffi::c_int;

use jfn_platform_abi::{MpvHost, WindowDecorations};

/// Whether the system's Metal device advertises the Mac2 GPU family.
///
/// MoltenVK's MTLHeap (placement-heap) path requires Mac2-class features.
/// Legacy Intel GPUs — e.g. the Iris Pro 5200, which reports only "Metal
/// GPUFamily macOS 1" — lack them, and the Apple driver aborts on the
/// first libplacebo GPU upload when MoltenVK tries to use heaps there.
/// Probing the live device (rather than matching model names) keeps the
/// workaround tied to the actual capability. Returns `true` when no Metal
/// device is present, so a machine we cannot probe keeps the fast path.
fn metal_has_mac2_family() -> bool {
    use objc2::runtime::AnyObject;
    // MTLGPUFamilyMac2, from <Metal/MTLDevice.h>.
    const MTL_GPU_FAMILY_MAC2: isize = 2002;
    #[link(name = "Metal", kind = "framework")]
    unsafe extern "C" {
        fn MTLCreateSystemDefaultDevice() -> *mut AnyObject;
    }
    unsafe {
        let device = MTLCreateSystemDefaultDevice();
        if device.is_null() {
            return true;
        }
        let has_mac2: bool = objc2::msg_send![device, supportsFamily: MTL_GPU_FAMILY_MAC2];
        let _: () = objc2::msg_send![device, release];
        has_mac2
    }
}

pub struct MacosMpvHost;

impl MpvHost for MacosMpvHost {
    fn prepare(&self, _configured: Option<WindowDecorations>) {
        unsafe {
            // Used by mpv's macOS Cocoa Common to locate the bundle.
            let key = c"MPVBUNDLE";
            let val = c"true";
            libc::setenv(key.as_ptr(), val.as_ptr(), 1);

            // MoltenVK's MTLHeap path crashes on legacy Metal GPUs: the Apple
            // Intel driver (e.g. Iris Pro 5200, which reports only "Metal
            // GPUFamily macOS 1") rejects the heap descriptor and aborts on the
            // first frame in libplacebo's GPU upload. Placement heaps require
            // the Mac2 feature set, so disable MoltenVK heaps only where Mac2 is
            // absent — Apple Silicon and Metal-3-class Intel keep the fast path.
            // The per-resource MTLBuffer/MTLTexture fallback is correct on every
            // GPU; the cost is negligible.
            if metal_has_mac2_family() {
                tracing::debug!(
                    target: "Platform",
                    "Metal Mac2 family present; keeping MoltenVK MTLHeap path"
                );
            } else {
                let key = c"MVK_CONFIG_USE_MTLHEAP";
                let val = c"0";
                libc::setenv(key.as_ptr(), val.as_ptr(), 1);
                tracing::info!(
                    target: "Platform",
                    "legacy Metal GPU without Mac2 family; disabled MoltenVK MTLHeap (MVK_CONFIG_USE_MTLHEAP=0)"
                );
            }
        }
    }

    fn run_vo_wait(&self, pump: &mut dyn FnMut(bool) -> bool) {
        unsafe {
            jfn_mpv::api::jfn_mpv_set_wakeup_callback(
                crate::macos_mpv_wakeup_cb,
                std::ptr::null_mut(),
            );
        }
        // Block until the main run loop services a source — e.g. the
        // dispatch block posted by the wakeup callback — never inside
        // mpv's own blocking wait, which would starve the run loop.
        while pump(false) {
            crate::macos_pump_block(60.0);
        }
        jfn_mpv::api::jfn_mpv_clear_wakeup_callback();
    }

    fn logical_content_size(&self) -> Option<(i32, i32)> {
        let mut w: c_int = 0;
        let mut h: c_int = 0;
        if crate::init::jfn_macos_query_logical_content_size(&mut w, &mut h) {
            Some((w, h))
        } else {
            None
        }
    }
}
