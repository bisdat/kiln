// SPDX-License-Identifier: MIT
//! Tauri commands: the check before Play (`kiln_manage::sfi_launch`). `launch_prepare` runs a start up to
//! its check before the play-once flag and answers its warnings with a one-start token, `launch_confirm` starts it with
//! that token (through launch_play's own steps, commands/launch.rs), `launch_cancel` drops the token. The dialog between
//! them is the window's; launch_play itself starts nothing whose warnings nobody acknowledged.
//!
//! The warnings need
//! kiln-manage's `sfi-scan` (this crate's feature `sfi`); a build without has none, and these commands then start
//! as launch_play does.

use crate::state::{blocking, dirs};
use kiln_manage::Error;
use kiln_manage::launch::{LaunchRequest, LaunchResult};
use kiln_manage::sfi_launch::{self, Book, Preflight};
use tauri::AppHandle;

/// `launch_prepare`: the start of `request` run up to its check (the active profile written again, or the play-once
/// profile written), nothing opened or started, no flag, no backup, no lock kept. The preflight: the warnings to show
/// (empty: no dialog), and the token `launch_confirm` takes from this window only.
#[tauri::command]
pub async fn launch_prepare(
    window: tauri::Window,
    game_id: String,
    request: LaunchRequest,
) -> Result<Preflight, Error> {
    let caller = window.label().to_owned();
    blocking(move || {
        let g = crate::state::game(&game_id)?;
        crate::state::refuse_running(&g)?;
        let dirs = dirs()?;
        // the install as launch_confirm will start it (a direct start's record follows its prefix first)
        let g = if kiln_manage::direct::mode_of(&dirs, &g) == kiln_manage::direct::StartMode::Direct
        {
            kiln_manage::game::sync_start(&dirs, &g, &crate::state::steam_roots()).unwrap_or(g)
        } else {
            g
        };
        sfi_launch::prepare(Book::global(), &dirs, &g, &request, Some(&caller))
    })
    .await
}

/// `launch_confirm`: the prepared start, checked again right before its flag. A change since the dialog rejects with
/// `consent_required` and the new preflight in `detail.preflight` (nothing started); a token that is used, canceled,
/// replaced, expired, or another game's, start's or window's rejects with `refused`.
#[tauri::command]
pub async fn launch_confirm(
    app: AppHandle,
    window: tauri::Window,
    game_id: String,
    token: String,
    request: LaunchRequest,
) -> Result<LaunchResult, Error> {
    let caller = window.label().to_owned();
    crate::commands::launch::start_game(app, game_id, move |dirs, g, starter| {
        sfi_launch::confirm(
            Book::global(),
            dirs,
            g,
            &token,
            &request,
            Some(&caller),
            None,
            starter,
        )
    })
    .await
}

/// `launch_cancel`: the dialog was closed; the token goes (true: there was one for this game).
#[tauri::command]
pub async fn launch_cancel(game_id: String, token: String) -> Result<bool, Error> {
    Ok(Book::global().cancel(&game_id, &token))
}

/// `eml_data_base`: the BASE "Restore the game's data" takes a kiln-mode game back to, by its exact label
/// (`kiln_manage::restore_data::base_origin`; read only, it hashes the backups). None: not kiln mode, or Kiln wrote
/// none of the game's data.
#[tauri::command]
pub async fn eml_data_base(game_id: String) -> Result<Option<kiln_manage::restore_data::BaseOrigin>, Error> {
    blocking(move || kiln_manage::restore_data::base_origin(&crate::state::game(&game_id)?.path)).await
}

#[cfg(test)]
mod tests {
    use crate::commands::launch::with_starter;
    use kiln_manage::game::{GameInstall, GameKind, PathMode, Platform};
    use kiln_manage::launch::{LaunchMode, LaunchRequest};
    use kiln_manage::paths::DataDirs;
    use kiln_manage::sfi_launch::{self, Book, TOKEN_TTL};
    use std::cell::Cell;
    use std::path::{Path, PathBuf};

