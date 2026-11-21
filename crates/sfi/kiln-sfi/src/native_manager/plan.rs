// SPDX-License-Identifier: MIT
//! The native manager's input: the native part of a frozen launch plan, in its load order.
//!
//! kiln-manage's producer writes each candidate package's DLLs (from the native selection) into the
//! plan with their SHA-256; this reader only reshapes what the strict reader already validated.
//! It reads no file: establishment is re-proved at load time against the pinned digests.
use super::selection::Mode;
use crate::launch_plan::LaunchPlan;
use serde_json::Value;
use std::path::{Path, PathBuf};

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PlannedDll {
    /// ShroudForge's status id, `dll:<folder>:<path>` (lower case).
    pub status_id: String,
    /// Package-relative path as planned (`/`-separated).
    pub path: String,
    /// The digest the plan pinned; the file must still have it when it loads.
    pub sha256: String,
    pub mode: Mode,
    /// Launch-plan source name: `selection`, `profile`, `ini`, `lua-registration` or `chain`.
    pub source: String,
    pub bridge_abi: u64,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PlannedPackage {
    pub mod_id: String,
    /// The package folder name (`mods/<directory>`).
    pub directory: String,
    pub root: PathBuf,
    pub dlls: Vec<PlannedDll>,
    /// Included dependency ids (the plan orders them first).
    pub dependencies: Vec<String>,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct NativePlan {
    pub plan_id: [u8; 16],
    pub packages: Vec<PlannedPackage>,
}
impl NativePlan {
    /// Candidate packages in `loadOrder`, each with its planned DLLs in declared order.
    pub fn from_launch_plan(plan: &LaunchPlan, game_root: &Path) -> Result<Self, String> {
        let document = plan.document();
        let packages = document["packages"]
            .as_array()
            .ok_or("plan packages missing")?;
        let mut result = Self {
            plan_id: plan.plan_id(),
            packages: Vec::new(),
        };
        for id in document["loadOrder"]
            .as_array()
            .ok_or("plan loadOrder missing")?
        {
            let id = id.as_str().ok_or("loadOrder entry is not a string")?;
            let package = packages
                .iter()
                .find(|p| p["id"].as_str() == Some(id))
                .ok_or("loadOrder names an unknown package")?;
            let root = text(&package["root"])?;
            let directory = root
                .strip_prefix("mods/")
                .ok_or("package root outside mods/")?
                .to_owned();
            let mut dlls = Vec::new();
            for dll in package["dlls"].as_array().ok_or("package dlls missing")? {
                let path = text(&dll["path"])?.to_owned();
                dlls.push(PlannedDll {
                    status_id: format!(
                        "dll:{}:{}",
                        directory.to_ascii_lowercase(),
                        path.to_ascii_lowercase()
                    ),
                    sha256: text(&dll["sha256"])?.to_owned(),
                    mode: match text(&dll["mode"])? {
                        "dinput8" => Mode::Dinput8,
                        _ => Mode::Library,
                    },
                    source: text(&dll["source"])?.to_owned(),
                    bridge_abi: dll["bridgeAbi"].as_u64().ok_or("bridgeAbi missing")?,
                    path,
                });
            }
            let dependencies = package["dependencies"]
                .as_array()
                .ok_or("package dependencies missing")?
                .iter()
                .filter(|d| d["included"].as_bool() == Some(true))
                .filter_map(|d| d["id"].as_str().map(str::to_owned))
                .collect();
            result.packages.push(PlannedPackage {
                mod_id: id.to_owned(),
                root: game_root.join("mods").join(&directory),
                directory,
                dlls,
                dependencies,
            });
        }
        Ok(result)
    }
}
fn text(v: &Value) -> Result<&str, String> {
    v.as_str().ok_or_else(|| "expected a plan string".into())
}
