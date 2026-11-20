//! Tauri commands: cooking mods before the game starts (kiln_manage::cook; docs/kiln/launcher/app.md "Cooking mods").
//! A mod with a cook recipe has files that are made on this PC from this game (models, animations, sounds). `cook_plan`
//! says which mods of the game's profile need a cook now; `cook_run` cooks them (or one mod: "Cook again") as a task,
//! with each line the tools print as its progress. A mod whose cook fails is reported as not cooked: the others are
//! still cooked, and the game still starts.

use super::TaskId;
use super::app::{spawn_task, to_value};
use crate::state::{blocking, dirs};
use kiln_manage::Error;
use kiln_manage::cook::{self, CookJob, CookRun};
use kiln_manage::cooktools::Locate;
use tauri::{AppHandle, Manager};

/// The Python the Windows bundle carries (`<resources>/python/python.exe`, tools/launcher/build-windows.sh). None on
/// Linux and in a development build: the cook tools then run on the system's python3 (or `$KILN_PYTHON`).
fn bundled_python(app: &AppHandle) -> Option<std::path::PathBuf> {
    app.path().resolve("python/python.exe", tauri::path::BaseDirectory::Resource).ok().filter(|p| p.is_file())
}

/// The mods of the game's profile that have something to cook, each with whether it needs a cook now (`reason`:
/// never cooked for this game, the mod changed, the game was updated, its last cook failed, its cooked files are
/// gone) and how its last cook ended (`last`). Reads only: no tool is started.
#[tauri::command]
pub async fn cook_plan(game_id: String) -> Result<Vec<CookJob>, Error> {
    blocking(move || cook::plan(&crate::state::game(&game_id)?)).await
}

/// Cooks the mods of the game's profile that need it, or with `mod_id` that one mod whatever its record says ("Cook
/// again"), as one task (result: CookRun, one entry per mod cooked; kiln_manage::cook::cook_due). The active profile
/// is written again first when it changed since its activation (`activation` in the result), so Play cooks the mods
/// it then starts. Each due mod is cooked from nothing, every recipe: what made it due may be the game, which a
/// recipe's own check does not read. The game's lock is held meanwhile: no start of the game, no other cook of it.
/// Refused while the game runs, and right after any start of it (Play's cook and "Cook again" alike). A tool that hangs is stopped within the
/// launcher's limits, and the task's Cancel stops the tool that runs: either with every process it started. A mod's
/// failed cook is in the result with the tools' last lines, never an error of the task: the others are cooked, and
/// the caller starts the game all the same. When no tool can run (no Python, no zstd), every due mod is reported as
/// not cooked with the one sentence that says what to install.
#[tauri::command]
pub async fn cook_run(app: AppHandle, game_id: String, mod_id: Option<String>) -> Result<TaskId, Error> {
    let g = blocking({ let id = game_id.clone(); move || crate::state::game(&id) }).await?;
    let python = bundled_python(&app);
    let title = match &mod_id {
        Some(id) => format!("Cook {id}"),
        None => "Cook mods".to_string(),
    };
    Ok(spawn_task(&app, "cook_run", title, move |p| {
        cook::refuse_running(super::engines::game_running(&g)?)?;
        let dirs = dirs()?;
        // the tools: the copy this launcher carries, unpacked once under the data folder; never a checkout
        let locate = Locate::from_process(None, Some(dirs.root.join("cooktools")), python);
        // the task's progress takes the tools' lines, and its Cancel stops the tool that runs (with what it started)
        let p = std::cell::RefCell::new(p);
        // Play's cook is the start's first step (launch_begin): each mod's time is its own (start_progress)
        let play = mod_id.is_none();
        let run = cook::cook_due(&dirs, &g, mod_id.as_deref(), &locate, &mut |i, n, job, line| {
            if play {
                let key = kiln_manage::start_progress::cook_key(&job.id, &job.identity);
                super::launch::track(&g.id, |t| t.cook_event(super::launch::now_ms(), i, n, &key, &job.name, line));
            }
            let said = match line {
                None => format!("Cooking {} ({} of {n})", job.name, i + 1),
                Some(l) => format!("{}: {l}", job.name),
            };
            p.borrow_mut().report(Some(i as f32 / n.max(1) as f32), &said);
        }, &|| p.borrow().canceled());
        if play {
            super::launch::track(&g.id, |t| t.cook_done(super::launch::now_ms()));
        }
        let run: CookRun = run?;
        if !run.mods.is_empty() {
            p.borrow_mut().report(Some(1.0), &match run.failed().count() {
                0 => format!("{} cooked", run.mods.len()),
                f => format!("{} cooked, {f} not", run.mods.len() - f),
            });
        }
        // Play's next step: the game data Mod.OnBoot reads, once per game build (kiln_manage::boot_snapshot; well under a
        // second when it is there), only when an enabled mod of this start asks for the boot phase ("no mod needs the boot
        // snapshot" otherwise: nothing runs). Never an error and never longer than its limit: without it the game starts
        // all the same and kiln.boot says why (`bootSnapshot` in the result: shell/cook.ts says it in one line).
        let snapshot = play.then(|| boot_snapshot_step(&g, &kiln_manage::boot_snapshot::boot_mods(&g), &locate, &p));
        let mut v = to_value(run)?;
        if let (Some(s), Some(o)) = (snapshot, v.as_object_mut()) {
            o.insert("bootSnapshot".into(), to_value(s)?);
        }
        Ok(v)
    }))
}

