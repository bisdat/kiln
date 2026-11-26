// SPDX-License-Identifier: MIT
//! `kilnctl registry status|rollback`: the registry stage's plans (kiln_manage::registry_plan). A rollback starts the next
//! boot on the previous (last good) plan's edits; it writes one request file beside the plans and never touches a save.
use crate::util::Km;
use anyhow::{Result, bail};
use kiln_manage::registry_plan::{self as rp, Change};
use std::path::{Path, PathBuf};

/// The changes printed before "... and N more".
const SHOWN: usize = 50;

/// An instance's server folder, `--dir`, else the client Steam knows. Writing into a Steam library needs `--real`.
fn game_of(name: Option<&str>, dir: Option<&Path>, write: bool, real: bool) -> Result<PathBuf> {
    match (name, dir) {
        (Some(_), Some(_)) => bail!("give a server instance or --dir, not both"),
        (Some(n), None) => Ok(crate::config::load(n)?.dir),
        (None, Some(d)) if write => Ok(crate::bundle::game_at(d, real)?.path),
        (None, Some(d)) => Ok(kiln_manage::game::inspect(d).km()?.path),
        (None, None) => Ok(crate::client::client_at(None, real || !write)?.path),
    }
}

fn short(h: &str) -> &str {
    &h[..h.len().min(12)]
}

pub fn status(name: Option<String>, dir: Option<PathBuf>) -> Result<i32> {
    let game = game_of(name.as_deref(), dir.as_deref(), false, false)?;
    let c = rp::current(&game).km()?;
    match &c.plan {
        Some(p) => println!("plan of the last start: {p} (written {}, phase {})", c.written.as_deref().unwrap_or("?"), c.phase.as_deref().unwrap_or("?")),
        None => println!("plan of the last start: none ({} has none)", rp::REPORT_REL),
    }
    println!("previous (last good) plan: {}", c.previous.as_deref().unwrap_or("none"));
    match &c.rollback {
        Some(r) => println!("rollback at the last start: {} plan {}{}", r["state"].as_str().unwrap_or("?"), r["plan"].as_str().map(short).unwrap_or("?"),
            r["reason"].as_str().map(|w| format!(": {w}")).unwrap_or_default()),
        None => println!("rollback at the last start: none"),
    }
    match rp::pending(&game).km()? {
        Some(q) => println!("rollback request: plan {} (requested {} by {}); the next start runs on it, `kilnctl registry rollback --clear` removes it",
            q.plan, q.requested, q.by),
        None => println!("rollback request: none"),
    }
    Ok(0)
}

pub struct RollbackArgs {
    pub name: Option<String>,
    pub dir: Option<PathBuf>,
    pub to: Option<String>,
    pub clear: bool,
    pub dry_run: bool,
    pub real: bool,
}

fn print_changes(changes: &[Change]) {
    if changes.is_empty() {
        println!("no value changes back (the two plans resolve to the same values)");
        return;
    }
    println!("changes back ({}):", changes.len());
    for c in changes.iter().take(SHOWN) {
        println!("  {}: {} -> {}", c.field, rp::value_text(c.from.as_ref()), rp::value_text(c.to.as_ref()));
    }
    if changes.len() > SHOWN {
        println!("  ... and {} more (after the start, {} lists every one under rollback.changedBack)", changes.len() - SHOWN, rp::REPORT_REL);
    }
}

pub fn rollback(a: RollbackArgs) -> Result<i32> {
    let game = game_of(a.name.as_deref(), a.dir.as_deref(), !a.dry_run, a.real)?;
    if a.clear {
        if rp::clear(&game).km()? {
            println!("rollback request removed: the next start runs on the mods' current registry edits again (saves are not touched)");
        } else {
            println!("no rollback request to remove");
        }
        return Ok(0);
    }
    let c = rp::current(&game).km()?;
    let Some(from) = c.plan.clone() else {
        bail!("no registry plan yet: {} names none (start the game or server once with Kiln)", rp::REPORT_REL);
    };
    let rolled_back = c.rollback.as_ref().is_some_and(|r| r["state"] == "applied");
    let to = match (a.to.clone(), c.previous.clone()) {
        (Some(t), _) => t,
        (None, Some(p)) => p,
        (None, None) => bail!(
            "no earlier good plan to roll back to: {} plan.previous is null (a plan becomes good when a world starts on it without a refused edit)",
            rp::REPORT_REL
        ),
    };
    if to == from {
        bail!("plan {} is the one the last start ran on: nothing to roll back to", short(&to));
    }
    let target = rp::plan(&game, &to).km()?;
    println!("registry plan of the last start: {}{} (written {})", short(&from), if rolled_back { " (itself a rollback)" } else { "" },
        c.written.as_deref().unwrap_or("?"));
    let mods = target.mods.iter().map(|m| format!("{} {}", m.id, m.version)).collect::<Vec<_>>();
    println!("rolling back to plan {}: {}", short(&to), if mods.is_empty() { "no registry edits at all".to_owned() } else { mods.join(", ") });
    match rp::plan(&game, &from) {
        Ok(now) => print_changes(&rp::changes(&now, &target)),
        Err(e) => println!("what changes back: unknown here ({}); the start lists it under rollback.changedBack", e.message),
    }
    println!("saves are not touched. A start refuses the plan whole, and runs the mods' current edits, when one of its mods is not");
    println!("loaded at the version above, one of its edits is refused on that game build, or it would change what saves hold against");
    println!("the plan of the last start (a narrower item level range, a shorter build-zone list; a lower max stack without Kiln's stack guard).");
    if a.dry_run {
        println!("dry run: nothing written");
        return Ok(0);
    }
    rp::ask(&game, &to, Some(&from), "kilnctl").km()?;
    println!("wrote {}/{}: the next start runs on plan {} (and every start after it, until `kilnctl registry rollback --clear`)",
        rp::DATA_REL, rp::REQUEST_NAME, short(&to));
    Ok(0)
}
