//! Logging backend.
//!
//! Two writers, both wrapped with `tracing_appender::non_blocking`:
//! - stderr (always)
//! - size-rotated file (optional, when `path` is non-empty)
//!
//! Every emitted line is filtered through the `redact` module so auth tokens
//! are 'x'-ed out. Anything other code writes to the real stderr (CEF
//! subprocesses, ffmpeg) is captured by a pipe-and-poll thread and
//! re-emitted as `[CEF]` debug records.

mod redact;

use parking_lot::Mutex;
use std::fs::{File, OpenOptions};
use std::io::{self, Write};
use std::path::{Path, PathBuf};
use std::sync::OnceLock;
use std::sync::atomic::{AtomicBool, Ordering};

use time::OffsetDateTime;
use time::format_description::FormatItem;
use time::macros::format_description;

use tracing_appender::non_blocking::{NonBlockingBuilder, WorkerGuard};
use tracing_core::{
    Callsite, Interest, Metadata, field::FieldSet, identify_callsite, metadata::Kind,
};
use tracing_subscriber::{EnvFilter, Registry, fmt, layer::SubscriberExt};

const CATEGORY_NAMES: &[&str] = &["Main", "mpv", "CEF", "Media", "Platform", "JS", "Resource"];

// Public category/level constants. Indices into CATEGORY_NAMES, and the u8
// values accepted by `log` / `log_enabled` / `jfn_log` / `jfn_log_enabled`.
pub const CATEGORY_CEF: u8 = 2;
pub const CATEGORY_JS: u8 = 5;
pub const CATEGORY_RESOURCE: u8 = 6;
pub const LEVEL_DEBUG: u8 = 1;
pub const LEVEL_INFO: u8 = 2;
pub const LEVEL_WARN: u8 = 3;
pub const LEVEL_ERROR: u8 = 4;

#[repr(u8)]
#[derive(Clone, Copy, Eq, PartialEq, Debug)]
enum Level {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
}

impl Level {
    fn from_u8(v: u8) -> Self {
        match v {
            0 => Level::Trace,
            1 => Level::Debug,
            2 => Level::Info,
            3 => Level::Warn,
            _ => Level::Error,
        }
    }
    fn label(self) -> &'static str {
        match self {
            Level::Trace => "TRACE",
            Level::Debug => "DEBUG",
            Level::Info => "INFO",
            Level::Warn => "WARN",
            Level::Error => "ERROR",
        }
    }
}

// =====================================================================
// Rotating file writer
// =====================================================================

const MAX_FILE_BYTES: u64 = 10 * 1024 * 1024;
const MAX_BACKUPS: usize = 3;

struct RotatingFile {
    path: PathBuf,
    file: File,
    bytes_written: u64,
}

impl RotatingFile {
    fn open(path: PathBuf) -> io::Result<Self> {
        // Start each run with a fresh file; prior run's contents shift into
        // the backup chain.
        rotate_backups(&path)?;
        let file = OpenOptions::new()
            .write(true)
            .create(true)
            .truncate(true)
            .open(&path)?;
        Ok(Self {
            path,
            file,
            bytes_written: 0,
        })
    }

    fn maybe_rotate(&mut self, incoming: usize) -> io::Result<()> {
        if self.bytes_written + incoming as u64 <= MAX_FILE_BYTES {
            return Ok(());
        }
        self.file.flush()?;
        rotate_backups(&self.path)?;
        self.file = OpenOptions::new()
            .write(true)
            .create(true)
            .truncate(true)
            .open(&self.path)?;
        self.bytes_written = 0;
        Ok(())
    }
}

fn rotate_backups(path: &PathBuf) -> io::Result<()> {
    let oldest = backup_path(path, MAX_BACKUPS);
    let _ = std::fs::remove_file(&oldest);
    for i in (1..MAX_BACKUPS).rev() {
        let src = backup_path(path, i);
        let dst = backup_path(path, i + 1);
        if src.exists() {
            std::fs::rename(&src, &dst)?;
        }
    }
    if path.exists() {
        std::fs::rename(path, backup_path(path, 1))?;
    }
    Ok(())
}

fn backup_path(path: &Path, n: usize) -> PathBuf {
    let mut s = path.as_os_str().to_owned();
    s.push(format!(".{}", n));
    PathBuf::from(s)
}

