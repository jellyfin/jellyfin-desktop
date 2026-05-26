//! Shared scaffolding for OS "now playing" media sinks (macOS
//! MPNowPlayingInfoCenter, Windows SMTC). Both platforms drove an
//! identical queue + consumer-thread harness and the same
//! kind→phase / command-dispatch logic; that lives here once. Each
//! platform supplies only a [`MediaSink`] whose `deliver` drives its
//! native transport.
//!
//! The Linux MPRIS sink ([`crate::mpris_sink`]) has its own zbus-reactor
//! thread and does not use [`run_sink`], but it shares [`MediaCommand`] /
//! [`seek_to_ms`] so transport command semantics live in one place.

// =====================================================================
// Transport commands — shared by every sink (macOS / Windows / MPRIS).
// =====================================================================

/// A media-key / remote command the OS transport can raise.
#[derive(Copy, Clone, PartialEq, Eq)]
pub enum MediaCommand {
    Play,
    Pause,
    PlayPause,
    Stop,
    Next,
    Previous,
}

/// Execute a transport command: play/pause/stop go straight to mpv;
/// next/previous route to the JS UI (the queue lives in jellyfin-web).
pub fn execute(cmd: MediaCommand) {
    match cmd {
        MediaCommand::Play => jfn_mpv::api::jfn_mpv_play(),
        MediaCommand::Pause => jfn_mpv::api::jfn_mpv_pause(),
        MediaCommand::PlayPause => jfn_mpv::api::jfn_mpv_toggle_pause(),
        MediaCommand::Stop => jfn_mpv::api::jfn_mpv_stop(),
        MediaCommand::Next => {
            crate::exec_js::call("if(window._nativeHostInput) window._nativeHostInput(['next']);")
        }
        MediaCommand::Previous => crate::exec_js::call(
            "if(window._nativeHostInput) window._nativeHostInput(['previous']);",
        ),
    }
}

/// Seek the UI to an absolute position in milliseconds. Routes to the JS
/// UI, which is the seek authority; mpv follows once the UI re-issues play.
pub fn seek_to_ms(ms: i64) {
    crate::exec_js::call(&format!("if(window._nativeSeek) window._nativeSeek({ms});"));
}

// =====================================================================
// Consumer-thread harness — used only by the macOS / Windows sinks; the
// Linux MPRIS sink runs its own zbus thread.
// =====================================================================

#[cfg(any(target_os = "macos", target_os = "windows"))]
mod harness {
    use parking_lot::{Condvar, Mutex};
    use std::collections::VecDeque;
    use std::sync::atomic::{AtomicBool, Ordering};
    use std::sync::{Arc, OnceLock};
    use std::time::Duration;

    use crate::types::{PlaybackEvent, PlaybackEventKind};

    /// Coarse playback phase the OS transports care about. mpv's richer
    /// `PlaybackPhase` collapses to these three for now-playing display.
    #[derive(Copy, Clone, PartialEq, Eq)]
    pub enum Phase {
        Playing,
        Paused,
        Stopped,
    }

    /// Map a playback event kind to the coarse transport phase.
    pub fn map_kind_to_phase(kind: PlaybackEventKind) -> Phase {
        match kind {
            PlaybackEventKind::Started => Phase::Playing,
            PlaybackEventKind::Paused | PlaybackEventKind::TrackLoaded => Phase::Paused,
            PlaybackEventKind::Finished
            | PlaybackEventKind::Canceled
            | PlaybackEventKind::Error => Phase::Stopped,
            _ => Phase::Stopped,
        }
    }

    /// A platform now-playing transport. The harness owns the event queue
    /// and the consumer thread; the impl only reacts to events.
    ///
    /// Not `Send`: the impl is built on, and never leaves, the consumer
    /// thread — only the `build` closure crosses the thread boundary. This
    /// lets backends hold thread-affine handles (e.g. Windows COM SMTC
    /// interfaces) directly.
    pub trait MediaSink {
        /// Called once on the consumer thread before draining begins.
        fn init(&mut self);
        /// Called for every queued event, in order.
        fn deliver(&mut self, ev: &PlaybackEvent);
        /// Called once on the consumer thread after `running` clears.
        fn teardown(&mut self);
    }

    struct Inner {
        queue: Mutex<VecDeque<PlaybackEvent>>,
        cv: Condvar,
        running: AtomicBool,
    }

    static SINK: OnceLock<Arc<Inner>> = OnceLock::new();

    const EVENT_QUEUE_CAP: usize = 256;

    fn inner() -> Arc<Inner> {
        SINK.get_or_init(|| {
            Arc::new(Inner {
                queue: Mutex::new(VecDeque::new()),
                cv: Condvar::new(),
                running: AtomicBool::new(false),
            })
        })
        .clone()
    }

    // Coordinator-side hook: jfn-playback invokes this for every event.
    // Cloning is cheap relative to the OS transport round-trips that follow.
    fn on_event(ev: &PlaybackEvent) {
        let inner = inner();
        {
            let mut q = inner.queue.lock();
            if q.len() >= EVENT_QUEUE_CAP {
                return;
            }
            q.push_back(ev.clone());
        }
        inner.cv.notify_one();
    }

    /// Start the process-wide media sink. `build` constructs the platform
    /// [`MediaSink`] on the consumer thread (so native transport handles are
    /// created there). No-op if already running.
    pub fn run_sink<S, F>(thread_name: &str, build: F)
    where
        S: MediaSink,
        F: FnOnce() -> S + Send + 'static,
    {
        let inner = inner();
        if inner.running.swap(true, Ordering::AcqRel) {
            return;
        }

        crate::ffi::register_event_sink(Box::new(on_event));

        std::thread::Builder::new()
            .name(thread_name.to_owned())
            .spawn(move || consumer_thread(inner, build))
            .expect("spawn media-sink thread");
    }

    /// Signal the consumer thread to exit at its next wake. No-op if not
    /// running.
    pub fn stop() {
        let inner = match SINK.get() {
            Some(i) => i.clone(),
            None => return,
        };
        if !inner.running.swap(false, Ordering::AcqRel) {
            return;
        }
        inner.cv.notify_all();
    }

    fn consumer_thread<S: MediaSink>(inner: Arc<Inner>, build: impl FnOnce() -> S) {
        let mut sink = build();
        sink.init();

        while inner.running.load(Ordering::Acquire) {
            let drained: Vec<PlaybackEvent> = {
                let mut q = inner.queue.lock();
                while q.is_empty() && inner.running.load(Ordering::Acquire) {
                    inner.cv.wait_for(&mut q, Duration::from_millis(100));
                }
                q.drain(..).collect()
            };
            for ev in &drained {
                sink.deliver(ev);
            }
        }

        sink.teardown();
    }
}

#[cfg(any(target_os = "macos", target_os = "windows"))]
pub use harness::{MediaSink, Phase, map_kind_to_phase, run_sink, stop};
