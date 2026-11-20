//! What Kiln counts as a mod's settings file, by its extension: one list for placement (`engine_mods`: the settings a
//! new version's copy keeps, the settings of a switched-off mod waiting in its stash), for import (`library`: a
//! package folder that holds only settings) and for kept versions (`kept`: what a roll back puts back). Logs are not
//! settings, but a kept version keeps them too: mods keep data in them (The Worst Map Mod's
//! `unlock_map_markers.log`, Creative Mode's `logs/`), and placement carries them over as files the mod wrote.
//! Anything else (scripts, DLLs, data of unknown kind) is never a settings file, whatever its folder.

/// The extensions of settings files (lower case).
pub const SETTINGS_EXTENSIONS: [&str; 7] = ["txt", "ini", "cfg", "json", "toml", "yaml", "yml"];

/// The extensions of logs (lower case).
pub const LOG_EXTENSIONS: [&str; 1] = ["log"];

/// The extension of the file name at the end of `rel` (a path with `/` or `\`), lower case; empty when it has none.
fn extension(rel: &str) -> String {
    let name = rel.rsplit(['/', '\\']).next().unwrap_or(rel);
    name.rsplit_once('.').map(|(stem, x)| if stem.is_empty() { String::new() } else { x.to_ascii_lowercase() }).unwrap_or_default()
}

/// Has `rel` a settings file's extension?
pub fn is_settings_extension(rel: &str) -> bool {
    SETTINGS_EXTENSIONS.contains(&extension(rel).as_str())
}

/// Is `rel` a log, by its extension?
pub fn is_log(rel: &str) -> bool {
    LOG_EXTENSIONS.contains(&extension(rel).as_str())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn settings_extensions_count_in_any_case_and_folder() {
        for rel in ["config.txt", "config.ini", "a.cfg", "settings.json", "mod.toml", "x.yaml", "x.yml", "CONFIG.INI", "sub/dir/Settings.Json",
                    "sub\\dir\\keys.ini", "quest_items.txt", "minimap_config.ini"] {
            assert!(is_settings_extension(rel), "{rel}");
            assert!(!is_log(rel), "{rel}");
        }
        for rel in ["unlock_map_markers.log", "logs/creative.log", "LOGS/FLAME.LOG"] {
            assert!(is_log(rel) && !is_settings_extension(rel), "{rel}");
        }
    }

    #[test]
    fn scripts_code_and_unknown_files_never_count() {
        for rel in ["run.bat", "run.cmd", "setup.ps1", "go.sh", "code.js", "code.py", "code.vbs", "mod.lua", "mod.luac", "dinput8.dll",
                    "game.exe", "plugin.asi", "lib.so", "cache.dat", "save.bin", "notes", ".ini", "dir.ini/file", "config.ini.bak", "x.txt.exe"] {
            assert!(!is_settings_extension(rel) && !is_log(rel), "{rel}");
        }
    }
}
