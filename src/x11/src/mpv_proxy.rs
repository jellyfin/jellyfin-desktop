//! X11 proxy mpv connects to instead of the real server.
//!
//! It binds a fresh display socket, points mpv's `DISPLAY` at it (via env,
//! before `mpv_create`), and forwards every byte — and every `SCM_RIGHTS` fd —
//! to the real server. Forwarding ancillary fds is mandatory: mpv's gpu VO
//! (DRI3/Present) and MIT-SHM pass dmabuf/shm fds over the socket, so a
//! byte-only splice would break video.
//!
//! The server→client direction is a verbatim byte+fd relay. The
//! client→server direction is framed to do two small rewrites:
//! - force `override_redirect` on mpv's root-parented `CreateWindow` (its VO
//!   top-level), so the WM never manages mpv's window — not even the instant
//!   before the app reparents it, which would otherwise flash a taskbar entry;
//! - neutralize mpv's `SetInputFocus` to `NoOperation`, so mpv's unmanaged
//!   window can't steal keyboard focus from the app top-level.

use std::borrow::Cow;
use std::io::{self, IoSlice, IoSliceMut, Write};
use std::net::TcpStream;
use std::os::fd::{AsFd, AsRawFd, FromRawFd, OwnedFd, RawFd};
use std::os::linux::net::SocketAddrExt;
use std::os::unix::net::{SocketAddr, UnixListener, UnixStream};
use std::path::PathBuf;
use std::sync::OnceLock;
use std::thread::{self, JoinHandle};

use nix::errno::Errno;
use nix::fcntl::OFlag;
use nix::poll::{PollFd, PollFlags, PollTimeout, poll};
use nix::sys::socket::{
    ControlMessage, ControlMessageOwned, MsgFlags, Shutdown, recvmsg, send, sendmsg, shutdown,
};
use nix::unistd::{pipe2, write};
use parking_lot::Mutex;
use x11rb::reexports::x11rb_protocol::errors::ParseError;
use x11rb::reexports::x11rb_protocol::parse_display::{
    ConnectAddress, ParsedDisplay, parse_display,
};
use x11rb::reexports::x11rb_protocol::protocol::xproto::{
    CREATE_WINDOW_REQUEST, CreateWindowRequest, NO_OPERATION_REQUEST, SET_INPUT_FOCUS_REQUEST,
    SetupRequest,
};
use x11rb::reexports::x11rb_protocol::x11_utils::{
    BigRequests, RequestHeader, TryParse, parse_request_header,
};
use x11rb::reexports::x11rb_protocol::xauth::{Family, get_auth};

/// The kernel caps `SCM_RIGHTS` at this many fds per message; a cmsg buffer
/// sized for it can never truncate fds.
const MAX_FDS_PER_MSG: usize = 253;
const CHUNK: usize = 64 * 1024;
/// `FamilyLocal` in the `.Xauthority` on-wire format (a `u16`, unlike the
/// core-protocol `Family` which is a `u8`).
const FAMILY_LOCAL: u16 = 256;

#[derive(Clone)]
enum UpstreamAddr {
    Abstract(u16),
    Path(String),
    Tcp(String, u16),
}

struct ProxyState {
    accept_thread: Option<JoinHandle<()>>,
    shutdown_w: OwnedFd,
    /// Filesystem socket to unlink on stop (dropping the listener does not).
    fs_socket_path: PathBuf,
    xauth_temp: Option<PathBuf>,
    orig_display: Option<String>,
    orig_xauth: Option<String>,
    restored: bool,
}

fn state() -> &'static Mutex<Option<ProxyState>> {
    static S: OnceLock<Mutex<Option<ProxyState>>> = OnceLock::new();
    S.get_or_init(|| Mutex::new(None))
}

