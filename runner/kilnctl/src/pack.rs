// SPDX-License-Identifier: MIT
//! `kilnctl pack`: Crucible packs (kiln_manage::modpack::pack): build one from a profile, verify, install as an instance,
//! update with the three-way merge, diff, undo, uninstall, and the server part. Docs: docs/kiln/launcher/crucible.md.
use crate::bundle::{Printer, data_dirs};
use crate::util::Km;
use anyhow::{Result, bail};
use clap::Subcommand;
use kiln_manage::modpack::ModpackSource;
use kiln_manage::modpack::merge::Pick;
use kiln_manage::modpack::layers;
use kiln_manage::modpack::pack::{self, BuildRequest, Env, InstallRequest, Need, OverrideSpec, PartSide, ServerInstallRequest, UpdateRequest};
use std::collections::BTreeMap;
use std::path::PathBuf;

#[derive(Subcommand)]
pub enum PackCmd {
    /// Build a .kilnpack from a launcher profile (its mods as the Library holds them, a layer, config overrides)
    Build {
        /// The profile id
        profile: String,
        /// The pack's version (SemVer)
        #[arg(long)]
        version: String,
        /// The pack's id [default: from the name]
        #[arg(long)]
        id: Option<String>,
        /// The pack's name [default: the profile's]
        #[arg(long)]
        name: Option<String>,
        /// A folder with the pack layer's content (content/*.json); its kiln.mod.json is written when absent
        #[arg(long)]
        layer: Option<PathBuf>,
        /// A config file of the profile to carry: PATH[:KEY,KEY...][@both|client|server] (keys: JSON a.b, INI section.key)
        #[arg(long = "override", value_name = "SPEC")]
        overrides: Vec<String>,
        /// Make a mod optional: ID (on by default) or ID=off
        #[arg(long = "optional", value_name = "ID[=on|off]")]
        optional: Vec<String>,
        /// A mod players need but servers do not take
        #[arg(long = "client-only", value_name = "ID")]
        client_only: Vec<String>,
        /// A mod only servers run
        #[arg(long = "server-only", value_name = "ID")]
        server_only: Vec<String>,
        /// A JSON file with the server part's game settings ({"gameSettings": {...}})
        #[arg(long)]
        server_settings: Option<PathBuf>,
        /// What changed in this version
        #[arg(long)]
        changes: Option<String>,
        /// Output folder
        #[arg(long, default_value = ".")]
        out: PathBuf,
        #[arg(long)]
        json: bool,
    },
    /// Check a .kilnpack (every hash, the layout, the limits) and show what it holds
    Verify {
        file: PathBuf,
        #[arg(long)]
        sha256: Option<String>,
        #[arg(long)]
        json: bool,
    },
    /// The pack analysis Crucible runs on every upload (checks, code scan, data conflicts, prefilled credits). Exit 0:
    /// no errors; 1: errors (the report says which). Reads nothing outside its inputs, never the network
    Check {
        /// A .kilnpack (or give --index and --files: Crucible's stored form)
        file: Option<PathBuf>,
        /// kiln-modpack.json of the stored form
        #[arg(long, requires = "files", conflicts_with = "file")]
        index: Option<PathBuf>,
        /// The folder with each file the index names, called by its sha256
        #[arg(long, requires = "index")]
        files: Option<PathBuf>,
        /// Kiln's own mods (Crucible's data/kiln-own-mods.json)
        #[arg(long)]
        own: Option<PathBuf>,
        /// Earlier confirmed credits: [{"kdh1", "author", "source"}]
        #[arg(long)]
        known: Option<PathBuf>,
        /// Blocked sha256/kdh1 values: a JSON list, or Crucible's {"blocked": [{"sha256"}]}
        #[arg(long)]
        blocklist: Option<PathBuf>,
        /// The game-data catalog for lint and the conflict analysis (default: none; names of the game unchecked)
        #[arg(long)]
        catalog: Option<PathBuf>,
        /// Where to unpack [default: the temp folder]
        #[arg(long)]
        work: Option<PathBuf>,
        #[arg(long)]
        json: bool,
    },
    /// Install a .kilnpack as an instance (a profile of its own). It runs code from other people: --yes says you agree
    Install {
        file: PathBuf,
        #[arg(long)]
        sha256: Option<String>,
        /// The profile's name [default: the pack's]
        #[arg(long)]
        name: Option<String>,
        /// An optional mod's switch: ID=on|off
        #[arg(long = "optional", value_name = "ID=on|off")]
        optional: Vec<String>,
        #[arg(long)]
        yes: bool,
        #[arg(long)]
        json: bool,
    },
    /// Update an instance to a newer .kilnpack (three-way merge of its settings; --resolve per conflict)
    Update {
        profile: String,
        file: PathBuf,
        #[arg(long)]
        sha256: Option<String>,
        /// PATH#KEY=pack|mine (as the plan names them)
        #[arg(long = "resolve", value_name = "PATH#KEY=pack|mine")]
        resolve: Vec<String>,
        /// Every conflict without its own --resolve
        #[arg(long, value_name = "pack|mine")]
        all: Option<String>,
        /// A new optional mod's switch: ID=on|off
        #[arg(long = "optional", value_name = "ID=on|off")]
        optional: Vec<String>,
        /// Let go of a player-layer field the pack sets too ("Drop my change"; KEY as the plan prints it)
        #[arg(long = "drop", value_name = "KEY")]
        drop: Vec<String>,
        #[arg(long)]
        yes: bool,
        #[arg(long)]
        json: bool,
    },
    /// The player layer of an instance (local.<profile>): list its data files, or set/remove some and save it
    Local {
        profile: String,
        /// content/NAME.json=FILE: that file's text as the player layer's content/NAME.json
        #[arg(long = "set", value_name = "content/NAME.json=FILE")]
        set: Vec<String>,
        /// content/NAME.json to take out
        #[arg(long = "remove", value_name = "content/NAME.json")]
        remove: Vec<String>,
        #[arg(long)]
        json: bool,
    },
    /// What updating an instance to a .kilnpack would do; or, given two .kilnpack files, what changed between them
    Diff {
        /// An instance's profile id, or the older .kilnpack
        from: String,
        to: PathBuf,
        #[arg(long)]
        json: bool,
    },
    /// Take an instance's last update back
    Undo { profile: String },
    /// Remove an instance (its profile goes to backups; the Library keeps the mods)
    Uninstall { profile: String },
    /// The installed instances
    List,
    /// Put a pack's server part on a server folder, then optionally build the player modpack (format 1) from it
    Server {
        file: PathBuf,
        /// The server folder (the one with enshrouded_server.exe)
        #[arg(long)]
        dir: PathBuf,
        /// An optional mod's switch: ID=on|off
        #[arg(long = "optional", value_name = "ID=on|off")]
        optional: Vec<String>,
        /// Leave enshrouded_server.json's game settings as they are
        #[arg(long)]
        no_settings: bool,
        /// Build the player modpack into this folder afterwards
        #[arg(long)]
        modpack_out: Option<PathBuf>,
        #[arg(long)]
        yes: bool,
    },
}

