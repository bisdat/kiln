// SPDX-License-Identifier: MIT
//! Launch-plan v1 structural/recorded-closure validator, with Kiln's own overrides.
//! This is not the package planner, a hash revalidation service, admission seal,
//! or runtime starter. SemVer ranges are retained; sfi-core resolves them. Engine
//! availability is dynamic and never substituted for loader-level admission.
use crate::same_folder;
use crate::strict_json::{self, hash, object, text};
use serde_json::Value;
use std::{
    collections::{BTreeMap, BTreeSet},
    io::Read,
    path::Path,
};
const MAX_PLAN: usize = 8 * 1024 * 1024;
/// `game.archiveBaseOrigin`: where the archive BASE the plan pins (`archiveBase_sha256`) came from, as
/// kiln-eml's read-only inspector classified it. Optional for a reader (a plan written before the producer had
/// it says nothing), closed when present: one of these tokens, never "vanilla" (integrity is not provenance).
pub const ARCHIVE_BASE_ORIGINS: [&str; 3] = [
    "first-managed-unclassified",
    "rebaselined-unclassified",
    "unrecorded",
];

pub struct ValidationContext {
    pub exe_sha256: String,
    pub side: String,
    pub upstream_180_final: bool,
}
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Permission {
    Held,
    OtherBackend,
    EligiblePendingAdmission,
}
#[derive(Debug)]
pub struct LaunchPlan {
    id: [u8; 16],
    selected: String,
    document: Value,
}
impl LaunchPlan {
    pub fn read(path: &Path, context: &ValidationContext) -> Result<Self, String> {
        if !std::fs::symlink_metadata(path)
            .map_err(|e| e.to_string())?
            .file_type()
            .is_file()
        {
            return Err("plan is not a regular file".into());
        }
        let mut bytes = Vec::new();
        std::fs::File::open(path)
            .map_err(|e| e.to_string())?
            .take(MAX_PLAN as u64 + 1)
            .read_to_end(&mut bytes)
            .map_err(|e| e.to_string())?;
        Self::parse(&bytes, context)
    }
    pub fn parse(bytes: &[u8], context: &ValidationContext) -> Result<Self, String> {
        let value = strict_json::parse(bytes, MAX_PLAN)?;
        let top = object(
            &value,
            &[
                "format",
                "minReader",
                "plan_id",
                "launch_id",
                "startupDialect",
                "writer",
                "profile",
                "game",
                "backend",
                "contract",
                "fingerprints",
                "packages",
                "loadOrder",
                "warnings",
            ],
        )?;
        if top["format"].as_u64() != Some(1) || top["minReader"].as_u64() != Some(1) {
            return Err("unsupported launch plan version/minReader".into());
        }
        let id = uuid(text(&top["plan_id"])?)?;
        if !top["launch_id"].is_null() {
            return Err("launcher plan must not contain a stamped launch UUID".into());
        }
        let dialect = choice(&top["startupDialect"], &["shroudforge", "eml", "none"])?;
        let writer = object(&top["writer"], &["component", "version"])?;
        if text(&writer["component"])? != "kiln-manage" {
            return Err("wrong plan writer".into());
        }
        text(&writer["version"])?;
        let profile = object(
            &top["profile"],
            &["id", "generation", "mode", "semantic_sha256"],
        )?;
        text(&profile["id"])?;
        number(&profile["generation"], 0, i64::MAX as u64)?;
        choice(&profile["mode"], &["active", "once"])?;
        hash(&profile["semantic_sha256"])?;
        let game = top["game"].as_object().ok_or("expected JSON object")?;
        const GAME: [&str; 4] = [
            "side",
            "exe_sha256",
            "archiveBase_sha256",
            "archiveInput_sha256",
        ];
        if GAME.iter().any(|key| !game.contains_key(*key))
            || game
                .keys()
                .any(|key| !GAME.contains(&key.as_str()) && key != "archiveBaseOrigin")
        {
            return Err(format!(
                "missing or unknown fields; expected {} and optionally archiveBaseOrigin",
                GAME.join(",")
            ));
        }
        if let Some(origin) = game.get("archiveBaseOrigin") {
            choice(origin, &ARCHIVE_BASE_ORIGINS)?;
        }
        let side = choice(&game["side"], &["client", "server"])?;
        if side != context.side || hash(&game["exe_sha256"])? != context.exe_sha256 {
            return Err("plan executable hash or side mismatch".into());
        }
        hash(&game["archiveBase_sha256"])?;
        hash(&game["archiveInput_sha256"])?;
        let backend = object(
            &top["backend"],
            &[
                "requested",
                "selected",
                "fallbackOrder",
                "dataWriter",
                "selectionReason",
            ],
        )?;
        let requested = choice(&backend["requested"], &["kiln", "hosted", "installed"])?;
        let selected = choice(
            &backend["selected"],
            &["kiln", "hosted", "installed", "external", "none"],
        )?;
        let fallback = strings(&backend["fallbackOrder"])?;
        if fallback.len() > 2
            || fallback
                .iter()
                .any(|s| !["kiln", "hosted", "installed"].contains(s))
        {
            return Err("invalid fallback order".into());
        }
        if selected != requested
            && !["external", "none"].contains(&selected)
            && !fallback.contains(&selected)
        {
            return Err("selected backend not in frozen fallback order".into());
        }
        let data_writer = choice(
            &backend["dataWriter"],
            &["kiln-eml", "shroudforge", "external", "none"],
        )?;
        if data_writer
            != match selected {
                "kiln" => "kiln-eml",
                "hosted" | "installed" => "shroudforge",
                "external" => "external",
                _ => "none",
            }
        {
            return Err("physical data writer does not match selected backend".into());
        }
        let selection_reason = backend["selectionReason"]
            .as_str()
            .ok_or("selectionReason must be a string")?;
        if selection_reason.contains('\0') {
            return Err("NUL in selection reason".into());
        }
        let contract = object(
            &top["contract"],
            &[
                "apiVersion",
                "sourceCommit",
                "upstream180Final",
                "inventory_sha256",
                "parity_sha256",
                "serviceContracts_sha256",
            ],
        )?;
        text(&contract["apiVersion"])?;
        if !hex(text(&contract["sourceCommit"])?, 40) {
            return Err("invalid contract source commit".into());
        }
        let declares_final = boolean(&contract["upstream180Final"])?;
        for key in [
            "inventory_sha256",
            "parity_sha256",
            "serviceContracts_sha256",
        ] {
            hash(&contract[key])?;
        }
        let fingerprints = object(
            &top["fingerprints"],
            &[
                "packages_sha256",
                "settings_sha256",
                "chain_sha256",
                "loaderInventory_sha256",
                "compat_sha256",
                "releasePolicy_sha256",
                "placementReceipts_sha256",
            ],
        )?;
        for v in fingerprints.values() {
            hash(v)?;
        }
        reasons(&top["warnings"])?;
        let packages = array(&top["packages"])?;
        if packages.len() > 4096 {
            return Err("too many plan packages".into());
        }
        let mut ids = BTreeMap::new();
        let mut roots: BTreeSet<String> = BTreeSet::new();
        let mut resolved_dlls = BTreeSet::new();
        let mut candidates = BTreeSet::new();
        let mut total_records = 0usize;
        let mut needs_sf = false;
        let mut has_eml = false;
        for package in packages {
            let p = object(
                package,
                &[
                    "id",
                    "version",
                    "kind",
                    "root",
                    "content_sha256",
                    "config_sha256",
                    "enabled",
                    "targets",
                    "capabilities",
                    "dependencies",
                    "requiredApis",
                    "services",
                    "requires180",
                    "saveSensitive",
                    "decision",
                    "reasons",
                    "data",
                    "lua",
                    "dlls",
                ],
            )?;
            let id = text(&p["id"])?;
            if id == "shroudforge-api" || ids.insert(id, p).is_some() {
                return Err("duplicate or virtual package id".into());
            }
            text(&p["version"])?;
            let kind = choice(&p["kind"], &["shroudforge", "eml", "native-selection"])?;
            let root = path(&p["root"])?;
            if !root.starts_with("mods/") || root.split('/').count() != 2 {
                return Err("package root must be game-relative mods/<single folder>".into());
            }
            // character by character as Windows folds names; check_folders also knows an 8.3 name or a link
            let folded_root = same_folder::name_key(root);
            if roots.iter().any(|other| {
                folded_root
                    .strip_prefix(other)
                    .is_some_and(|tail| tail.starts_with('/'))
                    || other
                        .strip_prefix(&folded_root)
                        .is_some_and(|tail| tail.starts_with('/'))
            }) {
                return Err("overlapping package roots".into());
            }
            if !roots.insert(folded_root) {
                return Err("case-colliding package roots".into());
            }
            hash(&p["content_sha256"])?;
            hash(&p["config_sha256"])?;
            let enabled = boolean(&p["enabled"])?;
            let targets = strings(&p["targets"])?;
            if targets.is_empty() || targets.iter().any(|t| !["client", "server"].contains(t)) {
                return Err("invalid package targets".into());
            }
            let capabilities = strings(&p["capabilities"])?;
            strings(&p["requiredApis"])?;
            boolean(&p["saveSensitive"])?;
            let requires180 = boolean(&p["requires180"])?;
            let decision = choice(
                &p["decision"],
                &["candidate", "skip", "disabled", "other-side"],
            )?;
            let why = reasons(&p["reasons"])?;
            if decision == "candidate" {
                if !enabled || !targets.contains(&side) || selected == "none" {
                    return Err("candidate is disabled, wrong-side, or has no backend".into());
                }
                if requires180 && !(declares_final && context.upstream_180_final) {
                    return Err("1.8 package is held by independent upstream release policy".into());
                }
                candidates.insert(id);
            }
            if decision == "skip" && why == 0 {
                return Err("skipped package has no actionable reason".into());
            }
            if decision == "disabled" && enabled {
                return Err("disabled decision contradicts enabled package".into());
            }
            if decision == "other-side" && targets.contains(&side) {
                return Err("other-side decision targets current side".into());
            }
            part(&p["data"], decision == "candidate")?;
            part(&p["lua"], decision == "candidate")?;
            // Both SF pins' mod_manifest_types::requires_pregame make patch an
            // unconditional startup-data requirement. Other parts are not guessed
            // from filenames or absence of capabilities; data-only is valid.
            if decision == "candidate"
                && capabilities.contains(&"patch")
                && p["data"]["required"].as_bool() != Some(true)
            {
                return Err("patch candidate must require admitted startup data".into());
            }
            if enabled && targets.contains(&side) {
                needs_sf |= kind == "shroudforge" && capabilities.contains(&"patch")
                    || kind == "eml"
                        && capabilities
                            .iter()
                            .any(|c| ["runtime", "runtime-register-dll"].contains(c));
                has_eml |= kind == "eml";
            }
            let deps = array(&p["dependencies"])?;
            let services = array(&p["services"])?;
            let dlls = array(&p["dlls"])?;
            total_records = total_records
                .checked_add(deps.len() + services.len() + dlls.len())
                .ok_or("record budget overflow")?;
            if total_records > 16384 {
                return Err("plan DLL/dependency/service record budget exceeded".into());
            }
            let mut dep_ids = BTreeSet::new();
            for dep in deps {
                let dep = object(dep, &["id", "range", "optional", "included"])?;
                let target = text(&dep["id"])?;
                text(&dep["range"])?;
                let optional = boolean(&dep["optional"])?;
                let included = boolean(&dep["included"])?;
                if !dep_ids.insert(target) || target == id {
                    return Err("duplicate/self dependency".into());
                }
                if decision == "candidate" && !optional && !included {
                    return Err("candidate excludes a required dependency".into());
                }
            }
            let mut service_names = BTreeSet::new();
            for service in services {
                let service = object(
                    service,
                    &["name", "minVersion", "minSize", "slots", "capabilities"],
                )?;
                if !service_names.insert(text(&service["name"])?) {
                    return Err("duplicate service requirement".into());
                }
                number(&service["minVersion"], 1, u32::MAX as u64)?;
                number(&service["minSize"], 8, u32::MAX as u64)?;
                strings(&service["slots"])?;
                strings(&service["capabilities"])?;
            }
            let mut paths = BTreeSet::new();
            for dll in dlls {
                let dll = object(
                    dll,
                    &[
                        "path",
                        "sha256",
                        "mode",
                        "source",
                        "required",
                        "gate",
                        "gateProof_sha256",
                        "bridgeAbi",
                    ],
                )?;
                let dll_path = path(&dll["path"])?;
                if !paths.insert(same_folder::name_key(dll_path))
                    || !resolved_dlls.insert(same_folder::name_key(&format!("{root}/{dll_path}")))
                {
                    return Err("case-colliding resolved DLL paths across packages".into());
                }
                hash(&dll["sha256"])?;
                hash(&dll["gateProof_sha256"])?;
                choice(&dll["mode"], &["library", "dinput8"])?;
                choice(
                    &dll["source"],
                    &["ini", "profile", "selection", "lua-registration", "chain"],
                )?;
                if !boolean(&dll["required"])? {
                    return Err("listed DLL must be required".into());
                }
                let gate = choice(&dll["gate"], &["pass", "refuse", "unknown"])?;
                if decision == "candidate" && gate != "pass" {
                    return Err("candidate DLL lacks gate pass".into());
                }
                let bridge = number(&dll["bridgeAbi"], 0, 2)?;
                if bridge == 1 || (bridge == 2 && !requires180) {
                    return Err("unknown or undeclared 1.8 bridge ABI".into());
                }
            }
        }
        let order = strings(&top["loadOrder"])?;
        if order.len() != candidates.len() || order.iter().any(|id| !candidates.contains(id)) {
            return Err("loadOrder must contain every candidate exactly once".into());
        }
        let positions: BTreeMap<_, _> = order.iter().enumerate().map(|(i, id)| (*id, i)).collect();
        for id in &order {
            let p = ids[id];
            for dep in array(&p["dependencies"])? {
                let dep = dep.as_object().ok_or("invalid dependency object")?;
                let target = text(&dep["id"])?;
                if target == "shroudforge-api" {
                    continue;
                }
                let included = dep["included"].as_bool() == Some(true);
                // SF plan_mod_load_order.rs:253-288 in both pins excludes an
                // optional edge only when the target is absent or disabled.
                if !included
                    && dep["optional"].as_bool() == Some(true)
                    && ids
                        .get(target)
                        .is_some_and(|target| target["enabled"].as_bool() == Some(true))
                {
                    return Err("enabled optional dependency cannot be excluded".into());
                }
                if included {
                    let target_at = positions
                        .get(target)
                        .ok_or("included dependency is not an admitted candidate")?;
                    if target_at >= positions.get(id).ok_or("candidate missing position")? {
                        return Err("dependency order/cycle violation".into());
                    }
                }
            }
        }
        let expected_dialect = if selected == "none" {
            "none"
        } else if needs_sf {
            "shroudforge"
        } else if has_eml {
            "eml"
        } else {
            "none"
        };
        if dialect != expected_dialect {
            return Err("startupDialect contradicts frozen enabled package dialect".into());
        }
        Ok(Self {
            id,
            selected: selected.into(),
            document: value,
        })
    }
    pub fn plan_id(&self) -> [u8; 16] {
        self.id
    }
    pub fn document(&self) -> &Value {
        &self.document
    }
    pub fn permission(&self, capability_allows: bool) -> Permission {
        if self.selected != "kiln" {
            Permission::OtherBackend
        } else if !capability_allows {
            Permission::Held
        } else {
            Permission::EligiblePendingAdmission
        }
    }
    pub const fn runtime_started(&self) -> bool {
        false
    }
    /// Refuses two packages whose folders below `<game>/mods` are one folder under two names, as [`Self::parse`] refuses
    /// two roots whose names fold to one: an 8.3 short name, a link or a junction is another name of the same folder
    /// that no string comparison sees. Each package folder is compared by identity (volume and file number, links
    /// followed); one that does not exist is checked by name only (by `parse`). One that exists but cannot be identified
    /// refuses the plan.
    pub fn check_folders(&self, game: &Path) -> Result<(), String> {
        let mut seen: BTreeMap<same_folder::Identity, &str> = BTreeMap::new();
        for package in self.packages() {
            let folder = game.join("mods").join(package.folder());
            match same_folder::identity(&folder) {
                Ok(Some(id)) => {
                    if let Some(other) = seen.insert(id, package.id) {
                        return Err(format!(
                            "overlapping package roots: {} and {} are one folder",
                            other, package.id
                        ));
                    }
                }
                Ok(None) => {}
                Err(e) => {
                    return Err(format!(
                        "package root {} cannot be identified ({e})",
                        package.root
                    ));
                }
            }
        }
        Ok(())
    }
}
/// The startup dialect a plan froze (`startupDialect`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Dialect {
    /// Kiln's ShroudForge startup pass applies the admitted packages' startup data.
    Shroudforge,
    /// kiln-eml's EML pass applies it.
    Eml,
    /// No startup pass this start.
    None,
}
/// A package's `kind`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Kind {
    Shroudforge,
    Eml,
    NativeSelection,
}
/// A package's `decision`. Only a candidate can run; the plan lists the others so that
/// nothing brings them back.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Decision {
    Candidate,
    Skip,
    Disabled,
    OtherSide,
}
/// A part's `admission`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PartAdmission {
    Admit,
    Refuse,
    NotNeeded,
}
/// A package's `data` or `lua` part.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Part {
    pub required: bool,
    pub admission: PartAdmission,
}
/// One `dependencies[]` record.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Dependency<'a> {
    pub id: &'a str,
    pub range: &'a str,
    pub optional: bool,
    /// The target is an admitted candidate ordered before this package.
    pub included: bool,
}
/// A reason's `severity`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Severity {
    Info,
    Warning,
    SaveWarning,
    InfrastructureError,
}
/// One `reasons[]` or `warnings[]` record.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Reason<'a> {
    pub code: &'a str,
    pub message: &'a str,
    pub caused_by: Vec<&'a str>,
    pub severity: Severity,
}
/// One package of a plan [`LaunchPlan::parse`] accepted: a typed view over the checked
/// document, nothing re-decided.
#[derive(Debug, Clone)]
pub struct PlanPackage<'a> {
    pub id: &'a str,
    pub version: &'a str,
    pub kind: Kind,
    /// Game-relative `mods/<one folder>`.
    pub root: &'a str,
    /// [`crate::digest::package_content_sha256`] of the package folder when the plan was written.
    pub content_sha256: [u8; 32],
    pub enabled: bool,
    pub targets: Vec<&'a str>,
    pub decision: Decision,
    pub reasons: Vec<Reason<'a>>,
    pub data: Part,
    pub lua: Part,
    /// The planned DLL records, as checked (the native manager's plan reads them).
    pub dlls: &'a [Value],
    pub dependencies: Vec<Dependency<'a>>,
    pub capabilities: Vec<&'a str>,
    pub save_sensitive: bool,
}
impl<'a> PlanPackage<'a> {
    /// The folder below `mods/` (the root is exactly `mods/<one folder>`).
    pub fn folder(&self) -> &'a str {
        self.root.strip_prefix("mods/").unwrap_or(self.root)
    }
    /// It ships native code: a planned DLL, or the capability to register one from Lua.
    pub fn has_native(&self) -> bool {
        !self.dlls.is_empty() || self.capabilities.contains(&"runtime-register-dll")
    }
}
impl LaunchPlan {
    /// Every package in the plan's order of listing.
    pub fn packages(&self) -> impl Iterator<Item = PlanPackage<'_>> {
        self.document["packages"]
            .as_array()
            .map(Vec::as_slice)
            .unwrap_or_default()
            .iter()
            .map(view)
    }
    /// The package with manifest id `id`, if the plan lists it.
    pub fn package(&self, id: &str) -> Option<PlanPackage<'_>> {
        self.packages().find(|p| p.id == id)
    }
    /// `loadOrder`: every candidate once, dependencies first.
    pub fn load_order(&self) -> Vec<&str> {
        str_list(&self.document["loadOrder"])
    }
    pub fn startup_dialect(&self) -> Dialect {
        match self.document["startupDialect"].as_str() {
            Some("shroudforge") => Dialect::Shroudforge,
            Some("eml") => Dialect::Eml,
            _ => Dialect::None,
        }
    }
    /// The plan's own `warnings` (the launcher's pre-Play findings, such as a save-sensitive
    /// package switched off since the last activation).
    pub fn warnings(&self) -> Vec<Reason<'_>> {
        reason_list(&self.document["warnings"])
    }
    /// `game.side`: `client` or `server` (the side [`ValidationContext`] required).
    pub fn game_side(&self) -> &str {
        self.document["game"]["side"].as_str().unwrap_or_default()
    }
    /// `game.archiveBaseOrigin`, one of [`ARCHIVE_BASE_ORIGINS`]; None when the plan does not say.
    pub fn archive_base_origin(&self) -> Option<&str> {
        self.document["game"]["archiveBaseOrigin"].as_str()
    }
}
// The view reads a document parse() already checked member by member; the fallbacks below are
// never taken for such a document and only keep the view free of panics.
fn view(p: &Value) -> PlanPackage<'_> {
    let part = |v: &Value| Part {
        required: v["required"].as_bool().unwrap_or(false),
        admission: match v["admission"].as_str() {
            Some("admit") => PartAdmission::Admit,
            Some("refuse") => PartAdmission::Refuse,
            _ => PartAdmission::NotNeeded,
        },
    };
    let mut content_sha256 = [0u8; 32];
    if let Some(text) = p["content_sha256"].as_str().filter(|t| hex(t, 64)) {
        for (i, byte) in content_sha256.iter_mut().enumerate() {
            *byte = u8::from_str_radix(&text[i * 2..i * 2 + 2], 16).unwrap_or(0);
        }
    }
    PlanPackage {
        id: p["id"].as_str().unwrap_or_default(),
        version: p["version"].as_str().unwrap_or_default(),
        kind: match p["kind"].as_str() {
            Some("eml") => Kind::Eml,
            Some("native-selection") => Kind::NativeSelection,
            _ => Kind::Shroudforge,
        },
        root: p["root"].as_str().unwrap_or_default(),
        content_sha256,
        enabled: p["enabled"].as_bool().unwrap_or(false),
        targets: str_list(&p["targets"]),
        decision: match p["decision"].as_str() {
            Some("candidate") => Decision::Candidate,
            Some("disabled") => Decision::Disabled,
            Some("other-side") => Decision::OtherSide,
            _ => Decision::Skip,
        },
        reasons: reason_list(&p["reasons"]),
        data: part(&p["data"]),
        lua: part(&p["lua"]),
        dlls: p["dlls"].as_array().map(Vec::as_slice).unwrap_or_default(),
        dependencies: p["dependencies"]
            .as_array()
            .map(Vec::as_slice)
            .unwrap_or_default()
            .iter()
            .map(|d| Dependency {
                id: d["id"].as_str().unwrap_or_default(),
                range: d["range"].as_str().unwrap_or_default(),
                optional: d["optional"].as_bool().unwrap_or(false),
                included: d["included"].as_bool().unwrap_or(false),
            })
            .collect(),
        capabilities: str_list(&p["capabilities"]),
        save_sensitive: p["saveSensitive"].as_bool().unwrap_or(false),
    }
}
fn str_list(v: &Value) -> Vec<&str> {
    v.as_array()
        .map(Vec::as_slice)
        .unwrap_or_default()
        .iter()
        .filter_map(Value::as_str)
        .collect()
}
fn reason_list(v: &Value) -> Vec<Reason<'_>> {
    v.as_array()
        .map(Vec::as_slice)
        .unwrap_or_default()
        .iter()
        .map(|r| Reason {
            code: r["code"].as_str().unwrap_or_default(),
            message: r["message"].as_str().unwrap_or_default(),
            caused_by: str_list(&r["causedBy"]),
            severity: match r["severity"].as_str() {
                Some("warning") => Severity::Warning,
                Some("save-warning") => Severity::SaveWarning,
                Some("infrastructure-error") => Severity::InfrastructureError,
                _ => Severity::Info,
            },
        })
        .collect()
}
fn hex(s: &str, n: usize) -> bool {
    s.len() == n
        && s.bytes()
            .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}