/// Start the proxy and repoint `DISPLAY`/`XAUTHORITY` at it. Returns `false`
/// (leaving the environment untouched) if it cannot bind or resolve the
/// upstream server. Idempotent.
pub fn start() -> bool {
    let mut guard = state().lock();
    if guard.is_some() {
        return true;
    }

    let orig_display = std::env::var("DISPLAY").ok();
    let parsed = match parse_display(None) {
        Ok(p) => p,
        Err(e) => {
            tracing::error!(target: "Main", "cannot parse DISPLAY: {e}");
            return false;
        }
    };
    let upstream = upstream_addresses(&parsed);

    let Some(bound) = bind_listeners() else {
        tracing::error!(target: "Main", "no free X11 display socket for the proxy");
        return false;
    };

    let (shutdown_r, shutdown_w) = match pipe2(OFlag::O_CLOEXEC) {
        Ok(pair) => pair,
        Err(e) => {
            tracing::error!(target: "Main", "proxy shutdown pipe failed: {e}");
            let _ = std::fs::remove_file(&bound.fs_path);
            return false;
        }
    };

    let orig_xauth = std::env::var("XAUTHORITY").ok();
    let xauth_temp = provision_auth(parsed.display, bound.number).unwrap_or(None);

    let BoundListeners {
        abstract_l,
        fs_l,
        fs_path,
        number,
    } = bound;
    let accept_thread = thread::Builder::new()
        .name("x11-proxy-accept".into())
        .spawn(move || run_acceptor(abstract_l, fs_l, shutdown_r, upstream))
        .ok();
    let Some(accept_thread) = accept_thread else {
        tracing::error!(target: "Main", "proxy accept thread spawn failed");
        let _ = std::fs::remove_file(&fs_path);
        return false;
    };

    let new_display = if parsed.screen == 0 {
        format!(":{number}")
    } else {
        format!(":{number}.{}", parsed.screen)
    };
    unsafe { std::env::set_var("DISPLAY", &new_display) };
    if let Some(p) = &xauth_temp {
        unsafe { std::env::set_var("XAUTHORITY", p) };
    }

    tracing::info!(target: "Main", "x11 proxy listening on {new_display}");
    *guard = Some(ProxyState {
        accept_thread: Some(accept_thread),
        shutdown_w,
        fs_socket_path: fs_path,
        xauth_temp,
        orig_display,
        orig_xauth,
        restored: false,
    });
    true
}

/// Put `DISPLAY`/`XAUTHORITY` back to the real server so the app's own
/// connections bypass the proxy. Must not run until mpv has connected (i.e.
/// after `mpv_initialize`); idempotent.
pub fn restore_real_display() {
    let mut guard = state().lock();
    let Some(st) = guard.as_mut() else {
        return;
    };
    if st.restored {
        return;
    }
    st.restored = true;
    match &st.orig_display {
        Some(d) => unsafe { std::env::set_var("DISPLAY", d) },
        None => unsafe { std::env::remove_var("DISPLAY") },
    }
    match &st.orig_xauth {
        Some(x) => unsafe { std::env::set_var("XAUTHORITY", x) },
        None if st.xauth_temp.is_some() => unsafe { std::env::remove_var("XAUTHORITY") },
        None => {}
    }
}

/// Stop accepting new connections and clean up the socket + temp auth files.
/// Established relays drain on their own when mpv closes its connection.
pub fn stop() {
    let Some(mut st) = state().lock().take() else {
        return;
    };
    let _ = write(&st.shutdown_w, &[1u8]);
    if let Some(h) = st.accept_thread.take() {
        let _ = h.join();
    }
    let _ = std::fs::remove_file(&st.fs_socket_path);
    if let Some(p) = &st.xauth_temp {
        let _ = std::fs::remove_file(p);
    }
}

struct BoundListeners {
    abstract_l: UnixListener,
    fs_l: UnixListener,
    fs_path: PathBuf,
    number: u32,
}

/// Find a display number free on both the abstract and filesystem X sockets and
/// bind both, so libxcb (abstract-first on Linux) and legacy path clients agree.
fn bind_listeners() -> Option<BoundListeners> {
    for number in 64u32..1024 {
        let name = format!("/tmp/.X11-unix/X{number}");
        let Ok(addr) = SocketAddr::from_abstract_name(name.as_bytes()) else {
            continue;
        };
        let Ok(abstract_l) = UnixListener::bind_addr(&addr) else {
            continue;
        };
        let fs_path = PathBuf::from(&name);
        let Ok(fs_l) = UnixListener::bind(&fs_path) else {
            continue;
        };
        return Some(BoundListeners {
            abstract_l,
            fs_l,
            fs_path,
            number,
        });
    }
    None
}

fn run_acceptor(
    abstract_l: UnixListener,
    fs_l: UnixListener,
    shutdown_r: OwnedFd,
    upstream: Vec<UpstreamAddr>,
) {
    let listeners = [&abstract_l, &fs_l];
    loop {
        let mut fds = [
            PollFd::new(abstract_l.as_fd(), PollFlags::POLLIN),
            PollFd::new(fs_l.as_fd(), PollFlags::POLLIN),
            PollFd::new(shutdown_r.as_fd(), PollFlags::POLLIN),
        ];
        match poll(&mut fds, PollTimeout::NONE) {
            Err(Errno::EINTR) => continue,
            Err(_) => break,
            Ok(_) => {}
        }
        if fds[2].revents().is_some_and(|r| !r.is_empty()) {
            break;
        }
        let ready = [
            fds[0]
                .revents()
                .is_some_and(|r| r.contains(PollFlags::POLLIN)),
            fds[1]
                .revents()
                .is_some_and(|r| r.contains(PollFlags::POLLIN)),
        ];
        for (listener, ready) in listeners.iter().zip(ready) {
            if !ready {
                continue;
            }
            match listener.accept() {
                Ok((stream, _)) => {
                    let up = upstream.clone();
                    let _ = thread::Builder::new()
                        .name("x11-proxy-conn".into())
                        .spawn(move || handle_conn(stream, up));
                }
                Err(e) => tracing::debug!(target: "x11-proxy", "accept failed: {e}"),
            }
        }
    }
}

