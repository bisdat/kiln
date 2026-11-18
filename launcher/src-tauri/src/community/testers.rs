//! The tester's state on this PC (step 8 of the Discord work fills this in).

use kiln_manage::community::testers::ResultChange;
use kiln_manage::Error;
use serde::Serialize;

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct TesterStatus {
    pub tester: bool,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PlanView {
    pub build: String,
}

pub fn forget_local() {}

pub fn status() -> Result<TesterStatus, Error> {
    Err(Error::not_implemented("tester_status"))
}

pub fn join(_beta: bool) -> Result<(), Error> {
    Err(Error::not_implemented("tester_join"))
}

pub fn leave() -> Result<(), Error> {
    Err(Error::not_implemented("tester_leave"))
}

pub fn set_result(_step: &str, _result: Option<ResultChange>, _note: Option<String>) -> Result<Option<PlanView>, Error> {
    Err(Error::not_implemented("tester_result_set"))
}

pub async fn send_results(_app: tauri::AppHandle, _with_report: bool) -> Result<Option<PlanView>, Error> {
    Err(Error::not_implemented("tester_results_send"))
}