/// Does this command use the launcher's data root (the lock)?
pub fn uses_data(cmd: &PackCmd) -> bool {
    !matches!(cmd, PackCmd::Verify { .. } | PackCmd::Check { .. })
}

fn switches(list: &[String], default_on: bool) -> Result<BTreeMap<String, bool>> {
    let mut out = BTreeMap::new();
    for s in list {
        let (id, on) = match s.split_once('=') {
            None if default_on => (s.as_str(), true),
            None => bail!("{s:?}: use ID=on or ID=off"),
            Some((id, "on")) => (id, true),
            Some((id, "off")) => (id, false),
            Some(_) => bail!("{s:?}: use ID=on or ID=off"),
        };
        out.insert(id.to_string(), on);
    }
    Ok(out)
}

fn pick(s: &str) -> Result<Pick> {
    match s {
        "pack" => Ok(Pick::Pack),
        "mine" => Ok(Pick::Mine),
        _ => bail!("{s:?}: pack or mine"),
    }
}

/// PATH[:KEY,KEY...][@side]
pub fn parse_override(s: &str) -> Result<OverrideSpec> {
    let (rest, side) = match s.rsplit_once('@') {
        Some((r, "both")) => (r, PartSide::Both),
        Some((r, "client")) => (r, PartSide::Client),
        Some((r, "server")) => (r, PartSide::Server),
        Some((_, x)) => bail!("{s:?}: side {x:?} is not both, client or server"),
        None => (s, PartSide::Both),
    };
    let (path, keys) = match rest.split_once(':') {
        Some((p, k)) => (p, k.split(',').filter(|k| !k.is_empty()).map(str::to_string).collect()),
        None => (rest, Vec::new()),
    };
    Ok(OverrideSpec { path: path.to_string(), side, keys })
}