fn handle_conn(client: UnixStream, upstream: Vec<UpstreamAddr>) {
    let up = match connect_upstream(&upstream) {
        Ok(fd) => fd,
        Err(e) => {
            tracing::debug!(target: "x11-proxy", "upstream connect failed: {e}");
            return;
        }
    };
    let client: OwnedFd = client.into();
    let cf = client.as_raw_fd();
    let uf = up.as_raw_fd();
    thread::scope(|s| {
        s.spawn(|| pump_requests(cf, uf));
        s.spawn(|| pump(uf, cf));
    });
}

/// Build the ordered upstream-address candidates from a parsed `DISPLAY`,
/// reusing x11rb's resolution and prepending the Linux abstract socket (which
/// x11rb does not try) for local servers.
fn upstream_addresses(parsed: &ParsedDisplay) -> Vec<UpstreamAddr> {
    let candidates: Vec<ConnectAddress<'_>> = parsed.connect_instruction().collect();
    let mut addrs = Vec::new();
    if candidates
        .iter()
        .any(|c| matches!(c, ConnectAddress::Socket(_)))
    {
        addrs.push(UpstreamAddr::Abstract(parsed.display));
    }
    for c in candidates {
        match c {
            ConnectAddress::Socket(path) => addrs.push(UpstreamAddr::Path(path)),
            ConnectAddress::Hostname(host, port) => {
                addrs.push(UpstreamAddr::Tcp(host.to_string(), port));
            }
            _ => {}
        }
    }
    addrs
}

fn connect_upstream(upstream: &[UpstreamAddr]) -> io::Result<OwnedFd> {
    let mut last = None;
    for addr in upstream {
        let result = match addr {
            UpstreamAddr::Abstract(number) => {
                let name = format!("/tmp/.X11-unix/X{number}");
                SocketAddr::from_abstract_name(name.as_bytes())
                    .and_then(|a| UnixStream::connect_addr(&a))
                    .map(OwnedFd::from)
            }
            UpstreamAddr::Path(path) => UnixStream::connect(path).map(OwnedFd::from),
            UpstreamAddr::Tcp(host, port) => TcpStream::connect((host.as_str(), *port)).map(|s| {
                let _ = s.set_nodelay(true);
                OwnedFd::from(s)
            }),
        };
        match result {
            Ok(fd) => return Ok(fd),
            Err(e) => last = Some(e),
        }
    }
    Err(last.unwrap_or_else(|| io::Error::new(io::ErrorKind::NotFound, "no upstream address")))
}

/// Forward one direction until EOF or error, then tear down both directions so
/// the sibling pump wakes from its blocking `recvmsg`.
fn pump(from: RawFd, to: RawFd) {
    let mut buf = vec![0u8; CHUNK];
    let mut cmsg = nix::cmsg_space!([RawFd; MAX_FDS_PER_MSG]);
    loop {
        match recv_with_fds(from, &mut buf, &mut cmsg) {
            Ok((0, _)) => break,
            Ok((n, fds)) => {
                if let Err(e) = send_with_fds(to, &buf[..n], fds) {
                    tracing::debug!(target: "x11-proxy", "relay send failed: {e}");
                    break;
                }
            }
            Err(e) => {
                tracing::debug!(target: "x11-proxy", "relay recv failed: {e}");
                break;
            }
        }
    }
    let _ = shutdown(from, Shutdown::Both);
    let _ = shutdown(to, Shutdown::Both);
}

