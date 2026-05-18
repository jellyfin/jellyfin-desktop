//! Coordinator: owns the single mutable state machine, worker thread, and
//! sink fanout. Producers post inputs from any thread; the worker drains
//! them in batches, runs transitions, publishes the canonical snapshot,
//! and hands events to registered sinks via the FFI vtable.

use std::collections::VecDeque;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};

use crate::wake_event::WakeEvent;

use crate::ffi::{ActionSinkEntry, EventSinkEntry};
use crate::state_machine::PlaybackStateMachine;
use crate::types::*;

#[derive(Debug)]
pub(crate) enum Input {
    FileLoaded,
    LoadStarting(String),
    PauseChanged(bool),
    EndFile {
        reason: EndReason,
        error_message: String,
    },
    SeekingChanged(bool),
    PausedForCache(bool),
    CoreIdle(bool),
    Position(i64),
    MediaType(MediaType),
    VideoFrameAvailable(bool),
    Speed(f64),
    Duration(i64),
    Fullscreen {
        fullscreen: bool,
        was_maximized: bool,
    },
    OsdDims {
        lw: i32,
        lh: i32,
        pw: i32,
        ph: i32,
    },
    BufferedRanges(Vec<PlaybackBufferedRange>),
    DisplayHz(f64),
    Metadata(MediaMetadata),
    Artwork(String),
    QueueCaps {
        can_go_next: bool,
        can_go_prev: bool,
    },
    Seeked(i64),
}

struct Shared {
    queue: Mutex<VecDeque<Input>>,
    wake: WakeEvent,
    running: AtomicBool,
    snapshot: Mutex<PlaybackSnapshot>,
    event_sinks: Mutex<Vec<EventSinkEntry>>,
    action_sinks: Mutex<Vec<ActionSinkEntry>>,
}

pub struct PlaybackCoordinator {
    shared: Arc<Shared>,
    join: Option<JoinHandle<()>>,
}

impl PlaybackCoordinator {
    pub fn new() -> Self {
        Self {
            shared: Arc::new(Shared {
                queue: Mutex::new(VecDeque::new()),
                wake: WakeEvent::new().expect("WakeEvent::new"),
                running: AtomicBool::new(false),
                snapshot: Mutex::new(PlaybackSnapshot::fresh()),
                event_sinks: Mutex::new(Vec::new()),
                action_sinks: Mutex::new(Vec::new()),
            }),
            join: None,
        }
    }

    pub fn start(&mut self) {
        if self.shared.running.swap(true, Ordering::SeqCst) {
            return;
        }
        let shared = Arc::clone(&self.shared);
        self.join = Some(thread::spawn(move || worker(shared)));
    }

    pub fn stop(&mut self) {
        if !self.shared.running.swap(false, Ordering::SeqCst) {
            return;
        }
        self.shared.wake.signal();
        if let Some(h) = self.join.take() {
            let _ = h.join();
        }
    }

    pub(crate) fn enqueue(&self, in_: Input) {
        {
            let mut q = self.shared.queue.lock().unwrap();
            q.push_back(in_);
        }
        self.shared.wake.signal();
    }

    pub fn snapshot(&self) -> PlaybackSnapshot {
        self.shared.snapshot.lock().unwrap().clone()
    }

    pub(crate) fn add_event_sink(&self, sink: EventSinkEntry) {
        self.shared.event_sinks.lock().unwrap().push(sink);
    }

    pub(crate) fn add_action_sink(&self, sink: ActionSinkEntry) {
        self.shared.action_sinks.lock().unwrap().push(sink);
    }
}

impl Drop for PlaybackCoordinator {
    fn drop(&mut self) {
        self.stop();
    }
}

fn worker(shared: Arc<Shared>) {
    let mut sm = PlaybackStateMachine::new();
    while shared.running.load(Ordering::Relaxed) {
        let work: VecDeque<Input> = {
            let mut q = shared.queue.lock().unwrap();
            std::mem::take(&mut *q)
        };

        if work.is_empty() {
            wait_for_wake(&shared.wake);
            shared.wake.drain();
            continue;
        }

        let mut events: Vec<PlaybackEvent> = Vec::new();
        let mut actions: Vec<PlaybackAction> = Vec::new();
        for input in work {
            apply(&mut sm, input, &mut events);
            actions.extend(sm.consume_actions());
        }

        let snap = sm.snapshot();
        for e in &mut events {
            e.snapshot = snap.clone();
        }
        {
            let mut s = shared.snapshot.lock().unwrap();
            *s = snap;
        }

        // Sinks: dispatched in registration order. Each sink's try_post
        // must not block; the sink owns its own queue + consumer thread.
        let event_sinks = shared.event_sinks.lock().unwrap();
        for sink in event_sinks.iter() {
            for e in &events {
                sink.dispatch(e);
            }
        }
        let action_sinks = shared.action_sinks.lock().unwrap();
        for sink in action_sinks.iter() {
            for a in &actions {
                sink.dispatch(a);
            }
        }
    }
}