fn print_json<T: serde::Serialize>(v: &T) -> Result<i32> {
    println!("{}", serde_json::to_string_pretty(v)?);
    Ok(0)
}

fn show_index(ix: &pack::PackIndex) {
    println!("pack {} {} ({}), {} mod(s)", ix.name, ix.version, ix.id, ix.mods.len());
    for m in &ix.mods {
        let dep = match m.dep { pack::Dep::Required => "required".to_string(), pack::Dep::Optional => format!("optional, {}", if m.default.unwrap_or(true) { "on" } else { "off" }) };
        println!("  {:<7} {:<24} {:<10} {dep}", m.kind.as_str(), m.id, m.version);
    }
    if let Some(l) = &ix.layer {
        println!("  layer   {:<24} {:<10} {}", l.id, l.version, l.hash);
    }
    for o in &ix.overrides {
        println!("  override {} ({:?}, {:?})", o.path, o.mode, o.side);
    }
}

fn show_plan(p: &pack::UpdatePlan) {
    println!("{} {} -> {}", p.pack, p.from, p.to);
    for m in &p.mods.added { println!("  + {} {}", m.id, m.version); }
    for m in &p.mods.updated { println!("  ~ {} {} -> {}", m.id, m.from, m.to); }
    for m in &p.mods.removed { println!("  - {} (kept in your Library)", m.id); }
    for m in &p.mods.yours { println!("  = {} (yours, kept)", m.id); }
    for s in &p.mods.kept_switches { println!("  = {s}"); }
    if p.layer.changed { println!("  pack data changed"); }
    for f in &p.files {
        if let Some(from) = &f.renamed_from { println!("  {from} is {} now: your values move along", f.path); }
        for k in &f.taken { println!("  {}: {k} updated by the pack", f.path); }
        for k in &f.kept { println!("  {}: {k} your change kept", f.path); }
        for c in &f.conflicts {
            println!("  CONFLICT {}#{}: pack had {}, now {}, you have {}", f.path, c.key, shown(&c.base), shown(&c.pack), shown(&c.mine));
        }
    }
    if let Some(l) = &p.local {
        println!("  your changes ({}, {} file(s)) are kept and still load last", l.id, l.files);
        for o in &l.overlaps {
            println!("  data {} {} {}: pack {}{}, yours {} still wins (--drop '{}')", o.section, o.target, o.field, o.pack,
                if o.changed { " (changed)" } else { "" }, o.mine, o.key);
        }
    }
    if p.conflicts > 0 {
        println!("{} conflict(s): --resolve PATH#KEY=pack|mine or --all pack|mine", p.conflicts);
    }
}

fn shown(v: &Option<serde_json::Value>) -> String {
    v.as_ref().map(|v| v.to_string()).unwrap_or_else(|| "(none)".into())
}

