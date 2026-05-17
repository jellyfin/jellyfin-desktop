//! Logging backend.
//!
//! Two writers, both wrapped with `tracing_appender::non_blocking`:
//! - stderr (always)
//! - size-rotated file (optional, when `path` is non-empty)
//!
//! Every emitted line is filtered through `jfn_log_redact` so auth tokens
//! are 'x'-ed out. Anything other code writes to the real stderr (CEF
//! subprocesses, ffmpeg) is captured by a pipe-and-poll thread and
//! re-emitted as `[CEF]` debug records.

use std::ffi::{CString, c_char};
use std::fs::{File, OpenOptions};
use std::io::{self, Write};
use std::path::PathBuf;
use std::sync::{Mutex, OnceLock};
#[cfg(unix)]
use std::thread::{self, JoinHandle};

use time::OffsetDateTime;
use time::format_description::FormatItem;
use time::macros::format_description;

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
fn make_console_writer() -> StderrWriter {
    let fd = unsafe { libc::dup(libc::STDERR_FILENO) };
    StderrWriter { fd }
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
fn make_console_writer() -> StderrWriter {
    StderrWriter
}

// =====================================================================
// State
// =====================================================================

struct State {
    min_level: Level,
    active_log_path: String,
    console_tx: tracing_appender::non_blocking::NonBlocking,
    _console_guard: tracing_appender::non_blocking::WorkerGuard,
    file_tx: Option<tracing_appender::non_blocking::NonBlocking>,
    _file_guard: Option<tracing_appender::non_blocking::WorkerGuard>,
    trace_console: bool,
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

fn format_console(category: &str, msg: &str, trace_mode: bool) -> Vec<u8> {
    let mut out = String::with_capacity(msg.len() + 32);
    if trace_mode {
        if let Ok(now) = OffsetDateTime::now_local() {
            if let Ok(s) = now.format(&CONSOLE_TRACE_FMT) {
                out.push_str(&s);
                out.push(' ');
            }
        }
    }
    out.push('[');
    out.push_str(category);
    out.push_str("] ");
    out.push_str(msg);
    out.push('\n');
    redact(&mut out);
    out.into_bytes()
}

fn format_file(category: &str, msg: &str, level: Level) -> Vec<u8> {
    let mut out = String::with_capacity(msg.len() + 48);
    if let Ok(now) = OffsetDateTime::now_local() {
        if let Ok(s) = now.format(&ISO_FILE_FMT) {
            out.push_str(&s);
            out.push(' ');
        }
    }
    let label = level.label();
    out.push_str(label);
    for _ in label.len()..7 {
        out.push(' ');
    }
    out.push(' ');
    out.push('[');
    out.push_str(category);
    out.push_str("] ");
    out.push_str(msg);
    out.push('\n');
    redact(&mut out);
    out.into_bytes()
}

fn redact(s: &mut String) {
    // log_redact only ever replaces ASCII bytes with 'x', preserving UTF-8
    // validity, so &mut [u8] from String is safe.
    let bytes = unsafe { s.as_bytes_mut() };
    if jfn_log_redact::contains_secret(bytes) {
        jfn_log_redact::censor(bytes);
    }
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

fn emit(category: u8, level: Level, msg: &str) {
    let mut guard = state().lock().unwrap();
    let Some(state) = guard.as_mut() else {
        return;
    };
    if (level as u8) < (state.min_level as u8) {
        return;
    }
    let cat_name = *CATEGORY_NAMES
        .get(category as usize)
        .unwrap_or(&"Unknown");

    let msg = msg.trim_end_matches(['\r', '\n']);
    let console_bytes = format_console(cat_name, msg, state.trace_console);
    let _ = state.console_tx.write_all(&console_bytes);

    if let Some(file_tx) = state.file_tx.as_mut() {
        let file_bytes = format_file(cat_name, msg, level);
        let _ = file_tx.write_all(&file_bytes);
    }
}

// =====================================================================
// FFI
// =====================================================================

/// # Safety
/// `path` may be null or a valid NUL-terminated C string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jfn_log_init(path: *const c_char, min_level: u8) {
    let level = Level::from_u8(min_level);
    let path_str = if path.is_null() {
        String::new()
    } else {
        unsafe { std::ffi::CStr::from_ptr(path) }
            .to_string_lossy()
            .into_owned()
    };

    // Capture a dup of stderr now so console writes survive the later
    // dup2() redirect installed by StderrCapture, and aren't fed back into
    // the capture pipe.
    let (console_tx, console_guard) =
        tracing_appender::non_blocking(make_console_writer());

    let (file_tx, file_guard) = if !path_str.is_empty() {
        match RotatingFile::open(PathBuf::from(&path_str)) {
            Ok(rf) => {
                let (tx, guard) = tracing_appender::non_blocking(rf);
                (Some(tx), Some(guard))
            }
            Err(_) => (None, None),
        }
    } else {
        (None, None)
    };

    let mut guard = state().lock().unwrap();
    if guard.is_some() {
        return;
    }
    let stderr_capture = StderrCapture::start();
    *guard = Some(State {
        min_level: level,
        active_log_path: path_str,
        console_tx,
        _console_guard: console_guard,
        file_tx,
        _file_guard: file_guard,
        trace_console: level == Level::Trace,
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
        // Dropping `s` flushes both NonBlocking writers via their
        // WorkerGuards and joins the worker threads.
        drop(s);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn jfn_log_enabled(_category: u8, level: u8) -> bool {
    let guard = state().lock().unwrap();
    let Some(state) = guard.as_ref() else {
        return false;
    };
    (Level::from_u8(level) as u8) >= (state.min_level as u8)
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
