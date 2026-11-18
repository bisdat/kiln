//! The tester programme on this PC (PLAN 3.9): the hardware the volunteer screen shows (read once per start, sent only
//! by Volunteer), the current test build's plan from the relay with only this hardware's steps, the results kept in
//! `community-tester.json` until Send results, and whether the team has read them.

use crate::state::blocking;
use kiln_manage::community::hardware::{self, Hardware};
use kiln_manage::community::testers::{self, Plan, ResultChange, Results, SentMark, StepFor, StepResult};
use kiln_manage::Error;
use serde::Serialize;
use std::sync::{LazyLock, Mutex, OnceLock};

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct StepView {
    pub id: String,
    pub title: String,
    pub how: Vec<String>,
    #[serde(rename = "for")]
    pub for_: Option<StepFor>,
    pub result: Option<StepResult>,
    pub note: String,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PlanView {
    pub build: String,
    pub url: String,
    pub until: String,
    pub steps: Vec<StepView>,
    pub hidden: usize,
    pub sent: Option<SentMark>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct TesterStatus {
    pub tester: bool,
    pub beta: bool,
    pub hardware: Hardware,
    pub plan: Option<PlanView>,
}

/// The last plan and sent mark the relay gave (Send results and a result's change use them).
static REMOTE: LazyLock<Mutex<Option<(Plan, Option<SentMark>)>>> = LazyLock::new(|| Mutex::new(None));

/// How the game runs, from the first client game the launcher knows.
fn runner() -> String {
    let Ok(d) = crate::state::dirs() else { return String::new() };
    let Some(g) = kiln_manage::game::known(&d).unwrap_or_default().into_iter().find(|g| g.kind == kiln_manage::game::GameKind::Client) else { return String::new() };
    match g.platform {
        kiln_manage::game::Platform::Windows => "Windows".into(),
        kiln_manage::game::Platform::Wine => "Wine".into(),
        kiln_manage::game::Platform::Proton => {
            let roots = crate::state::steam_roots();
            let tool = roots.iter().find_map(|r| kiln_manage::steam::compat_tool(r, g.app_id).ok().flatten());
            let version = g.prefix.as_ref().and_then(|p| p.parent()).and_then(|c| std::fs::read_to_string(c.join("version")).ok())
                .map(|v| v.trim().to_string()).filter(|v| !v.is_empty());
            tool.or(version.map(|v| format!("Proton {v}"))).unwrap_or_else(|| "Proton".into())
        }
    }
}

/// What Volunteer would share, read once per launcher start (shown before anything is sent).
pub fn hardware() -> Hardware {
    static HW: OnceLock<Hardware> = OnceLock::new();
    HW.get_or_init(|| {
        let os = kiln_manage::report::os_info().name;
        #[cfg(windows)]
        {
            hardware::read_windows(&os, &runner())
        }
        #[cfg(not(windows))]
        {
            hardware::read_linux(&hardware::Sys::default(), &os, &runner())
        }
    })
    .clone()
}

fn beta() -> bool {
    let ch = crate::state::read_settings().ok().and_then(|s| s.update_channel);
    match ch.as_deref() {
        Some(c) => c == "beta",
        None => crate::distribution::beta().is_some(),
    }
}

fn view(plan: &Plan, sent: Option<SentMark>, hw: &Hardware) -> Result<PlanView, Error> {
    let d = crate::state::dirs()?;
    let local = Results::load(&d, &plan.build);
    let (mine, hidden) = testers::steps_for(plan, &hw.caps);
    let steps = mine.into_iter().map(|s| {
        let l = local.steps.get(&s.id).cloned().unwrap_or_default();
        StepView { id: s.id.clone(), title: s.title.clone(), how: s.how.clone(), for_: s.for_.clone(), result: l.result, note: l.note }
    }).collect();
    Ok(PlanView { build: plan.build.clone(), url: plan.url.clone(), until: plan.until.clone(), steps, hidden, sent })
}

pub fn forget_local() {
    *REMOTE.lock().unwrap() = None;
    if let Ok(d) = crate::state::dirs() {
        Results::forget(&d);
    }
}

pub fn status() -> Result<TesterStatus, Error> {
    let hw = hardware();
    let beta = beta();
    if !crate::commands::community::signed_in() {
        return Ok(TesterStatus { tester: false, beta, hardware: hw, plan: None });
    }
    let remote = match crate::commands::community::relay_call(|r| r.tester()) {
        Ok(r) => r,
        // offline: what the account said last, without a plan
        Err(_) => return Ok(TesterStatus { tester: crate::commands::community::account().is_some_and(|a| a.tester), beta, hardware: hw, plan: None }),
    };
    let plan = match (&remote.plan, remote.tester) {
        (Some(p), true) if kiln_manage::community::is_discord_url(&p.url) => Some(p.clone()),
        _ => None,
    };
    let sent = plan.as_ref().and_then(|p| remote.sent_for(p));
    *REMOTE.lock().unwrap() = plan.clone().map(|p| (p, sent.clone()));
    let plan = match plan { Some(p) => Some(view(&p, sent, &hw)?), None => None };
    Ok(TesterStatus { tester: remote.tester, beta, hardware: hw, plan })
}

pub fn join(beta: bool) -> Result<(), Error> {
    let hw = hardware();
    crate::commands::community::relay_call(|r| r.tester_join(&hw, beta))?;
    crate::commands::app::launcher_log("community", "volunteered as a tester (hardware sent to the Kiln team)");
    Ok(())
}

pub fn leave() -> Result<(), Error> {
    crate::commands::community::relay_call(|r| r.tester_leave())?;
    forget_local();
    Ok(())
}

pub fn set_result(step: &str, result: Option<ResultChange>, note: Option<String>) -> Result<Option<PlanView>, Error> {
    let Some((plan, sent)) = REMOTE.lock().unwrap().clone() else { return Ok(None) };
    if sent.is_some() {
        return Err(Error::new(kiln_manage::ErrorCode::Refused, "These results were sent already.").with_detail(serde_json::json!({ "kind": "sent" })));
    }
    let d = crate::state::dirs()?;
    let mut local = Results::load(&d, &plan.build);
    local.set(&plan, step, result, note.as_deref())?;
    local.save(&d)?;
    Ok(Some(view(&plan, None, &hardware())?))
}

pub async fn send_results(app: tauri::AppHandle, with_report: bool) -> Result<Option<PlanView>, Error> {
    let Some((plan, _)) = REMOTE.lock().unwrap().clone() else { return Ok(None) };
    let hw = hardware();
    let d = crate::state::dirs()?;
    let wire = Results::load(&d, &plan.build).wire(&plan, &hw.caps)?;
    // the support report for the team, as "Send to Kiln support" builds it (the first client game)
    let input = if with_report {
        let game = kiln_manage::game::known(&d).unwrap_or_default().into_iter().find(|g| g.kind == kiln_manage::game::GameKind::Client);
        Some(crate::commands::logs::report_input(&app, game.map(|g| crate::commands::logs::LogTarget::Game(g.id)))?)
    } else {
        None
    };
    blocking(move || {
        let zip = match input {
            Some(input) => {
                let r = kiln_manage::report::build(&input);
                let tmp = std::env::temp_dir().join(format!(".kiln-community-{}.zip", kiln_manage::hash::hex(&kiln_manage::util::random_bytes::<8>()?)));
                let w = kiln_manage::report::write_zip(&r, &tmp).and_then(|_| std::fs::read(&tmp).map_err(kiln_manage::util::io_at(&tmp, "reading")));
                let _ = std::fs::remove_file(&tmp);
                Some(w?)
            }
            None => None,
        };
        let name = kiln_manage::report::file_name(std::time::SystemTime::now());
        let sent = crate::commands::community::relay_call(|r| r.tester_results(wire, zip.as_deref().map(|z| (name.as_str(), z))))?.mark();
        *REMOTE.lock().unwrap() = Some((plan.clone(), Some(sent.clone())));
        crate::commands::app::launcher_log("community", &format!("test results sent for {}", plan.build));
        Ok(Some(view(&plan, Some(sent), &hardware())?))
    })
    .await
}
