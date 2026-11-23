// SPDX-License-Identifier: MIT
//! Which loader a game's ShroudForge-format mods go to: main's routes (ShroudForge's own loader, Kiln's
//! hosted or installed ShroudForge), or Kiln's interpreter (kiln-sfi) when kiln.json selects it.
//!
//! One decision, [`decide`]: pure, over [`Inputs`] that [`route_of`] reads once per operation. The operations of
//! [`crate::engine_mods`] and [`crate::own_loaders`] that place, switch, adopt, set a setting, recover or remove a copy
//! take that one [`Route`] down to every folder they name; their `*_routed` forms take it from the caller (the
//! activation, with the selection it froze into the launch plan).
//!
//! - **The selection**, read as the core reads it at boot ([`crate::sfi_backend::resolve_game`], the reader the launch
//!   plan producer uses too): only a selected `kiln` routes to the interpreter. Hosted, installed, an invalid setting
//!   and a `kiln` the release hold refuses keep main's routes byte for byte. The core refuses the last two at boot
//!   (every ShroudForge backend stands aside); the plan producer and the pre-Play warning say so.
//! - **ShroudForge's own loader** (a ShroudForge file under a proxy name next to the game, or a `kiln/chain.ini` or
//!   `dinput8-chain.ini` line): it wins, and both Kiln backends stand aside (kiln-sfi checks again when it starts), so
//!   its mods keep main's route into the mods folder ShroudForge's own config names.
//! - **EML-R's own loader** also blocks Kiln's interpreter because EML-R may run packages in `<game>/mods`.
//! - **The interpreter's route** ([`Route::Interpreter`]) changes two things and nothing else:
//!   - ShroudForge's mods folder is `<game>/mods` ([`Route::sf_mods`]), the folder kiln-sfi and the launch plan read.
//!     Nothing below `<game>/shroudforge` is read or written to place, switch, adopt, set a setting, recover or remove
//!     a copy: no install, config, state or status probe of ShroudForge's.
//!   - Kiln's interpreter carries the profile's EML mods where Kiln's hosted ShroudForge did (own_loaders
//!     `Via::Interpreter`), when the profile makes it run them (a ShroudForge mod switched on, an EML package that
//!     needs ShroudForge's runtime, ShroudForge drop-ins with a switched-on package in `<game>/mods`), and only while
//!     it can run: `KILN_SHROUDFORGE=0` stands it aside (own_loaders' note) and without
//!     `kiln/plugins/kiln-sfi.dll` nothing runs them ([`Runner`]). An EML package that needs ShroudForge's runtime is
//!     then not placed at all, so no startup data is applied without its runtime.
//!
//!   Every other loader's route is main's: EML's, EMF's (its Lua mods too), XHL-Core's, Greeneye's, Flame's,
//!   Shroudtopia's, SET's, Enshwick's, shroudlink's (its core too), Ember's, Enshrouded ModLoader's, the game-folder
//!   natives (`crate::game_files`) and Copy mode's paths.
//!
//! Without the `sfi-scan` feature [`route_of`] reads nothing and every route is main's.

use crate::sfi_backend::Backend;
use std::path::{Path, PathBuf};

/// Kiln's interpreter, game-relative (the core's plugins folder; native/core/src/launch.cpp `is_dialect_dll`).
pub const INTERPRETER_PLUGIN: &str = "kiln/plugins/kiln-sfi.dll";
/// The folder the interpreter and the launch plan read packages from, game-relative: `<game>/mods` (kiln-sfi's
/// `plan_source`, [`crate::sfi_plan`]; not the core's `KILN_PATH_MODS`, which is `<game>/kiln/mods`).
pub const INTERPRETER_MODS: &str = "mods";

/// What the route of a game's ShroudForge-format mods depends on.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Inputs {
    /// The backend the next start gets, as the core decides it from kiln.json ([`crate::sfi_backend::Resolution`]
    /// `selected`); None when the setting is refused (invalid, or `kiln` while the interpreter is held).
    pub selected: Option<Backend>,
    /// `KILN_SHROUDFORGE=0`: every ShroudForge backend stands aside, the interpreter too.
    pub env_off: bool,
    /// A loader that may run ShroudForge mods in the game.
    pub own_loader: Option<OwnLoader>,
    /// [`INTERPRETER_PLUGIN`] is there.
    pub interpreter: bool,
}

/// Where a game's ShroudForge-format mods go.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Route {
    /// Main's routes, unchanged.
    Legacy(Legacy),
    /// Kiln's interpreter has them, in `<game>/mods`.
    Interpreter(Runner),
}

/// Why a route is main's.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Legacy {
    /// Built without the `sfi-scan` feature: no selection is read.
    Off,
    /// kiln.json selects hosted or installed ShroudForge (or none, the default: hosted).
    Selected(Backend),
    /// kiln.json's setting is refused at boot (an invalid value, or `kiln` while the interpreter is held).
    Refused,
    /// kiln.json selects the interpreter, and ShroudForge's own loader (this file) is installed: it runs them.
    OwnLoader(String),
    /// kiln.json selects the interpreter, and EML-R's game-folder proxy may run those mods itself.
    EmlR(String),
}

