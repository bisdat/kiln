// SPDX-License-Identifier: MIT
//! `kilnctl pack`: Crucible packs (kiln_manage::modpack::pack): build one from a profile, verify, install as an instance,
//! update with the three-way merge, diff, undo, uninstall, and the server part. Docs: docs/kiln/launcher/crucible.md.
use crate::bundle::{Printer, data_dirs};
use crate::util::Km;
use anyhow::{Result, bail};
use clap::Subcommand;
use kiln_manage::modpack::ModpackSource;
use kiln_manage::modpack::merge::Pick;
use kiln_manage::modpack::pack::{self, BuildRequest, Env, InstallRequest, Need, OverrideSpec, PartSide, ServerInstallRequest, UpdateRequest};
use kiln_manage::modpack::studio;
use std::collections::BTreeMap;
use std::path::PathBuf;

#[derive(Subcommand)]
pub enum PackCmd {
    /// Start a pack source folder (Pack Studio's form: kiln-pack.json, layer/, credits.json) from a profile, or with a new
    /// empty profile; build, lint, credits and publish then work on the folder (keep it in git)
    Init {
        /// The folder (must not hold a pack yet)
        dir: PathBuf,
        /// The pack's name
        #[arg(long)]
        name: String,
        /// The profile it is made from [default: a new empty profile named after the pack]
        #[arg(long)]
        profile: Option<String>,
        /// The pack's id [default: from the name]
        #[arg(long)]
        id: Option<String>,
    },
    /// Add mods to a pack source's profile: Library entries (KIND:ID@VERSION or ID@VERSION) or files/folders to import
    Add {
        dir: PathBuf,
        #[arg(required = true)]
        what: Vec<String>,
    },
    /// The pack's profile settings that differ from the mods' defaults; with --take, the keys the pack carries
    #[command(name = "capture-settings")]
    CaptureSettings {
        dir: PathBuf,
        /// A settings file and its keys: PATH[:KEY,KEY...][@both|client|server] (no keys: the whole file). Replaces the
        /// pack's choice; --take none clears it
        #[arg(long = "take", value_name = "SPEC")]
        take: Vec<String>,
        #[arg(long)]
        json: bool,
    },
    /// kiln lint over the pack layer, the pack's profile as the other mods. Exit 1: errors
    Lint {
        dir: PathBuf,
        /// The game-data catalog [default: the launcher's]
        #[arg(long)]
        catalog: Option<PathBuf>,
        #[arg(long)]
        json: bool,
    },
    /// The credits of a pack source: what is saved, and what the pack analysis identifies; --set fills a row
    Credits {
        dir: PathBuf,
        /// ID=AUTHOR,https://... (the mod's credit; AUTHOR may not hold a comma)
        #[arg(long = "set", value_name = "ID=AUTHOR,URL")]
        set: Vec<String>,
        /// Take every identified row the analysis found for mods without a saved credit
        #[arg(long)]
        accept: bool,
        #[arg(long)]
        json: bool,
    },
    /// Publish the pack source's next version to Crucible (link only, or --public with a credit for every mod). Needs
    /// a Crucible session: KILN_CRUCIBLE_SESSION or --session-file (the launcher's Discord sign-in)
    Publish {
        dir: PathBuf,
        /// The version [default: the source's next]
        #[arg(long)]
        version: Option<String>,
        #[arg(long)]
        changes: Option<String>,
        /// A public pack (credits for every mod, a summary; --yes confirms you credited every mod and may share the files)
        #[arg(long)]
        public: bool,
        #[arg(long)]
        yes: bool,
        #[arg(long)]
        session_file: Option<PathBuf>,
        #[arg(long)]
        json: bool,
    },
    /// Build a .kilnpack from a launcher profile (its mods as the Library holds them, a layer, config overrides), or from
    /// a pack source folder (kilnctl pack init; its own choices, the other options then do not apply)
    Build {
        /// The profile id, or a pack source folder given as a path (./my-pack, /home/me/my-pack)
        profile: String,
        /// The pack's version (SemVer; a pack source's own when not given)
        #[arg(long)]
        version: Option<String>,
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
        #[arg(long)]
        yes: bool,
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
        for k in &f.taken { println!("  {}: {k} updated by the pack", f.path); }
        for k in &f.kept { println!("  {}: {k} your change kept", f.path); }
        for c in &f.conflicts {
            println!("  CONFLICT {}#{}: pack had {}, now {}, you have {}", f.path, c.key, shown(&c.base), shown(&c.pack), shown(&c.mine));
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
            // a folder is named as a path (it has a separator, or is "."): a profile id never shadows a folder or the other way round
            let folder = std::path::Path::new(&profile);
            let looks_like_path = profile.contains('/') || profile.contains(std::path::MAIN_SEPARATOR) || profile == "." || profile == "..";
            if looks_like_path && folder.join(studio::SOURCE_FILE).is_file() {
                if id.is_some() || name.is_some() || layer.is_some() || !overrides.is_empty() || !optional.is_empty() || !client_only.is_empty()
                    || !server_only.is_empty() || server_settings.is_some() || changes.is_some() {
                    bail!("{profile} is a pack source: its kiln-pack.json holds those choices (only --version and --out apply)");
                }
                let v = version.as_deref();
                let built = if json { studio::build_as(&dirs, folder, v, Some(&out), &mut kiln_manage::task::NoProgress) } else { studio::build_as(&dirs, folder, v, Some(&out), &mut Printer::new()) }.km()?;
                if json { return print_json(&built); }
                show_index(&built.index);
                for n in &built.notes { println!("  note: {n}"); }
                println!("{} ({} bytes, sha256 {})", built.file.display(), built.size, built.sha256);
                return Ok(0);
            }
            let Some(version) = version else { bail!("--version is needed to build a pack from a profile") };
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
        PackCmd::Init { dir, name, profile, id } => {
            let dirs = data_dirs()?;
            let st = studio::init(&dirs, &dir, &studio::NewPack { name, from: profile, id }).km()?;
            println!("pack source {} ({}) in {}, made from the profile {} ({} mods)", st.source.name, st.source.id, dir.display(),
                st.profile_name.as_deref().unwrap_or(&st.source.profile), st.mods.len());
            println!("next: kilnctl pack add | capture-settings | lint | credits | build | publish {}", dir.display());
            Ok(0)
        }
        PackCmd::Add { dir, what } => {
            let dirs = data_dirs()?;
            let s = studio::read_source(&dir).km()?;
            let mut refs = Vec::new();
            let mut paths = Vec::new();
            let lib = kiln_manage::library::list(&dirs, None).km()?;
            for w in &what {
                if std::path::Path::new(w).exists() {
                    paths.push(PathBuf::from(w));
                    continue;
                }
                let (kind, rest) = match w.split_once(':') {
                    Some((k, r)) => (Some(kiln_manage::manifest::ModKind::parse(k).ok_or_else(|| anyhow::anyhow!("{w}: {k} is not a mod kind"))?), r),
                    None => (None, w.as_str()),
                };
                let (id, version) = rest.split_once('@').map(|(a, b)| (a, Some(b))).unwrap_or((rest, None));
                let e = lib.iter().find(|e| e.id == id && kind.is_none_or(|k| e.kind == k)).ok_or_else(|| anyhow::anyhow!("{w}: not in the Library (give a file to import)"))?;
                let v = match version { Some(v) => e.versions.iter().find(|x| x.version == v), None => e.versions.first() }
                    .ok_or_else(|| anyhow::anyhow!("{w}: the Library has no such version"))?;
                refs.push(kiln_manage::library::LibraryRef { kind: e.kind, id: e.id.clone(), version: v.version.clone(), hash: v.hash.clone() });
            }
            if !paths.is_empty() {
                let r = kiln_manage::library::import(&dirs, &paths, &mut Printer::new()).km()?;
                for s in &r.skipped { println!("  skipped {}: {}", s.path, s.reason); }
                refs.extend(r.imported.into_iter().chain(r.existing));
            }
            let mut p = kiln_manage::profile::get(&dirs, &s.profile).km()?;
            for r in &refs {
                let list = p.mods.list_mut(r.kind);
                list.retain(|m| m.id != r.id);
                list.push(kiln_manage::profile::ProfileMod { id: r.id.clone(), version: r.version.clone(), hash: r.hash.clone(), enabled: true });
                println!("  + {} {} {}", r.kind.as_str(), r.id, r.version);
            }
            kiln_manage::profile::save(&dirs, &p).km()?;
            Ok(0)
        }
        PackCmd::CaptureSettings { dir, take, json } => {
            let dirs = data_dirs()?;
            if !take.is_empty() {
                let choices: Vec<studio::CaptureChoice> = if take == ["none"] { Vec::new() } else {
                    take.iter().map(|s| parse_override(s).map(|o| studio::CaptureChoice { whole: o.keys.is_empty(), path: o.path, side: o.side, keys: o.keys })).collect::<Result<_>>()?
                };
                let st = studio::set_capture(&dirs, &dir, &choices).km()?;
                for o in &st.source.overrides { println!("  {} {:?} {}", o.path, o.side, if o.keys.is_empty() { "(whole file)".to_string() } else { o.keys.join(", ") }); }
                return Ok(0);
            }
            let files = studio::capture(&dirs, &dir).km()?;
            if json { return print_json(&files); }
            for f in &files {
                match &f.refused {
                    Some(why) => println!("{}: can't travel ({why})", f.path),
                    None => println!("{}{}", f.path, if f.whole { " (in the pack, whole file)" } else if f.defaults { "" } else { " (no defaults known)" }),
                }
                for k in &f.keys {
                    let mark = if k.secret.is_some() { "secret" } else if k.chosen { "[x]" } else { "[ ]" };
                    let def = k.default.as_ref().map(|d| format!(" (default {d})")).unwrap_or_default();
                    println!("  {mark} {} = {}{def}{}", k.key, k.value, if k.personal { " (personal)" } else { "" });
                }
            }
            Ok(0)
        }
        PackCmd::Lint { dir, catalog, json } => {
            let dirs = data_dirs()?;
            let r = studio::layer_lint(&dirs, &dir, catalog).km()?;
            if json { print_json(&r)?; } else {
                for d in &r.diagnostics { println!("{}:{}:{} {:?} {} {}", d.file, d.line, d.col, d.severity, d.code, d.message); }
                println!("lint: {} error(s), {} warning(s)", r.summary.errors, r.summary.warnings);
            }
            Ok(if r.summary.errors > 0 { 1 } else { 0 })
        }
        PackCmd::Credits { dir, set, accept, json } => {
            let dirs = data_dirs()?;
            let mut st = studio::open(&dirs, &dir).km()?;
            let report = studio::analyze(&dirs, &dir, &mut kiln_manage::task::NoProgress).km()?;
            let mut credits = st.credits.clone();
            if accept {
                for c in &report.credits {
                    if let (Some(a), Some(s)) = (&c.author, &c.source) && !credits.contains_key(&c.id) {
                        credits.insert(c.id.clone(), pack::Credit { author: a.clone(), source: s.clone(), match_kind: Some(c.match_kind.clone()) });
                    }
                }
            }
            for s in &set {
                let (id, rest) = s.split_once('=').ok_or_else(|| anyhow::anyhow!("{s:?}: ID=AUTHOR,URL"))?;
                let (author, url) = rest.rsplit_once(',').ok_or_else(|| anyhow::anyhow!("{s:?}: ID=AUTHOR,URL"))?;
                credits.insert(id.to_string(), pack::Credit { author: author.trim().into(), source: url.trim().into(), match_kind: Some("manual".into()) });
            }
            if accept || !set.is_empty() {
                st = studio::set_credits(&dirs, &dir, &credits).km()?;
            }
            if json { return print_json(&serde_json::json!({ "credits": st.credits, "identified": report.credits, "needs": studio::public_needs(&st) })); }
            for m in &st.mods {
                let found = report.credits.iter().find(|c| c.id == m.id);
                match &m.credit {
                    Some(c) => println!("  {:<24} {} {} ({})", m.id, c.author, c.source, c.match_kind.as_deref().unwrap_or("manual")),
                    None => match found.and_then(|c| c.author.as_ref().map(|a| (a, c))) {
                        Some((a, c)) => println!("  {:<24} ? suggested: {a} {} ({}, {}) - kilnctl pack credits --accept", m.id, c.source.as_deref().unwrap_or("-"), c.match_kind, c.confidence),
                        None => println!("  {:<24} ? not identified: --set {}=AUTHOR,https://...", m.id, m.id),
                    },
                }
            }
            let needs = studio::public_needs(&st);
            println!("{}", if needs.is_empty() { "public: ready".to_string() } else { format!("public needs: {}", needs.join("; ")) });
            Ok(0)
        }
        PackCmd::Publish { dir, version, changes, public, yes, session_file, json } => publish(dir, version, changes, public, yes, session_file, json),
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
        PackCmd::Update { profile, file, sha256, resolve, all, optional, yes, json } => {
            if !yes { bail!("a pack runs code from other people on this PC: add --yes if you trust it"); }
            let dirs = data_dirs()?;
            let mut res = BTreeMap::new();
            for r in &resolve {
                let Some((k, p)) = r.rsplit_once('=') else { bail!("{r:?}: PATH#KEY=pack|mine") };
                res.insert(k.to_string(), pick(p)?);
            }
            let req = UpdateRequest { profile, source: ModpackSource::Path { path: file, sha256 }, consent: true, allow_http: false,
                optional: switches(&optional, false)?, resolve: res, all: all.as_deref().map(pick).transpose()? };
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

#[cfg(feature = "crucible")]
fn publish(dir: PathBuf, version: Option<String>, changes: Option<String>, public: bool, yes: bool, session_file: Option<PathBuf>, json: bool) -> Result<i32> {
    use kiln_manage::crucible::{Crucible, Endpoints, publish::{self, PublishRequest}};
    let dirs = data_dirs()?;
    let session = match session_file {
        Some(p) => std::fs::read_to_string(&p).map_err(|e| anyhow::anyhow!("{}: {e}", p.display()))?.trim().to_string(),
        None => std::env::var("KILN_CRUCIBLE_SESSION").map_err(|_| anyhow::anyhow!("publishing needs a Crucible session: KILN_CRUCIBLE_SESSION or --session-file"))?,
    };
    let (ep, transport): (Endpoints, Box<dyn kiln_manage::net::Transport>) = match std::env::var("KILN_CRUCIBLE_DEV") {
        // a local service (debug builds only, as the launcher)
        Ok(spec) if cfg!(debug_assertions) && !spec.trim().is_empty() => {
            let (ep, t) = kiln_manage::crucible::dev::from_spec(&spec).km()?;
            (ep, Box::new(t))
        }
        _ => (Endpoints::production(), Box::new(kiln_manage::net::UreqTransport::new())),
    };
    if !ep.enabled() {
        bail!("Crucible is not available in this version of Kiln yet");
    }
    let s = studio::read_source(&dir).km()?;
    let req = PublishRequest {
        version: version.unwrap_or(s.version),
        changes,
        visibility: if public { studio::Visibility::Public } else { studio::Visibility::Link },
        attest: yes,
        game_builds: Vec::new(),
        fresh: false,
    };
    let c = Crucible::new(&ep, transport.as_ref(), env!("CARGO_PKG_VERSION")).with_session(Some(session));
    let r = if json { publish::publish(&dirs, &c, &dir, &req, None, &mut kiln_manage::task::NoProgress) } else { publish::publish(&dirs, &c, &dir, &req, None, &mut Printer::new()) }.km()?;
    if json { return print_json(&r); }
    println!("published {} {} ({}): {} code {}", r.pack.title, r.version.version, if r.public { "public" } else { "link only" }, r.pack.url, r.pack.code);
    println!("  {} file(s), {} bytes sent, {} bytes Crucible had", r.files, r.uploaded, r.skipped);
    for n in &r.needs { println!("  before it can be public: {n}"); }
    Ok(0)
}

#[cfg(not(feature = "crucible"))]
fn publish(_dir: PathBuf, _version: Option<String>, _changes: Option<String>, _public: bool, _yes: bool, _session_file: Option<PathBuf>, _json: bool) -> Result<i32> {
    bail!("this kilnctl was built without Crucible (feature crucible)")
}