    /// A game folder with Kiln (no profile activated: the legacy layout), its data folder, and its install record;
    /// `kiln`: kiln.json selects Kiln's interpreter (the hold lifted) and the interpreter is there.
    fn game(tag: &str, kiln: bool) -> (PathBuf, DataDirs, GameInstall) {
        let root = std::env::temp_dir().join(format!("kl-sfi-launch-{tag}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        let game = root.join("game");
        std::fs::create_dir_all(game.join("kiln/plugins")).unwrap();
        std::fs::write(game.join("enshrouded.exe"), b"MZ").unwrap();
        if kiln {
            std::fs::write(
                game.join("kiln/kiln.json"),
                br#"{"preview": ["kiln-sfi.interpreter"], "shroudforge": {"backend": "kiln"}}"#,
            )
            .unwrap();
            std::fs::write(game.join("kiln/plugins/kiln-sfi.dll"), b"interpreter").unwrap();
        }
        let inst = GameInstall {
            id: format!("g-{tag}"),
            kind: GameKind::Client,
            path: game,
            exe: "enshrouded.exe".into(),
            app_id: kiln_manage::CLIENT_APP_ID,
            library: None,
            platform: Platform::Proton,
            prefix: None,
            build_id: None,
            exe_sha256: None,
            steam_flavor: None,
            path_mode: PathMode::Copy,
            source: "manual".into(),
        };
        (root.clone(), DataDirs::at(root.join("data")), inst)
    }

    fn cmd() -> kiln_manage::direct::DirectCommand {
        kiln_manage::direct::DirectCommand {
            program: "umu-run-never-started".into(),
            args: Vec::new(),
            cwd: ".".into(),
            env: Vec::new(),
            env_remove: Vec::new(),
            runner: None,
            prefix: None,
        }
    }

    const REQ: LaunchRequest = LaunchRequest {
        mode: LaunchMode::Profile,
        profile_id: None,
        skip_backup: true,
    };

    /// The launcher's starter under KILN_LAUNCH_DRY_RUN asks neither Steam nor the exe, for launch_play's start and for a
    /// confirmed one alike; without it, each start asks exactly once.
    #[test]
    fn dry_run_never_asks_steam_or_the_exe() {
        let (root, dirs, g) = game("dry", false);
        let (opened, spawned) = (Cell::new(0), Cell::new(0));
        let open = |_: &str| {
            opened.set(opened.get() + 1);
            Ok(())
        };
        let spawn = |_: &kiln_manage::direct::DirectCommand, _: &Path| {
            spawned.set(spawned.get() + 1);
            Ok(())
        };
        let c = cmd();
        let log = root.join("start.log");
        let book = Book::new(TOKEN_TTL);
        for dry in [true, false] {
            for direct in [None, Some((&c, log.as_path()))] {
                with_starter(&g.id, direct, dry, &open, &spawn, &|_, _| {}, |s| {
                    sfi_launch::play_interactive(&book, &dirs, &g, &REQ, Some("main"), Some(&[]), s)
                })
                .unwrap();
                let p = sfi_launch::prepare(&book, &dirs, &g, &REQ, Some("main")).unwrap();
                assert!(p.warnings.is_empty(), "{:?}", p.warnings);
                with_starter(&g.id, direct, dry, &open, &spawn, &|_, _| {}, |s| {
                    sfi_launch::confirm(
                        &book,
                        &dirs,
                        &g,
                        &p.token,
                        &REQ,
                        Some("main"),
                        Some(&[]),
                        s,
                    )
                })
                .unwrap();
            }
            let want = if dry { 0 } else { 2 };
            assert_eq!((opened.get(), spawned.get()), (want, want), "dry run {dry}");
        }
        let _ = std::fs::remove_dir_all(&root);
    }

    /// A kiln start with a warning (no launch plan: the core starts no ShroudForge mod): launch_play's start opens
    /// nothing and answers the preflight; its token confirms from the window that asked only, once.
    #[cfg(feature = "sfi")]
    #[test]
    fn launch_plays_start_opens_nothing_until_the_window_that_asked_confirms() {
        let (root, dirs, g) = game("kiln", true);
        let opened = Cell::new(0);
        let open = |_: &str| {
            opened.set(opened.get() + 1);
            Ok(())
        };
        let spawn = |_: &kiln_manage::direct::DirectCommand, _: &Path| {
            opened.set(opened.get() + 1);
            Ok(())
        };
        let c = cmd();
        let log = root.join("start.log");
        let book = Book::new(TOKEN_TTL);
        for direct in [None, Some((&c, log.as_path()))] {
            let start = |token: Option<&str>, caller: &str| {
                with_starter(
                    &g.id,
                    direct,
                    false,
                    &open,
                    &spawn,
                    &|_, _| {},
                    |s| match token {
                        None => sfi_launch::play_interactive(
                            &book,
                            &dirs,
                            &g,
                            &REQ,
                            Some(caller),
                            Some(&[]),
                            s,
                        ),
                        Some(t) => sfi_launch::confirm(
                            &book,
                            &dirs,
                            &g,
                            t,
                            &REQ,
                            Some(caller),
                            Some(&[]),
                            s,
                        ),
                    },
                )
            };
            let e = start(None, "main").unwrap_err();
            let p = sfi_launch::preflight_of(&e)
                .cloned()
                .unwrap_or_else(|| panic!("{e:?}"));
            assert_eq!(p["warnings"][0]["code"], "plan-missing", "{p:#}");
            assert_eq!(
                opened.get(),
                0,
                "{direct:?}: nothing opened before the confirm"
            );
            let token = p["token"].as_str().unwrap().to_owned();
            let e = start(Some(&token), "deck").unwrap_err();
            assert_eq!(e.detail.unwrap()["token"], "caller");
            let e = start(None, "main").unwrap_err();
            let token = sfi_launch::preflight_of(&e).unwrap()["token"]
                .as_str()
                .unwrap()
                .to_owned();
            start(Some(&token), "main").unwrap();
            assert_eq!(opened.get(), 1, "{direct:?}");
            assert_eq!(
                start(Some(&token), "main").unwrap_err().detail.unwrap()["token"],
                "unknown"
            );
            assert_eq!(opened.get(), 1);
            opened.set(0);
        }
        let _ = std::fs::remove_dir_all(&root);
    }
}
