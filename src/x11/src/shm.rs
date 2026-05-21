//! SHM segment lifecycle.
//!
//! Wraps `shmget/shmat/shmctl/shmdt` plus the matching xcb-shm
//! attach/detach so a `ShmBuffer` ends up registered with the X server
//! and ready for `xcb_shm_put_image`.

use xcb::{Xid, XidNew};

use crate::x11_state::ShmBuffer;

/// Allocate or reuse a SHM buffer at (w, h). Returns false on failure;
/// `buf` is left in its previous state when reuse condition matched, or
/// in `empty()` state on failure.
pub fn shm_alloc(buf: &mut ShmBuffer, conn: &xcb::Connection, w: i32, h: i32) -> bool {
    let size: usize = (w as usize) * (h as usize) * 4;
    if !buf.data.is_null() && buf.w == w && buf.h == h {
        return true;
    }

    if !buf.data.is_null() {
        conn.send_request(&xcb::shm::Detach { shmseg: buf.seg });
        unsafe { libc::shmdt(buf.data as *const _) };
        buf.data = std::ptr::null_mut();
    }

    let shmid = unsafe { libc::shmget(libc::IPC_PRIVATE, size, libc::IPC_CREAT | 0o600) };
    if shmid < 0 {
        return false;
    }
    buf.shmid = shmid;

    let p = unsafe { libc::shmat(shmid, std::ptr::null(), 0) };
    if p == (-1isize) as *mut _ {
        unsafe { libc::shmctl(shmid, libc::IPC_RMID, std::ptr::null_mut()) };
        buf.data = std::ptr::null_mut();
        return false;
    }
    buf.data = p as *mut u8;

    // Mark for removal — kernel frees once last process detaches.
    unsafe { libc::shmctl(shmid, libc::IPC_RMID, std::ptr::null_mut()) };

    let seg: xcb::shm::Seg = conn.generate_id();
    conn.send_request(&xcb::shm::Attach {
        shmseg: seg,
        shmid: shmid as u32,
        read_only: false,
    });

    buf.seg = seg;
    buf.w = w;
    buf.h = h;
    buf.size = size;
    true
}

pub fn shm_free(buf: &mut ShmBuffer, conn: Option<&xcb::Connection>) {
    if buf.data.is_null() {
        return;
    }
    if let Some(c) = conn {
        // Skip detach if seg id is 0 (uninitialized).
        if buf.seg.resource_id() != 0 {
            c.send_request(&xcb::shm::Detach { shmseg: buf.seg });
        }
    }
    unsafe { libc::shmdt(buf.data as *const _) };
    buf.data = std::ptr::null_mut();
    buf.seg = xcb::shm::Seg::new(0);
    buf.w = 0;
    buf.h = 0;
    buf.size = 0;
}
