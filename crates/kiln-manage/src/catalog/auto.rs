//! Kiln's own upkeep of the client catalog for the launcher (Pack Studio's pickers, fields and lint): the catalog is
//! made when it is missing or not current (the game was updated, or [`super::RECIPE`] changed), one check at a time,
//! and the outcome is kept for the UI. Refusals are reported and never retried by themselves: Steam updating the game
//! (`steam_updating`), exe and data from two builds (`conflict`), a destination inside the game folder (`refused`).
//! Threads are the caller's: [`AutoCatalog::begin`] marks a check as running (answers false when one runs already), then
//! [`AutoCatalog::run`] does the work on the caller's thread. docs/kiln/launcher/pack-studio.md.

use super::{BuildOptions, build, file_for, kfc_version, side_of, status};
use crate::task::Progress;
use crate::{Error, Result};
use serde::Serialize;
use std::path::{Path, PathBuf};
use std::sync::Mutex;
use std::time::SystemTime;

/// Where the upkeep is.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub enum AutoState {
    /// Not checked yet in this process.
    Idle,
    /// A check (and a build when needed) runs now.
    Building,
    /// The client catalog is current.
    Ready,
    /// It could not be made: `error` says why.
    Failed,
    /// No Enshrouded client to make it from.
    NoGame,
}

/// What the UI shows.
#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct AutoStatus {
    pub state: AutoState,
    /// A client catalog file is there (the pickers use it even when the last rebuild failed).
    pub exists: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub path: Option<PathBuf>,
    /// Ready: whether the last check made the file (false: it was current).
    #[serde(skip_serializing_if = "Option::is_none")]
    pub built: Option<bool>,
    /// Ready and built: how long the build took.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub seconds: Option<f64>,
    /// Failed or NoGame: why (code `steam_updating`, `conflict`, `refused`, `not_found`, ...).
    #[serde(skip_serializing_if = "Option::is_none")]
    pub error: Option<Error>,
}

impl AutoStatus {
    const IDLE: AutoStatus = AutoStatus { state: AutoState::Idle, exists: false, path: None, built: None, seconds: None, error: None };
}

/// What the last check found current: a later check with the same game data and the same file skips reading the
/// catalog again (it is large).
#[derive(Debug, Clone, PartialEq)]
struct Fresh {
    game: PathBuf,
    kfc: String,
    file: PathBuf,
    modified: SystemTime,
}

struct Inner {
    status: AutoStatus,
    fresh: Option<Fresh>,
}

/// One per process (the launcher keeps it in a static).
pub struct AutoCatalog {
    inner: Mutex<Inner>,
}

impl Default for AutoCatalog {
    fn default() -> Self {
        Self::new()
    }
}

fn modified(p: &Path) -> Option<SystemTime> {
    std::fs::metadata(p).and_then(|m| m.modified()).ok()
}

impl AutoCatalog {
    pub const fn new() -> AutoCatalog {
        AutoCatalog { inner: Mutex::new(Inner { status: AutoStatus::IDLE, fresh: None }) }
    }

    fn lock(&self) -> std::sync::MutexGuard<'_, Inner> {
        self.inner.lock().unwrap_or_else(|e| e.into_inner())
    }

    pub fn status(&self) -> AutoStatus {
        self.lock().status.clone()
    }

    /// Marks a check as running. false: one runs already (its outcome comes with [`Self::status`]).
    pub fn begin(&self) -> bool {
        let mut g = self.lock();
        if g.status.state == AutoState::Building {
            return false;
        }
        g.status.state = AutoState::Building;
        true
    }

    /// Ends a begun check with an error found before [`Self::run`] (the data folder held, Steam updating, ...).
    pub fn fail(&self, e: Error, dest: Option<&Path>) -> AutoStatus {
        let file = dest.map(|d| file_for(d, "client"));
        let mut g = self.lock();
        g.fresh = None;
        g.status = AutoStatus {
            state: AutoState::Failed,
            exists: file.as_deref().is_some_and(Path::is_file),
            path: file,
            built: None,
            seconds: None,
            error: Some(e),
        };
        g.status.clone()
    }

    /// The check [`Self::begin`] started: `<dest>/client.json` from the client at `game` (None: no client), made when it
    /// is missing or not current, left alone when it is. Answers the new status (Ready, Failed or NoGame).
    pub fn run(&self, game: Option<&Path>, dest: &Path, p: &mut dyn Progress) -> AutoStatus {
        let file = file_for(dest, "client");
        let Some(game) = game else {
            let mut g = self.lock();
            g.fresh = None;
            g.status = AutoStatus {
                state: AutoState::NoGame,
                exists: file.is_file(),
                path: Some(file),
                built: None,
                seconds: None,
                error: Some(Error::not_found("No Enshrouded client found. Add it in Settings → Games.")),
            };
            return g.status.clone();
        };
        let known = self.lock().fresh.clone();
        let r = self.check(game, dest, &file, known.as_ref(), p);
        let mut g = self.lock();
        match r {
            Ok((built, seconds, fresh)) => {
                g.fresh = fresh;
                g.status = AutoStatus { state: AutoState::Ready, exists: true, path: Some(file), built: Some(built), seconds, error: None };
            }
            Err(e) => {
                g.fresh = None;
                g.status = AutoStatus { state: AutoState::Failed, exists: file.is_file(), path: Some(file), built: None, seconds: None, error: Some(e) };
            }
        }
        g.status.clone()
    }

    /// [`Self::begin`] and [`Self::run`] on this thread; with a check running already, its status.
    pub fn ensure(&self, game: Option<&Path>, dest: &Path, p: &mut dyn Progress) -> AutoStatus {
        if !self.begin() {
            return self.status();
        }
        self.run(game, dest, p)
    }

    /// (built, seconds, what is current now).
    fn check(&self, game: &Path, dest: &Path, file: &Path, known: Option<&Fresh>, p: &mut dyn Progress) -> Result<(bool, Option<f64>, Option<Fresh>)> {
        crate::gameart::refuse_inside_game(dest, game)?;
        if side_of(game) != Some("client") {
            return Err(Error::not_found(format!("{} is not an Enshrouded client folder (no enshrouded.kfc)", game.display())));
        }
        let kfc = kfc_version(game);
        let fresh_now = |kfc: &Option<String>| -> Option<Fresh> {
            Some(Fresh { game: game.to_path_buf(), kfc: kfc.clone()?, file: file.to_path_buf(), modified: modified(file)? })
        };
        // nothing changed since the last check found it current: no need to read the catalog again
        if let (Some(k), Some(now)) = (known, fresh_now(&kfc))
            && *k == now
        {
            return Ok((false, None, Some(now)));
        }
        let current = status(dest, Some(game))?.into_iter().find(|s| s.side == "client").and_then(|s| s.current) == Some(true);
        if current {
            return Ok((false, None, fresh_now(&kfc)));
        }
        // build() refuses while Steam updates the game, and for exe and data of two builds
        let info = build(game, dest, &BuildOptions { force: true, ..Default::default() }, p)?;
        Ok((info.built, Some(info.seconds), fresh_now(&kfc_version(game))))
    }
}
