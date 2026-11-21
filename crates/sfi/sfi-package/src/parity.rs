// SPDX-License-Identifier: MIT
//! Parity catalog format 1: a mod-facing, versioned artifact like an ABI.
//!
//! The catalog is emitted by the conformance proof run (example `export_parity`) from the live
//! evidence of one successful run. This module only
//! defines the DTO, its strict loader, canonical encoding/digest and a pure query API; it
//! never computes or upgrades a status, executes Lua or reads committed registry claims.
//! Schema: `schema/parity-catalog-v1.schema.json`. Changing format 1 requires an explicit
//! schema/consumer compatibility decision and a new `schema_version`.
use crate::package::revisions::revision as sha256_hex;
use serde::{Deserialize, Serialize};
use std::collections::{BTreeMap, BTreeSet};

pub const SCHEMA_VERSION: u32 = 1;
pub const FORMAT: &str = "kiln-sfi-parity-catalog";

/// Observed ShroudForge contract revisions. Never chosen from the API version string:
/// both pins report 1.0.0.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
pub enum RevisionId {
    #[serde(rename = "sf179")]
    Sf179,
    #[serde(rename = "sf180")]
    Sf180,
}
impl RevisionId {
    pub const ALL: [RevisionId; 2] = [RevisionId::Sf179, RevisionId::Sf180];
    pub fn source_version(self) -> &'static str {
        match self {
            Self::Sf179 => "1.7.9",
            Self::Sf180 => "1.8.0",
        }
    }
}

/// Where a public name lives. Functions, methods, fields and namespaces differ by
/// `SurfaceKind`; annotation-only types and contract data are never callable globals.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum Scope {
    RootGlobal,
    NamespaceMember,
    UserdataMember,
    ReturnedField,
    TypeDeclaration,
    ContractData,
}

/// Per-revision presence, distinct from parity. `NotInRevision` is not an oracle observation
/// of absence; `Absent` is (SF lacks the callable, so a Full absence obligation is not
/// support); `NA` is excluded and can never carry Full support.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum Presence {
    Callable,
    Value,
    Declaration,
    Absent,
    NotInRevision,
    #[serde(rename = "n/a")]
    NA,
    Unknown,
}

