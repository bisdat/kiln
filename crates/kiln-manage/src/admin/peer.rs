//! Whether a loopback connection comes from a program of this user. On Linux any local user can connect to a
//! loopback port and read other users' command lines (`/proc/<pid>/cmdline`); the kernel's socket table
//! (`/proc/net/tcp`, `/proc/net/tcp6`) names the owner of each connection's client end. Elsewhere this is not
//! checked (Windows hides other users' command lines from standard users; macOS is not a launcher platform).
//!
//! Used by the web panel gateway, its bootstrap page and the SSH tunnel's held port ([`admits`]). On Linux a
//! connection is served only when its client end is found and is this user's: a connection the table does not show
//! (a read that raced the table's changes, after one retry) is refused, never waved through. (Before the review of
//! 0.2.0-dev.31.12 the gateway let such a connection through, and a remote panel takes everything that arrives through
//! the gateway's ssh as loopback.)

use std::net::SocketAddr;

/// Whether a connection from `peer` to `listener` may be served: on Linux only a found client end of this user, after
/// a second look when the first read of the table did not find it; elsewhere every connection.
pub fn admits(listener: SocketAddr, peer: SocketAddr) -> bool {
    decide(|| same_user(listener, peer))
}

/// [`admits`]'s rule over a lookup (two tries, 5 ms apart, while it answers None).
pub fn decide(mut lookup: impl FnMut() -> Option<bool>) -> bool {
    if !cfg!(target_os = "linux") {
        return true;
    }
    for i in 0..2 {
        if let Some(v) = lookup() {
            return v;
        }
        if i == 0 {
            std::thread::sleep(std::time::Duration::from_millis(5));
        }
    }
    false
}

/// Some(true): the client end of `peer` -> `listener` belongs to this user; Some(false): to another; None: unknown
/// (not Linux, or not in this network namespace's table).
pub fn same_user(listener: SocketAddr, peer: SocketAddr) -> Option<bool> {
    #[cfg(target_os = "linux")]
    {
        // SAFETY: geteuid has no preconditions.
        let me = unsafe { libc::geteuid() };
        for file in ["/proc/net/tcp", "/proc/net/tcp6"] {
            let Ok(text) = std::fs::read_to_string(file) else { continue };
            if let Some(uid) = owner(&text, peer, listener) {
                return Some(uid == me);
            }
        }
        None
    }
    #[cfg(not(target_os = "linux"))]
    {
        let _ = (listener, peer);
        None
    }
}

/// The uid on the line of `text` (a /proc/net/tcp{,6} table) whose local address is `local` and remote `remote`.
pub fn owner(text: &str, local: SocketAddr, remote: SocketAddr) -> Option<u32> {
    for line in text.lines().skip(1) {
        let f: Vec<&str> = line.split_whitespace().collect();
        if f.len() < 8 {
            continue;
        }
        if parse_addr(f[1]) == Some(local) && parse_addr(f[2]) == Some(remote) {
            return f[7].parse().ok();
        }
    }
    None
}

/// `0100007F:1F90` (IPv4) or 32 hex digits `:PORT` (IPv6): addresses in host byte order per 32-bit word.
pub fn parse_addr(s: &str) -> Option<SocketAddr> {
    let (a, p) = s.split_once(':')?;
    let port = u16::from_str_radix(p, 16).ok()?;
    match a.len() {
        8 => {
            let v = u32::from_str_radix(a, 16).ok()?;
            Some(SocketAddr::new(std::net::Ipv4Addr::from(v.to_ne_bytes()).into(), port))
        }
        32 => {
            let mut b = [0u8; 16];
            for i in 0..4 {
                let v = u32::from_str_radix(&a[i * 8..i * 8 + 8], 16).ok()?;
                b[i * 4..i * 4 + 4].copy_from_slice(&v.to_ne_bytes());
            }
            let v6 = std::net::Ipv6Addr::from(b);
            // a v4 client of a dual-stack socket shows as ::ffff:a.b.c.d
            Some(SocketAddr::new(v6.to_ipv4_mapped().map(Into::into).unwrap_or(v6.into()), port))
        }
        _ => None,
    }
}