/// A loader that blocks the interpreter, with its game-relative file.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum OwnLoader {
    ShroudForge(String),
    EmlR(String),
}

pub const EML_R_BLOCK_REASON: &str =
    "EML-R's own loader is in the game folder. It may run these mods itself, so Kiln's interpreter stands aside.";

/// Whether the interpreter runs this start.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Runner {
    Ready,
    /// `KILN_SHROUDFORGE=0` stands it aside.
    EnvOff,
    /// [`INTERPRETER_PLUGIN`] is not there.
    Missing,
}

/// The route for these inputs.
pub fn decide(i: &Inputs) -> Route {
    match i.selected {
        Some(Backend::Kiln) => {}
        Some(b) => return Route::Legacy(Legacy::Selected(b)),
        None => return Route::Legacy(Legacy::Refused),
    }
    if let Some(loader) = &i.own_loader {
        return Route::Legacy(match loader {
            OwnLoader::ShroudForge(file) => Legacy::OwnLoader(file.clone()),
            OwnLoader::EmlR(file) => Legacy::EmlR(file.clone()),
        });
    }
    Route::Interpreter(if i.env_off {
        Runner::EnvOff
    } else if !i.interpreter {
        Runner::Missing
    } else {
        Runner::Ready
    })
}

impl Route {
    /// The interpreter's route.
    pub fn interpreter(&self) -> bool {
        matches!(self, Route::Interpreter(_))
    }

    /// The interpreter's route, and it runs this start.
    pub fn runs(&self) -> bool {
        *self == Route::Interpreter(Runner::Ready)
    }

    /// ShroudForge's mods folder for this route: `<game>/mods` for the interpreter, else the folder ShroudForge's own
    /// config names ([`crate::shroudforge::mods_dir`], read now, as main reads it).
    pub fn sf_mods(&self, game: &Path) -> PathBuf {
        match self {
            Route::Interpreter(_) => game.join(INTERPRETER_MODS),
            Route::Legacy(_) => crate::shroudforge::mods_dir(game),
        }
    }

    /// What an EML package that needs ShroudForge's runtime says when nothing runs it on this route; None: main's
    /// sentence. (While the interpreter runs, such a package always has it: own_loaders `find_routed`.)
    pub fn runtime_note(&self) -> Option<&'static str> {
        match self {
            Route::Interpreter(Runner::EnvOff) => Some(crate::own_loaders::RUNTIME_ENV_OFF_NOTE),
            Route::Interpreter(_) => Some(INTERPRETER_MISSING_NOTE),
            Route::Legacy(_) => None,
        }
    }
}

/// [`Route::runtime_note`] while kiln.json selects the interpreter and it is not installed.
pub const INTERPRETER_MISSING_NOTE: &str = "needs ShroudForge's runtime: kiln.json selects Kiln's ShroudForge \
     interpreter (shroudforge.backend: \"kiln\"), which is not installed (kiln/plugins/kiln-sfi.dll), so nothing runs it; \
     its data was not applied, so the game is as without the mod. Install the interpreter, or select another backend, \
     and activate again";

/// The route of the ShroudForge-format mods in `game` (a game or server folder), read now: kiln.json as the core reads
/// it, the game's side from its executable, the environment of this process, and (only when kiln.json selects the
/// interpreter) the own-loader blockers and [`INTERPRETER_PLUGIN`]. Nothing below `<game>/shroudforge` is read.
pub fn route_of(game: &Path) -> Route {
    #[cfg(feature = "sfi-scan")]
    {
        decide(&inputs_of(game))
    }
    #[cfg(not(feature = "sfi-scan"))]
    {
        let _ = game;
        Route::Legacy(Legacy::Off)
    }
}

/// The [`Inputs`] of `game` (see [`route_of`]).
#[cfg(feature = "sfi-scan")]
pub fn inputs_of(game: &Path) -> Inputs {
    use crate::manifest::RunSide;
    let side = if game.join(crate::game::SERVER_EXE).is_file() {
        RunSide::Server
    } else {
        RunSide::Client
    };
    let r = crate::sfi_backend::resolve_game(game, side, &crate::sfi_backend::Env::from_process());
    inputs_from(game, &r)
}

/// The [`Inputs`] of `game` whose next start kiln.json resolves to `selection`: the answer the caller already has, read
/// once with the side and the environment it knows (an activation: the install's kind, the launch-plan producer's
/// answer, `crate::sfi_plan::produce_resolved`). Only a selected `kiln` reads more (own-loader blockers,
/// [`INTERPRETER_PLUGIN`]).
#[cfg(feature = "sfi-scan")]
pub fn inputs_from(game: &Path, selection: &crate::sfi_backend::Resolution) -> Inputs {
    // hosted and installed starts read nothing more than kiln.json
    let kiln = selection.selected == Some(Backend::Kiln);
    Inputs {
        selected: selection.selected,
        env_off: selection.env_off_note.is_some(),
        own_loader: kiln
            .then(|| crate::own_loaders::interpreter_blocker(game))
            .flatten(),
        interpreter: kiln && game.join(INTERPRETER_PLUGIN).is_file(),
    }
}

