// SPDX-License-Identifier: MIT
//! The parity catalog side of the scan: which observed ShroudForge revision Kiln answers for (1.7.9, the
//! target; never chosen from the API version string, which is 1.0.0 in both pins), whether the catalog the caller
//! handed in is the one this build expects, and what it says about each scanned reference. Every answer here is a
//! warning: a missing, unreadable or stale catalog says "coverage unknown", never "ready", and a reference the catalog
//! marks partial, missing, absent or 1.8-only stays a warning even when the code surely runs.
use sfi_package::parity::{
    Catalog, ExpectedCatalog, Outcome, ParityStatus, Presence, RevisionId, Scope,
};
use std::collections::BTreeSet;

/// The revision every query uses: ShroudForge 1.7.9 at the fork pin. Nothing 1.8-only is planned.
pub const REVISION: RevisionId = RevisionId::Sf179;

/// API roots used when no verified catalog names them. Only for counting what is left unanswered.
pub const DEFAULT_ROOTS: [&str; 7] = [
    "shroudforge",
    "builtin",
    "extended",
    "runtime",
    "game",
    "loader",
    "integer",
];

/// The catalog the caller supplies: its bytes and the exact fingerprints this build expects.
pub enum CatalogInput<'a> {
    Missing,
    Provided {
        bytes: &'a [u8],
        expected: &'a ExpectedCatalog,
    },
}

#[derive(Clone, Debug, PartialEq, Eq, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Coverage {
    /// verified, missing, unreadable or stale.
    pub status: &'static str,
    /// sf179.
    pub revision: &'static str,
    pub digest: Option<String>,
    pub inventory_sha256: Option<String>,
    pub detail: Option<String>,
}

/// Reads and checks the catalog. Only a verified catalog answers questions.
pub fn load(input: &CatalogInput<'_>) -> (Option<Catalog>, Coverage) {
    let coverage = |status, detail: Option<String>| Coverage {
        status,
        revision: "sf179",
        digest: None,
        inventory_sha256: None,
        detail,
    };
    let CatalogInput::Provided { bytes, expected } = input else {
        return (
            None,
            coverage(
                "missing",
                Some("no parity catalog: API coverage is unknown".into()),
            ),
        );
    };
    let catalog = match Catalog::from_json(bytes) {
        Ok(c) => c,
        Err(e) => {
            return (
                None,
                coverage("unreadable", Some(format!("{e}: API coverage is unknown"))),
            );
        }
    };
    if let Err(e) = catalog.check_expected(expected) {
        let mut c = coverage("stale", Some(format!("{e}: API coverage is unknown")));
        c.digest = Some(catalog.digest().to_owned());
        return (None, c);
    }
    let inventory = catalog
        .header()
        .revisions
        .iter()
        .find(|r| r.id == REVISION)
        .map(|r| r.inventory_sha256.clone());
    let mut c = coverage("verified", None);
    c.digest = Some(catalog.digest().to_owned());
    c.inventory_sha256 = inventory;
    (Some(catalog), c)
}

/// The root names of the catalog's callable surface (root globals and namespace members), or [`DEFAULT_ROOTS`].
/// Type declarations, returned fields, userdata members and contract data never make a root.
pub fn roots(catalog: Option<&Catalog>) -> BTreeSet<String> {
    match catalog {
        Some(c) => c
            .symbols()
            .iter()
            .filter(|s| matches!(s.scope, Scope::RootGlobal | Scope::NamespaceMember))
            .map(|s| s.name.split('.').next().unwrap_or(&s.name).to_owned())
            .collect(),
        None => DEFAULT_ROOTS.iter().map(|s| (*s).to_owned()).collect(),
    }
}

/// What the catalog says about one reference, when it is worth a warning.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Answer {
    /// Known, present and Full: nothing to say.
    Supported,
    /// The name exists in the catalog but is not a global or namespace member (a type declaration, a returned field,
    /// a userdata member, contract data): a free global of that name is not that surface.
    NotAGlobal(Scope),
    /// Present in 1.7.9, Kiln's proof partial or missing.
    Unproven(ParityStatus),
    /// ShroudForge 1.7.9 does not have it (an absence obligation, even a Full one, is not a callable).
    Absent,
    /// Only in 1.8 (held).
    NotInRevision,
    /// Excluded from parity (n/a).
    Excluded,
    /// Not in the catalog.
    Unknown,
}

pub fn assess(catalog: &Catalog, chain: &str) -> Answer {
    let scope = if chain.contains('.') {
        Scope::NamespaceMember
    } else {
        Scope::RootGlobal
    };
    let answer = catalog.query(REVISION, chain, Some(scope));
    if answer.outcome == Outcome::Unknown && answer.via.is_empty() {
        let any = catalog.query(REVISION, chain, None);
        if let Some(found) = any.scope
            && !matches!(found, Scope::RootGlobal | Scope::NamespaceMember)
        {
            return Answer::NotAGlobal(found);
        }
    }
    match answer.outcome {
        Outcome::Known => match (answer.presence, answer.parity) {
            (
                Some(Presence::Callable | Presence::Value | Presence::Declaration),
                Some(ParityStatus::Full),
            ) => Answer::Supported,
            (_, Some(p)) => Answer::Unproven(p),
            _ => Answer::Unknown,
        },
        Outcome::Absent => Answer::Absent,
        Outcome::NotInRevision => Answer::NotInRevision,
        Outcome::Excluded => Answer::Excluded,
        Outcome::Unknown => Answer::Unknown,
    }
}
