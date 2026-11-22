//! Work the game does before its own window exists: kiln-eml (the EML runner, a Kiln plugin) fingerprints and patches the
//! game's data files at the very start of the process, which can take a while, and says what it is doing in
//! `kiln/state/eml.json` (`phase: "running"` and a `progress` object, rewritten a few times a second and at least once a
//! second; docs/kiln/eml.md "Reading the game data"). This reads it for the Play page's launch status: "Preparing the
//! game data for EML mods: 12.3 of 48.6 GiB." A kiln-mode start's startup pass (stage `startup`) says its step instead:
//! "Preparing ShroudForge mods for Kiln's interpreter: checking the prepared game data (3 of 5)."
//!
//! A `running` file that is not of the game that runs now (the game crashed or was killed mid-run, or the file is from an
//! earlier start) says nothing: it counts only while the game's process runs, the file was written after that process
//! started (the same rule as `launch::boot_is_current`) and was written in the last few seconds ([`STALE`]; a startup
//! pass's in the last two minutes, [`STARTUP_STALE`]).

use crate::game::GameInstall;
use crate::Result;
use serde::{Deserialize, Serialize};
use std::time::{Duration, SystemTime};

/// `kiln/state/eml.json`, relative to the game folder.
pub const EML_JSON: &str = "kiln/state/eml.json";

/// A running file older than this was left by a process that is gone or stuck (the writer touches it every second).
pub const STALE: Duration = Duration::from_secs(15);

/// [`STALE`] for a kiln-mode start's startup pass (stage `startup`). It writes the file when a step begins and while it
/// moves bytes, never on a timer: it may run while a package DLL's initializer holds the loader lock, so it starts no
/// thread to touch the file (kiln-eml progress.rs `PassSteps`). Its steps that move no bytes (the wait for kiln-sfi's
/// terminal, up to 20 s; the packages' startup code; a slow flush to the drive) can outlast `STALE`, so its file counts
/// as long as the start timeline waits for the early plugins (`start_progress::QUIET_EARLY_MS`).
pub const STARTUP_STALE: Duration = Duration::from_millis(crate::start_progress::QUIET_EARLY_MS);

/// No byte read for this long while reading: the line says so.
const QUIET: u64 = 15_000;

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PreStart {
    /// `starting`, `checking`, `prepare`, `mods`, `writing`, `saving`; `startup` for a kiln-mode start's startup pass.
    pub stage: String,
    /// The stage for a person ("Checking the game data").
    pub label: String,
    pub done_bytes: u64,
    pub total_bytes: u64,
    pub files_done: u64,
    pub files_total: u64,
    /// The file being read now.
    pub file: Option<String>,
    /// Milliseconds since a byte was last read (while a file is being read).
    pub quiet_ms: u64,
    /// Stage `startup`: the step (`checking`, `mods`, `verify`, `write`, `save`), its number and how many there are.
    pub step: Option<String>,
    pub step_number: u64,
    pub steps: u64,
    /// The sentence for the launch status.
    pub message: String,
}

#[derive(Deserialize)]
struct EmlState {
    #[serde(default)]
    format: u32,
    #[serde(default)]
    phase: String,
    progress: Option<Progress>,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct Progress {
    #[serde(default)]
    stage: String,
    #[serde(default)]
    label: String,
    #[serde(default)]
    done_bytes: u64,
    #[serde(default)]
    total_bytes: u64,
    #[serde(default)]
    files_done: u64,
    #[serde(default)]
    files_total: u64,
    file: Option<String>,
    #[serde(default)]
    quiet_ms: u64,
    #[serde(default)]
    step: Option<String>,
    #[serde(default)]
    step_number: u64,
    #[serde(default)]
    steps: u64,
}

/// A kiln-mode startup pass's step for a person (`kiln-eml` progress.rs `PassStep::label`).
fn step_label(step: &str) -> Option<&'static str> {
    Some(match step {
        "checking" => "checking the game files",
        "mods" => "running the mods",
        "verify" => "checking the prepared game data",
        "write" => "writing the game data",
        "save" => "saving the result",
        _ => return None,
    })
}

