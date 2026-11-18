//! The tester programme (PLAN 3.9): the current test build's plan from the relay, the steps for this hardware, the
//! player's results kept on this PC until they press Send results, and whether the team has read them.

use super::hardware::Capability;
use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum StepResult {
    Worked,
    Failed,
    Skipped,
}

/// Who a step is for.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct StepFor {
    /// "AMD RX 6000 and newer".
    pub label: String,
    /// Any one of these makes the step this PC's.
    pub needs: Vec<Capability>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PlanStep {
    pub id: String,
    pub title: String,
    /// Plain steps, one per line.
    pub how: Vec<String>,
    /// None: every tester.
    #[serde(rename = "for", default)]
    pub for_: Option<StepFor>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Plan {
    /// "0.2.0-dev.30 Beta 2"
    pub build: String,
    /// The build's Kiln version ("0.2.0-dev.30"): what results name (absent: `build`).
    #[serde(default)]
    pub version: Option<String>,
    /// The build's `#testing` post.
    pub url: String,
    /// RFC 3339.
    pub until: String,
    pub steps: Vec<PlanStep>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ReadMark {
    pub by: String,
    pub at: String,
}

/// Results sent for a build, and whether the team read them.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SentMark {
    pub at: String,
    #[serde(default)]
    pub read: Option<ReadMark>,
    #[serde(default)]
    pub replied: bool,
}

/// One build's results as the relay keeps them (`Result`: `{build, sentAt, url, read, replied}`).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ResultMark {
    pub build: String,
    pub sent_at: String,
    #[serde(default)]
    pub url: Option<String>,
    #[serde(default)]
    pub read: Option<ReadMark>,
    #[serde(default)]
    pub replied: bool,
}

impl ResultMark {
    pub fn mark(&self) -> SentMark {
        SentMark { at: self.sent_at.clone(), read: self.read.clone(), replied: self.replied }
    }
}

/// `GET /tester`: `{tester, hardware, results}`. The relay serves no test plan yet; one beside them (`plan`) is read.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RemoteTester {
    #[serde(default)]
    pub tester: bool,
    #[serde(default)]
    pub results: Vec<ResultMark>,
    #[serde(default)]
    pub plan: Option<Plan>,
}

impl Plan {
    /// What results for this build are filed under.
    pub fn version(&self) -> &str {
        self.version.as_deref().unwrap_or(&self.build)
    }
}

impl RemoteTester {
    /// The results sent for the plan's build, if any.
    pub fn sent_for(&self, plan: &Plan) -> Option<SentMark> {
        self.results.iter().find(|r| r.build == plan.version()).map(ResultMark::mark)
    }
}

/// A change of a step's result: one of the three, or back to "not tried".
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum ResultChange {
    Worked,
    Failed,
    Skipped,
    Clear,
}

/// The plan's steps for this hardware, and how many are for other hardware.
pub fn steps_for<'a>(plan: &'a Plan, caps: &[Capability]) -> (Vec<&'a PlanStep>, usize) {
    let (mine, other): (Vec<&PlanStep>, Vec<&PlanStep>) = plan.steps.iter().partition(|s| s.for_.as_ref().is_none_or(|f| f.needs.iter().any(|n| caps.contains(n))));
    (mine, other.len())
}

/// One step's result and line on this PC.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Local {
    #[serde(default)]
    pub result: Option<StepResult>,
    #[serde(default)]
    pub note: String,
}

/// `community-tester.json`: the results for one build, kept until Send results.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Results {
    pub build: String,
    #[serde(default)]
    pub steps: std::collections::BTreeMap<String, Local>,
}

/// The longest line on a step.
pub const NOTE_MAX: usize = 140;

impl Results {
    pub fn path(dirs: &crate::paths::DataDirs) -> std::path::PathBuf {
        dirs.root.join("community-tester.json")
    }

    /// The results for `build` (a file for another build starts fresh).
    pub fn load(dirs: &crate::paths::DataDirs, build: &str) -> Results {
        match crate::util::read_json::<Results>(&Self::path(dirs)) {
            Ok(Some(r)) if r.build == build => r,
            _ => Results { build: build.to_string(), steps: Default::default() },
        }
    }

    pub fn save(&self, dirs: &crate::paths::DataDirs) -> crate::Result<()> {
        crate::util::write_json_atomic(&Self::path(dirs), self)
    }

    pub fn forget(dirs: &crate::paths::DataDirs) {
        let _ = std::fs::remove_file(Self::path(dirs));
    }

    /// Applies a change to a step of the plan (`result` or `note` None: that one stays).
    pub fn set(&mut self, plan: &Plan, step: &str, result: Option<ResultChange>, note: Option<&str>) -> crate::Result<()> {
        if !plan.steps.iter().any(|s| s.id == step) {
            return Err(crate::Error::not_found("No such step in this test build."));
        }
        let e = self.steps.entry(step.to_string()).or_default();
        if let Some(r) = result {
            e.result = match r {
                ResultChange::Worked => Some(StepResult::Worked),
                ResultChange::Failed => Some(StepResult::Failed),
                ResultChange::Skipped => Some(StepResult::Skipped),
                ResultChange::Clear => None,
            };
        }
        if let Some(n) = note {
            e.note = n.chars().filter(|c| !c.is_control()).take(NOTE_MAX).collect::<String>().trim().to_string();
        }
        Ok(())
    }

