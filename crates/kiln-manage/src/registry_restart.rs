// SPDX-License-Identifier: MIT
//! What the registry left waiting for a restart (0.2.0-dev.31.14): the rows of `kiln/state/registry.json`'s `refused`
//! list whose reason starts with the stable token `RestartRequired: ` (native/services/content/registry/report.h). A
//! reload, a runtime registry pass or a live edit leaves them: a restart field a reloaded pack changed (it applies at the
//! next start), a field the start never read, a registry section or an entry operation added after the start. `kilnctl
//! status` prints them; the kiln/control `reload <id>` reply names the reloaded mods' own (data.registry), the launcher's
//! Conflicts view lists them.
use crate::{Error, ErrorCode, Result};
use serde::Serialize;
use serde_json::Value;
use std::path::Path;

/// The report, relative to the game folder (or a dedicated server's folder).
pub const REPORT_REL: &str = "kiln/state/registry.json";
/// The stable token a waiting row's reason starts with.
pub const TOKEN: &str = "RestartRequired: ";
/// The stage caps the report's rows, not its bytes (report.h): a real one stays well below this.
const MAX_BYTES: u64 = 16 << 20;

/// One edit that waits for a restart.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct Edit {
    #[serde(rename = "mod")]
    pub mod_id: String,
    pub registry: String,
    pub id: String,
    /// The Kiln field; "(raw path)" for an Advanced-tier raw path (its member is never named).
    pub field: String,
    pub source: String,
    pub reason: String,
}

impl Edit {
    /// Where it is: `items[game:wood].maxStack`, `balance.levelCap` (every entry), else the reason.
    pub fn place(&self) -> String {
        match (self.registry.is_empty(), self.field.is_empty(), self.id.is_empty() || self.id == "*") {
            (true, _, _) => self.reason.clone(),
            (false, true, _) => self.registry.clone(),
            (false, false, true) => format!("{}.{}", self.registry, self.field),
            (false, false, false) => format!("{}[{}].{}", self.registry, self.id, self.field),
        }
    }
}

/// The waiting edits of one report.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize)]
pub struct Waiting {
    /// `counts.restartRequired` (every row, also those the report's cap left out); without it (a report of an older Kiln),
    /// the rows listed.
    pub total: u64,
    pub edits: Vec<Edit>,
    pub written: Option<String>,
}

/// The waiting edits in a report's text.
pub fn parse(text: &str) -> Result<Waiting> {
    let v: Value = serde_json::from_str(text).map_err(|e| Error::new(ErrorCode::Parse, format!("{REPORT_REL}: {e}")))?;
    let s = |row: &Value, key: &str| row.get(key).and_then(Value::as_str).unwrap_or_default().to_owned();
    let mut edits = Vec::new();
    for row in v.get("refused").and_then(Value::as_array).into_iter().flatten() {
        let reason = s(row, "reason");
        if !reason.starts_with(TOKEN) {
            continue;
        }
        let raw = row.get("raw").and_then(Value::as_bool).unwrap_or(false);
        edits.push(Edit {
            mod_id: s(row, "mod"),
            registry: s(row, "registry"),
            id: s(row, "id"),
            field: if raw { "(raw path)".to_owned() } else { s(row, "field") },
            source: s(row, "source"),
            reason,
        });
    }
    let total = v.get("counts").and_then(|c| c.get("restartRequired")).and_then(Value::as_u64).unwrap_or(edits.len() as u64);
    Ok(Waiting { total: total.max(edits.len() as u64), edits, written: v.get("written").and_then(Value::as_str).map(str::to_owned) })
}

/// The waiting edits of the game (or server) folder's last report; `None` when it has none yet.
pub fn read(game: &Path) -> Result<Option<Waiting>> {
    use std::io::Read;
    let path = game.join(REPORT_REL);
    let file = match std::fs::File::open(&path) {
        Ok(f) => f,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(None),
        Err(e) => return Err(Error::new(ErrorCode::Io, format!("{REPORT_REL} could not be read: {e}"))),
    };
    let mut bytes = Vec::new();
    file.take(MAX_BYTES + 1)
        .read_to_end(&mut bytes)
        .map_err(|e| Error::new(ErrorCode::Io, format!("{REPORT_REL} could not be read: {e}")))?;
    if bytes.len() as u64 > MAX_BYTES {
        return Err(Error::new(ErrorCode::Io, format!("{REPORT_REL} is larger than {} MiB", MAX_BYTES >> 20)));
    }
    let text = String::from_utf8(bytes).map_err(|_| Error::new(ErrorCode::Parse, format!("{REPORT_REL} is not UTF-8")))?;
    parse(&text).map(Some)
}