/// `12.3 GiB`, `312 B`.
fn size(bytes: u64) -> String {
    const UNITS: [&str; 5] = ["B", "KiB", "MiB", "GiB", "TiB"];
    let mut value = bytes as f64;
    let mut unit = 0;
    while value >= 1024.0 && unit < UNITS.len() - 1 {
        value /= 1024.0;
        unit += 1;
    }
    if unit == 0 { format!("{bytes} B") } else { format!("{value:.1} {}", UNITS[unit]) }
}

fn message(p: &Progress) -> String {
    if p.stage == "startup" {
        let head = if p.label == "Preparing the game data for EML mods" { p.label.as_str() }
            else { "Preparing ShroudForge mods for Kiln's interpreter" };
        return match p.step.as_deref().and_then(step_label) {
            Some(label) if p.step_number > 0 && p.steps >= p.step_number => format!("{head}: {label} ({} of {}).", p.step_number, p.steps),
            Some(label) => format!("{head}: {label}."),
            None => format!("{head}."),
        };
    }
    let head = "Preparing the game data for EML mods";
    let mut text = match p.stage.as_str() {
        "checking" if p.total_bytes > 0 => format!(
            "{head}: {} of {}. Only the first start after a game update takes this long.",
            size(p.done_bytes.min(p.total_bytes)), size(p.total_bytes)
        ),
        "checking" => format!("{head}: checking the game files."),
        "prepare" => format!("{head}: preparing the vanilla backup and the game's types."),
        "mods" => format!("{head}: running the mods."),
        "writing" => format!("{head}: writing the patched game data."),
        "saving" => format!("{head}: saving what it learned about the files."),
        _ => format!("{head}."),
    };
    if p.quiet_ms >= QUIET {
        text.push_str(&format!(" No data for {} s: the drive may be slow or busy.", p.quiet_ms / 1000));
    }
    text
}

/// The progress a state file's text says, when it is a running one (format 1, `phase: "running"`, a `progress` object).
/// Whether it is current is `is_current`'s business.
pub fn parse(text: &str) -> Option<PreStart> {
    let state: EmlState = serde_json::from_str(text.trim_start_matches('\u{feff}')).ok()?;
    if state.format != 1 || state.phase != "running" {
        return None;
    }
    let p = state.progress?;
    Some(PreStart {
        message: message(&p),
        stage: p.stage,
        label: p.label,
        done_bytes: p.done_bytes,
        total_bytes: p.total_bytes,
        files_done: p.files_done,
        files_total: p.files_total,
        file: p.file,
        quiet_ms: p.quiet_ms,
        step: p.step,
        step_number: p.step_number,
        steps: p.steps,
    })
}

/// Does a running file written at `written` belong to the game process that started at `started` and still say something
/// at `now`? An unknown start counts as current (the process runs and the file is fresh), an unknown write time as not.
pub fn is_current(written: Option<SystemTime>, started: Option<SystemTime>, now: SystemTime) -> bool {
    is_current_within(written, started, now, STALE)
}

/// `is_current` for a file that may go unwritten for `stale` (`stale_after` its stage).
pub fn is_current_within(written: Option<SystemTime>, started: Option<SystemTime>, now: SystemTime, stale: Duration) -> bool {
    let Some(written) = written else { return false };
    if now.duration_since(written).is_ok_and(|age| age > stale) {
        return false;
    }
    crate::launch::written_after_start(Some(written), started)
}

/// How long a running file whose progress is at `stage` may go unwritten: [`STARTUP_STALE`] for a startup pass, else
/// [`STALE`].
pub fn stale_after(stage: &str) -> Duration {
    if stage == "startup" { STARTUP_STALE } else { STALE }
}

