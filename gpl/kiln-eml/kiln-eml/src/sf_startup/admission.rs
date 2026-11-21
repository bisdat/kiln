// SPDX-License-Identifier: GPL-3.0-or-later
//! kiln-eml: the admission set of a kiln-mode start's pass, decided once before the pass and never widened:
//! the plan's pass members (`PlanFacts::pass_members`), minus
//! - every one when kiln-sfi never attached to the seam (ShroudForge dialect: `backend-unavailable`): without the
//!   interpreter no ShroudForge Lua runs, so none of its data may either;
//! - the packages the native terminal withdrew (`native-refused`, with the terminal's text and chain), or, when kiln-sfi
//!   attached but no terminal arrived, every package with native code (`native-terminal-missing`);
//! - in the ShroudForge dialect, a package the discovery does not have as the plan lists it (not found, switched off,
//!   for the other side) and, in every dialect, one whose folder's content digest is no longer the plan's
//!   (`package-changed`);
//! - every package that needs a left-out one (a required dependency), with the cause's code and the chain nearest first.
//!
//! An optional dependency on a left-out package is skipped, as ShroudForge's planner skips one on a package that is
//! switched off (plan_mod_load_order.rs `visit`, Kiln's fork at 2df1de68, MIT, Copyright (c) 2026 bonsaibauer; notice in
//! sf_pass.rs).
use std::{collections::BTreeMap, path::Path};

use sfi_core::package::discover::Registry;

use crate::{
    sf_pass::StartupDialect,
    startup::{NativeFacts, PlanFacts, Refusal},
};

/// What the pass may run.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Admission {
    /// The admitted pass members, in load order.
    pub order: Vec<String>,
    /// Every pass member left out, with why.
    pub excluded: BTreeMap<String, Refusal>,
}

fn refusal(code: &str, text: impl Into<String>) -> Refusal {
    Refusal {
        code: code.into(),
        text: text.into(),
        chain: Vec::new(),
    }
}

/// The admission set (see the module docs). `digest` hashes a package folder (sfi-policy's content digest);
/// `registry` is the discovery the ShroudForge pass registers from; `stem` names the side.
pub fn admit(
    plan: &PlanFacts,
    native: &NativeFacts,
    digest: &dyn Fn(&Path) -> Result<[u8; 32], String>,
    game: &Path,
    registry: Option<&Registry>,
    stem: &str,
) -> Admission {
    let members = plan.pass_members();
    let shroudforge = plan.dialect == StartupDialect::Shroudforge;
    let server = stem == "enshrouded_server";
    // the causes: the terminal's withdrawals (any package, Lua-only ones too), then the members' own
    let mut causes: BTreeMap<String, Refusal> = native
        .withdrawn
        .iter()
        .map(|(id, w)| {
            (
                id.clone(),
                Refusal {
                    code: "native-refused".into(),
                    text: w.text.clone(),
                    chain: w.chain.clone(),
                },
            )
        })
        .collect();
    for p in &members {
        if causes.contains_key(&p.id) {
            continue;
        }
        let cause = if shroudforge && !native.attached {
            Some(refusal(
                "backend-unavailable",
                "Kiln's interpreter did not take part in this start, so the ShroudForge mods' startup data is \
                 not applied (their Lua could not run either)",
            ))
        } else if native.attached && !native.received && p.native {
            Some(refusal(
                "native-terminal-missing",
                "kiln-sfi did not report how the package DLLs loaded before the startup pass, so packages with native code \
                 do not start this time",
            ))
        } else if shroudforge && !discovered(registry, &p.id, server) {
            Some(refusal(
                "package-changed",
                "Kiln could not find this package as the launch plan lists it (removed, switched off or changed since the \
                 launcher made the plan); start the game again from the Kiln launcher",
            ))
        } else {
            match digest(&game.join(&p.root)) {
                Ok(now) if now == p.content_sha256 => None,
                Ok(_) => Some(refusal(
                    "package-changed",
                    "this package's files changed since the launcher made this start's launch plan; start the game again \
                     from the Kiln launcher",
                )),
                Err(why) => Some(refusal(
                    "package-changed",
                    format!("this package's folder cannot be checked against the launch plan ({why})"),
                )),
            }
        };
        if let Some(cause) = cause {
            causes.insert(p.id.clone(), cause);
        }
    }
    // what needs a left-out package, through any number of others (required dependencies)
    loop {
        let mut added = false;
        for p in &members {
            if causes.contains_key(&p.id) {
                continue;
            }
            let needed = p
                .dependencies
                .iter()
                .filter(|d| !d.optional && d.id != "shroudforge-api")
                .find_map(|d| causes.get(&d.id).map(|cause| (d.id.clone(), cause.clone())));
            if let Some((dependency, cause)) = needed {
                let mut chain = vec![dependency.clone()];
                chain.extend(cause.chain.iter().cloned());
                chain.retain(|c| *c != p.id);
                chain.truncate(64);
                causes.insert(
                    p.id.clone(),
                    Refusal {
                        code: cause.code.clone(),
                        text: crate::startup::clamp(
                            &format!("needs {dependency}, which does not start this time: {}", cause.text),
                            4096,
                        ),
                        chain,
                    },
                );
                added = true;
            }
        }
        if !added {
            break;
        }
    }
    let excluded: BTreeMap<String, Refusal> = members
        .iter()
        .filter_map(|p| causes.get(&p.id).map(|c| (p.id.clone(), c.clone())))
        .collect();
    Admission {
        order: members
            .iter()
            .filter(|p| !excluded.contains_key(&p.id))
            .map(|p| p.id.clone())
            .collect(),
        excluded,
    }
}

/// The discovery has the package switched on and for this side (what `StartupPlan::new` requires of it).
fn discovered(registry: Option<&Registry>, id: &str, server: bool) -> bool {
    let Some(registry) = registry else { return true };
    registry.get(id).is_some_and(|record| {
        let manifest = &record.parsed().manifest;
        manifest.enabled && manifest.target.supports_process(server)
    })
}