/// Conservative whole-surface parity over every required portion (all owners, overloads,
/// declarations, typed parts). `Excluded` pairs exactly with `Presence::NA`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum ParityStatus {
    Full,
    Partial,
    Missing,
    Excluded,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum AliasRelation {
    /// A FINAL delegate: semantics are the target's; no own obligations.
    Delegate,
    /// A namespace identity (`builtin.*`): the same table, not child behaviour.
    NamespaceIdentity,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ContractRevision {
    pub id: RevisionId,
    pub source_version: String,
    pub source_pin: String,
    pub inventory_sha256: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct FamilyDigest {
    pub family: String,
    pub sha256: String,
}

/// Opaque live-evidence attestation: digests and counts only, never assertion names,
/// paths or addresses.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct LiveEvidence {
    pub scope: String,
    pub modules: u32,
    pub runtime_modules: u32,
    pub assertions: u32,
    pub mappings: u32,
    pub sha256: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ObligationCounts {
    pub full: u32,
    pub partial: u32,
    pub missing: u32,
    #[serde(rename = "n/a")]
    pub na: u32,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct CatalogHeader {
    pub format: String,
    pub revisions: Vec<ContractRevision>,
    /// SHA-256 of the frozen API contract's manifest the snapshot derives from.
    pub final_manifest_sha256: String,
    pub frozen: Vec<FamilyDigest>,
    pub policy_sha256: String,
    pub registry_sha256: String,
    pub backend_build: String,
    pub evidence: LiveEvidence,
    /// Every model obligation accounted for exactly once: symbol-bound plus the rest
    /// (normative behaviour/policy rows, which are not public symbols).
    pub symbol_obligations: ObligationCounts,
    pub other_obligations: ObligationCounts,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SymbolRevision {
    pub revision: RevisionId,
    pub presence: Presence,
    pub parity: ParityStatus,
    /// Required obligations of this surface in this revision.
    pub required: u32,
    pub proven_full: u32,
    /// Obligation test IDs with executed Partial/Full proof in the exporting run.
    pub tests: Vec<String>,
    /// SHA-256 over this revision's (obligation, status) pairs from the live run.
    pub digest: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct PartRevision {
    pub revision: RevisionId,
    pub parity: ParityStatus,
    pub digest: String,
}

/// One owner's portion of a composite surface (e.g. table.insert's ordinary dispatch and
/// its typed-resource extension). The parent is Full only when every part is Full.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct CompositePart {
    pub id: String,
    pub owner: String,
    pub milestone: String,
    pub revisions: Vec<PartRevision>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct AliasOf {
    pub target: String,
    pub relation: AliasRelation,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Symbol {
    pub name: String,
    pub scope: Scope,
    pub kind: String,
    /// Only when provenance is known; the oldest observed pin is not an introduction.
    #[serde(deserialize_with = "required_nullable")]
    pub introduced: Option<String>,
    #[serde(deserialize_with = "required_nullable")]
    pub alias_of: Option<AliasOf>,
    /// Empty only for pure delegates, which resolve through `alias_of`.
    pub revisions: Vec<SymbolRevision>,
    pub composite: Vec<CompositePart>,
}

/// Format 1 requires nullable keys to be present (the schema lists them as required); with
/// `deserialize_with` serde reports a missing key instead of defaulting it to `None`.
fn required_nullable<'de, D, T>(deserializer: D) -> Result<Option<T>, D::Error>
where
    D: serde::Deserializer<'de>,
    T: Deserialize<'de>,
{
    Option::<T>::deserialize(deserializer)
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Body {
    schema_version: u32,
    header: CatalogHeader,
    symbols: Vec<Symbol>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Envelope {
    schema_version: u32,
    header: CatalogHeader,
    symbols: Vec<Symbol>,
    digest: String,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum CatalogError {
    Malformed(String),
    UnsupportedVersion(u64),
    Invalid(String),
    Digest { recorded: String, computed: String },
    Fingerprint(String),
}
impl std::fmt::Display for CatalogError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Malformed(m) => write!(f, "malformed parity catalog: {m}"),
            Self::UnsupportedVersion(v) => {
                write!(f, "unsupported parity catalog schema_version {v}")
            }
            Self::Invalid(m) => write!(f, "invalid parity catalog: {m}"),
            Self::Digest { recorded, computed } => {
                write!(
                    f,
                    "parity catalog digest mismatch: recorded {recorded}, computed {computed}"
                )
            }
            Self::Fingerprint(m) => {
                write!(f, "parity catalog does not match the expected build: {m}")
            }
        }
    }
}
impl std::error::Error for CatalogError {}

fn invalid<T>(message: impl Into<String>) -> Result<T, CatalogError> {
    Err(CatalogError::Invalid(message.into()))
}
fn is_sha256(value: &str) -> bool {
    value.len() == 64
        && value
            .bytes()
            .all(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f'))
}

/// A validated catalog. Construction only through [`Catalog::new`] or [`Catalog::from_json`],
/// both of which run every invariant below and bind the canonical digest.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Catalog {
    header: CatalogHeader,
    symbols: Vec<Symbol>,
    digest: String,
}

/// Exact fingerprints a consumer or stager requires; any difference refuses the catalog.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ExpectedCatalog {
    pub final_manifest_sha256: String,
    pub frozen: Vec<FamilyDigest>,
    pub inventories: Vec<(RevisionId, String)>,
    pub policy_sha256: String,
    pub registry_sha256: String,
    pub backend_build: String,
    /// When set (stage-time re-verification), the live evidence must match exactly too.
    pub evidence_sha256: Option<String>,
}

impl Catalog {
    /// Validate and seal a producer's catalog (canonical ordering is required, not fixed up).
    pub fn new(header: CatalogHeader, symbols: Vec<Symbol>) -> Result<Self, CatalogError> {
        let body = Body {
            schema_version: SCHEMA_VERSION,
            header,
            symbols,
        };
        validate(&body)?;
        let digest = sha256_hex(&canonical(&body));
        Ok(Self {
            header: body.header,
            symbols: body.symbols,
            digest,
        })
    }

    /// Strict loader: rejects malformed JSON, duplicate keys, unknown fields, unsupported
    /// versions, contradictions and digest mismatches. It never fills in a status.
    pub fn from_json(bytes: &[u8]) -> Result<Self, CatalogError> {
        let value = strict_value(bytes)?;
        match value
            .get("schema_version")
            .and_then(crate::json::Value::as_u64)
        {
            Some(1) => {}
            Some(other) => return Err(CatalogError::UnsupportedVersion(other)),
            None => {
                return Err(CatalogError::Malformed(
                    "missing integer schema_version".into(),
                ));
            }
        }
        // Fields in file order, so the first problem reported is the first in the file.
        let envelope: Envelope =
            crate::json::from_value(&value).map_err(CatalogError::Malformed)?;
        let body = Body {
            schema_version: envelope.schema_version,
            header: envelope.header,
            symbols: envelope.symbols,
        };
        validate(&body)?;
        let computed = sha256_hex(&canonical(&body));
        if computed != envelope.digest {
            return Err(CatalogError::Digest {
                recorded: envelope.digest,
                computed,
            });
        }
        Ok(Self {
            header: body.header,
            symbols: body.symbols,
            digest: computed,
        })
    }

    /// Pretty, deterministic file encoding. The digest covers the canonical compact body
    /// (everything except the digest field itself).
    pub fn to_json(&self) -> Vec<u8> {
        let envelope = Envelope {
            schema_version: SCHEMA_VERSION,
            header: self.header.clone(),
            symbols: self.symbols.clone(),
            digest: self.digest.clone(),
        };
        let mut bytes = serde_json::to_vec_pretty(&envelope).expect("catalog serializes");
        bytes.push(b'\n');
        bytes
    }

    pub fn header(&self) -> &CatalogHeader {
        &self.header
    }
    pub fn symbols(&self) -> &[Symbol] {
        &self.symbols
    }
    pub fn digest(&self) -> &str {
        &self.digest
    }

    pub fn check_expected(&self, expected: &ExpectedCatalog) -> Result<(), CatalogError> {
        let h = &self.header;
        let mismatch = |what: &str| Err(CatalogError::Fingerprint(what.into()));
        if h.final_manifest_sha256 != expected.final_manifest_sha256 {
            return mismatch("FINAL manifest");
        }
        if h.frozen != expected.frozen {
            return mismatch("frozen contract families");
        }
        let inventories: Vec<_> = h
            .revisions
            .iter()
            .map(|r| (r.id, r.inventory_sha256.clone()))
            .collect();
        if inventories != expected.inventories {
            return mismatch("source inventories");
        }
        if h.policy_sha256 != expected.policy_sha256 {
            return mismatch("policy");
        }
        if h.registry_sha256 != expected.registry_sha256 {
            return mismatch("registry");
        }
        if h.backend_build != expected.backend_build {
            return mismatch("backend build");
        }
        if let Some(evidence) = &expected.evidence_sha256
            && &h.evidence.sha256 != evidence
        {
            return mismatch("live evidence");
        }
        Ok(())
    }

    /// Longest proper dotted prefix of `name` that is a namespace identity.
    fn identity_prefix<'a>(&'a self, name: &'a str) -> Option<(&'a str, &'a str)> {
        name.rmatch_indices('.').find_map(|(index, _)| {
            let prefix = &name[..index];
            let alias = self.find(prefix)?.alias_of.as_ref()?;
            (alias.relation == AliasRelation::NamespaceIdentity)
                .then_some((prefix, alias.target.as_str()))
        })
    }

    fn find(&self, name: &str) -> Option<&Symbol> {
        self.symbols
            .binary_search_by(|s| s.name.as_str().cmp(name))
            .ok()
            .map(|i| &self.symbols[i])
    }

    /// Pure lookup for one reference in one observed revision. Delegates resolve to their
    /// authority; namespace identities are reported, never turned into child behaviour.
    /// Unknown stays unknown; nothing is executed.
    pub fn query(&self, revision: RevisionId, name: &str, scope: Option<Scope>) -> Answer {
        let mut via = Vec::new();
        let mut current = name.to_owned();
        loop {
            let Some(symbol) = self.find(&current) else {
                // An inherited suffix under a namespace identity (runtime.types.X ->
                // game.types.X): rename the prefix, then look the child up on its own.
                // The identity never lends the child a status it does not have.
                match self.identity_prefix(&current) {
                    Some((prefix, target)) if via.len() < self.symbols.len() => {
                        via.push((prefix.to_owned(), AliasRelation::NamespaceIdentity));
                        current = format!("{target}{}", &current[prefix.len()..]);
                        continue;
                    }
                    _ => return Answer::unknown(name, via),
                }
            };
            if let Some(expected) = scope
                && via.is_empty()
                && symbol.scope != expected
            {
                return Answer::unknown(name, via);
            }
            match (&symbol.alias_of, symbol.revisions.is_empty()) {
                (Some(alias), true) => {
                    via.push((symbol.name.clone(), alias.relation));
                    current = alias.target.clone();
                }
                _ => {
                    let entry = symbol
                        .revisions
                        .iter()
                        .find(|r| r.revision == revision)
                        .expect("validated: one entry per revision");
                    if let Some(alias) = &symbol.alias_of {
                        via.push((symbol.name.clone(), alias.relation));
                    }
                    return Answer {
                        reference: name.into(),
                        resolved: Some(symbol.name.clone()),
                        scope: Some(symbol.scope),
                        outcome: match entry.presence {
                            Presence::Callable | Presence::Value | Presence::Declaration => {
                                Outcome::Known
                            }
                            Presence::Absent => Outcome::Absent,
                            Presence::NotInRevision => Outcome::NotInRevision,
                            Presence::NA => Outcome::Excluded,
                            Presence::Unknown => Outcome::Unknown,
                        },
                        presence: Some(entry.presence),
                        parity: Some(entry.parity),
                        via,
                    };
                }
            }
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Outcome {
    Known,
    Absent,
    NotInRevision,
    Excluded,
    Unknown,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Answer {
    pub reference: String,
    pub resolved: Option<String>,
    pub scope: Option<Scope>,
    pub outcome: Outcome,
    pub presence: Option<Presence>,
    pub parity: Option<ParityStatus>,
    /// Alias hops taken, in order, with their relation.
    pub via: Vec<(String, AliasRelation)>,
}
impl Answer {
    fn unknown(name: &str, via: Vec<(String, AliasRelation)>) -> Self {
        Self {
            reference: name.into(),
            resolved: None,
            scope: None,
            outcome: Outcome::Unknown,
            presence: None,
            parity: None,
            via,
        }
    }
}

fn canonical(body: &Body) -> Vec<u8> {
    serde_json::to_vec(body).expect("catalog serializes")
}

/// serde_json's Value keeps the last of duplicate keys; refuse them instead.
fn strict_value(bytes: &[u8]) -> Result<crate::json::Value, CatalogError> {
    use crate::json::Value;
    struct Strict(Value);
    impl<'de> Deserialize<'de> for Strict {
        fn deserialize<D: serde::Deserializer<'de>>(d: D) -> Result<Self, D::Error> {
            struct V;
            impl<'de> serde::de::Visitor<'de> for V {
                type Value = Strict;
                fn expecting(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
                    f.write_str("JSON")
                }
                fn visit_bool<E>(self, v: bool) -> Result<Strict, E> {
                    Ok(Strict(v.into()))
                }
                fn visit_i64<E>(self, v: i64) -> Result<Strict, E> {
                    Ok(Strict(v.into()))
                }
                fn visit_u64<E>(self, v: u64) -> Result<Strict, E> {
                    Ok(Strict(v.into()))
                }
                fn visit_f64<E: serde::de::Error>(self, _: f64) -> Result<Strict, E> {
                    Err(E::custom("floating-point numbers are not part of format 1"))
                }
                fn visit_str<E>(self, v: &str) -> Result<Strict, E> {
                    Ok(Strict(v.into()))
                }
                fn visit_string<E>(self, v: String) -> Result<Strict, E> {
                    Ok(Strict(v.into()))
                }
                fn visit_unit<E>(self) -> Result<Strict, E> {
                    Ok(Strict(Value::Null))
                }
                fn visit_seq<A: serde::de::SeqAccess<'de>>(
                    self,
                    mut a: A,
                ) -> Result<Strict, A::Error> {
                    let mut items = Vec::new();
                    while let Some(Strict(v)) = a.next_element()? {
                        items.push(v);
                    }
                    Ok(Strict(items.into()))
                }
                fn visit_map<A: serde::de::MapAccess<'de>>(
                    self,
                    mut a: A,
                ) -> Result<Strict, A::Error> {
                    let mut map = crate::json::Map::new();
                    while let Some(key) = a.next_key::<String>()? {
                        let Strict(value) = a.next_value()?;
                        if map.insert(key.clone(), value).is_some() {
                            return Err(serde::de::Error::custom(format!("duplicate key {key:?}")));
                        }
                    }
                    Ok(Strict(map.into()))
                }
            }
            d.deserialize_any(V)
        }
    }
    serde_json::from_slice::<Strict>(bytes)
        .map(|s| s.0)
        .map_err(|e| CatalogError::Malformed(e.to_string()))
}

fn validate(body: &Body) -> Result<(), CatalogError> {
    if body.schema_version != SCHEMA_VERSION {
        return Err(CatalogError::UnsupportedVersion(body.schema_version.into()));
    }
    let h = &body.header;
    if h.format != FORMAT {
        return invalid(format!("format {:?}", h.format));
    }
    let ids: Vec<_> = h.revisions.iter().map(|r| r.id).collect();
    if ids != RevisionId::ALL {
        return invalid("revisions must be exactly sf179, sf180");
    }
    for r in &h.revisions {
        if r.source_version != r.id.source_version()
            || r.source_pin.len() != 40
            || !is_sha256(&r.inventory_sha256)
        {
            return invalid(format!("revision {:?} metadata", r.id));
        }
    }
    let mut families = BTreeSet::new();
    for f in &h.frozen {
        if !is_sha256(&f.sha256) || !families.insert(f.family.as_str()) {
            return invalid(format!("frozen family {}", f.family));
        }
    }
    if families.is_empty() || !h.frozen.windows(2).all(|w| w[0].family < w[1].family) {
        return invalid("frozen families must be non-empty and sorted");
    }
    for digest in [
        &h.final_manifest_sha256,
        &h.policy_sha256,
        &h.registry_sha256,
        &h.evidence.sha256,
    ] {
        if !is_sha256(digest) {
            return invalid("header digest is not lowercase SHA-256");
        }
    }
    if h.backend_build.is_empty() || h.evidence.scope != "aggregate" {
        return invalid("backend build and aggregate evidence scope are required");
    }
    let mut names = BTreeMap::new();
    for (index, symbol) in body.symbols.iter().enumerate() {
        if index > 0 && body.symbols[index - 1].name >= symbol.name {
            return invalid(format!(
                "symbols must be sorted and unique: {}",
                symbol.name
            ));
        }
        names.insert(symbol.name.as_str(), symbol);
    }
    for symbol in &body.symbols {
        validate_symbol(symbol)?;
    }
    // Alias targets exist, and every chain (delegates AND namespace identities) ends at an
    // unaliased symbol with own revisions: no cycle of either relation.
    for symbol in &body.symbols {
        let mut seen = BTreeSet::new();
        let mut current: &Symbol = symbol;
        while let Some(alias) = &current.alias_of {
            if !seen.insert(current.name.as_str()) {
                return invalid(format!("alias cycle through {}", symbol.name));
            }
            current = match names.get(alias.target.as_str()) {
                Some(target) => target,
                None => return invalid(format!("missing alias target {}", alias.target)),
            };
        }
        if current.revisions.is_empty() {
            return invalid(format!("alias chain without an authority: {}", symbol.name));
        }
    }
    Ok(())
}

fn validate_symbol(s: &Symbol) -> Result<(), CatalogError> {
    let name = &s.name;
    if name.is_empty() || s.kind.is_empty() {
        return invalid("symbol name and kind are required");
    }
    if let Some(introduced) = &s.introduced
        && !RevisionId::ALL
            .iter()
            .any(|r| r.source_version() == introduced)
    {
        return invalid(format!(
            "{name}: introduced must be an observed revision when known"
        ));
    }
    if s.revisions.is_empty() {
        if s.alias_of.as_ref().map(|a| a.relation) != Some(AliasRelation::Delegate)
            || !s.composite.is_empty()
        {
            return invalid(format!("{name}: only a pure delegate may omit revisions"));
        }
        return Ok(());
    }
    if s.alias_of.as_ref().is_some_and(|a| &a.target == name) {
        return invalid(format!("{name}: self alias"));
    }
    let order: Vec<_> = s.revisions.iter().map(|r| r.revision).collect();
    if order != RevisionId::ALL {
        return invalid(format!("{name}: one entry per observed revision, in order"));
    }
    for r in &s.revisions {
        let at = || format!("{name}@{:?}", r.revision);
        if !is_sha256(&r.digest) || r.proven_full > r.required {
            return invalid(format!("{}: counts/digest", at()));
        }
        if (r.presence == Presence::NA) != (r.parity == ParityStatus::Excluded) {
            return invalid(format!(
                "{}: n/a presence pairs exactly with excluded parity",
                at()
            ));
        }
        let consistent = match r.parity {
            ParityStatus::Full => {
                r.required > 0 && r.proven_full == r.required && !r.tests.is_empty()
            }
            ParityStatus::Partial => !r.tests.is_empty() && r.proven_full < r.required,
            ParityStatus::Missing => r.tests.is_empty() && r.proven_full == 0,
            ParityStatus::Excluded => r.tests.is_empty() && r.proven_full == 0,
        };
        if !consistent {
            return invalid(format!("{}: contradictory parity/test evidence", at()));
        }
        if r.presence == Presence::Unknown && r.parity != ParityStatus::Missing {
            return invalid(format!("{}: unknown presence cannot carry proof", at()));
        }
        if !r.tests.windows(2).all(|w| w[0] < w[1]) {
            return invalid(format!("{}: tests must be sorted and unique", at()));
        }
    }
    let mut parts = BTreeSet::new();
    for part in &s.composite {
        if !parts.insert(part.id.as_str())
            || part
                .revisions
                .iter()
                .map(|r| r.revision)
                .collect::<Vec<_>>()
                != RevisionId::ALL
            || part.revisions.iter().any(|r| !is_sha256(&r.digest))
        {
            return invalid(format!("{name}: composite part {}", part.id));
        }
    }
    if !s.composite.is_empty() && s.composite.len() < 2 {
        return invalid(format!("{name}: a composite needs every owner's part"));
    }
    for (index, r) in s.revisions.iter().enumerate() {
        if r.parity == ParityStatus::Full
            && s.composite
                .iter()
                .any(|p| p.revisions[index].parity != ParityStatus::Full)
        {
            return invalid(format!(
                "{name}@{:?}: composite Full needs every part Full",
                r.revision
            ));
        }
    }
    Ok(())
}
