//! Support reports to Kiln support: prepared once, shown, sent as shown (step 5 of the Discord work fills this in).

use kiln_manage::community::known::KnownList;
use kiln_manage::community::{ReportDraft, SendFrom, SupportPost};
use kiln_manage::Error;
use serde::Serialize;

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ReportPreview {
    pub prepared: String,
}

pub fn keep_known(_list: &KnownList) -> Result<(), Error> {
    Ok(())
}

pub async fn prepare(_app: tauri::AppHandle, _target: Option<crate::commands::logs::LogTarget>, _from: SendFrom) -> Result<ReportPreview, Error> {
    Err(Error::not_implemented("community_report_prepare"))
}

pub fn send(_prepared: &str, _draft: &ReportDraft) -> Result<SupportPost, Error> {
    Err(Error::not_implemented("community_report_send"))
}
