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

use std::ffi::{CString, c_char};
use std::fs::{File, OpenOptions};
use std::io::{self, Write};
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Mutex, OnceLock};
#[cfg(unix)]
use std::thread::{self, JoinHandle};

use time::OffsetDateTime;
use time::format_description::FormatItem;
use time::macros::format_description;

use tracing_appender::non_blocking::{NonBlockingBuilder, WorkerGuard};
use tracing_core::{
    Callsite, Interest, Metadata, field::FieldSet, identify_callsite, metadata::Kind,
};
use tracing_subscriber::{EnvFilter, Registry, fmt, layer::SubscriberExt};

// Keep enum values aligned with src/logging.h (LogCategory enum).
const CATEGORY_NAMES: &[&str] = &[
    "Main", "mpv", "CEF", "Media", "Platform", "JS", "Resource",
];
const CATEGORY_CEF: u8 = 2;

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

fn backup_path(path: &PathBuf, n: usize) -> PathBuf {
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
// Direct stderr Write — bypasses Rust's BufWriter so each record reaches
// the terminal immediately.
// =====================================================================

#[cfg(unix)]
struct StderrWriter {
    fd: libc::c_int,
}

#[cfg(unix)]
impl Write for StderrWriter {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        let n = unsafe { libc::write(self.fd, buf.as_ptr() as *const _, buf.len()) };
        if n < 0 {
            Err(io::Error::last_os_error())
        } else {
            Ok(n as usize)
        }
    }
    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

#[cfg(unix)]
impl Drop for StderrWriter {
    fn drop(&mut self) {
        if self.fd >= 0 {
            unsafe { libc::close(self.fd) };
        }
    }
}

#[cfg(unix)]
fn make_console_writer() -> (StderrWriter, bool) {
    let fd = unsafe { libc::dup(libc::STDERR_FILENO) };
    let is_tty = fd >= 0 && unsafe { libc::isatty(fd) } == 1;
    (StderrWriter { fd }, is_tty)
}

#[cfg(windows)]
fn enable_vt_mode() {
    // Best-effort: tell conhost to honor ANSI SGR escapes on stderr.
    // Win10+ supports ENABLE_VIRTUAL_TERMINAL_PROCESSING; older builds
    // silently fail and we render with no color.
    const ENABLE_VIRTUAL_TERMINAL_PROCESSING: u32 = 0x0004;
    const STD_ERROR_HANDLE: u32 = 0xFFFFFFF4; // (DWORD)-12
    type HANDLE = *mut std::ffi::c_void;
    type DWORD = u32;
    type BOOL = i32;
    unsafe extern "system" {
        fn GetStdHandle(nStdHandle: DWORD) -> HANDLE;
        fn GetConsoleMode(hConsoleHandle: HANDLE, lpMode: *mut DWORD) -> BOOL;
        fn SetConsoleMode(hConsoleHandle: HANDLE, dwMode: DWORD) -> BOOL;
    }
    unsafe {
        let h = GetStdHandle(STD_ERROR_HANDLE);
        if h.is_null() || (h as isize) == -1 {
            return;
        }
        let mut mode: DWORD = 0;
        if GetConsoleMode(h, &mut mode) == 0 {
            return;
        }
        SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
}

#[cfg(windows)]
struct StderrWriter;

#[cfg(windows)]
impl Write for StderrWriter {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        io::stderr().write(buf)
    }
    fn flush(&mut self) -> io::Result<()> {
        io::stderr().flush()
    }
}

#[cfg(windows)]
fn make_console_writer() -> (StderrWriter, bool) {
    // io::stderr().is_terminal() handles MSYS/conhost/etc. correctly.
    use std::io::IsTerminal;
    let is_tty = io::stderr().is_terminal();
    if is_tty {
        enable_vt_mode();
    }
    (StderrWriter, is_tty)
}

// =====================================================================
// State
// =====================================================================

struct State {
    active_log_path: String,
    _console_guard: WorkerGuard,
    _file_guard: Option<WorkerGuard>,
    stderr_capture: Option<StderrCapture>,
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
// EnvFilter. C++ macros consult this via jfn_log_enabled to skip
// formatting when filtered out, recovering the per-callsite gating that
// tracing's own Interest cache would do for pure-Rust callers.
// =====================================================================

const N_LEVELS: usize = 5;
const ENABLED_LEN: usize = 7 * N_LEVELS; // CATEGORY_NAMES.len() == 7

static ENABLED: [AtomicBool; ENABLED_LEN] =
    [const { AtomicBool::new(false) }; ENABLED_LEN];

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

fn prime_enabled_table() {
    use tracing::Level as L;
    let levels = [
        (0u8, L::TRACE),
        (1, L::DEBUG),
        (2, L::INFO),
        (3, L::WARN),
        (4, L::ERROR),
    ];
    for (cat_idx, cat_name) in CATEGORY_NAMES.iter().enumerate() {
        for (lvl_u8, tlvl) in levels {
            let md = probe_meta(cat_name, tlvl);
            let on = tracing::dispatcher::get_default(|d| d.enabled(&md));
            ENABLED[enabled_slot(cat_idx as u8, lvl_u8)].store(on, Ordering::Relaxed);
        }
    }
}

/// True if the directive string contains a trace-level component
/// anywhere — used to decide whether to prepend HH:MM:SS.mmm on console
/// lines (preserving the previous crate's trace-mode timestamps).
fn directive_contains_trace(filter: &str) -> bool {
    filter
        .split(',')
        .any(|tok| tok.trim().eq_ignore_ascii_case("trace") || tok.trim().to_ascii_lowercase().ends_with("=trace"))
}

// =====================================================================
// stderr capture
// =====================================================================

#[cfg(unix)]
struct StderrCapture {
    original_fd: libc::c_int,
    signal_write: libc::c_int,
    join: Option<JoinHandle<()>>,
}

#[cfg(unix)]
impl StderrCapture {
    fn start() -> Option<Self> {
        unsafe {
            let original = libc::dup(libc::STDERR_FILENO);
            if original < 0 {
                return None;
            }

            let mut pipe_fds = [0; 2];
            if libc::pipe(pipe_fds.as_mut_ptr()) < 0 {
                libc::close(original);
                return None;
            }
            let pipe_read = pipe_fds[0];
            let pipe_write = pipe_fds[1];

            let mut signal_fds = [0; 2];
            if libc::pipe(signal_fds.as_mut_ptr()) < 0 {
                libc::close(original);
                libc::close(pipe_read);
                libc::close(pipe_write);
                return None;
            }
            let signal_read = signal_fds[0];
            let signal_write = signal_fds[1];

            if libc::dup2(pipe_write, libc::STDERR_FILENO) < 0 {
                libc::close(original);
                libc::close(pipe_read);
                libc::close(pipe_write);
                libc::close(signal_read);
                libc::close(signal_write);
                return None;
            }
            libc::close(pipe_write);

            let join = thread::spawn(move || capture_loop(pipe_read, signal_read));

            Some(StderrCapture {
                original_fd: original,
                signal_write,
                join: Some(join),
            })
        }
    }

    fn stop(&mut self) {
        unsafe {
            if self.original_fd >= 0 {
                libc::dup2(self.original_fd, libc::STDERR_FILENO);
                libc::close(self.original_fd);
                self.original_fd = -1;
            }
            let buf = b"x";
            libc::write(self.signal_write, buf.as_ptr() as *const _, 1);
        }
        if let Some(h) = self.join.take() {
            let _ = h.join();
        }
        unsafe {
            libc::close(self.signal_write);
        }
        self.signal_write = -1;
    }
}

#[cfg(windows)]
struct StderrCapture;

#[cfg(windows)]
impl StderrCapture {
    fn start() -> Option<Self> {
        None
    }
    fn stop(&mut self) {}
}

#[cfg(unix)]
fn capture_loop(pipe_read: libc::c_int, signal_read: libc::c_int) {
    let mut buf = [0u8; 4096];
    let mut partial = Vec::<u8>::new();
    unsafe {
        loop {
            let mut pfds = [
                libc::pollfd {
                    fd: pipe_read,
                    events: libc::POLLIN,
                    revents: 0,
                },
                libc::pollfd {
                    fd: signal_read,
                    events: libc::POLLIN,
                    revents: 0,
                },
            ];
            let rc = libc::poll(pfds.as_mut_ptr(), 2, -1);
            if rc < 0 {
                break;
            }
            if pfds[1].revents & libc::POLLIN != 0 {
                break;
            }
            if pfds[0].revents & libc::POLLIN != 0 {
                let n = libc::read(pipe_read, buf.as_mut_ptr() as *mut _, buf.len());
                if n <= 0 {
                    break;
                }
                partial.extend_from_slice(&buf[..n as usize]);
                while let Some(pos) = partial.iter().position(|&b| b == b'\n') {
                    let line: Vec<u8> = partial.drain(..=pos).take(pos).collect();
                    if !line.is_empty() {
                        let msg = String::from_utf8_lossy(&line).into_owned();
                        emit(CATEGORY_CEF, Level::Debug, &msg);
                    }
                }
            }
        }
        libc::close(pipe_read);
        libc::close(signal_read);
    }
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

// =====================================================================
// FFI
// =====================================================================

/// # Safety
/// `path` and `filter` may be null or valid NUL-terminated C strings.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_log_init(path: *const c_char, filter: *const c_char) {
    let path_str = if path.is_null() {
        String::new()
    } else {
        unsafe { std::ffi::CStr::from_ptr(path) }
            .to_string_lossy()
            .into_owned()
    };
    let filter_str_raw = if filter.is_null() {
        String::new()
    } else {
        unsafe { std::ffi::CStr::from_ptr(filter) }
            .to_string_lossy()
            .into_owned()
    };
    let filter_str = if filter_str_raw.trim().is_empty() {
        "info".to_string()
    } else {
        filter_str_raw
    };

    // Bail early on second init: dispatcher is already installed and the
    // capture pipe / guards live in STATE. Mirrors prior behavior.
    {
        let guard = state().lock().unwrap();
        if guard.is_some() {
            return;
        }
    }

    // Capture a dup of stderr now so console writes survive the later
    // dup2() redirect installed by StderrCapture, and aren't fed back into
    // the capture pipe.
    let (console_writer, is_tty) = make_console_writer();
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

    let stderr_capture = StderrCapture::start();

    let mut guard = state().lock().unwrap();
    *guard = Some(State {
        active_log_path: path_str,
        _console_guard: console_guard,
        _file_guard: file_guard,
        stderr_capture,
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_log_shutdown() {
    let mut guard = state().lock().unwrap();
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

#[unsafe(no_mangle)]
pub extern "C" fn jfn_log_enabled(category: u8, level: u8) -> bool {
    if (category as usize) >= CATEGORY_NAMES.len() || (level as usize) >= N_LEVELS {
        return false;
    }
    ENABLED[enabled_slot(category, level)].load(Ordering::Relaxed)
}

/// # Safety
/// `msg` must point to `len` bytes of readable memory (or be null when
/// `len == 0`).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_log(category: u8, level: u8, msg: *const c_char, len: usize) {
    if len == 0 || msg.is_null() {
        return;
    }
    let slice = unsafe { std::slice::from_raw_parts(msg as *const u8, len) };
    let text = String::from_utf8_lossy(slice);
    emit(category, Level::from_u8(level), &text);
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_log_active_path() -> *mut c_char {
    let guard = state().lock().unwrap();
    let s = guard
        .as_ref()
        .map(|st| st.active_log_path.clone())
        .unwrap_or_default();
    CString::new(s)
        .map(|c| c.into_raw())
        .unwrap_or(std::ptr::null_mut())
}

/// # Safety
/// `s` must be null or a pointer previously returned by
/// [`jfn_log_active_path`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_log_free_string(s: *mut c_char) {
    if !s.is_null() {
        unsafe { drop(CString::from_raw(s)) };
    }
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
        if self.trace_mode {
            if let Ok(now) = OffsetDateTime::now_local() {
                if let Ok(s) = now.format(&CONSOLE_TRACE_FMT) {
                    write!(w, "{s} ")?;
                }
            }
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
        if let Ok(now) = OffsetDateTime::now_local() {
            if let Ok(s) = now.format(&ISO_FILE_FMT) {
                write!(w, "{s} ")?;
            }
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
    use std::sync::{Arc, Mutex as StdMutex};

    #[derive(Clone)]
    struct VecSink(Arc<StdMutex<Vec<u8>>>);
    impl Write for VecSink {
        fn write(&mut self, b: &[u8]) -> io::Result<usize> {
            self.0.lock().unwrap().extend_from_slice(b);
            Ok(b.len())
        }
        fn flush(&mut self) -> io::Result<()> {
            Ok(())
        }
    }

    #[test]
    fn redact_make_writer_censors_before_forwarding() {
        let sink = VecSink(Arc::new(StdMutex::new(Vec::new())));
        let make = RedactMake(sink.clone());
        {
            let mut w = make.make_writer();
            w.write_all(b"GET /Items?api_key=abc123secret&x=1\n")
                .unwrap();
        }
        let bytes = sink.0.lock().unwrap().clone();
        let text = String::from_utf8(bytes).unwrap();
        assert!(
            !text.contains("abc123secret"),
            "secret leaked through redactor: {text}"
        );
        assert!(text.contains("api_key=xxx"), "expected censored bytes: {text}");
    }

    #[test]
    fn redact_make_writer_passes_clean_bytes() {
        let sink = VecSink(Arc::new(StdMutex::new(Vec::new())));
        let make = RedactMake(sink.clone());
        {
            let mut w = make.make_writer();
            w.write_all(b"[mpv] hello\n").unwrap();
        }
        assert_eq!(&*sink.0.lock().unwrap(), b"[mpv] hello\n");
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
        // Build a one-shot Registry with just our EnvFilter, scoped to
        // this thread via with_default. prime_enabled_table reads from
        // the thread-local dispatcher, so the global STATE isn't touched.
        // Reset ENABLED slot first so a leak from another test can't
        // confuse the assertion.
        ENABLED[enabled_slot(cat_idx as u8, lvl)].store(false, Ordering::Relaxed);
        let filter = EnvFilter::new(directive);
        let subscriber = Registry::default().with(filter);
        let dispatch = tracing::Dispatch::new(subscriber);
        tracing::dispatcher::with_default(&dispatch, || prime_enabled_table());
        ENABLED[enabled_slot(cat_idx as u8, lvl)].load(Ordering::Relaxed)
    }

    #[test]
    fn enabled_table_respects_global_filter() {
        // "warn" → Info+ disabled, Warn/Error enabled for any category.
        assert!(!probe_with_filter("warn", 0 /* Main */, 2 /* Info */));
        assert!(probe_with_filter("warn", 0, 3 /* Warn */));
        assert!(probe_with_filter("warn", 0, 4 /* Error */));
    }

    #[test]
    fn enabled_table_respects_target_override() {
        // Global warn, but mpv=trace → mpv Trace enabled, Main Trace not.
        assert!(probe_with_filter("warn,mpv=trace", 1 /* mpv */, 0 /* Trace */));
        assert!(!probe_with_filter("warn,mpv=trace", 0 /* Main */, 0));
    }

    #[test]
    fn enabled_table_respects_off_directive() {
        // Global info, CEF=off → CEF Error disabled, Main Error enabled.
        assert!(!probe_with_filter("info,CEF=off", 2 /* CEF */, 4 /* Error */));
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
        let bytes = sink.0.lock().unwrap().clone();
        String::from_utf8(bytes).unwrap()
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
