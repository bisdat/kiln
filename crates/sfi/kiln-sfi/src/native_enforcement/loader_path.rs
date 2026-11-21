// SPDX-License-Identifier: MIT
//! The spelling of an inspected DLL path that kiln-sfi hands to LoadLibraryExW.
//!
//! `std::fs::canonicalize` returns a verbatim `\\?\` path. Native Windows does not match a verbatim path to the
//! kernel32.dll every process already has: `LoadLibraryExW("\\?\C:\WINDOWS\system32\kernel32.dll")` fails with
//! ERROR_INVALID_ADDRESS (487), where the plain spelling returns the loaded module (Windows 11 25H2).
//! Wine accepts both. So every load uses the plain spelling, but only when Win32
//! path normalization leaves it naming the very same file; otherwise the verbatim path stays. The spelling never
//! becomes an identity: the mapped image is still proved against the inspected file after every load.
//!
//! The verbatim spelling is not taken as written either: the native loader drops trailing dots and spaces from every
//! component and appends ".dll" to a file name with no extension, for `\\?\` paths too (Windows 11:
//! `entry.dll.`, `entry.dll ` and `folder.\entry.dll` loaded the sibling without the dot or space, and `entry` loaded
//! `entry.DLL`). The mapping proof would refuse such a load only
//! after the other file's DllMain ran, so [`loadable`] refuses those paths before any load.

/// Below MAX_PATH, so the plain spelling needs no long-path opt-in.
const PLAIN_LIMIT: usize = 260;

