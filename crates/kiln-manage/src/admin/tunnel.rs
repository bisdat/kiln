//! A forward to a port on a remote server through the user's ssh config and agent (`BatchMode=yes`).
//!
//! (0.2.0-dev.31.12) The launcher binds the local end itself and holds it for the tunnel's life: each connection
//! accepted there runs its own `ssh -W <target> <host>` (stdio forwarding) and the bytes are copied between the two.
//! Before, a free port was looked up, released and handed to `ssh -L`, and another program could take it in between
//! (and be talked to as if it were the server); now no port is ever free between lookup and use, and ssh opens no
//! listener at all. A connection's ssh child ends with its connection; closing (or dropping) the tunnel stops
//! accepting and ends every child.
//!
//! (Review of 0.2.0-dev.31.12) Every accepted connection costs an ssh process and a sign-in on the remote machine, so
//! the held port serves only this user's programs on Linux ([`super::peer::admits`]) and at most [`MAX_CHILDREN`]
//! connections at once (KAP uses one; `ssh -L` multiplexed them in one process). A KAP hello through the tunnel waits
//! [`SSH_SETUP`] longer than the handshake, since ssh connects and signs in before the first byte arrives.

use crate::server::remote::RemoteSpec;
use crate::{Error, ErrorCode, Result};
use std::io::Read;
use std::net::{Ipv4Addr, Shutdown, SocketAddr, TcpListener, TcpStream};
use std::process::{Child, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

/// How long a tunneled connection may wait for ssh to connect and sign in, on top of the KAP handshake's own time
/// (ssh's ConnectTimeout is 10 s; the tunnel waited 15 s for `ssh -L` before 0.2.0-dev.31.12).
pub const SSH_SETUP: Duration = Duration::from_secs(20);

/// At most this many connections (and ssh children) of one tunnel at once; one more is closed at once.
pub const MAX_CHILDREN: usize = 8;

pub struct SshTunnel {
    pub local_port: u16,
    pub remote_port: u16,
    shared: Arc<Shared>,
    accept: Option<std::thread::JoinHandle<()>>,
}

struct Shared {
    stop: AtomicBool,
    children: Mutex<Vec<Arc<Mutex<Child>>>>,
    last_error: Mutex<Option<String>>,
}

/// `ssh -o BatchMode=yes ... -W <target> -- <host>` with its stdin and stdout piped: one forwarded connection.
pub fn ssh_stdio(remote: &RemoteSpec, target: SocketAddr) -> Result<Child> {
    remote.validate()?;
    let fwd = target.to_string(); // "[::1]:p" for IPv6, as -W reads it
    // -W implies -N, -T, ExitOnForwardFailure and ClearAllForwardings (the user's LocalForward lines don't apply)
    let mut cmd = remote.ssh(&["-o", "ServerAliveInterval=15", "-W", &fwd]);
    cmd.stdin(Stdio::piped()).stdout(Stdio::piped()).stderr(Stdio::piped());
    cmd.spawn().map_err(|e| Error::new(ErrorCode::Network, format!("cannot run ssh: {e}")))
}

/// Copies `r` to `w` chunk by chunk, each written at once, until `r` ends or `w` fails. (std::io::copy between a socket
/// and a pipe takes Linux's splice path, which held a short message back until the end of the stream.)
pub fn pump(r: &mut dyn Read, w: &mut dyn std::io::Write) -> u64 {
    let mut buf = vec![0u8; 16 * 1024];
    let mut n = 0u64;
    loop {
        let k = match r.read(&mut buf) {
            Ok(0) | Err(_) => return n,
            Ok(k) => k,
        };
        if w.write_all(&buf[..k]).and_then(|_| w.flush()).is_err() {
            return n;
        }
        n += k as u64;
    }
}

/// Copies bytes between `tcp` and an `ssh -W` child until both directions end, then reaps the child. ssh's error
/// text (at most 2 KB) goes to `on_error` when it exits with a failure.
pub fn splice_child(tcp: TcpStream, child: Arc<Mutex<Child>>, on_error: impl FnOnce(String) + Send + 'static) {
    let (stdin, stdout, stderr) = {
        let mut c = child.lock().unwrap_or_else(|p| p.into_inner());
        (c.stdin.take(), c.stdout.take(), c.stderr.take())
    };
    let (Some(mut stdin), Some(mut stdout)) = (stdin, stdout) else { return };
    let Ok(mut tcp_r) = tcp.try_clone() else { return };
    let mut tcp_w = tcp;
    let err_text = std::thread::spawn(move || {
        let mut s = String::new();
        if let Some(mut e) = stderr {
            let mut buf = Vec::new();
            let _ = (&mut e).take(2048).read_to_end(&mut buf);
            let _ = std::io::copy(&mut e, &mut std::io::sink());
            s = String::from_utf8_lossy(&buf).trim().to_string();
        }
        s
    });
    let up = std::thread::spawn(move || {
        pump(&mut tcp_r, &mut stdin);
        drop(stdin); // EOF to the remote side
    });
    pump(&mut stdout, &mut tcp_w);
    let _ = tcp_w.shutdown(Shutdown::Write);
    // the remote side ended: unblock the other direction too
    let _ = tcp_w.shutdown(Shutdown::Read);
    let _ = up.join();
    let status = {
        let mut c = child.lock().unwrap_or_else(|p| p.into_inner());
        let _ = c.kill();
        c.wait().ok()
    };
    let text = err_text.join().unwrap_or_default();
    if status.is_some_and(|s| !s.success()) && !text.is_empty() && !text.contains("Killed") {
        on_error(text);
    }
}

impl SshTunnel {
    /// A tunnel to 127.0.0.1:`remote_port` on the remote machine.
    pub fn open(remote: &RemoteSpec, remote_port: u16) -> Result<SshTunnel> {
        Self::open_to(remote, SocketAddr::new(Ipv4Addr::LOCALHOST.into(), remote_port))
    }

    /// A tunnel to `target` as the remote machine sees it, accepting on a port of 127.0.0.1 that it holds.
    pub fn open_to(remote: &RemoteSpec, target: SocketAddr) -> Result<SshTunnel> {
        remote.validate()?;
        let listener = TcpListener::bind((Ipv4Addr::LOCALHOST, 0))?;
        let local_port = listener.local_addr()?.port();
        listener.set_nonblocking(true)?;
        let shared = Arc::new(Shared { stop: AtomicBool::new(false), children: Mutex::new(Vec::new()), last_error: Mutex::new(None) });
        let (sh, spec) = (shared.clone(), remote.clone());
        let accept = std::thread::Builder::new().name(format!("ssh-tunnel-{local_port}")).spawn(move || {
            let local = listener.local_addr().ok();
            while !sh.stop.load(Ordering::Relaxed) {
                match listener.accept() {
                    Ok((tcp, peer_addr)) => {
                        let _ = tcp.set_nonblocking(false);
                        let _ = tcp.set_nodelay(true);
                        let running = {
                            let mut list = sh.children.lock().unwrap_or_else(|p| p.into_inner());
                            list.retain(|c| c.lock().map(|mut c| matches!(c.try_wait(), Ok(None))).unwrap_or(false));
                            list.len()
                        };
                        // another user's program, or one connection too many: no ssh for it
                        if running >= MAX_CHILDREN || !local.is_some_and(|l| super::peer::admits(l, peer_addr)) {
                            let _ = tcp.shutdown(Shutdown::Both);
                            continue;
                        }
                        match ssh_stdio(&spec, target) {
                            Ok(child) => {
                                let child = Arc::new(Mutex::new(child));
                                sh.children.lock().unwrap_or_else(|p| p.into_inner()).push(child.clone());
                                let sh2 = sh.clone();
                                std::thread::spawn(move || {
                                    splice_child(tcp, child, move |e| *sh2.last_error.lock().unwrap_or_else(|p| p.into_inner()) = Some(e));
                                });
                            }
                            Err(e) => *sh.last_error.lock().unwrap_or_else(|p| p.into_inner()) = Some(e.message),
                        }
                    }
                    Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => std::thread::sleep(Duration::from_millis(50)),
                    Err(_) => std::thread::sleep(Duration::from_millis(50)),
                }
            }
        })?;
        Ok(SshTunnel { local_port, remote_port: target.port(), shared, accept: Some(accept) })
    }

    /// The tunnel still accepts connections.
    pub fn alive(&mut self) -> bool {
        self.accept.as_ref().is_some_and(|t| !t.is_finished()) && !self.shared.stop.load(Ordering::Relaxed)
    }

    /// What ssh said the last time a forwarded connection failed (no route, authentication, the target refused).
    pub fn last_error(&self) -> Option<String> {
        self.shared.last_error.lock().unwrap_or_else(|p| p.into_inner()).clone()
    }

    /// A reader of [`Self::last_error`] that outlives the tunnel (it moves into the connection that uses it). It waits
    /// a moment for a failing ssh to exit, since the connection sees the end of stream before ssh's exit status.
    pub fn errors(&self) -> impl Fn() -> Option<String> + use<> {
        let sh = self.shared.clone();
        move || {
            for _ in 0..20 {
                if let Some(e) = sh.last_error.lock().unwrap_or_else(|p| p.into_inner()).clone() {
                    return Some(e);
                }
                std::thread::sleep(Duration::from_millis(25));
            }
            None
        }
    }

    pub fn close(mut self) -> Result<()> {
        self.stop();
        Ok(())
    }

    fn stop(&mut self) {
        self.shared.stop.store(true, Ordering::Relaxed);
        if let Some(t) = self.accept.take() {
            let _ = t.join(); // the listener goes with the thread
        }
        for c in self.shared.children.lock().unwrap_or_else(|p| p.into_inner()).drain(..) {
            let mut c = c.lock().unwrap_or_else(|p| p.into_inner());
            let _ = c.kill();
            let _ = c.wait();
        }
    }
}

impl Drop for SshTunnel {
    fn drop(&mut self) {
        self.stop();
    }
}

/// Copies bytes between two TCP streams until both directions end (a direct forward, no ssh).
pub fn splice_tcp(a: TcpStream, b: TcpStream) {
    let (Ok(mut a_r), Ok(mut b_r)) = (a.try_clone(), b.try_clone()) else { return };
    let (mut a_w, mut b_w) = (a, b);
    let up = std::thread::spawn(move || {
        pump(&mut a_r, &mut b_w);
        let _ = b_w.shutdown(Shutdown::Write);
    });
    pump(&mut b_r, &mut a_w);
    let _ = a_w.shutdown(Shutdown::Write);
    let _ = up.join();
}