impl Write for RotatingFile {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        self.maybe_rotate(buf.len())?;
        let n = self.file.write(buf)?;
        self.bytes_written += n as u64;
        Ok(n)
    }
    fn flush(&mut self) -> io::Result<()> {
        self.file.flush()
    }
}

// =====================================================================
// Per-OS console writer + stderr capture
// =====================================================================

#[cfg(unix)]
mod imp {
    use super::{CATEGORY_CEF, Level, emit};
    use nix::errno::Errno;
    use nix::poll::{PollFd, PollFlags, PollTimeout, poll};
    use nix::unistd::{dup, dup2_stderr, isatty, pipe, read, write};
    use std::io::{self, Write};
    use std::os::fd::{AsFd, OwnedFd};
    use std::thread::{self, JoinHandle};

    // Holds a dup of the original stderr taken before StderrCapture's
    // dup2 redirect; writing via io::stderr() here would feed each log
    // line back into the capture pipe.
    pub(super) struct StderrWriter {
        fd: Option<OwnedFd>,
    }

    impl Write for StderrWriter {
        fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
            match &self.fd {
                Some(fd) => Ok(write(fd, buf)?),
                None => Err(Errno::EBADF.into()),
            }
        }
        fn flush(&mut self) -> io::Result<()> {
            Ok(())
        }
    }

    pub(super) fn make_console_writer() -> (StderrWriter, bool) {
        let fd = dup(io::stderr()).ok();
        let is_tty = fd.as_ref().is_some_and(|fd| isatty(fd).unwrap_or(false));
        (StderrWriter { fd }, is_tty)
    }

    pub(super) struct StderrCapture {
        original_fd: Option<OwnedFd>,
        signal_write: Option<OwnedFd>,
        join: Option<JoinHandle<()>>,
    }

    impl StderrCapture {
        pub(super) fn start() -> Option<Self> {
            let original = dup(io::stderr()).ok()?;
            let (pipe_read, pipe_write) = pipe().ok()?;
            let (signal_read, signal_write) = pipe().ok()?;

            dup2_stderr(&pipe_write).ok()?;
            drop(pipe_write);

            let join = thread::spawn(move || capture_loop(&pipe_read, &signal_read));

            Some(StderrCapture {
                original_fd: Some(original),
                signal_write: Some(signal_write),
                join: Some(join),
            })
        }

        pub(super) fn stop(&mut self) {
            // Order: restore STDERR FIRST (so any concurrent writer drains to
            // the real fd from now on), THEN wake the capture thread via the
            // signal pipe, THEN join, THEN close the signal write end. Joining
            // before closing signal_write avoids a window where the capture
            // thread races a close on its read end.
            if let Some(original) = self.original_fd.take() {
                let _ = dup2_stderr(&original);
            }
            if let Some(w) = &self.signal_write {
                let _ = write(w, b"x");
            }
            if let Some(h) = self.join.take()
                && let Err(e) = h.join()
            {
                eprintln!("[logging] stderr capture thread panicked: {e:?}");
            }
            self.signal_write = None;
        }
    }

    fn capture_loop(pipe_read: &OwnedFd, signal_read: &OwnedFd) {
        let mut buf = [0u8; 4096];
        let mut partial = Vec::<u8>::new();
        loop {
            let mut pfds = [
                PollFd::new(pipe_read.as_fd(), PollFlags::POLLIN),
                PollFd::new(signal_read.as_fd(), PollFlags::POLLIN),
            ];
            if poll(&mut pfds, PollTimeout::NONE).is_err() {
                break;
            }
            let readable =
                |pfd: &PollFd| pfd.revents().is_some_and(|r| r.contains(PollFlags::POLLIN));
            if readable(&pfds[1]) {
                break;
            }
            if readable(&pfds[0]) {
                let Ok(n) = read(pipe_read, &mut buf) else {
                    break;
                };
                if n == 0 {
                    break;
                }
                partial.extend_from_slice(&buf[..n]);
                while let Some(pos) = partial.iter().position(|&b| b == b'\n') {
                    let line: Vec<u8> = partial.drain(..=pos).take(pos).collect();
                    if !line.is_empty() {
                        let msg = String::from_utf8_lossy(&line).into_owned();
                        emit(CATEGORY_CEF, Level::Debug, &msg);
                    }
                }
            }
        }
    }
}

#[cfg(windows)]
mod imp {
    use std::io::{self, Write};