fn uuid(s: &str) -> Result<[u8; 16], String> {
    if s.len() != 36 || [8, 13, 18, 23].iter().any(|i| s.as_bytes()[*i] != b'-') {
        return Err("invalid canonical plan UUID".into());
    }
    let compact = s.replace('-', "");
    if !hex(&compact, 32) {
        return Err("invalid UUID digits".into());
    }
    let mut out = [0; 16];
    for (i, b) in out.iter_mut().enumerate() {
        *b = u8::from_str_radix(&compact[i * 2..i * 2 + 2], 16).map_err(|e| e.to_string())?;
    }
    Ok(out)
}
fn number(v: &Value, min: u64, max: u64) -> Result<u64, String> {
    let n = v.as_u64().ok_or("expected nonnegative integer")?;
    if n < min || n > max {
        Err("integer out of field range".into())
    } else {
        Ok(n)
    }
}
fn boolean(v: &Value) -> Result<bool, String> {
    v.as_bool().ok_or_else(|| "expected boolean".into())
}
fn choice<'a>(v: &'a Value, choices: &[&str]) -> Result<&'a str, String> {
    let s = text(v)?;
    if !choices.contains(&s) {
        Err(format!("unknown enum value: {s}"))
    } else {
        Ok(s)
    }
}
fn array(v: &Value) -> Result<&Vec<Value>, String> {
    let a = v.as_array().ok_or("expected array")?;
    if a.len() > 16384 {
        return Err("array budget exceeded".into());
    }
    Ok(a)
}
fn strings(v: &Value) -> Result<Vec<&str>, String> {
    let mut seen = BTreeSet::new();
    let mut out = Vec::new();
    for value in array(v)? {
        let s = text(value)?;
        if !seen.insert(s) {
            return Err("duplicate string array value".into());
        }
        out.push(s);
    }
    Ok(out)
}
fn reasons(v: &Value) -> Result<usize, String> {
    let a = array(v)?;
    for value in a {
        let r = object(value, &["code", "message", "causedBy", "severity"])?;
        text(&r["code"])?;
        text(&r["message"])?;
        strings(&r["causedBy"])?;
        choice(
            &r["severity"],
            &["info", "warning", "save-warning", "infrastructure-error"],
        )?;
    }
    Ok(a.len())
}
fn part(v: &Value, candidate: bool) -> Result<(), String> {
    let p = object(v, &["required", "admission"])?;
    let required = boolean(&p["required"])?;
    let admission = choice(&p["admission"], &["admit", "refuse", "not-needed"])?;
    if (candidate && admission != if required { "admit" } else { "not-needed" })
        || (!candidate && admission == "admit")
        || (!required && admission != "not-needed")
    {
        return Err("package part admission inconsistent".into());
    }
    Ok(())
}
fn path(v: &Value) -> Result<&str, String> {
    let s = text(v)?;
    if s.len() > 32768
        || s.contains(['\\', ':', '<', '>', '"', '|', '?', '*'])
        || s.chars().any(char::is_control)
    {
        return Err("invalid Windows-relative path".into());
    }
    for component in s.split('/') {
        let base = component
            .split('.')
            .next()
            .unwrap_or("")
            .to_ascii_uppercase();
        if component.is_empty()
            || component == "."
            || component == ".."
            || component.ends_with([' ', '.'])
            || ["CON", "PRN", "AUX", "NUL"].contains(&base.as_str())
            || (base.len() == 4
                && ["COM", "LPT"].iter().any(|prefix| base.starts_with(prefix))
                && matches!(base.as_bytes()[3], b'1'..=b'9'))
        {
            return Err("unsafe path component/device name".into());
        }
    }
    Ok(s)
}
