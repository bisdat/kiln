//! Native-only owner capabilities, separate from shared.json and every IPC result. Bound to the exact API and
//! profile. Unix files are atomic 0600; Windows encrypts to this OS user with DPAPI before writing the private file.
use super::*;

#[derive(Serialize, Deserialize)]
pub(super) struct Owner {
    pub pack: Option<String>,
    pub key: String,
}

pub(super) fn path(dirs: &DataDirs, api: &str, profile: &str) -> PathBuf {
    let name = crate::hash::sha256_hex(format!("{api}\n{profile}").as_bytes());
    root(dirs).join("owners").join(format!("{name}.key"))
}

pub(super) fn load(dirs: &DataDirs, api: &str, profile: &str) -> Result<Option<Owner>> {
    let bytes = match std::fs::read(path(dirs, api, profile)) {
        Ok(b) => b,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(None),
        Err(_) => return Err(err(ErrorCode::Refused, "The owner key for this link cannot be read on this device.")),
    };
    let bytes = protect(&bytes, false)?;
    let owner: Owner = serde_json::from_slice(&bytes).map_err(|_| err(ErrorCode::Refused, "The owner key for this link cannot be read on this device."))?;
    if owner.key.len() != 64 || !owner.key.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b)) {
        return Err(err(ErrorCode::Refused, "The owner key for this link is invalid."));
    }
    Ok(Some(owner))
}

pub(super) fn save(dirs: &DataDirs, api: &str, profile: &str, owner: &Owner) -> Result<()> {
    let bytes = protect(&serde_json::to_vec(owner)?, true)?;
    crate::community::session::write_private(&path(dirs, api, profile), &bytes)
}

#[cfg(not(windows))]
fn protect(bytes: &[u8], _seal: bool) -> Result<Vec<u8>> { Ok(bytes.to_vec()) }

#[cfg(windows)]
fn protect(bytes: &[u8], seal: bool) -> Result<Vec<u8>> {
    use windows_sys::Win32::Security::Cryptography::{CRYPT_INTEGER_BLOB, CRYPTPROTECT_UI_FORBIDDEN, CryptProtectData, CryptUnprotectData};
    use windows_sys::Win32::Foundation::LocalFree;
    let input = CRYPT_INTEGER_BLOB { cbData: bytes.len().try_into().map_err(|_| Error::invalid("owner key too large"))?, pbData: bytes.as_ptr() as *mut u8 };
    let mut output = CRYPT_INTEGER_BLOB { cbData: 0, pbData: std::ptr::null_mut() };
    // No UI, machine-wide scope or account credentials. DPAPI ties the ciphertext to this Windows user's logon.
    let ok = unsafe {
        if seal { CryptProtectData(&input, std::ptr::null(), std::ptr::null(), std::ptr::null(), std::ptr::null(), CRYPTPROTECT_UI_FORBIDDEN, &mut output) }
        else { CryptUnprotectData(&input, std::ptr::null_mut(), std::ptr::null(), std::ptr::null(), std::ptr::null(), CRYPTPROTECT_UI_FORBIDDEN, &mut output) }
    };
    if ok == 0 { return Err(err(ErrorCode::Refused, "The owner key for this link cannot be protected or opened on this device.")); }
    let result = unsafe { std::slice::from_raw_parts(output.pbData, output.cbData as usize).to_vec() };
    unsafe { LocalFree(output.pbData.cast()); }
    Ok(result)
}