    fn enable_vt_mode() {
        // Best-effort: tell conhost to honor ANSI SGR escapes on stderr.
        // Win10+ supports ENABLE_VIRTUAL_TERMINAL_PROCESSING; older builds
        // silently fail and we render with no color.
        const ENABLE_VIRTUAL_TERMINAL_PROCESSING: u32 = 0x0004;
        const STD_ERROR_HANDLE: u32 = 0xFFFFFFF4; // (DWORD)-12
        type Handle = *mut std::ffi::c_void;
        type Dword = u32;
        type Bool = i32;
        unsafe extern "system" {
            fn GetStdHandle(nStdHandle: Dword) -> Handle;
            fn GetConsoleMode(hConsoleHandle: Handle, lpMode: *mut Dword) -> Bool;
            fn SetConsoleMode(hConsoleHandle: Handle, dwMode: Dword) -> Bool;
        }
        unsafe {
            let h = GetStdHandle(STD_ERROR_HANDLE);
            if h.is_null() || (h as isize) == -1 {
                return;
            }
            let mut mode: Dword = 0;
            if GetConsoleMode(h, &mut mode) == 0 {
                return;
            }
            SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
    }

    pub(super) struct StderrWriter;

    impl Write for StderrWriter {
        fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
            io::stderr().write(buf)
        }
        fn flush(&mut self) -> io::Result<()> {
            io::stderr().flush()
        }
    }

    pub(super) fn make_console_writer() -> (StderrWriter, bool) {
        use std::io::IsTerminal;
        let is_tty = io::stderr().is_terminal();
        if is_tty {
            enable_vt_mode();
        }
        (StderrWriter, is_tty)
    }

    pub(super) struct StderrCapture;

    impl StderrCapture {
        pub(super) fn start() -> Option<Self> {
            None
        }
        pub(super) fn stop(&mut self) {}
    }
}

// =====================================================================
// State
// =====================================================================

struct State {
    active_log_path: String,
    _console_guard: WorkerGuard,
    _file_guard: Option<WorkerGuard>,
    stderr_capture: Option<imp::StderrCapture>,
}

static STATE: OnceLock<Mutex<Option<State>>> = OnceLock::new();

fn state() -> &'static Mutex<Option<State>> {
    STATE.get_or_init(|| Mutex::new(None))
}

