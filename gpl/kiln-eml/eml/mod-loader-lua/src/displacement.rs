// SPDX-License-Identifier: GPL-3.0-or-later
// Lossless collision handling. Neither journals nor saved containers in this folder are ever pruned.
use std::{collections::BTreeMap, fs, io, path::{Path, PathBuf}};
use crate::{ownership::{checked_path, digest, Digest}, transaction};

// kiln-eml: keyed by the recovery folder's own (unique) name, not its path: the writer publishes from the canonical game
// folder (a verbatim \\?\ path on Windows) and the run asks for guidance with the folder as configured.
thread_local! { static MOVED: std::cell::RefCell<BTreeMap<std::ffi::OsString, Vec<String>>> = const { std::cell::RefCell::new(BTreeMap::new()) }; }
pub(crate) fn reset() { MOVED.with(|m| m.borrow_mut().clear()); }

pub(crate) struct Plan { folder: PathBuf, files: BTreeMap<String, Digest> }
impl Plan {
    pub(crate) fn record(root: &Path, stem: &str, vanilla: usize, files: BTreeMap<String, Digest>) -> io::Result<Self> {
        let parent = root.join("kiln/eml/displaced");
        checked_path(root, &parent)?;
        fs::create_dir_all(&parent)?;
        let folder = parent.join(uuid::Uuid::new_v4().to_string());
        fs::create_dir(&folder)?; // a new private namespace, never an existing recovery folder
        for dir in [root, &root.join("kiln"), &root.join("kiln/eml"), &parent] { transaction::sync_dir(dir)?; }
        failpoint!("displace.folder", folder);
        let mut journal = serde_json::json!({"format":1,"stem":stem,"vanillaContainers":vanilla,"files":files});
        journal.sort_all_objects(); // kiln-eml: sorted keys under serde_json's preserve_order too
        transaction::atomic_write(&folder.join("moves.json"), &serde_json::to_vec(&journal)?)?;
        Ok(Self { folder, files })
    }

    /// Only after the durable vanilla-header barrier, under the same writer guard as the ownership check.
    /// All moves precede any further archive writes. A failed move leaves earlier moves recoverable, never undone.
    pub(crate) fn apply(&self, root: &Path, mut boundary: impl FnMut() -> io::Result<()>) -> io::Result<()> {
        checked_path(root, &self.folder)?;
        let key = self.folder.file_name().ok_or_else(|| io::Error::other("recovery folder has no name"))?.to_owned();
        for (name, expected) in &self.files {
            let source = root.join(name);
            if digest(root, &source)? != *expected { return Err(io::Error::other(format!("container {name} changed since its recovery move was recorded; retained without publishing"))); }
            let target = self.folder.join(name);
            transaction::move_new(&source, &target).map_err(|e| io::Error::other(format!("cannot move {name} into {}: {e}; no further archives published", self.folder.display())))?;
            MOVED.with(|m| m.borrow_mut().entry(key.clone()).or_default().push(format!("{name} ({} bytes) -> {}", expected.size, target.strip_prefix(root).unwrap().to_string_lossy().replace('\\', "/"))));
            failpoint!("displace.moved", source);
            boundary()?;
        }
        Ok(())
    }
}

pub(crate) fn guidance(root: &Path) -> io::Result<Vec<String>> {
    let parent = root.join("kiln/eml/displaced");
    checked_path(root, &parent)?;
    let entries = match fs::read_dir(&parent) {
        Ok(entries) => entries,
        Err(e) if e.kind() == io::ErrorKind::NotFound => return Ok(Vec::new()),
        Err(e) => return Err(e),
    };
    let mut messages = Vec::new();
    for e in entries {
        let e = e?;
        checked_path(root, &e.path())?;
        if !e.file_type()?.is_dir() { continue; }
        let moved = MOVED.with(|m| m.borrow().get(&e.file_name()).cloned());
        messages.push(match moved {
            Some(files) => format!("Archive containers moved aside: {}. The originals are preserved; Kiln never deletes this recovery folder", files.join(", ")),
            None => format!("Archive recovery folder retained: {}/. Inspect its move journal and any saved containers; Kiln never deletes this folder", e.path().strip_prefix(root).unwrap().to_string_lossy().replace('\\', "/")),
        });
    }
    messages.sort();
    Ok(messages)
}

#[cfg(test)]
mod tests {
    use super::*;
    // A writer publishes from the canonical game folder (create_writer canonicalizes it: a verbatim \\?\ path on
    // Windows), while the run reports guidance for the folder as configured. The containers moved aside are named
    // whatever the game folder's spelling.
    #[test]
    fn moved_containers_are_reported_whatever_the_game_folder_spelling() {
        let base = std::env::temp_dir().join(format!("kiln-displaced-spelling-{}", uuid::Uuid::new_v4()));
        fs::create_dir_all(base.join("sub")).unwrap();
        fs::create_dir(base.join("game")).unwrap();
        let canonical = fs::canonicalize(base.join("game")).unwrap();
        let configured = base.join("sub").join("..").join("game");
        fs::write(canonical.join("g_001.dat"), "player content").unwrap();
        reset();
        let files = BTreeMap::from([("g_001.dat".to_owned(), digest(&canonical, &canonical.join("g_001.dat")).unwrap())]);
        let plan = Plan::record(&canonical, "g", 1, files).unwrap();
        plan.apply(&canonical, || Ok(())).unwrap();
        for root in [&canonical, &configured] {
            let messages = guidance(root).unwrap().join(" ");
            assert!(messages.contains("Archive containers moved aside: g_001.dat (14 bytes) -> kiln/eml/displaced/"),
                "{}: {messages}", root.display());
        }
        reset();
        fs::remove_dir_all(base).unwrap();
    }
    // The move journal keeps main's sorted key order under serde_json's preserve_order.
    #[test]
    fn move_journal_keeps_sorted_key_order() {
        let root = std::env::temp_dir().join(format!("kiln-displaced-order-{}", uuid::Uuid::new_v4()));
        fs::create_dir(&root).unwrap();
        fs::write(root.join("g_001.dat"), "player content").unwrap();
        let files = BTreeMap::from([("g_001.dat".to_owned(), digest(&root, &root.join("g_001.dat")).unwrap())]);
        let plan = Plan::record(&root, "g", 1, files).unwrap();
        let journal = fs::read(plan.folder.join("moves.json")).unwrap();
        assert!(journal.starts_with(br#"{"files":{"g_001.dat":"#), "{}", String::from_utf8_lossy(&journal));
        assert!(crate::ownership::tests::keys_sorted(&journal, false));
        fs::remove_dir_all(root).unwrap();
    }
}
