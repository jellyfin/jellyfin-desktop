//! Coordinator: owns the single mutable state machine, worker thread, and
//! sink fanout. Producers post inputs from any thread; the worker drains
//! them in batches, runs transitions, publishes the canonical snapshot,
//! and hands events to registered sinks via the FFI vtable.

use parking_lot::Mutex;
use std::collections::VecDeque;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread::{self, JoinHandle};

use jfn_wake_event::WakeEvent;

use crate::ffi::{ActionSink, EventSink};
use crate::state_machine::PlaybackStateMachine;
use crate::types::*;

#[derive(Debug)]
pub enum Input {
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
    event_sinks: Mutex<Vec<EventSink>>,
    action_sinks: Mutex<Vec<ActionSink>>,
    builtin_event_sinks: Mutex<Vec<EventSink>>,
    builtin_action_sinks: Mutex<Vec<ActionSink>>,
}

pub struct PlaybackCoordinator {
    shared: Arc<Shared>,
    join: Option<JoinHandle<()>>,
}

#[derive(Clone)]
pub struct CoordinatorHandle(Arc<Shared>);

impl CoordinatorHandle {
    pub fn enqueue(&self, in_: Input) {
        {
            let mut q = self.0.queue.lock();
            q.push_back(in_);
        }
        self.0.wake.signal();
    }

    pub fn snapshot(&self) -> PlaybackSnapshot {
        self.0.snapshot.lock().clone()
    }
}

impl PlaybackCoordinator {
    /// Returns `None` if the wake eventfd can't be created (fd exhaustion).
    pub fn new() -> Option<Self> {
        Some(Self {
            shared: Arc::new(Shared {
                queue: Mutex::new(VecDeque::new()),
                wake: WakeEvent::new()?,
                running: AtomicBool::new(false),
                snapshot: Mutex::new(PlaybackSnapshot::fresh()),
                event_sinks: Mutex::new(Vec::new()),
                action_sinks: Mutex::new(Vec::new()),
                builtin_event_sinks: Mutex::new(Vec::new()),
                builtin_action_sinks: Mutex::new(Vec::new()),
            }),
            join: None,
        })
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
        if let Some(h) = self.join.take()
            && let Err(e) = h.join()
        {
            eprintln!("[playback] coordinator worker panicked: {e:?}");
        }
    }

    pub fn handle(&self) -> CoordinatorHandle {
        CoordinatorHandle(Arc::clone(&self.shared))
    }

    pub fn enqueue(&self, in_: Input) {
        self.handle().enqueue(in_);
    }

    pub fn snapshot(&self) -> PlaybackSnapshot {
        self.handle().snapshot()
    }

    pub fn add_event_sink(&self, sink: EventSink) {
        self.shared.event_sinks.lock().push(sink);
    }

    pub fn add_action_sink(&self, sink: ActionSink) {
        self.shared.action_sinks.lock().push(sink);
    }

    pub fn add_builtin_event_sink(&self, sink: EventSink) {
        self.shared.builtin_event_sinks.lock().push(sink);
    }

    pub fn add_builtin_action_sink(&self, sink: ActionSink) {
        self.shared.builtin_action_sinks.lock().push(sink);
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
            let mut q = shared.queue.lock();
            std::mem::take(&mut *q)
        };

        if work.is_empty() {
            shared.wake.wait();
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
            let mut s = shared.snapshot.lock();
            *s = snap;
        }

        // Sinks: dispatched in registration order. Each closure must
        // not block; sinks own their own queue + consumer thread.
        let event_sinks = shared.event_sinks.lock();
        for sink in event_sinks.iter() {
            for e in &events {
                sink(e);
            }
        }
        let action_sinks = shared.action_sinks.lock();
        for sink in action_sinks.iter() {
            for a in &actions {
                sink(a);
            }
        }
        let builtin_event_sinks = shared.builtin_event_sinks.lock();
        for sink in builtin_event_sinks.iter() {
            for e in &events {
                sink(e);
            }
        }
        let builtin_action_sinks = shared.builtin_action_sinks.lock();
        for sink in builtin_action_sinks.iter() {
            for a in &actions {
                sink(a);
            }
        }
    }
}

fn apply(sm: &mut PlaybackStateMachine, input: Input, out: &mut Vec<PlaybackEvent>) {
    let mut emitted = match input {
        Input::FileLoaded => sm.on_file_loaded(),
        Input::LoadStarting(id) => sm.on_load_starting(id),
        Input::PauseChanged(paused) => sm.on_pause_changed(paused),
        Input::EndFile {
            reason,
            error_message,
        } => sm.on_end_file(reason, error_message),
        Input::SeekingChanged(seeking) => sm.on_seeking_changed(seeking),
        Input::PausedForCache(pfc) => sm.on_paused_for_cache(pfc),
        Input::CoreIdle(ci) => sm.on_core_idle(ci),
        Input::Position(p) => sm.on_position(p),
        Input::MediaType(t) => sm.on_media_type(t),
        Input::VideoFrameAvailable(a) => sm.on_video_frame_available(a),
        Input::Speed(r) => sm.on_speed(r),
        Input::Duration(d) => sm.on_duration(d),
        Input::Fullscreen {
            fullscreen,
            was_maximized,
        } => sm.on_fullscreen(fullscreen, was_maximized),
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
        Input::QueueCaps {
            can_go_next,
            can_go_prev,
        } => {
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
    use std::sync::mpsc;

    #[test]
    fn snapshot_starts_fresh() {
        let coord = PlaybackCoordinator::new().unwrap();
        let s = coord.snapshot();
        assert_eq!(s.presence, PlayerPresence::None);
        assert_eq!(s.phase, PlaybackPhase::Stopped);
        assert_eq!(s.rate, 1.0);
    }

    #[test]
    fn worker_updates_snapshot_after_input() {
        let mut coord = PlaybackCoordinator::new().unwrap();
        coord.start();
        // Register a sink BEFORE enqueuing so the first dispatched batch
        // signals the channel. Sinks fire on the worker thread after the
        // snapshot is published, so receiving = snapshot is up-to-date.
        let (tx, rx) = mpsc::sync_channel::<()>(1);
        coord.add_event_sink(Box::new(move |_ev| {
            let _ = tx.try_send(());
        }));
        coord.enqueue(Input::FileLoaded);
        rx.recv().expect("worker never published an event");
        let s = coord.snapshot();
        assert_eq!(s.presence, PlayerPresence::Present);
        assert_eq!(s.phase, PlaybackPhase::Starting);
        coord.stop();
    }
}
