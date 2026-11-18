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

/// `GET /tester`.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RemoteTester {
    #[serde(default)]
    pub tester: bool,
    #[serde(default)]
    pub plan: Option<Plan>,
    #[serde(default)]
    pub sent: Option<SentMark>,
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