/// Client→server pump: like [`pump`], but frames the X11 request stream so
/// [`ReqParser`] can rewrite requests. Only whole requests are forwarded; a
/// request split across reads (and any fds it carries) is held until complete.
fn pump_requests(from: RawFd, to: RawFd) {
    let mut buf = vec![0u8; CHUNK];
    let mut cmsg = nix::cmsg_space!([RawFd; MAX_FDS_PER_MSG]);
    let mut parser = ReqParser::default();
    let mut held: Vec<u8> = Vec::new();
    let mut held_fds: Vec<OwnedFd> = Vec::new();
    let mut out: Vec<u8> = Vec::new();
    loop {
        match recv_with_fds(from, &mut buf, &mut cmsg) {
            Ok((0, _)) => break,
            Ok((n, mut fds)) => {
                let mut data = std::mem::take(&mut held);
                data.extend_from_slice(&buf[..n]);
                held_fds.append(&mut fds);
                out.clear();
                let consumed = parser.process(&data, &mut out);
                if consumed == 0 {
                    held = data;
                    continue;
                }
                // Flush all pending fds now: fds must reach the server no later
                // than the request that consumes them, and delivering them with
                // an earlier send is harmless (the server queues them in order).
                let fds_out = std::mem::take(&mut held_fds);
                if let Err(e) = send_with_fds(to, &out, fds_out) {
                    tracing::debug!(target: "x11-proxy", "relay send failed: {e}");
                    break;
                }
                held = data.split_off(consumed);
            }
            Err(e) => {
                tracing::debug!(target: "x11-proxy", "relay recv failed: {e}");
                break;
            }
        }
    }
    let _ = shutdown(from, Shutdown::Both);
    let _ = shutdown(to, Shutdown::Both);
}

/// Ceiling on a single request's byte length; past it we assume a parse desync
/// and fall back to a verbatim relay rather than buffer unbounded.
const MAX_REQUEST_BYTES: usize = 64 * 1024 * 1024;

#[derive(Default)]
struct ReqParser {
    setup_done: bool,
    /// Window IDs mpv has created. A `CreateWindow` whose parent is not in here
    /// is parented to a pre-existing window (the root) — mpv's VO top-level.
    created: std::collections::HashSet<u32>,
    blind: bool,
}

impl ReqParser {
    /// Append the leading complete requests of `input` to `out` (rewritten
    /// where needed) and return how many `input` bytes were consumed. A
    /// trailing partial request is left for the next call.
    fn process(&mut self, input: &[u8], out: &mut Vec<u8>) -> usize {
        if self.blind {
            out.extend_from_slice(input);
            return input.len();
        }

        let mut off = 0;
        if !self.setup_done {
            // x11rb-protocol silently misparses non-native byte order.
            let native = if cfg!(target_endian = "little") {
                b'l'
            } else {
                b'B'
            };
            if input.first() != Some(&native) {
                self.blind = true;
                out.extend_from_slice(input);
                return input.len();
            }
            match SetupRequest::try_parse(input) {
                Ok((_, remaining)) => {
                    let total = input.len() - remaining.len();
                    out.extend_from_slice(&input[..total]);
                    off = total;
                    self.setup_done = true;
                }
                Err(ParseError::InsufficientData) => return 0,
                Err(_) => {
                    self.blind = true;
                    out.extend_from_slice(input);
                    return input.len();
                }
            }
        }

        while off < input.len() {
            let avail = &input[off..];
            // A zero length field can only be a BIG-REQUESTS length, so
            // `Enabled` is correct without tracking the extension handshake.
            let (header, body) = match parse_request_header(avail, BigRequests::Enabled) {
                Ok(v) => v,
                Err(ParseError::InsufficientData) => break,
                Err(_) => {
                    self.blind = true;
                    out.extend_from_slice(avail);
                    return input.len();
                }
            };
            let header_len = avail.len() - body.len();
            let Some(total) = (header.remaining_length as usize)
                .checked_mul(4)
                .map(|b| b + header_len)
                .filter(|&t| t <= MAX_REQUEST_BYTES)
            else {
                self.blind = true;
                out.extend_from_slice(avail);
                return input.len();
            };
            if avail.len() < total {
                break;
            }
            let raw = &avail[..total];
            if header.major_opcode == CREATE_WINDOW_REQUEST {
                self.emit_create_window(header, &raw[header_len..], raw, out);
            } else if header.major_opcode == SET_INPUT_FOCUS_REQUEST {
                // mpv's window is override_redirect, so a SetInputFocus would
                // bypass the WM and steal keyboard focus from the app top-level.
                // Rewrite to NoOperation (same length, no reply) so the app keeps
                // focus and owns all input.
                let start = out.len();
                out.extend_from_slice(raw);
                out[start] = NO_OPERATION_REQUEST;
            } else {
                out.extend_from_slice(raw);
            }
            off += total;
        }
        off
    }