/// One line for a status view: "none" or "<n> edit(s) wait for a restart: <mod> <place>, ..." (at most `shown` named).
pub fn summary(w: &Waiting, shown: usize) -> String {
    if w.total == 0 {
        return "no registry edit waits for a restart".to_owned();
    }
    let named: Vec<String> = w.edits.iter().take(shown).map(|e| format!("{} {}", e.mod_id, e.place())).collect();
    let more = w.total.saturating_sub(named.len() as u64);
    format!(
        "{} registry edit{} wait{} for a restart: {}{}",
        w.total,
        if w.total == 1 { "" } else { "s" },
        if w.total == 1 { "s" } else { "" },
        named.join(", "),
        if more > 0 { format!(" and {more} more") } else { String::new() }
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    const REPORT: &str = r#"{"format": 1, "written": "2026-11-25T20:00:00Z", "refused": [
        {"mod": "bigpack", "source": "content/a.json", "stage": "alter", "n": 1, "registry": "balance", "id": "*", "field": "levelCap",
         "reason": "RestartRequired: balance[*].levelCap is read at game start; the reloaded value applies at the next start"},
        {"mod": "bigpack", "source": "content/a.json", "stage": "alter", "n": 2, "registry": "items", "id": "game:wood", "field": "maxStack",
         "reason": "unknown field"},
        {"mod": "lib", "source": "live:4", "stage": "alter", "n": 0, "registry": "items", "id": "game:wood", "field": "x", "raw": true,
         "reason": "RestartRequired: the base did not read items.x at start"},
        {"mod": "other", "source": "content/o.json", "stage": "alter", "n": 0,
         "reason": "RestartRequired: a registry section added after the start needs a restart; this edit was not applied"}
    ], "counts": {"fields": 0, "conflicts": 0, "refused": 4, "added": 0, "removed": 0, "restartRequired": 3}}"#;

    #[test]
    fn rows_with_the_token_are_the_waiting_edits_with_their_places() {
        let w = parse(REPORT).unwrap();
        assert_eq!(w.total, 3);
        let places: Vec<String> = w.edits.iter().map(Edit::place).collect();
        assert_eq!(places, ["balance.levelCap", "items[game:wood].(raw path)",
            "RestartRequired: a registry section added after the start needs a restart; this edit was not applied"]);
        assert_eq!(w.written.as_deref(), Some("2026-11-25T20:00:00Z"));
        assert_eq!(summary(&w, 2), "3 registry edits wait for a restart: bigpack balance.levelCap, lib items[game:wood].(raw path) and 1 more");
    }

    #[test]
    fn a_report_without_waiting_rows_or_of_an_older_kiln() {
        let w = parse(r#"{"refused": [{"mod": "m", "reason": "unknown field"}], "counts": {"refused": 1}}"#).unwrap();
        assert_eq!((w.total, w.edits.len()), (0, 0));
        assert_eq!(summary(&w, 5), "no registry edit waits for a restart");
        // no counts.restartRequired (an older Kiln): the rows listed
        let old = parse(r#"{"refused": [{"mod": "m", "registry": "items", "id": "game:wood", "field": "maxStack", "reason": "RestartRequired: x"}]}"#).unwrap();
        assert_eq!(old.total, 1);
        assert_eq!(summary(&old, 5), "1 registry edit waits for a restart: m items[game:wood].maxStack");
        assert!(parse("{not json").is_err());
    }

    #[test]
    fn read_finds_the_report_in_the_game_folder() {
        let dir = std::env::temp_dir().join(format!("kiln-restart-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        assert_eq!(read(&dir).unwrap(), None);
        std::fs::create_dir_all(dir.join("kiln/state")).unwrap();
        std::fs::write(dir.join(REPORT_REL), REPORT).unwrap();
        assert_eq!(read(&dir).unwrap().unwrap().total, 3);
        let _ = std::fs::remove_dir_all(&dir);
    }
}
