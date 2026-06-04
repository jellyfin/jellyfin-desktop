//! SHM segment lifecycle.
//!
//! Wraps `shmget/shmat/shmctl/shmdt` plus the matching x11rb MIT-SHM
//! attach/detach so a `ShmBuffer` ends up registered with the X server
//! and ready for `shm_put_image`.

use x11rb::connection::Connection;
use x11rb::protocol::shm::ConnectionExt as X11rbShmConnection;
use x11rb::rust_connection::RustConnection;

use crate::x11_state::ShmBuffer;

/// Allocate or reuse a SHM buffer at (w, h). Returns false on failure;
/// `buf` is left in its previous state when reuse condition matched, or
/// in `empty()` state on failure.
pub fn shm_alloc(buf: &mut ShmBuffer, conn: &RustConnection, w: i32, h: i32) -> bool {
    let size: usize = (w as usize) * (h as usize) * 4;
    if !buf.data.is_null() && buf.w == w && buf.h == h {
        return true;
    }

    if !buf.data.is_null() {
        let _ = conn.shm_detach(buf.seg);
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

    let Ok(seg) = conn.generate_id() else {
        unsafe { libc::shmdt(buf.data as *const _) };
        buf.data = std::ptr::null_mut();
        return false;
    };
    if conn.shm_attach(seg, shmid as u32, false).is_err() {
        unsafe { libc::shmdt(buf.data as *const _) };
        buf.data = std::ptr::null_mut();
        return false;
    }

    buf.seg = seg;
    buf.w = w;
    buf.h = h;
    buf.size = size;
    true
}

pub fn shm_free(buf: &mut ShmBuffer, conn: Option<&RustConnection>) {
    if buf.data.is_null() {
        return;
    }
    if let Some(c) = conn {
        // Skip detach if seg id is 0 (uninitialized).
        if buf.seg != 0 {
            let _ = c.shm_detach(buf.seg);
        }
    }
    unsafe { libc::shmdt(buf.data as *const _) };
    buf.data = std::ptr::null_mut();
    buf.seg = 0;
    buf.w = 0;
    buf.h = 0;
    buf.size = 0;
}