pub fn run(cmd: PackCmd) -> Result<i32> {
    match cmd {
        PackCmd::Build { profile, version, id, name, layer, overrides, optional, client_only, server_only, server_settings, changes, out, json } => {
            let dirs = data_dirs()?;
            let mut env = BTreeMap::new();
            for id in client_only { env.insert(id, Env { client: Need::Required, server: Need::Unsupported }); }
            for id in server_only { env.insert(id, Env { client: Need::Unsupported, server: Need::Required }); }
            let server_settings = match server_settings {
                Some(p) => Some(serde_json::from_str(&std::fs::read_to_string(&p)?)?),
                None => None,
            };
            let req = BuildRequest {
                profile, id, name, version, changes, layer, server_settings, env,
                optional: switches(&optional, true)?,
                overrides: overrides.iter().map(|s| parse_override(s)).collect::<Result<_>>()?,
                ..Default::default()
            };
            let built = if json { pack::build(&dirs, &req, &out, &mut kiln_manage::task::NoProgress) } else { pack::build(&dirs, &req, &out, &mut Printer::new()) }.km()?;
            if json { return print_json(&built); }
            show_index(&built.index);
            for n in &built.notes { println!("  note: {n}"); }
            println!("{} ({} bytes, sha256 {})", built.file.display(), built.size, built.sha256);
            Ok(0)
        }
        PackCmd::Verify { file, sha256, json } => {
            let ix = pack::verify(&file, sha256.as_deref()).km()?;
            if json { return print_json(&ix); }
            show_index(&ix);
            println!("ok");
            Ok(0)
        }
        PackCmd::Check { file, index, files, own, known, blocklist, catalog, work, json } => {
            use kiln_manage::modpack::check::{self, CheckOptions, Input, Level};
            let read_json = |p: &PathBuf| -> Result<serde_json::Value> {
                Ok(serde_json::from_slice(&std::fs::read(p).map_err(|e| anyhow::anyhow!("{}: {e}", p.display()))?)?)
            };
            let mut o = CheckOptions { work, ..Default::default() };
            if let Some(p) = &own { o.own = Some(serde_json::from_value(read_json(p)?)?); }
            if let Some(p) = &known { o.known = serde_json::from_value(read_json(p)?)?; }
            if let Some(p) = &blocklist { o.blocked = CheckOptions::read_blocklist(&read_json(p)?); }
            o.lint.no_catalog = catalog.is_none();
            o.lint.catalog = catalog;
            let input = match (&file, &index, &files) {
                (Some(f), None, None) => Input::Pack(f),
                (None, Some(i), Some(d)) => Input::Stored { index: i, files: d },
                _ => bail!("give a .kilnpack, or --index and --files"),
            };
            let r = check::check(input, &o).km()?;
            if json {
                println!("{}", serde_json::to_string_pretty(&r)?);
            } else {
                if let Some(p) = &r.pack { println!("pack {} {} ({})", p.name, p.version, p.id); }
                for f in &r.findings {
                    let l = match f.level { Level::Error => "error", Level::Warning => "warning", Level::Note => "note" };
                    println!("  {l} {}: {}", f.code, f.message);
                }
                for c in &r.credits {
                    println!("  credit {:<24} {} {} ({}, {})", c.id, c.author.as_deref().unwrap_or("?"), c.source.as_deref().unwrap_or("-"), c.match_kind, c.confidence);
                }
                println!("{}: {} error(s), {} warning(s), {} note(s)", if r.ok { "ok" } else { "not ok" }, r.summary.errors, r.summary.warnings, r.summary.notes);
            }
            Ok(if r.ok { 0 } else { 1 })
        }
        PackCmd::Install { file, sha256, name, optional, yes, json } => {
            if !yes { bail!("a pack runs code from other people on this PC: add --yes if you trust it"); }
            let dirs = data_dirs()?;
            let req = InstallRequest { source: ModpackSource::Path { path: file, sha256 }, consent: true, allow_http: false, name,
                optional: switches(&optional, false)?, activate: Some(false) };
            let r = pack::install(&dirs, None, &req, &mut Printer::new()).km()?;
            if json { return print_json(&r); }
            println!("installed {} {} as profile {} ({}); activate it with: kilnctl client activate {}", r.instance.name, r.instance.version, r.profile.id, r.profile.name, r.profile.id);
            Ok(0)
        }
        PackCmd::Update { profile, file, sha256, resolve, all, optional, drop, yes, json } => {
            if !yes { bail!("a pack runs code from other people on this PC: add --yes if you trust it"); }
            let dirs = data_dirs()?;
            let mut res = BTreeMap::new();
            for r in &resolve {
                let Some((k, p)) = r.rsplit_once('=') else { bail!("{r:?}: PATH#KEY=pack|mine") };
                res.insert(k.to_string(), pick(p)?);
            }
            let req = UpdateRequest { profile, source: ModpackSource::Path { path: file, sha256 }, consent: true, allow_http: false,
                optional: switches(&optional, false)?, resolve: res, all: all.as_deref().map(pick).transpose()?, drop };
            match pack::update(&dirs, &req, &mut Printer::new()) {
                Ok(r) => {
                    if json { return print_json(&r); }
                    show_plan(&r.plan);
                    println!("updated; kilnctl pack undo {} takes it back", r.profile.id);
                    Ok(0)
                }
                Err(e) if e.code == kiln_manage::ErrorCode::Conflict => {
                    if let Some(plan) = e.detail.clone().and_then(|d| serde_json::from_value::<pack::UpdatePlan>(d).ok()) {
                        show_plan(&plan);
                    }
                    bail!("{}", e.message)
                }
                Err(e) => bail!("{}", e.message),
            }
        }
        PackCmd::Diff { from, to, json } => {
            let as_file = PathBuf::from(&from);
            if as_file.is_file() {
                let (a, b) = (pack::verify(&as_file, None).km()?, pack::verify(&to, None).km()?);
                let d = pack::diff(&a, &b);
                if json { return print_json(&d); }
                println!("{} -> {}", d.from, d.to);
                for m in &d.added { println!("  + {} {}", m.id, m.version); }
                for m in &d.updated { println!("  ~ {} {} -> {}", m.id, m.from, m.to); }
                for m in &d.removed { println!("  - {} {}", m.id, m.version); }
                if d.layer_changed { println!("  pack data changed"); }
                for o in &d.overrides_added { println!("  + {o}"); }
                for o in &d.overrides_changed { println!("  ~ {o}"); }
                for o in &d.overrides_removed { println!("  - {o}"); }
                if d.server_settings_changed { println!("  server settings changed"); }
                return Ok(0);
            }
            let dirs = data_dirs()?;
            let plan = pack::plan_update(&dirs, &from, &to, None, &BTreeMap::new()).km()?;
            if json { return print_json(&plan); }
            show_plan(&plan);
            Ok(0)
        }
        PackCmd::Local { profile, set, remove, json } => {
            let dirs = data_dirs()?;
            if !(set.is_empty() && remove.is_empty()) {
                for s in &set {
                    let Some((rel, from)) = s.split_once('=') else { bail!("{s:?}: content/NAME.json=FILE") };
                    let text = std::fs::read_to_string(from)?;
                    layers::write(&dirs, &profile, rel, Some(&text)).km()?;
                }
                for rel in &remove {
                    layers::write(&dirs, &profile, rel, None).km()?;
                }
                layers::save(&dirs, &profile).km()?;
            }
            let files = layers::files(&dirs, &profile).km()?;
            if json { return print_json(&files); }
            for f in &files { println!("{} ({} bytes)", f.path, f.text.len()); }
            if files.is_empty() { println!("no player layer: kilnctl pack local {profile} --set content/mine.json=FILE makes one"); }
            Ok(0)
        }
        PackCmd::Undo { profile } => {
            let p = pack::undo(&data_dirs()?, &profile).km()?;
            println!("profile {} is back as it was before its last update (generation {})", p.id, p.generation);
            Ok(0)
        }
        PackCmd::Uninstall { profile } => {
            let r = pack::uninstall(&data_dirs()?, &profile).km()?;
            println!("removed instance {} (in backups/profiles now)", r.profile);
            for u in &r.unused { println!("  unused in the Library: {} {} {}", u.kind.as_str(), u.id, u.version); }
            Ok(0)
        }
        PackCmd::List => {
            for (pid, i) in pack::instances(&data_dirs()?).km()? {
                println!("{pid:<24} {} {} ({}){}", i.name, i.version, i.pack, i.previous.map(|p| format!(", updated from {p}")).unwrap_or_default());
            }
            Ok(0)
        }
        PackCmd::Server { file, dir, optional, no_settings, modpack_out, yes } => {
            if !yes { bail!("a pack runs code from other people on this server: add --yes if you trust it"); }
            let dirs = data_dirs()?;
            let inst = kiln_manage::server::instance::steam_instance("kilnctl-pack", &dir, None);
            let req = ServerInstallRequest { source: ModpackSource::Path { path: file, sha256: None }, consent: true, allow_http: false,
                optional: switches(&optional, false)?, settings: !no_settings, modpack_out };
            let r = pack::install_server(&dirs, &inst, &req, &mut Printer::new()).km()?;
            for m in &r.mods { println!("  {} {}", m.folder, if m.enabled { "" } else { "(off)" }); }
            for o in &r.overrides { println!("  config {o}"); }
            if r.settings { println!("  game settings written"); }
            for id in &r.left_out { println!("  left out (no server folder for its kind): {id}"); }
            if let Some(mp) = &r.modpack { println!("player modpack {} (sha256 {}): publish it in the server's Modpack page", mp.file.display(), mp.sha256); }
            Ok(0)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn override_specs_parse() {
        let o = parse_override("alpha.json:range,loot.smash@server").unwrap();
        assert_eq!((o.path.as_str(), o.side, o.keys.clone()), ("alpha.json", PartSide::Server, vec!["range".to_string(), "loot.smash".to_string()]));
        let o = parse_override("emf/beast.ini").unwrap();
        assert_eq!((o.path.as_str(), o.side, o.keys.len()), ("emf/beast.ini", PartSide::Both, 0));
        assert!(parse_override("a.json@moon").is_err());
        assert_eq!(switches(&["a".into(), "b=off".into()], true).unwrap(), BTreeMap::from([("a".to_string(), true), ("b".to_string(), false)]));
        assert!(switches(&["a".into()], false).is_err());
    }
}