/// The boot game-data step of a start (kiln_manage::boot_snapshot): made, or found, when one of `mods` (the start's
/// enabled mods that ask for the boot phase) needs it; skipped without running anything otherwise. Its lines are the
/// task's progress, and the task's Cancel stops the tool. Never an error, never longer than its limit.
/// The start's own timeline follows it too (kiln_manage::start_progress, when Play began one: `launch_begin`): while the
/// tool checks, the time after the cook is not a stalled start; once it builds a snapshot, the start shows its
/// `boot-snapshot` step with the tool's lines, ended when the tool ends. A snapshot no mod needs, or one found current,
/// shows no step.
fn boot_snapshot_step(g: &kiln_manage::game::GameInstall, mods: &[String], locate: &Locate,
                      p: &std::cell::RefCell<&mut dyn kiln_manage::task::Progress>) -> kiln_manage::boot_snapshot::BootSnapshot {
    use kiln_manage::boot_snapshot as bs;
    use super::launch::{now_ms, track};
    let s = bs::make_if_needed(mods, || {
        track(&g.id, |t| t.boot_snapshot_check());
        let tools = kiln_manage::cooktools::locate(locate).map_err(|e| e.message);
        let s = bs::make_with(tools, &g.path, false, Some(bs::LAUNCHER_LIMIT), &mut |l| {
            track(&g.id, |t| t.boot_snapshot_line(now_ms(), l));
            p.borrow_mut().report(None, &format!("Game data for OnBoot mods ({}, once per game build): {l}", bs::SIZE_TEXT));
        }, &|| p.borrow().canceled());
        track(&g.id, |t| t.boot_snapshot_done(now_ms()));
        s
    });
    p.borrow_mut().report(Some(1.0), &s.summary());
    s
}

/// Play once with another profile (launch mode `once`): the same boot game-data step Play runs after its cook, for the
/// mods that single start loads (kiln_manage::boot_snapshot::boot_mods_once: the profile's enabled Kiln mods and, by
/// its rule, the Kiln drop-ins), before `launch_play` writes that profile's file. A task whose result is the
/// BootSnapshot (`skipped` when no mod of that start needs it). A single start cooks nothing. Refused while the game
/// runs; `not_found` for an unknown profile. A failed or missing producer is in the result, never the task's error:
/// the start goes on and kiln.boot says why the game data is unavailable.
#[tauri::command]
pub async fn boot_snapshot_run(app: AppHandle, game_id: String, profile_id: String) -> Result<TaskId, Error> {
    let g = blocking({ let id = game_id.clone(); move || crate::state::game(&id) }).await?;
    let python = bundled_python(&app);
    Ok(spawn_task(&app, "boot_snapshot_run", "Game data for OnBoot mods".to_string(), move |p| {
        cook::refuse_running(super::engines::game_running(&g)?)?;
        let dirs = dirs()?;
        let mods = kiln_manage::boot_snapshot::boot_mods_once(&dirs, &g, &profile_id)?;
        let locate = Locate::from_process(None, Some(dirs.root.join("cooktools")), python);
        let p = std::cell::RefCell::new(p);
        to_value(boot_snapshot_step(&g, &mods, &locate, &p))
    }))
}