    fn emit_create_window(
        &mut self,
        header: RequestHeader,
        body: &[u8],
        raw: &[u8],
        out: &mut Vec<u8>,
    ) {
        let Ok(mut req) = CreateWindowRequest::try_parse_request(header, body) else {
            out.extend_from_slice(raw);
            return;
        };
        let root_parented = !self.created.contains(&req.parent);
        self.created.insert(req.wid);
        if !root_parented {
            out.extend_from_slice(raw);
            return;
        }
        req.value_list = Cow::Owned(req.value_list.into_owned().override_redirect(1));
        let (bufs, _) = req.serialize();
        for buf in &bufs {
            out.extend_from_slice(buf);
        }
    }
}

fn recv_with_fds(fd: RawFd, buf: &mut [u8], cmsg: &mut [u8]) -> io::Result<(usize, Vec<OwnedFd>)> {
    let mut iov = [IoSliceMut::new(buf)];
    let msg = loop {
        match recvmsg::<()>(fd, &mut iov, Some(&mut *cmsg), MsgFlags::empty()) {
            Err(Errno::EINTR) => continue,
            Err(e) => return Err(e.into()),
            Ok(m) => break m,
        }
    };
    let mut fds = Vec::new();
    for c in msg.cmsgs()? {
        if let ControlMessageOwned::ScmRights(list) = c {
            fds.extend(list.into_iter().map(|f| unsafe { OwnedFd::from_raw_fd(f) }));
        }
    }
    Ok((msg.bytes, fds))
}

/// The fds attach to `buf`'s first byte: delivered whole on the first
/// `sendmsg`, so a short send finishes with plain `send`s.
fn send_with_fds(fd: RawFd, buf: &[u8], fds: Vec<OwnedFd>) -> io::Result<()> {
    if buf.is_empty() {
        return Ok(());
    }

    let raw: Vec<RawFd> = fds.iter().map(AsRawFd::as_raw_fd).collect();
    let iov = [IoSlice::new(buf)];
    let scm = [ControlMessage::ScmRights(&raw)];
    let cmsgs = if raw.is_empty() { &[][..] } else { &scm[..] };
    let first = loop {
        match sendmsg::<()>(fd, &iov, cmsgs, MsgFlags::MSG_NOSIGNAL, None) {
            Err(Errno::EINTR) => continue,
            Err(e) => return Err(e.into()),
            Ok(n) => break n,
        }
    };
    drop(fds);

    let mut off = first;
    while off < buf.len() {
        match send(fd, &buf[off..], MsgFlags::MSG_NOSIGNAL) {
            Err(Errno::EINTR) => continue,
            Err(e) => return Err(e.into()),
            Ok(0) => return Err(io::ErrorKind::WriteZero.into()),
            Ok(n) => off += n,
        }
    }
    Ok(())
}

/// Provision an auth cookie for the proxy's display number so mpv's connection
/// is accepted. Reads the real display's cookie via x11rb and writes a single
/// re-keyed entry into a private, jellium-owned runtime dir. Returns `None` when
/// the server has no cookie (e.g. an unauthenticated `xhost +local:` session).
fn provision_auth(display: u16, proxy_number: u32) -> io::Result<Option<PathBuf>> {
    let host = gethostname::gethostname().into_encoded_bytes();
    let (name, data) = match get_auth(Family::LOCAL, &host, display) {
        Ok(Some(auth)) => auth,
        Ok(None) => return Ok(None),
        Err(e) => {
            tracing::debug!(target: "x11-proxy", "xauth lookup failed: {e}");
            return Ok(None);
        }
    };

    // mpv reaches the proxy over a local socket, so the entry it looks up is
    // keyed by FamilyLocal + this host, under the proxy's display number.
    let mut out = Vec::new();
    write_xauth_entry(
        &mut out,
        FAMILY_LOCAL,
        &host,
        proxy_number.to_string().as_bytes(),
        &name,
        &data,
    );

    let dir = jfn_paths::runtime_dir();
    let mut file = tempfile::Builder::new()
        .prefix("xauth-")
        .tempfile_in(&dir)?;
    file.write_all(&out)?;
    file.flush()?;
    let (_file, path) = file.keep().map_err(|e| e.error)?;
    Ok(Some(path))
}

fn write_xauth_entry(
    out: &mut Vec<u8>,
    family: u16,
    address: &[u8],
    number: &[u8],
    name: &[u8],
    data: &[u8],
) {
    fn block(out: &mut Vec<u8>, b: &[u8]) {
        out.extend_from_slice(&(b.len() as u16).to_be_bytes());
        out.extend_from_slice(b);
    }
    out.extend_from_slice(&family.to_be_bytes());
    block(out, address);
    block(out, number);
    block(out, name);
    block(out, data);
}
