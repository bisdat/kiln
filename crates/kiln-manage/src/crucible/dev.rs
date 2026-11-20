//! A Crucible running on this machine (development and the end-to-end test): its URLs are http://127.0.0.1:<port>, which
//! the launcher's network layer never reaches (https on public hosts only). A dev endpoint names each local origin
//! with a made-up https host (`https://api.crucible.test`); [`super::Endpoints::rewrite`] turns the service's own URLs into
//! those, and this transport sends a request for such a host to its loopback address, with the Host header the service
//! signed its upload URLs for. Only loopback addresses; the launcher builds this only from KILN_CRUCIBLE_DEV (debug
//! builds or the crucible-dev feature).

use crate::net::{self, Body, Method, Response, Transport, WireRequest};
use crate::{Error, ErrorCode, Result};
use std::io::{Read, Write};
use std::net::SocketAddr;

/// One local origin: the made-up https host, where it really is, and the Host header it expects.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Local {
    pub host: String,
    pub addr: SocketAddr,
    pub host_header: String,
}

pub struct LocalTransport {
    map: Vec<Local>,
}

impl LocalTransport {
    pub fn new(map: Vec<Local>) -> Result<LocalTransport> {
        if map.iter().any(|l| !l.addr.ip().is_loopback()) {
            return Err(Error::new(ErrorCode::Refused, "A local Crucible must be on this machine (a loopback address)."));
        }
        Ok(LocalTransport { map })
    }

    fn send(&self, req: WireRequest) -> Result<(Response, Vec<u8>)> {
        let fail = || net::network_error(&req.url);
        let local = self.map.iter().find(|l| l.host == req.url.host).ok_or_else(|| {
            Error::new(ErrorCode::Refused, format!("{} is not a local Crucible host. Nothing was sent.", req.url.host))
        })?;
        let mut c = std::net::TcpStream::connect_timeout(&local.addr, std::time::Duration::from_secs(5)).map_err(|_| fail())?;
        c.set_read_timeout(Some(std::time::Duration::from_secs(60))).map_err(|_| fail())?;
        let mut head = format!("{} {} HTTP/1.1\r\nHost: {}\r\nConnection: close\r\n", req.method.as_str(), req.url.path_and_query, local.host_header);
        for (k, v) in &req.headers {
            head.push_str(&format!("{k}: {v}\r\n"));
        }
        let len = req.body.len();
        if len > 0 || matches!(req.method, Method::Post | Method::Put | Method::Patch | Method::Delete) {
            head.push_str(&format!("Content-Length: {len}\r\n"));
        }
        head.push_str("\r\n");
        c.write_all(head.as_bytes()).map_err(|_| fail())?;
        match req.body {
            Body::Empty => {}
            Body::Bytes(b) => c.write_all(&b).map_err(|_| fail())?,
            Body::File { file, len } => {
                std::io::copy(&mut net::Exactly::new(file, len), &mut c).map_err(|_| fail())?;
            }
            Body::Reader { reader, len } => {
                std::io::copy(&mut net::Exactly::new(reader, len), &mut c).map_err(|_| fail())?;
            }
        }
        let mut raw = Vec::new();
        c.take(req.max_response + 64 * 1024).read_to_end(&mut raw).map_err(|_| fail())?;
        let end = raw.windows(4).position(|w| w == b"\r\n\r\n").ok_or_else(fail)?;
        let text = String::from_utf8_lossy(&raw[..end]).into_owned();
        let mut lines = text.split("\r\n");
        let status: u16 = lines.next().and_then(|l| l.split(' ').nth(1)).and_then(|s| s.parse().ok()).ok_or_else(fail)?;
        let headers: Vec<(String, String)> = lines.filter_map(|l| l.split_once(':')).map(|(k, v)| (k.trim().to_string(), v.trim().to_string())).collect();
        let mut body = raw[end + 4..].to_vec();
        let header = |n: &str| headers.iter().find(|(k, _)| k.eq_ignore_ascii_case(n)).map(|(_, v)| v.clone());
        if header("transfer-encoding").is_some_and(|v| v.eq_ignore_ascii_case("chunked")) {
            body = dechunk(&body).ok_or_else(fail)?;
        } else if let Some(n) = header("content-length").and_then(|v| v.parse::<usize>().ok()) {
            if body.len() < n {
                return Err(fail());
            }
            body.truncate(n);
        }
        if body.len() as u64 > req.max_response {
            return Err(net::too_large(&req.url));
        }
        Ok((Response { status, headers, body: Vec::new() }, body))
    }
}

fn dechunk(mut b: &[u8]) -> Option<Vec<u8>> {
    let mut out = Vec::new();
    loop {
        let line = b.windows(2).position(|w| w == b"\r\n")?;
        let size = usize::from_str_radix(std::str::from_utf8(&b[..line]).ok()?.split(';').next()?.trim(), 16).ok()?;
        b = &b[line + 2..];
        if size == 0 {
            return Some(out);
        }
        if b.len() < size + 2 {
            return None;
        }
        out.extend_from_slice(&b[..size]);
        b = &b[size + 2..];
    }
}

impl Transport for LocalTransport {
    fn exchange(&self, req: WireRequest) -> Result<Response> {
        let (mut r, body) = self.send(req)?;
        r.body = body;
        Ok(r)
    }

    fn download(&self, req: WireRequest, sink: &mut dyn Write) -> Result<Response> {
        let (r, body) = self.send(req)?;
        if (200..300).contains(&r.status) {
            sink.write_all(&body)?;
        }
        Ok(r)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn chunked_bodies_are_put_together() {
        assert_eq!(dechunk(b"5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n").unwrap(), b"hello world");
        assert!(dechunk(b"5\r\nhel").is_none());
    }

    #[test]
    fn only_loopback_addresses() {
        let l = |a: &str| Local { host: "api.crucible.test".into(), addr: a.parse().unwrap(), host_header: "x".into() };
        assert!(LocalTransport::new(vec![l("127.0.0.1:8080")]).is_ok());
        assert!(LocalTransport::new(vec![l("192.168.1.2:8080")]).is_err());
    }
}