const ISO_FILE_FMT: &[FormatItem<'static>] =
    format_description!("[year]-[month]-[day]T[hour]:[minute]:[second]");
const CONSOLE_TRACE_FMT: &[FormatItem<'static>] =
    format_description!("[hour]:[minute]:[second].[subsecond digits:3]");

// =====================================================================
// Per-(category, level) enablement table — primed once at init from the
// EnvFilter. Lets call sites skip formatting when filtered out, matching
// the gating tracing's own Interest cache provides for pure-Rust callers.
// =====================================================================

const N_LEVELS: usize = 5;
const ENABLED_LEN: usize = 7 * N_LEVELS; // CATEGORY_NAMES.len() == 7

static ENABLED: [AtomicBool; ENABLED_LEN] = [const { AtomicBool::new(false) }; ENABLED_LEN];

#[inline]
fn enabled_slot(category: u8, level: u8) -> usize {
    (category as usize) * N_LEVELS + (level as usize)
}

struct ProbeCallsite;
impl Callsite for ProbeCallsite {
    fn set_interest(&self, _: Interest) {}
    fn metadata(&self) -> &Metadata<'_> {
        // Never invoked: we only feed synthesized Metadata to
        // Dispatch::enabled, which doesn't consult the callsite's own
        // metadata() — only the metadata we pass in.
        unreachable!("ProbeCallsite::metadata should not be called")
    }
}
static PROBE_CS: ProbeCallsite = ProbeCallsite;

fn probe_meta(target: &'static str, level: tracing::Level) -> Metadata<'static> {
    static EMPTY: &[&str] = &[];
    Metadata::new(
        "jfn_log_probe",
        target,
        level,
        None,
        None,
        None,
        FieldSet::new(EMPTY, identify_callsite!(&PROBE_CS)),
        Kind::EVENT,
    )
}

fn compute_enabled_table() -> [bool; ENABLED_LEN] {
    use tracing::Level as L;
    let levels = [
        (0u8, L::TRACE),
        (1, L::DEBUG),
        (2, L::INFO),
        (3, L::WARN),
        (4, L::ERROR),
    ];
    let mut table = [false; ENABLED_LEN];
    for (cat_idx, cat_name) in CATEGORY_NAMES.iter().enumerate() {
        for (lvl_u8, tlvl) in levels {
            let md = probe_meta(cat_name, tlvl);
            let on = tracing::dispatcher::get_default(|d| d.enabled(&md));
            table[enabled_slot(cat_idx as u8, lvl_u8)] = on;
        }
    }
    table
}

fn prime_enabled_table() {
    for (i, on) in compute_enabled_table().iter().enumerate() {
        ENABLED[i].store(*on, Ordering::Relaxed);
    }
}

/// True if the directive string contains a trace-level component
/// anywhere — used to decide whether to prepend HH:MM:SS.mmm on console
/// lines (preserving the previous crate's trace-mode timestamps).
fn directive_contains_trace(filter: &str) -> bool {
    filter.split(',').any(|tok| {
        tok.trim().eq_ignore_ascii_case("trace")
            || tok.trim().to_ascii_lowercase().ends_with("=trace")
    })
}

// =====================================================================
// Emit
// =====================================================================

// `tracing::event!` requires a literal `target` (it builds a `static`
// Callsite at the call site). Since `jfn_log`'s category is a runtime u8,
// we materialize all 8×5 callsites by matching on (category, level)
// before dispatch. Each `event!` arm gets its own callsite + Interest
// cache, matching what pure-Rust callers would see.
macro_rules! emit_with_target {
    ($lvl:expr, $tgt:expr, $msg:expr) => {{
        use tracing::Level as L;
        match $lvl {
            Level::Trace => tracing::event!(target: $tgt, L::TRACE, "{}", $msg),
            Level::Debug => tracing::event!(target: $tgt, L::DEBUG, "{}", $msg),
            Level::Info  => tracing::event!(target: $tgt, L::INFO,  "{}", $msg),
            Level::Warn  => tracing::event!(target: $tgt, L::WARN,  "{}", $msg),
            Level::Error => tracing::event!(target: $tgt, L::ERROR, "{}", $msg),
        }
    }};
}

fn emit(category: u8, level: Level, msg: &str) {
    let msg = msg.trim_end_matches(['\r', '\n']);
    match category {
        0 => emit_with_target!(level, "Main", msg),
        1 => emit_with_target!(level, "mpv", msg),
        2 => emit_with_target!(level, "CEF", msg),
        3 => emit_with_target!(level, "Media", msg),
        4 => emit_with_target!(level, "Platform", msg),
        5 => emit_with_target!(level, "JS", msg),
        6 => emit_with_target!(level, "Resource", msg),
        _ => emit_with_target!(level, "Unknown", msg),
    }
}

pub fn jfn_log_init(path: &str, filter: &str) {
    let path_str = path.to_string();
    let filter_str_raw = filter.to_string();
    let filter_str = if filter_str_raw.trim().is_empty() {
        "info".to_string()
    } else {
        filter_str_raw
    };

    // Bail early on second init: dispatcher is already installed and the
    // capture pipe / guards live in STATE. Mirrors prior behavior.
    {
        let guard = state().lock();
        if guard.is_some() {
            return;
        }
    }

    // Capture a dup of stderr now so console writes survive the later
    // dup2() redirect installed by StderrCapture, and aren't fed back into
    // the capture pipe.
    let (console_writer, is_tty) = imp::make_console_writer();
    let color = is_tty && std::env::var_os("NO_COLOR").is_none();
    let (console_nb, console_guard) = NonBlockingBuilder::default()
        .lossy(true)
        .finish(console_writer);

    let (file_nb, file_guard) = if !path_str.is_empty() {
        match RotatingFile::open(PathBuf::from(&path_str)) {
            Ok(rf) => {
                let (nb, g) = NonBlockingBuilder::default().lossy(true).finish(rf);
                (Some(nb), Some(g))
            }
            Err(_) => (None, None),
        }
    } else {
        (None, None)
    };

    let env_filter = match EnvFilter::try_new(&filter_str) {
        Ok(f) => f,
        Err(_) => EnvFilter::new("info"),
    };

    let trace_mode = directive_contains_trace(&filter_str);

    let console_layer = fmt::layer()
        .event_format(ConsoleFormat { trace_mode, color })
        .with_writer(RedactMake(console_nb));

    let subscriber = Registry::default().with(env_filter).with(console_layer);

    // Add file layer conditionally without changing the subscriber type
    // for the install call. SubscriberExt::with returns a new type each
    // time, so we use boxed dispatch.
    let dispatch: tracing::Dispatch = if let Some(file_nb) = file_nb {
        let file_layer = fmt::layer()
            .event_format(FileFormat)
            .with_writer(RedactMake(file_nb));
        subscriber.with(file_layer).into()
    } else {
        subscriber.into()
    };

    // Fail-soft: if a dispatcher was already installed (unlikely given
    // the STATE.is_some() early-return above, but possible across crate
    // boundaries in tests), proceed without panicking.
    let _ = tracing::dispatcher::set_global_default(dispatch);

    prime_enabled_table();

    let stderr_capture = imp::StderrCapture::start();

    let mut guard = state().lock();
    *guard = Some(State {
        active_log_path: path_str,
        _console_guard: console_guard,
        _file_guard: file_guard,
        stderr_capture,
    });
}

pub fn jfn_log_shutdown() {
    let mut guard = state().lock();
    if let Some(mut s) = guard.take() {
        if let Some(mut cap) = s.stderr_capture.take() {
            cap.stop();
        }
        // Drop file guard first so the file worker flushes before the
        // console worker; final console line therefore appears after
        // file flush completes on exit.
        s._file_guard = None;
        drop(s);
    }
}

pub fn log_enabled(category: u8, level: u8) -> bool {
    if (category as usize) >= CATEGORY_NAMES.len() || (level as usize) >= N_LEVELS {
        return false;
    }
    ENABLED[enabled_slot(category, level)].load(Ordering::Relaxed)
}

pub fn log(category: u8, level: u8, msg: &str) {
    emit(category, Level::from_u8(level), msg);
}

pub fn active_path() -> String {
    let guard = state().lock();
    guard
        .as_ref()
        .map(|st| st.active_log_path.clone())
        .unwrap_or_default()
}

// =====================================================================
// tracing-subscriber FormatEvent impls (commit 2 wires these in)
// =====================================================================

use tracing::{Event, Subscriber, field::Field};
use tracing_subscriber::{
    fmt::{FmtContext, FormatEvent, FormatFields, format::Writer},
    registry::LookupSpan,
};

/// Records only the `"message"` field; ignores any structured fields.
#[derive(Default)]
struct MsgVisitor(String);

impl tracing::field::Visit for MsgVisitor {
    fn record_debug(&mut self, field: &Field, value: &dyn std::fmt::Debug) {
        if field.name() == "message" {
            use std::fmt::Write;
            let _ = write!(self.0, "{value:?}");
        }
    }
    fn record_str(&mut self, field: &Field, value: &str) {
        if field.name() == "message" {
            self.0.push_str(value);
        }
    }
}

struct ConsoleFormat {
    trace_mode: bool,
    color: bool,
}

fn level_to_local_level(l: &tracing::Level) -> Level {
    match *l {
        tracing::Level::TRACE => Level::Trace,
        tracing::Level::DEBUG => Level::Debug,
        tracing::Level::INFO => Level::Info,
        tracing::Level::WARN => Level::Warn,
        tracing::Level::ERROR => Level::Error,
    }
}

fn ansi_for(level: &tracing::Level) -> (&'static str, &'static str) {
    match *level {
        tracing::Level::ERROR => ("\x1b[31m", "\x1b[0m"),
        tracing::Level::WARN => ("\x1b[33m", "\x1b[0m"),
        tracing::Level::INFO => ("\x1b[32m", "\x1b[0m"),
        tracing::Level::DEBUG => ("\x1b[36m", "\x1b[0m"),
        tracing::Level::TRACE => ("\x1b[2m", "\x1b[0m"),
    }
}

impl<S, N> FormatEvent<S, N> for ConsoleFormat
where
    S: Subscriber + for<'a> LookupSpan<'a>,
    N: for<'a> FormatFields<'a> + 'static,
{
    fn format_event(
        &self,
        _ctx: &FmtContext<'_, S, N>,
        mut w: Writer<'_>,
        event: &Event<'_>,
    ) -> std::fmt::Result {
        if self.trace_mode
            && let Ok(now) = OffsetDateTime::now_local()
            && let Ok(s) = now.format(&CONSOLE_TRACE_FMT)
        {
            write!(w, "{s} ")?;
        }
        let meta = event.metadata();
        let target = meta.target();
        if self.color {
            let (open, close) = ansi_for(meta.level());
            write!(w, "{open}[{target}]{close} ")?;
        } else {
            write!(w, "[{target}] ")?;
        }
        let mut v = MsgVisitor::default();
        event.record(&mut v);
        writeln!(w, "{}", v.0)
    }
}

struct FileFormat;

impl<S, N> FormatEvent<S, N> for FileFormat
where
    S: Subscriber + for<'a> LookupSpan<'a>,
    N: for<'a> FormatFields<'a> + 'static,
{
    fn format_event(
        &self,
        _ctx: &FmtContext<'_, S, N>,
        mut w: Writer<'_>,
        event: &Event<'_>,
    ) -> std::fmt::Result {
        if let Ok(now) = OffsetDateTime::now_local()
            && let Ok(s) = now.format(&ISO_FILE_FMT)
        {
            write!(w, "{s} ")?;
        }
        let meta = event.metadata();
        let label = level_to_local_level(meta.level()).label();
        write!(w, "{label}")?;
        for _ in label.len()..7 {
            w.write_char(' ')?;
        }
        write!(w, " [{}] ", meta.target())?;
        let mut v = MsgVisitor::default();
        event.record(&mut v);
        writeln!(w, "{}", v.0)
    }
}

// =====================================================================
// Redacting MakeWriter — runs `jfn_log_redact::censor` on each event's
// bytes before they reach the underlying non-blocking writer.
// =====================================================================

use tracing_subscriber::fmt::MakeWriter;

struct RedactMake<W>(W);

struct RedactGuard<W: Write> {
    inner: W,
    buf: Vec<u8>,
}

impl<W: Write> Write for RedactGuard<W> {
    fn write(&mut self, b: &[u8]) -> io::Result<usize> {
        self.buf.extend_from_slice(b);
        Ok(b.len())
    }
    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

impl<W: Write> Drop for RedactGuard<W> {
    fn drop(&mut self) {
        if redact::contains_secret(&self.buf) {
            redact::censor(&mut self.buf);
        }
        let _ = self.inner.write_all(&self.buf);
    }
}

impl<'a, W> MakeWriter<'a> for RedactMake<W>
where
    W: Write + Clone + 'a,
{
    type Writer = RedactGuard<W>;
    fn make_writer(&'a self) -> Self::Writer {
        RedactGuard {
            inner: self.0.clone(),
            buf: Vec::new(),
        }
    }
}

// =====================================================================
// Tests
// =====================================================================

#[cfg(test)]
mod tests {
    use super::*;
    use parking_lot::Mutex as StdMutex;
    use std::sync::Arc;

    #[derive(Clone)]
    struct VecSink(Arc<StdMutex<Vec<u8>>>);
    impl Write for VecSink {
        fn write(&mut self, b: &[u8]) -> io::Result<usize> {
            self.0.lock().extend_from_slice(b);
            Ok(b.len())
        }
        fn flush(&mut self) -> io::Result<()> {
            Ok(())
        }
    }

    #[test]
    fn redact_make_writer_censors_before_forwarding() -> io::Result<()> {
        let sink = VecSink(Arc::new(StdMutex::new(Vec::new())));
        let make = RedactMake(sink.clone());
        {
            let mut w = make.make_writer();
            w.write_all(b"GET /Items?api_key=abc123secret&x=1\n")?;
        }
        let bytes = sink.0.lock().clone();
        let text = String::from_utf8_lossy(&bytes);
        assert!(
            !text.contains("abc123secret"),
            "secret leaked through redactor: {text}"
        );
        assert!(
            text.contains("api_key=xxx"),
            "expected censored bytes: {text}"
        );
        Ok(())
    }

    #[test]
    fn redact_make_writer_passes_clean_bytes() -> io::Result<()> {
        let sink = VecSink(Arc::new(StdMutex::new(Vec::new())));
        let make = RedactMake(sink.clone());
        {
            let mut w = make.make_writer();
            w.write_all(b"[mpv] hello\n")?;
        }
        assert_eq!(&*sink.0.lock(), b"[mpv] hello\n");
        Ok(())
    }

    #[test]
    fn level_label_padding_matches_file_format() {
        // FileFormat pads level label to 7 chars via a fill loop.
        // Sanity-check Level::label widths so padding code stays correct.
        assert_eq!(Level::Trace.label(), "TRACE");
        assert_eq!(Level::Debug.label(), "DEBUG");
        assert_eq!(Level::Info.label(), "INFO");
        assert_eq!(Level::Warn.label(), "WARN");
        assert_eq!(Level::Error.label(), "ERROR");
        for l in [
            Level::Trace,
            Level::Debug,
            Level::Info,
            Level::Warn,
            Level::Error,
        ] {
            assert!(l.label().len() <= 7);
        }
    }

    #[test]
    fn directive_trace_detection() {
        assert!(directive_contains_trace("trace"));
        assert!(directive_contains_trace("info,mpv=trace"));
        assert!(directive_contains_trace("debug,CEF=trace,mpv=warn"));
        assert!(!directive_contains_trace("info"));
        assert!(!directive_contains_trace("debug,mpv=warn"));
        assert!(!directive_contains_trace("warn,CEF=off"));
    }

    fn probe_with_filter(directive: &str, cat_idx: usize, lvl: u8) -> bool {
        let filter = EnvFilter::new(directive);
        let subscriber = Registry::default().with(filter);
        let dispatch = tracing::Dispatch::new(subscriber);
        let table = tracing::dispatcher::with_default(&dispatch, compute_enabled_table);
        table[enabled_slot(cat_idx as u8, lvl)]
    }

    #[test]
    fn enabled_table_respects_global_filter() {
        // "warn" → Info+ disabled, Warn/Error enabled for any category.
        assert!(!probe_with_filter(
            "warn", 0, /* Main */
            2  /* Info */
        ));
        assert!(probe_with_filter("warn", 0, 3 /* Warn */));
        assert!(probe_with_filter("warn", 0, 4 /* Error */));
    }

    #[test]
    fn enabled_table_respects_target_override() {
        // Global warn, but mpv=trace → mpv Trace enabled, Main Trace not.
        assert!(probe_with_filter(
            "warn,mpv=trace",
            1, /* mpv */
            0  /* Trace */
        ));
        assert!(!probe_with_filter("warn,mpv=trace", 0 /* Main */, 0));
    }

    #[test]
    fn enabled_table_respects_off_directive() {
        // Global info, CEF=off → CEF Error disabled, Main Error enabled.
        assert!(!probe_with_filter(
            "info,CEF=off",
            2, /* CEF */
            4  /* Error */
        ));
        assert!(probe_with_filter("info,CEF=off", 0 /* Main */, 4));
    }

    fn capture_console(color: bool) -> String {
        let sink = VecSink(Arc::new(StdMutex::new(Vec::new())));
        let layer = fmt::layer()
            .event_format(ConsoleFormat {
                trace_mode: false,
                color,
            })
            .with_writer({
                let sink = sink.clone();
                move || sink.clone()
            });
        let subscriber = Registry::default().with(layer);
        let dispatch = tracing::Dispatch::new(subscriber);
        tracing::dispatcher::with_default(&dispatch, || {
            tracing::event!(target: "mpv", tracing::Level::ERROR, "boom");
        });
        let bytes = sink.0.lock().clone();
        String::from_utf8_lossy(&bytes).into_owned()
    }

    #[test]
    fn console_format_no_color_is_plain() {
        let s = capture_console(false);
        assert!(s.starts_with("[mpv] "), "unexpected line: {s:?}");
        assert!(!s.contains('\x1b'), "unexpected ANSI in: {s:?}");
        assert!(s.ends_with("boom\n"), "unexpected line: {s:?}");
    }

    #[test]
    fn console_format_color_wraps_target() {
        let s = capture_console(true);
        let (open, close) = ansi_for(&tracing::Level::ERROR);
        let expected_prefix = format!("{open}[mpv]{close} ");
        assert!(
            s.starts_with(&expected_prefix),
            "expected colored prefix in: {s:?}"
        );
    }

    #[test]
    fn ansi_for_each_level_distinct() {
        let palette = [
            tracing::Level::ERROR,
            tracing::Level::WARN,
            tracing::Level::INFO,
            tracing::Level::DEBUG,
            tracing::Level::TRACE,
        ];
        let mut seen = std::collections::HashSet::new();
        for l in palette {
            let (open, _) = ansi_for(&l);
            assert!(open.starts_with("\x1b["), "missing ANSI escape: {open:?}");
            assert!(seen.insert(open), "duplicate color for {l:?}");
        }
    }
}