/// What the game that runs now is doing before its window: None when no game runs, when `kiln/state/eml.json` is not a
/// running state, or when it is not this game's or is stale (`is_current`).
pub fn read(install: &GameInstall) -> Result<Option<PreStart>> {
    let Some(process) = crate::proc::find_game(install)? else { return Ok(None) };
    let path = install.path.join(EML_JSON);
    let written = std::fs::metadata(&path).and_then(|m| m.modified()).ok();
    let started = crate::proc::start_time(process.pid);
    // read only a file that can still count; then by its stage
    if !is_current_within(written, started, SystemTime::now(), STALE.max(STARTUP_STALE)) {
        return Ok(None);
    }
    let Ok(text) = std::fs::read_to_string(&path) else { return Ok(None) };
    Ok(parse(&text).filter(|p| is_current_within(written, started, SystemTime::now(), stale_after(&p.stage))))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn running(stage: &str, done: u64, total: u64, quiet: u64) -> String {
        format!(
            r#"{{"format":1,"pid":4,"phase":"running","mods":[],"progress":{{"stage":"{stage}","label":"x","doneBytes":{done},"totalBytes":{total},"filesDone":3,"filesTotal":55,"file":"enshrouded_003.dat","quietMs":{quiet},"stageMs":9000,"startedMs":1,"updatedMs":2}}}}"#
        )
    }

    #[test]
    fn the_checking_stage_says_how_much_of_how_much() {
        let p = parse(&running("checking", 13_194_139_533, 52_183_056_384, 0)).unwrap();
        assert_eq!(
            p.message,
            "Preparing the game data for EML mods: 12.3 GiB of 48.6 GiB. Only the first start after a game update takes this long."
        );
        assert_eq!((p.done_bytes, p.total_bytes, p.files_done, p.files_total), (13_194_139_533, 52_183_056_384, 3, 55));
        assert_eq!(p.file.as_deref(), Some("enshrouded_003.dat"));
        // more done than announced: the line stops at the total
        let p = parse(&running("checking", 99, 10, 0)).unwrap();
        assert!(p.message.contains("10 B of 10 B"), "{}", p.message);
    }

    #[test]
    fn the_other_stages_say_what_they_do_and_a_silent_read_says_so() {
        for (stage, words) in [("checking", "checking the game files"), ("prepare", "preparing the vanilla backup"), ("mods", "running the mods"),
                               ("writing", "writing the patched"), ("saving", "saving what it learned"), ("starting", "EML mods.")] {
            let p = parse(&running(stage, 0, 0, 0)).unwrap();
            assert!(p.message.contains(words), "{stage}: {}", p.message);
            assert!(p.message.starts_with("Preparing the game data for EML mods"));
        }
        let p = parse(&running("checking", 5, 10, 42_000)).unwrap();
        assert!(p.message.ends_with("No data for 42 s: the drive may be slow or busy."), "{}", p.message);
        assert!(!parse(&running("checking", 5, 10, 14_000)).unwrap().message.contains("No data"));
    }

    #[test]
    fn the_contract_example_is_read() {
        let text = std::fs::read_to_string(concat!(env!("CARGO_MANIFEST_DIR"), "/../../schema/examples/state-eml-running.json")).unwrap();
        let p = parse(&text).expect("a running state");
        assert_eq!((p.stage.as_str(), p.done_bytes, p.total_bytes, p.files_total), ("checking", 13_194_139_533, 52_183_056_384, 55));
        assert!(p.message.starts_with("Preparing the game data for EML mods: 12.3 GiB of 48.6 GiB."), "{}", p.message);
    }

    #[test]
    fn a_kiln_mode_startup_pass_says_its_step() {
        let text = std::fs::read_to_string(concat!(env!("CARGO_MANIFEST_DIR"), "/../../schema/examples/state-eml-startup.json")).unwrap();
        let p = parse(&text).unwrap();
        assert_eq!(p.message, "Preparing ShroudForge mods for Kiln's interpreter: checking the prepared game data (3 of 5).");
        assert_eq!((p.stage.as_str(), p.step.as_deref(), p.step_number, p.steps), ("startup", Some("verify"), 3, 5));
        for (step, words) in [("checking", "checking the game files"), ("mods", "running the mods"), ("write", "writing the game data"), ("save", "saving the result")] {
            let p = parse(&text.replace("\"verify\"", &format!("\"{step}\""))).unwrap();
            assert!(p.message.contains(words), "{step}: {}", p.message);
        }
        // a step this launcher does not know, or no numbers: the head alone, or the step without them
        assert_eq!(parse(&text.replace("\"verify\"", "\"later\"")).unwrap().message, "Preparing ShroudForge mods for Kiln's interpreter.");
        assert_eq!(parse(&text.replace("\"stepNumber\": 3", "\"stepNumber\": 0")).unwrap().message,
            "Preparing ShroudForge mods for Kiln's interpreter: checking the prepared game data.");
        // the EML stages carry no step
        assert_eq!(parse(&running("mods", 0, 0, 0)).unwrap().step, None);
    }

    #[test]
    fn an_eml_startup_pass_keeps_its_own_wording() {
        let text = std::fs::read_to_string(concat!(env!("CARGO_MANIFEST_DIR"), "/../../schema/examples/state-eml-startup.json")).unwrap()
            .replace("Preparing ShroudForge mods for Kiln's interpreter", "Preparing the game data for EML mods");
        assert_eq!(parse(&text).unwrap().message, "Preparing the game data for EML mods: checking the prepared game data (3 of 5).");
    }

    #[test]
    fn size_formatting_uses_binary_units() {
        assert_eq!(size(0), "0 B");
        assert_eq!(size(1023), "1023 B");
        assert_eq!(size(1024), "1.0 KiB");
        assert_eq!(size(1536), "1.5 KiB");
        assert_eq!(size(48 << 30), "48.0 GiB");
    }

    #[test]
    fn only_a_running_format_1_file_with_progress_counts() {
        assert!(parse(r#"{"format":1,"phase":"done","mods":[]}"#).is_none());
        assert!(parse(r#"{"format":1,"phase":"running","mods":[]}"#).is_none(), "no progress object");
        assert!(parse(&running("checking", 1, 2, 0).replace("\"format\":1", "\"format\":2")).is_none());
        assert!(parse("not json").is_none());
        assert!(parse(&format!("\u{feff}{}", running("mods", 0, 0, 0))).is_some());
        // an older kiln-eml (no progress fields) and a newer one (more fields) both read
        assert!(parse(r#"{"format":1,"phase":"idle"}"#).is_none());
        assert!(parse(&running("mods", 0, 0, 0).replace("\"stageMs\"", "\"somethingNew\":true,\"stageMs\"")).is_some());
    }

    #[test]
    fn a_running_file_counts_only_for_the_game_that_runs_now_and_only_while_it_is_fresh() {
        let now = SystemTime::now();
        let ago = |s: u64| now - Duration::from_secs(s);
        // written 2 s ago, the game started 60 s ago
        assert!(is_current(Some(ago(2)), Some(ago(60)), now));
        // left by an earlier start: written before this process began (2 s of slack for file times)
        assert!(!is_current(Some(ago(10)), Some(ago(5)), now));
        assert!(is_current(Some(ago(6)), Some(ago(5)), now), "within the slack");
        // stale: the process runs but nothing wrote the file for a while (stuck, or the writer died)
        assert!(!is_current(Some(ago(20)), Some(ago(60)), now));
        // unknown process start: the freshness decides; unknown write time: not current
        assert!(is_current(Some(ago(3)), None, now));
        assert!(!is_current(None, Some(ago(60)), now));
        // a clock that went back a little: not stale
        assert!(is_current(Some(now + Duration::from_secs(1)), Some(ago(60)), now));
    }

    #[test]
    fn a_startup_pass_file_counts_through_the_steps_that_move_no_bytes() {
        // kiln-eml's startup pass writes the file when a step begins and while it moves bytes, never on a timer: the wait
        // for kiln-sfi's terminal (up to 20 s), the packages' startup code or a slow flush leave it unwritten for longer
        // than STALE, and the launch status keeps naming the step meanwhile
        let now = SystemTime::now();
        let ago = |s: u64| now - Duration::from_secs(s);
        assert!(is_current_within(Some(ago(40)), Some(ago(60)), now, stale_after("startup")));
        assert_eq!(STARTUP_STALE, Duration::from_secs(120));
        assert!(is_current_within(Some(ago(120)), Some(ago(200)), now, stale_after("startup")));
        // the EML stages keep theirs: that writer touches the file every second
        assert_eq!(stale_after("mods"), STALE);
        assert!(!is_current_within(Some(ago(40)), Some(ago(60)), now, stale_after("checking")));
        // a pass that wrote nothing for longer than the start timeline waits for the early plugins: stale too
        assert!(!is_current_within(Some(ago(130)), Some(ago(200)), now, stale_after("startup")));
        // and never a file of an earlier start
        assert!(!is_current_within(Some(ago(40)), Some(ago(30)), now, stale_after("startup")));
    }
}