/// The route of `game` for `selection` ([`inputs_from`]): one decision an operation takes down to every step, and the
/// same answer the launch-plan producer is given.
#[cfg(feature = "sfi-scan")]
pub fn route_from(game: &Path, selection: &crate::sfi_backend::Resolution) -> Route {
    decide(&inputs_from(game, selection))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn inputs(
        selected: Option<Backend>,
        env_off: bool,
        own_loader: Option<&str>,
        interpreter: bool,
    ) -> Inputs {
        Inputs {
            selected,
            env_off,
            own_loader: own_loader.map(|file| OwnLoader::ShroudForge(file.into())),
            interpreter,
        }
    }

    #[test]
    fn every_input_has_exactly_one_route() {
        use Backend::*;
        let own = Some("dbghelp.dll");
        let legacy = |l| Route::Legacy(l);
        // (selected, KILN_SHROUDFORGE=0, ShroudForge's own loader, kiln-sfi.dll) -> route
        let mut rows = Vec::new();
        for env_off in [false, true] {
            for loader in [None, own] {
                for interpreter in [false, true] {
                    rows.push((
                        inputs(Some(Hosted), env_off, loader, interpreter),
                        legacy(Legacy::Selected(Hosted)),
                    ));
                    rows.push((
                        inputs(Some(Installed), env_off, loader, interpreter),
                        legacy(Legacy::Selected(Installed)),
                    ));
                    rows.push((
                        inputs(None, env_off, loader, interpreter),
                        legacy(Legacy::Refused),
                    ));
                    let kiln = match (loader, env_off, interpreter) {
                        (Some(f), ..) => legacy(Legacy::OwnLoader(f.into())),
                        (None, true, _) => Route::Interpreter(Runner::EnvOff),
                        (None, false, false) => Route::Interpreter(Runner::Missing),
                        (None, false, true) => Route::Interpreter(Runner::Ready),
                    };
                    rows.push((inputs(Some(Kiln), env_off, loader, interpreter), kiln));
                }
            }
        }
        assert_eq!(rows.len(), 32);
        for (i, want) in &rows {
            assert_eq!(&decide(i), want, "{i:?}");
        }
        // only a selected kiln with no ShroudForge loader of its own reaches the interpreter, and only one of those runs
        assert_eq!(
            rows.iter().filter(|(i, _)| decide(i).interpreter()).count(),
            4
        );
        assert_eq!(rows.iter().filter(|(i, _)| decide(i).runs()).count(), 1);
    }

    /// Without the `sfi-scan` feature nothing is read: a game whose kiln.json selects the interpreter keeps main's routes.
    #[cfg(not(feature = "sfi-scan"))]
    #[test]
    fn without_the_feature_every_route_is_mains() {
        let game = std::env::temp_dir().join(format!("km-sfi-routing-off-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&game);
        std::fs::create_dir_all(game.join("kiln/plugins")).unwrap();
        std::fs::write(
            game.join("kiln/kiln.json"),
            r#"{"preview": ["kiln-sfi.interpreter"], "shroudforge": {"backend": "kiln"}}"#,
        )
        .unwrap();
        std::fs::write(game.join(INTERPRETER_PLUGIN), b"interpreter").unwrap();
        assert_eq!(route_of(&game), Route::Legacy(Legacy::Off));
        assert_eq!(
            route_of(&game).sf_mods(&game),
            crate::shroudforge::mods_dir(&game)
        );
        let _ = std::fs::remove_dir_all(&game);
    }

    #[test]
    fn the_interpreter_names_its_own_folder_and_notes() {
        let game = Path::new("/g");
        for r in [Runner::Ready, Runner::EnvOff, Runner::Missing] {
            assert_eq!(Route::Interpreter(r).sf_mods(game), game.join("mods"));
        }
        assert_eq!(
            Route::Interpreter(Runner::Ready).runtime_note(),
            Some(INTERPRETER_MISSING_NOTE)
        );
        assert_eq!(
            Route::Interpreter(Runner::Missing).runtime_note(),
            Some(INTERPRETER_MISSING_NOTE)
        );
        assert_eq!(
            Route::Interpreter(Runner::EnvOff).runtime_note(),
            Some(crate::own_loaders::RUNTIME_ENV_OFF_NOTE)
        );
        for l in [
            Legacy::Off,
            Legacy::Selected(Backend::Hosted),
            Legacy::Refused,
            Legacy::OwnLoader("winmm.dll".into()),
        ] {
            assert_eq!(Route::Legacy(l).runtime_note(), None);
        }
    }
}
