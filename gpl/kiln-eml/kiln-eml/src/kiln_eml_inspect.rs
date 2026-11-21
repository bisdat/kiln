// SPDX-License-Identifier: GPL-3.0-or-later
//! kiln-eml-inspect: the read-only archive inspector, a separate process the launcher runs before a start.
//!
//!   kiln-eml-inspect --game <absolute dir> --side client|server [--hash-containers]
//!
//! Prints one inspection as JSON on stdout (schema/archive-inspection.schema.json) and exits 0, also when it refuses
//! (`status: "refused"` with its reasons). Exit 2: bad arguments, nothing on stdout. Exit 1: stdout failed.
//! A disjoint entry: it never reaches the patch runner (`runner::run`), Lua, packages, caches, locks or logs, and it
//! writes nothing anywhere (kiln-eml's `inspection` module).
use kiln_eml::inspection::{self, Options, Side};
use std::{io::Write, path::PathBuf};

fn main() {
    let usage = || -> ! {
        eprintln!(
            "usage: kiln-eml-inspect --game <absolute dir> --side client|server [--hash-containers]"
        );
        std::process::exit(2)
    };
    let (mut game, mut side, mut hash) = (None::<PathBuf>, None, false);
    let mut args = std::env::args_os().skip(1);
    while let Some(arg) = args.next() {
        match arg.to_str() {
            Some("--game") if game.is_none() => {
                game = Some(args.next().unwrap_or_else(|| usage()).into())
            }
            Some("--side") if side.is_none() => {
                side = match args.next().as_ref().and_then(|s| s.to_str()) {
                    Some("client") => Some(Side::Client),
                    Some("server") => Some(Side::Server),
                    _ => usage(),
                }
            }
            Some("--hash-containers") if !hash => hash = true,
            _ => usage(),
        }
    }
    let (Some(game), Some(side)) = (game, side) else {
        usage()
    };
    if !game.is_absolute() {
        usage();
    }
    let result = inspection::inspect(
        &game,
        &Options {
            side,
            hash_containers: hash,
        },
    );
    let mut text = serde_json::to_string(&result).unwrap_or_else(|_| std::process::exit(1));
    text.push('\n');
    let mut out = std::io::stdout().lock();
    if out
        .write_all(text.as_bytes())
        .and_then(|()| out.flush())
        .is_err()
    {
        std::process::exit(1);
    }
}
