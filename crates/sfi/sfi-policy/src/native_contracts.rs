// SPDX-License-Identifier: MIT
//! Versioned host-owned native reviews and ruling 10/11 admission policy.
//! Registry records are exact-file SHA-256 reviews, never inferred from string
//! co-occurrence. Generic dynamic loading without banned-name evidence requires
//! actual runtime enforcement; it is not a proof of safe future behavior.
use crate::{
    pe_gate::{self, GateVerdict, Image, InspectionIssueKind, Policy},
    strict_json,
};
use serde_json::Value;
use sha2::{Digest, Sha256};
use std::collections::BTreeMap;

#[derive(Debug, Clone)]
struct Contract {
    verdict: String,
    absent: String,
}
#[derive(Debug, Clone)]
pub struct NativeContracts {
    contracts: BTreeMap<String, Contract>,
}
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AdmissionVerdict {
    Pass,
    AdmittedWithEnforcement,
    NeedsContract,
    Refused,
}
/// The shipped, host-owned review registry. It ships empty until reviewed contracts exist.
pub const SHIPPED: &[u8] = include_bytes!("../sfi-native-contracts.json");
#[derive(Debug, Clone)]
pub struct AdmissionReport {
    pub verdict: AdmissionVerdict,
    pub reasons: Vec<String>,
    pub evidence: Vec<String>,
    pub classification: Option<String>,
    pub needs_reviewed_absent_path: bool,
    pub partial: bool,
    pub requires_enforcement: bool,
}
impl NativeContracts {
    /// Read only a host-owned shipped registry, never a player plan's replacement.
    /// Corrupt/missing registries are errors, not an implicit empty allow-list.
    pub fn read(path: &std::path::Path) -> Result<Self, String> {
        use std::io::Read;
        let mut bytes = Vec::new();
        std::fs::File::open(path)
            .map_err(|e| e.to_string())?
            .take(1024 * 1024 + 1)
            .read_to_end(&mut bytes)
            .map_err(|e| e.to_string())?;
        Self::parse(&bytes)
    }
    pub fn parse(bytes: &[u8]) -> Result<Self, String> {
        let value = strict_json::parse(bytes, 1024 * 1024)?;
        let top = strict_json::object(&value, &["format", "minReader", "contracts"])?;
        if top["format"].as_u64() != Some(1) || top["minReader"].as_u64() != Some(1) {
            return Err("unsupported native-contract registry version".into());
        }
        let rows = top["contracts"]
            .as_object()
            .ok_or("contracts must be an object")?;
        if rows.len() > 4096 {
            return Err("native-contract count budget exceeded".into());
        }
        let mut contracts = BTreeMap::new();
        for (digest, value) in rows {
            strict_json::hash(&Value::String(digest.clone()))?;
            let row = strict_json::object(
                value,
                &["verdict", "evidence", "reviewer", "date", "absentPath"],
            )?;
            let verdict = strict_json::text(&row["verdict"])?;
            if ![
                "pass",
                "presence-probe",
                "banned-export-resolution",
                "unknown-dynamic",
                "refused",
            ]
            .contains(&verdict)
            {
                return Err("unknown native-contract verdict".into());
            }
            let evidence = row["evidence"]
                .as_array()
                .ok_or("evidence must be an array")?;
            if evidence.is_empty() || evidence.len() > 64 {
                return Err("review needs bounded disassembly evidence".into());
            }
            for entry in evidence {
                if strict_json::text(entry)?.len() > 4096 {
                    return Err("review evidence too long".into());
                }
            }
            if strict_json::text(&row["reviewer"])?.len() > 256 {
                return Err("reviewer too long".into());
            }
            if !valid_date(strict_json::text(&row["date"])?) {
                return Err("invalid review date".into());
            }
            let absent = strict_json::text(&row["absentPath"])?;
            if ![
                "supported",
                "feature-disabled",
                "not-reviewed",
                "not-applicable",
            ]
            .contains(&absent)
            {
                return Err("unknown absent-path review status".into());
            }
            if (verdict == "presence-probe" || verdict == "banned-export-resolution")
                && absent == "not-applicable"
            {
                return Err("probe/export review needs explicit absent-path status".into());
            }
            contracts.insert(
                digest.clone(),
                Contract {
                    verdict: verdict.into(),
                    absent: absent.into(),
                },
            );
        }
        Ok(Self { contracts })
    }
    /// The registry this build ships (`sfi-native-contracts.json`, embedded at build time).
    pub fn shipped() -> Result<Self, String> {
        Self::parse(SHIPPED)
    }
    pub fn sha256(bytes: &[u8]) -> String {
        format!("{:x}", Sha256::digest(bytes))
    }
    pub fn evaluate(&self, entry: &str, images: &[Image<'_>]) -> AdmissionReport {
        self.evaluate_policy(entry, images, &[], false)
    }
    /// Read-only cross-platform inventory is advisory, never a load permit.
    /// Unresolved imports still require real OS/package binding before any load.
    pub fn advisory(&self, entry: &str, images: &[Image<'_>]) -> AdmissionReport {
        self.evaluate_policy(entry, images, &[], true)
    }
    /// Host-side inventory facts for kiln-manage and diagnostics: [`Self::advisory`]'s
    /// verdict behind a type that carries no enforcement or permit field.
    pub fn inventory(&self, entry: &str, images: &[Image<'_>]) -> AdvisoryInventory {
        AdvisoryInventory(self.advisory(entry, images))
    }
    /// kiln-sfi's resolver only: `verified_system_names` are the basenames of system
    /// images the OS loader itself resolved and kiln-sfi snapshotted. The report is still
    /// not a permit; kiln-sfi's private `LoadPermit` is built from the same closure.
    #[cfg(feature = "os-binding")]
    pub fn evaluate_bound(
        &self,
        entry: &str,
        images: &[Image<'_>],
        verified_system_names: &[&str],
    ) -> AdmissionReport {
        self.evaluate_policy(entry, images, verified_system_names, false)
    }
    fn evaluate_policy(
        &self,
        entry: &str,
        images: &[Image<'_>],
        system_names: &[&str],
        advisory: bool,
    ) -> AdmissionReport {
        let inspection = pe_gate::gate(
            entry,
            images,
            &Policy {
                reviews: &[],
                system_modules: system_names,
            },
        );
        let mut result = AdmissionReport {
            verdict: AdmissionVerdict::Pass,
            reasons: Vec::new(),
            evidence: inspection.evidence,
            classification: None,
            needs_reviewed_absent_path: false,
            partial: false,
            requires_enforcement: true,
        };
        match inspection.verdict {
            GateVerdict::Banned {
                image,
                module,
                symbols,
                kind,
            } => {
                result.verdict = AdmissionVerdict::Refused;
                result.reasons.push(format!("{image}: banned {kind:?} import {module} [{}]; static/delay dependencies load before DllMain and have no review exception", symbols.join(",")));
                result.requires_enforcement = false;
                return result;
            }
            GateVerdict::UnknownDynamic { reasons } => {
                for reason in reasons {
                    if reason.kind == InspectionIssueKind::Structural
                        || (!advisory && reason.kind == InspectionIssueKind::UnresolvedDependency)
                    {
                        result.verdict = AdmissionVerdict::Refused;
                        result.requires_enforcement = false;
                    }
                    result.reasons.push(reason.message);
                }
                if result.verdict == AdmissionVerdict::Refused {
                    return result;
                }
            }
            GateVerdict::Pass => (),
        }
        if !inspection
            .inspected
            .iter()
            .any(|name| name.eq_ignore_ascii_case(entry))
        {
            result.verdict = AdmissionVerdict::Refused;
            result.requires_enforcement = false;
            result
                .reasons
                .push("entry DLL was not successfully inspected".into());
            return result;
        }
        for name in inspection.inspected {
            let Some(image) = images.iter().find(|image| image.path == name) else {
                result.verdict = AdmissionVerdict::Refused;
                result.requires_enforcement = false;
                result
                    .reasons
                    .push("inspected snapshot missing from inventory".into());
                return result;
            };
            let banned_evidence = banned_name_evidence(image.bytes);
            result.evidence.extend(
                banned_evidence
                    .iter()
                    .map(|e| format!("{name}: banned-name evidence {e}")),
            );
            let contract = self.contracts.get(&Self::sha256(image.bytes));
            if name.eq_ignore_ascii_case(entry) {
                result.classification = contract.map(|c| c.verdict.clone());
            }
            match contract {
                Some(contract) if contract.verdict == "refused" => {
                    result.verdict = AdmissionVerdict::Refused;
                    result
                        .reasons
                        .push(format!("{name}: exact-hash reviewed refusal"));
                }
                Some(contract) if contract.verdict == "pass" => (),
                Some(contract) if contract.verdict == "presence-probe" => {
                    if contract.absent == "not-reviewed" {
                        result.needs_reviewed_absent_path = true;
                        promote(&mut result.verdict, AdmissionVerdict::NeedsContract);
                        result.reasons.push(format!(
                            "{name}: presence-probe needs reviewed absent-path contract"
                        ));
                    } else {
                        promote(
                            &mut result.verdict,
                            AdmissionVerdict::AdmittedWithEnforcement,
                        );
                        result.partial |= contract.absent == "feature-disabled";
                        result.reasons.push(format!(
                            "{name}: reviewed presence-probe; absent path {}",
                            contract.absent
                        ));
                    }
                }
                Some(contract) if contract.verdict == "banned-export-resolution" => {
                    if contract.absent != "supported" {
                        result.verdict = AdmissionVerdict::Refused;
                        result.reasons.push(format!("{name}: banned-export-resolution has no reviewed guarded supported alternative"));
                    } else {
                        promote(
                            &mut result.verdict,
                            AdmissionVerdict::AdmittedWithEnforcement,
                        );
                        result.reasons.push(format!("{name}: reviewed guarded dynamic export path is unreachable when banned module is absent; supported alternative"));
                    }
                }
                _ if !banned_evidence.is_empty() => {
                    promote(&mut result.verdict, AdmissionVerdict::NeedsContract);
                    result.reasons.push(format!("{name}: banned-name evidence needs an exact-hash disassembly/absent-path contract; strings do not classify a call path"));
                }
                _ => {
                    promote(
                        &mut result.verdict,
                        AdmissionVerdict::AdmittedWithEnforcement,
                    );
                    result.reasons.push(format!("{name}: generic unknown dynamic behavior admitted only with initial/post-load module checks and lifetime loader notification"));
                }
            }
        }
        result.requires_enforcement = matches!(
            result.verdict,
            AdmissionVerdict::Pass | AdmissionVerdict::AdmittedWithEnforcement
        );
        result
    }
}
/// Read-only facts about one package closure, for a host-side inventory (kiln-manage's
/// pre-select scan, corpus diagnostics). Unresolved system imports are advisory here: the
/// host cannot bind Windows system images. It is never a load permit, carries no
/// enforcement field and converts into nothing that admits a load.
#[derive(Debug, Clone)]
pub struct AdvisoryInventory(AdmissionReport);
impl AdvisoryInventory {
    pub fn verdict(&self) -> AdmissionVerdict {
        self.0.verdict
    }
    /// Pass or admitted-with-enforcement: kiln mode may plan the package, and kiln-sfi
    /// re-inspects and binds it at load. Needs-contract and refused are authoritative skips.
    pub fn eligible(&self) -> bool {
        matches!(
            self.0.verdict,
            AdmissionVerdict::Pass | AdmissionVerdict::AdmittedWithEnforcement
        )
    }
    pub fn reasons(&self) -> &[String] {
        &self.0.reasons
    }
    pub fn evidence(&self) -> &[String] {
        &self.0.evidence
    }
    pub fn classification(&self) -> Option<&str> {
        self.0.classification.as_deref()
    }
    pub fn needs_reviewed_absent_path(&self) -> bool {
        self.0.needs_reviewed_absent_path
    }
    /// A reviewed feature-disabled absent path: the package runs without that feature.
    pub fn partial(&self) -> bool {
        self.0.partial
    }
}
fn promote(current: &mut AdmissionVerdict, next: AdmissionVerdict) {
    fn rank(v: AdmissionVerdict) -> u8 {
        match v {
            AdmissionVerdict::Pass => 0,
            AdmissionVerdict::AdmittedWithEnforcement => 1,
            AdmissionVerdict::NeedsContract => 2,
            AdmissionVerdict::Refused => 3,
        }
    }
    if rank(next) > rank(*current) {
        *current = next;
    }
}
fn valid_date(s: &str) -> bool {
    if s.len() != 10
        || s.as_bytes()[4] != b'-'
        || s.as_bytes()[7] != b'-'
        || !s
            .bytes()
            .enumerate()
            .all(|(i, b)| i == 4 || i == 7 || b.is_ascii_digit())
    {
        return false;
    }
    let (Ok(year), Ok(month), Ok(day)) = (
        s[..4].parse::<u32>(),
        s[5..7].parse::<u32>(),
        s[8..].parse::<u32>(),
    ) else {
        return false;
    };
    let days = match month {
        1 | 3 | 5 | 7 | 8 | 10 | 12 => 31,
        4 | 6 | 9 | 11 => 30,
        2 if year % 4 == 0 && (year % 100 != 0 || year % 400 == 0) => 29,
        2 => 28,
        _ => 0,
    };
    year != 0 && day > 0 && day <= days
}
fn banned_name_evidence(bytes: &[u8]) -> Vec<String> {
    [
        "shroudforge-runtime",
        concat!("kf", "c-runtime"),
        concat!("Kf", "cRuntime"),
    ]
    .iter()
    .filter(|name| {
        let wide: Vec<u8> = name.encode_utf16().flat_map(u16::to_le_bytes).collect();
        bytes
            .windows(name.len())
            .any(|b| b.eq_ignore_ascii_case(name.as_bytes()))
            || bytes
                .windows(wide.len())
                .any(|b| b.eq_ignore_ascii_case(&wide))
    })
    .map(|name| (*name).into())
    .collect()
}