    /// What `POST /tester/results` sends (the relay's `{build, steps: [{id, title, result, note?}]}`): every step of this
    /// PC's with its result and line. Refused while a step has no result.
    pub fn wire(&self, plan: &Plan, caps: &[Capability]) -> crate::Result<serde_json::Value> {
        let (mine, _) = steps_for(plan, caps);
        let mut steps = Vec::new();
        for s in mine {
            let l = self.steps.get(&s.id).cloned().unwrap_or_default();
            let Some(r) = l.result else {
                return Err(crate::Error::invalid("Try each step, or mark it Couldn't try.").with_detail(serde_json::json!({ "kind": "untried" })));
            };
            let mut step = serde_json::json!({ "id": s.id, "title": s.title.chars().take(100).collect::<String>(), "result": r });
            if !l.note.is_empty() {
                step["note"] = serde_json::json!(l.note);
            }
            steps.push(step);
        }
        Ok(serde_json::json!({ "build": plan.version(), "steps": steps }))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn plan() -> Plan {
        let step = |id: &str, needs: Option<Vec<Capability>>| PlanStep { id: id.into(), title: id.into(), how: vec!["Do it.".into()],
            for_: needs.map(|n| StepFor { label: "x".into(), needs: n }) };
        Plan { build: "0.2.0-dev.30 Beta 2".into(), version: Some("0.2.0-dev.30".into()), url: "https://discord.com/channels/1/2".into(), until: "2026-11-21T00:00:00Z".into(),
            steps: vec![step("start", None), step("fsr4", Some(vec![Capability::Fsr4])), step("xess", Some(vec![Capability::Xess])), step("deck", Some(vec![Capability::Deck])), step("reload", None)] }
    }

    #[test]
    fn the_relays_results_list_gives_the_builds_sent_and_read_marks() {
        let r: RemoteTester = serde_json::from_value(serde_json::json!({ "tester": true, "hardware": { "gpu": "x" }, "results": [
            { "build": "0.2.0-dev.29", "sentAt": "2026-11-01T00:00:00.000Z", "url": "https://discord.com/channels/1/1", "read": null, "replied": false },
            { "build": "0.2.0-dev.30", "sentAt": "2026-11-18T10:00:00.000Z", "url": "https://discord.com/channels/1/2", "read": { "by": "Rowan", "at": "2026-11-18T12:00:00.000Z" }, "replied": true }
        ] })).unwrap();
        assert!(r.tester && r.plan.is_none());
        let s = r.sent_for(&plan()).unwrap();
        assert_eq!((s.at.as_str(), s.read.unwrap().by.as_str(), s.replied), ("2026-11-18T10:00:00.000Z", "Rowan", true));
    }

    #[test]
    fn a_pc_sees_the_steps_for_everyone_and_for_its_hardware() {
        let p = plan();
        let (mine, hidden) = steps_for(&p, &[Capability::Fsr4]);
        assert_eq!(mine.iter().map(|s| s.id.as_str()).collect::<Vec<_>>(), ["start", "fsr4", "reload"]);
        assert_eq!(hidden, 2);
        let (mine, hidden) = steps_for(&p, &[]);
        assert_eq!((mine.len(), hidden), (2, 3));
        let (mine, _) = steps_for(&p, &[Capability::Deck, Capability::Fsr4]);
        assert_eq!(mine.len(), 4);
    }

    #[test]
    fn results_are_kept_per_build_and_sent_only_when_every_step_has_one() {
        let t = super::super::testing::tmp("tester-results");
        let d = crate::paths::DataDirs::at(t.path());
        let p = plan();
        let caps = vec![Capability::Fsr4];
        let mut r = Results::load(&d, &p.build);
        r.set(&p, "start", Some(ResultChange::Worked), None).unwrap();
        r.set(&p, "fsr4", Some(ResultChange::Failed), Some("Water smears when I turn the camera")).unwrap();
        // a line after the pick keeps the pick
        r.set(&p, "fsr4", None, Some("Water smears")).unwrap();
        assert_eq!(r.steps["fsr4"].result, Some(StepResult::Failed));
        assert!(r.set(&p, "nope", Some(ResultChange::Worked), None).is_err());
        let e = r.wire(&p, &caps).unwrap_err();
        assert_eq!(e.detail.unwrap()["kind"], "untried");
        r.set(&p, "reload", Some(ResultChange::Skipped), Some(&"n".repeat(300))).unwrap();
        r.save(&d).unwrap();
        let back = Results::load(&d, &p.build);
        assert_eq!(back.steps["reload"].note.chars().count(), NOTE_MAX);
        let v = back.wire(&p, &caps).unwrap();
        assert_eq!(v["build"], "0.2.0-dev.30", "results name the build's version");
        assert_eq!(v["steps"].as_array().unwrap().len(), 3, "the steps for other hardware are not sent");
        assert_eq!(v["steps"][0], serde_json::json!({ "id": "start", "title": "start", "result": "worked" }));
        assert_eq!(v["steps"][1], serde_json::json!({ "id": "fsr4", "title": "fsr4", "result": "failed", "note": "Water smears" }));
        let mut keys: Vec<&str> = v.as_object().unwrap().keys().map(String::as_str).collect();
        keys.sort();
        assert_eq!(keys, ["build", "steps"]);
        // another build starts fresh; clearing a result goes back to not tried
        assert!(Results::load(&d, "0.2.0-dev.30 Beta 3").steps.is_empty());
        let mut c = back.clone();
        c.set(&p, "start", Some(ResultChange::Clear), None).unwrap();
        assert_eq!(c.steps["start"].result, None);
    }
}