#[cfg(unix)]
fn wait_for_wake(w: &WakeEvent) {
    let mut pfd = libc::pollfd {
        fd: w.fd(),
        events: libc::POLLIN,
        revents: 0,
    };
    unsafe {
        libc::poll(&mut pfd, 1, -1);
    }
}

#[cfg(windows)]
fn wait_for_wake(w: &WakeEvent) {
    use windows_sys::Win32::System::Threading::WaitForSingleObject;
    unsafe {
        WaitForSingleObject(w.handle(), u32::MAX);
    }
}

fn apply(sm: &mut PlaybackStateMachine, input: Input, out: &mut Vec<PlaybackEvent>) {
    let mut emitted = match input {
        Input::FileLoaded => sm.on_file_loaded(),
        Input::LoadStarting(id) => sm.on_load_starting(id),
        Input::PauseChanged(paused) => sm.on_pause_changed(paused),
        Input::EndFile { reason, error_message } => sm.on_end_file(reason, error_message),
        Input::SeekingChanged(seeking) => sm.on_seeking_changed(seeking),
        Input::PausedForCache(pfc) => sm.on_paused_for_cache(pfc),
        Input::CoreIdle(ci) => sm.on_core_idle(ci),
        Input::Position(p) => sm.on_position(p),
        Input::MediaType(t) => sm.on_media_type(t),
        Input::VideoFrameAvailable(a) => sm.on_video_frame_available(a),
        Input::Speed(r) => sm.on_speed(r),
        Input::Duration(d) => sm.on_duration(d),
        Input::Fullscreen { fullscreen, was_maximized } => {
            sm.on_fullscreen(fullscreen, was_maximized)
        }
        Input::OsdDims { lw, lh, pw, ph } => sm.on_osd_dims(lw, lh, pw, ph),
        Input::BufferedRanges(r) => sm.on_buffered_ranges(r),
        Input::DisplayHz(h) => sm.on_display_hz(h),
        Input::Metadata(m) => {
            // Route media_type through the SM so snapshot.media_type
            // tracks metadata changes (idle inhibit reads it).
            let mut events = sm.on_media_type(m.media_type);
            let mut ev = PlaybackEvent::new(PlaybackEventKind::MetadataChanged);
            ev.metadata = m;
            events.push(ev);
            events
        }
        Input::Artwork(uri) => {
            let mut ev = PlaybackEvent::new(PlaybackEventKind::ArtworkChanged);
            ev.artwork_uri = uri;
            vec![ev]
        }
        Input::QueueCaps { can_go_next, can_go_prev } => {
            let mut ev = PlaybackEvent::new(PlaybackEventKind::QueueCapsChanged);
            ev.can_go_next = can_go_next;
            ev.can_go_prev = can_go_prev;
            vec![ev]
        }
        Input::Seeked(p) => {
            let mut events = sm.on_position(p);
            events.push(PlaybackEvent::new(PlaybackEventKind::Seeked));
            events
        }
    };
    out.append(&mut emitted);
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::{Duration, Instant};

    #[test]
    fn snapshot_starts_fresh() {
        let coord = PlaybackCoordinator::new();
        let s = coord.snapshot();
        assert_eq!(s.presence, PlayerPresence::None);
        assert_eq!(s.phase, PlaybackPhase::Stopped);
        assert_eq!(s.rate, 1.0);
    }

    #[test]
    fn worker_updates_snapshot_after_input() {
        let mut coord = PlaybackCoordinator::new();
        coord.start();
        coord.enqueue(Input::FileLoaded);
        let deadline = Instant::now() + Duration::from_millis(500);
        loop {
            let s = coord.snapshot();
            if s.presence == PlayerPresence::Present {
                assert_eq!(s.phase, PlaybackPhase::Starting);
                break;
            }
            if Instant::now() > deadline {
                panic!("snapshot never updated");
            }
            thread::sleep(Duration::from_millis(2));
        }
        coord.stop();
    }
}
