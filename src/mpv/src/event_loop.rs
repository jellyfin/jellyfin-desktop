//! Background thread that drains `mpv_wait_event` and delivers owned
//! [`Event`]s to a consumer.
//!
//! libmpv's event queue must be drained from somewhere; the standard pattern
//! is a dedicated thread blocked in `mpv_wait_event`. This module wraps that
//! loop so callers see a typed [`Receiver<Event>`] instead of raw FFI.
//!
//! The loop exits when:
//! - libmpv delivers `MPV_EVENT_SHUTDOWN` (forwarded to the consumer first), or
//! - [`EventLoop::stop`] is called, or
//! - the consumer drops the receiver (send error breaks the loop).

use crate::event::Event;
use crate::handle::Handle;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{Receiver, Sender, channel};
use std::thread::{self, JoinHandle};

pub struct EventLoop {
    handle: Arc<Handle>,
    stop: Arc<AtomicBool>,
    thread: Option<JoinHandle<()>>,
}

impl EventLoop {
    /// Spawn the drain thread. Returns the loop owner and a [`Receiver`]
    /// for typed events. The wakeup callback on `handle` is left untouched —
    /// `mpv_wait_event(-1)` blocks until libmpv has something to deliver.
    pub fn spawn(handle: Arc<Handle>) -> (Self, Receiver<Event>) {
        let (tx, rx) = channel();
        let stop = Arc::new(AtomicBool::new(false));
        let thread = {
            let handle = Arc::clone(&handle);
            let stop = Arc::clone(&stop);
            thread::Builder::new()
                .name("jfn-mpv-events".into())
                .spawn(move || drain(handle, stop, tx))
                .expect("spawn jfn-mpv-events thread")
        };
        (
            Self {
                handle,
                stop,
                thread: Some(thread),
            },
            rx,
        )
    }

    /// Signal the loop to exit and wake `mpv_wait_event` so the next
    /// iteration observes the flag. Joins the thread.
    pub fn stop(&mut self) {
        if self.thread.is_none() {
            return;
        }
        self.stop.store(true, Ordering::Release);
        self.handle.wakeup();
        if let Some(t) = self.thread.take() {
            let _ = t.join();
        }
    }
}

impl Drop for EventLoop {
    fn drop(&mut self) {
        self.stop();
    }
}

fn drain(handle: Arc<Handle>, stop: Arc<AtomicBool>, tx: Sender<Event>) {
    loop {
        if stop.load(Ordering::Acquire) {
            return;
        }
        let event = handle.wait_event(-1.0);
        match event {
            // Timeout sentinel; spurious wakeup (e.g. from `Handle::wakeup`).
            Event::None => continue,
            Event::Shutdown => {
                // Forward shutdown so consumers can react, then exit.
                let _ = tx.send(Event::Shutdown);
                return;
            }
            other => {
                if tx.send(other).is_err() {
                    return;
                }
            }
        }
    }
}