/// The plain Win32 spelling of a verbatim `\\?\X:\...` or `\\?\UNC\server\share\...` path, or `None` when Win32
/// normalization could turn the plain spelling into another file (`.`/`..` or empty components, trailing dots or
/// spaces, DOS device names, stream or wildcard characters, MAX_PATH or longer) or the path is not verbatim.
pub(super) fn plain_spelling(path: &[u16]) -> Option<Vec<u16>> {
    let text = String::from_utf16(path).ok()?;
    let rest = text.strip_prefix(r"\\?\")?;
    let bytes = rest.as_bytes();
    let (head, tail) = if bytes.len() >= 2 && bytes[0].is_ascii_alphabetic() && bytes[1] == b':' {
        (rest[..2].to_string(), &rest[2..])
    } else if bytes.len() > 4 && rest[..4].eq_ignore_ascii_case(r"UNC\") {
        let mut parts = rest[4..].splitn(3, '\\');
        let (server, share) = (parts.next()?, parts.next()?);
        if !stable(server) || !stable(share) {
            return None;
        }
        (
            format!(r"\\{server}\{share}"),
            &rest[4 + server.len() + 1 + share.len()..],
        )
    } else {
        return None;
    };
    let components = tail.strip_prefix('\\')?;
    if components.is_empty() || !components.split('\\').all(stable) {
        return None;
    }
    let plain: Vec<u16> = format!("{head}{tail}").encode_utf16().collect();
    (plain.len() < PLAIN_LIMIT).then_some(plain)
}

/// Refuses a canonical path the Windows loader would not map as spelled: a component ending in a dot or a space, or a
/// file name with no extension (no dot).
pub(super) fn loadable(path: &[u16]) -> Result<(), String> {
    let rest = path
        .strip_prefix(&[0x5c, 0x5c, 0x3f, 0x5c][..])
        .unwrap_or(path);
    let respelled = rest
        .split(|&unit| unit == 0x5c)
        .any(|component| matches!(component.last(), Some(0x2e | 0x20)));
    let named = rest
        .rsplit(|&unit| unit == 0x5c)
        .next()
        .is_some_and(|name| name.contains(&0x2e));
    if respelled || !named {
        return Err(format!(
            "the Windows loader would not map {} as inspected (it drops trailing dots and spaces and appends .dll to a \
             name with no extension); refusing before any load",
            String::from_utf16_lossy(path)
        ));
    }
    Ok(())
}

/// A component Win32 normalization passes through unchanged and resolves to a file, not a device.
fn stable(component: &str) -> bool {
    if component.is_empty()
        || component == "."
        || component == ".."
        || component.ends_with(['.', ' '])
        || component
            .chars()
            .any(|c| (c as u32) < 32 || matches!(c, '/' | ':' | '*' | '?' | '"' | '<' | '>' | '|'))
    {
        return false;
    }
    let stem = component
        .split('.')
        .next()
        .unwrap_or("")
        .trim_end_matches(' ')
        .to_ascii_uppercase();
    let numbered = (stem.starts_with("COM") || stem.starts_with("LPT"))
        && stem.chars().count() == 4
        && matches!(stem.chars().nth(3), Some('0'..='9' | '¹' | '²' | '³'));
    !(numbered
        || matches!(
            stem.as_str(),
            "CON" | "PRN" | "AUX" | "NUL" | "CONIN$" | "CONOUT$" | "CLOCK$"
        ))
}

#[cfg(test)]
mod tests {
    use super::{loadable, plain_spelling};
    fn load(path: &str) -> bool {
        loadable(&path.encode_utf16().collect::<Vec<_>>()).is_ok()
    }
    #[test]
    fn a_path_the_loader_would_respell_is_refused_before_any_load() {
        for path in [
            r"\\?\C:\Windows\System32\kernel32.dll",
            r"\\?\C:\mods\pkg\entry.dll",
            r"\\?\C:\mods\pkg\entry.bin",
            r"\\?\C:\mods\ pkg\ entry.dll",
            r"\\?\C:\mods\.hidden\.dll",
            r"\\?\UNC\server\share\mods\a.dll",
            r"\\?\Volume{00000000-0000-0000-0000-000000000000}\a.dll",
        ] {
            assert!(load(path), "{path}");
        }
        for path in [
            r"\\?\C:\mods\pkg\entry.dll.",
            r"\\?\C:\mods\pkg\entry.dll ",
            r"\\?\C:\mods\pkg.\entry.dll",
            r"\\?\C:\mods\pkg \entry.dll",
            r"\\?\C:\mods\pkg\entry",
            r"\\?\C:\mods\pkg\entry.",
            r"\\?\C:\mods.d\entry",
        ] {
            assert!(!load(path), "{path}");
        }
    }
    fn spell(path: &str) -> Option<String> {
        let wide: Vec<u16> = path.encode_utf16().collect();
        plain_spelling(&wide).map(|p| String::from_utf16(&p).unwrap())
    }
    #[test]
    fn verbatim_drive_and_unc_paths_get_their_plain_spelling() {
        assert_eq!(
            spell(r"\\?\C:\Windows\System32\kernel32.dll").as_deref(),
            Some(r"C:\Windows\System32\kernel32.dll")
        );
        assert_eq!(
            spell(r"\\?\Z:\mnt\mods\pkg\entry.dll").as_deref(),
            Some(r"Z:\mnt\mods\pkg\entry.dll")
        );
        assert_eq!(
            spell(r"\\?\UNC\server\share\mods\a.dll").as_deref(),
            Some(r"\\server\share\mods\a.dll")
        );
        assert_eq!(
            spell(r"\\?\C:\mods\.hidden\a b.dll").as_deref(),
            Some(r"C:\mods\.hidden\a b.dll")
        );
    }
    #[test]
    fn a_spelling_win32_could_resolve_elsewhere_stays_verbatim() {
        for path in [
            r"C:\Windows\System32\kernel32.dll",
            r"\\?\C:\mods\a.dll.",
            r"\\?\C:\mods\a.dll ",
            r"\\?\C:\mods.\a.dll",
            r"\\?\C:\mods\..\a.dll",
            r"\\?\C:\mods\.\a.dll",
            r"\\?\C:\mods\\a.dll",
            r"\\?\C:\mods\nul.dll",
            r"\\?\C:\mods\NUL",
            r"\\?\C:\mods\com1.dll",
            r"\\?\C:\mods\lpt9",
            r"\\?\C:\mods\CON .dll",
            r"\\?\C:\mods\conin$.dll",
            r"\\?\C:\mods\a.dll:stream",
            r"\\?\C:\mods\a/b.dll",
            r"\\?\C:\mods\a*.dll",
            r"\\?\C:\",
            r"\\?\C:",
            r"\\?\Volume{00000000-0000-0000-0000-000000000000}\a.dll",
            r"\\?\GLOBALROOT\Device\HarddiskVolume1\a.dll",
            r"\\?\UNC\server",
            r"\\?\UNC\server\nul\a.dll",
            r"\\.\C:\a.dll",
        ] {
            assert_eq!(spell(path), None, "{path}");
        }
        // The plain spelling must stay below MAX_PATH (260 with its NUL): 259 characters fit, 260 do not.
        let long = format!(r"\\?\C:\{}\a.dll", "d".repeat(251));
        assert_eq!(spell(&long), None);
        let fits = format!(r"\\?\C:\{}\a.dll", "d".repeat(250));
        assert_eq!(spell(&fits).map(|p| p.len()), Some(259));
        assert!(spell(r"\\?\C:\mods\com10.dll").is_some());
        assert!(spell(r"\\?\C:\mods\console.dll").is_some());
        assert_eq!(
            plain_spelling(&[0x5c, 0x5c, 0x3f, 0x5c, 0x43, 0x3a, 0x5c, 0xd800]),
            None
        );
    }
}
