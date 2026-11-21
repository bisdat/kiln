//! What a tester shares (PLAN 3.9): the graphics card, its driver, the video memory, the system and how the game
//! runs, and from those what this PC can test that the team can't (FSR 4, XeSS, frame generation, the Steam Deck).
//! Read without new privileges; the volunteer screen shows exactly this before anything is sent.
//!
//! Linux: the graphics devices in `/sys/class/drm/card*/device` (vendor and device ids, the kernel driver, amdgpu's
//! video memory; the one the firmware started on first, else the one with the most memory), their names from
//! `pci.ids`, the Mesa version from the name of its `libgallium-<version>.so`, NVIDIA's driver version from
//! `/proc/driver/nvidia/version` (and its video memory from `nvidia-smi` when it is there), the system from
//! `/etc/os-release`, a Steam Deck from its DMI board name. Windows: the display adapter's class key in the registry
//! (name, driver version, memory). How the game runs comes from the caller (the game install's Proton or Windows).

use serde::{Deserialize, Serialize};

/// What the screens say for a fact the launcher could not read (en.json `community.tester.unknown`).
pub const NOT_KNOWN: &str = "Not known";

/// Hardware the team wants tested.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Capability {
    /// AMD RX 6000 and newer.
    Fsr4,
    /// Intel Arc.
    Xess,
    /// NVIDIA RTX 40 and 50.
    Framegen,
    /// A Steam Deck.
    Deck,
}

impl Capability {
    /// The word the screens show.
    pub fn label(self) -> &'static str {
        match self {
            Capability::Fsr4 => "FSR 4",
            Capability::Xess => "XeSS",
            Capability::Framegen => "frame generation",
            Capability::Deck => "Steam Deck",
        }
    }
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Hardware {
    pub gpu: String,
    pub driver: String,
    pub vram: String,
    pub os: String,
    /// How the game runs: "Windows", "GE-Proton10-3", "Proton 10.0", "Wine 10.0".
    pub runner: String,
    pub caps: Vec<Capability>,
    /// `caps` in words.
    pub covers: Vec<String>,
}

impl Hardware {
    /// What `POST /tester` sends (the relay's `{gpu, driver?, vram?, os?, runner?, covers?}`, one line of at most 120
    /// characters each, `gpu` required): the lines the screen showed. A fact the launcher could not read is left out
    /// (the screen said "Not known"; for the card, that word goes).
    pub fn wire(&self) -> serde_json::Value {
        let cut = |s: &str| s.chars().filter(|c| !c.is_control()).take(120).collect::<String>().trim().to_string();
        let mut v = serde_json::Map::new();
        let gpu = cut(&self.gpu);
        v.insert("gpu".into(), serde_json::json!(if gpu.is_empty() { NOT_KNOWN.to_string() } else { gpu }));
        for (k, x) in [("driver", &self.driver), ("vram", &self.vram), ("os", &self.os), ("runner", &self.runner)] {
            let x = cut(x);
            if !x.is_empty() {
                v.insert(k.into(), serde_json::json!(x));
            }
        }
        v.insert("covers".into(), serde_json::json!(self.covers.iter().take(8).collect::<Vec<_>>()));
        serde_json::Value::Object(v)
    }
}

/// What the reader found about one graphics device.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Gpu {
    pub vendor: u16,
    pub device: u16,
    /// "AMD Radeon RX 7700 XT / 7800 XT", "NVIDIA GeForce RTX 4070 SUPER".
    pub name: String,
    /// "Mesa 26.2.5 (amdgpu)", "NVIDIA 615.78.08".
    pub driver: String,
    pub vram_bytes: Option<u64>,
    /// The firmware started on this one (`boot_vga`).
    pub boot: bool,
}

pub const AMD: u16 = 0x1002;
pub const NVIDIA: u16 = 0x10de;
pub const INTEL: u16 = 0x8086;

fn vendor_word(v: u16) -> &'static str {
    match v {
        AMD => "AMD",
        NVIDIA => "NVIDIA",
        INTEL => "Intel",
        _ => "",
    }
}

/// A device's name from pci.ids: the marketing name in brackets when there is one ("Navi 32 [Radeon RX 7700 XT /
/// 7800 XT]" -> "AMD Radeon RX 7700 XT / 7800 XT"), with the vendor's short word.
pub fn pci_name(ids: &str, vendor: u16, device: u16) -> Option<String> {
    let v = format!("{vendor:04x}");
    let d = format!("\t{device:04x}  ");
    let mut in_vendor = false;
    for line in ids.lines() {
        if line.starts_with('#') || line.is_empty() {
            continue;
        }
        if !line.starts_with('\t') {
            if in_vendor {
                return None;
            }
            in_vendor = line.len() > 4 && line[..4].eq_ignore_ascii_case(&v) && line.as_bytes().get(4) == Some(&b' ');
            continue;
        }
        if in_vendor && line.to_ascii_lowercase().starts_with(&d) {
            let raw = line[d.len()..].trim();
            let name = match (raw.find('['), raw.rfind(']')) {
                (Some(a), Some(b)) if b > a => raw[a + 1..b].trim(),
                _ => raw,
            };
            let w = vendor_word(vendor);
            return Some(if w.is_empty() || name.to_ascii_lowercase().starts_with(&w.to_ascii_lowercase()) { name.to_string() } else { format!("{w} {name}") });
        }
    }
    None
}

/// What this hardware can test that the team cannot, from the card's name and the device.
pub fn capabilities(gpu_vendor: u16, gpu_name: &str, deck: bool) -> Vec<Capability> {
    let mut caps = Vec::new();
    let n = gpu_name.to_ascii_uppercase();
    let series = |prefix: &str, firsts: &[&str]| {
        n.match_indices(prefix).any(|(i, _)| {
            let rest = n[i + prefix.len()..].trim_start();
            let digits: String = rest.chars().take_while(|c| c.is_ascii_digit()).collect();
            digits.len() == 4 && firsts.iter().any(|f| digits.starts_with(f))
        })
    };
    if deck {
        caps.push(Capability::Deck);
    }
    if gpu_vendor == AMD && series("RX", &["6", "7", "9"]) {
        caps.push(Capability::Fsr4);
    }
    if gpu_vendor == INTEL && n.contains("ARC") {
        caps.push(Capability::Xess);
    }
    if gpu_vendor == NVIDIA && series("RTX", &["40", "50"]) {
        caps.push(Capability::Framegen);
    }
    caps
}

/// "16 GB", "1 GB shared" (a Deck's).
pub fn vram_words(bytes: Option<u64>, shared: bool) -> String {
    match bytes {
        Some(b) if b > 0 => {
            let gb = (b as f64 / (1u64 << 30) as f64).round().max(1.0) as u64;
            if shared { format!("{gb} GB shared") } else { format!("{gb} GB") }
        }
        _ => String::new(),
    }
}

/// Where a Linux reader looks (tests point it at a recorded tree).
#[derive(Debug, Clone)]
pub struct Sys {
    pub drm: std::path::PathBuf,
    pub pci_ids: Vec<std::path::PathBuf>,
    pub nvidia_version: std::path::PathBuf,
    pub lib_dirs: Vec<std::path::PathBuf>,
    pub dmi: std::path::PathBuf,
    /// Run `nvidia-smi` for NVIDIA's video memory.
    pub nvidia_smi: bool,
}

impl Default for Sys {
    fn default() -> Self {
        Sys {
            drm: "/sys/class/drm".into(),
            pci_ids: vec!["/usr/share/hwdata/pci.ids".into(), "/usr/share/misc/pci.ids".into(), "/usr/share/pci.ids".into()],
            nvidia_version: "/proc/driver/nvidia/version".into(),
            lib_dirs: vec!["/usr/lib".into(), "/usr/lib64".into(), "/usr/lib/x86_64-linux-gnu".into()],
            dmi: "/sys/class/dmi/id".into(),
            nvidia_smi: true,
        }
    }
}

fn read_hex(p: &std::path::Path) -> Option<u16> {
    let t = std::fs::read_to_string(p).ok()?;
    u16::from_str_radix(t.trim().trim_start_matches("0x"), 16).ok()
}

/// Mesa's version from its library's file name (`libgallium-26.2.5-arch1.1.so` -> "26.2.5").
pub fn mesa_version(dirs: &[std::path::PathBuf]) -> Option<String> {
    for d in dirs {
        let Ok(rd) = std::fs::read_dir(d) else { continue };
        for e in rd.flatten() {
            let n = e.file_name().to_string_lossy().into_owned();
            if let Some(rest) = n.strip_prefix("libgallium-").and_then(|r| r.strip_suffix(".so")) {
                let v: String = rest.chars().take_while(|c| c.is_ascii_digit() || *c == '.').collect();
                if v.contains('.') {
                    return Some(v.trim_end_matches('.').to_string());
                }
            }
        }
    }
    None
}

/// The graphics devices under `sys.drm`.
pub fn gpus(sys: &Sys) -> Vec<Gpu> {
    let ids = sys.pci_ids.iter().find_map(|p| std::fs::read_to_string(p).ok()).unwrap_or_default();
    let mesa = mesa_version(&sys.lib_dirs);
    let mut out = Vec::new();
    let Ok(rd) = std::fs::read_dir(&sys.drm) else { return out };
    let mut cards: Vec<_> = rd.flatten().map(|e| e.file_name().to_string_lossy().into_owned())
        .filter(|n| n.strip_prefix("card").is_some_and(|r| !r.is_empty() && r.bytes().all(|b| b.is_ascii_digit()))).collect();
    cards.sort();
    for c in cards {
        let dev = sys.drm.join(&c).join("device");
        let (Some(vendor), Some(device)) = (read_hex(&dev.join("vendor")), read_hex(&dev.join("device"))) else { continue };
        let driver = std::fs::read_link(dev.join("driver")).ok().and_then(|l| l.file_name().map(|n| n.to_string_lossy().into_owned())).unwrap_or_default();
        let name = pci_name(&ids, vendor, device).unwrap_or_else(|| format!("{} {device:04x}", vendor_word(vendor)).trim().to_string());
        let driver_words = if driver == "nvidia" {
            std::fs::read_to_string(&sys.nvidia_version).ok().and_then(|t| {
                t.lines().next()?.split_whitespace().find(|w| w.chars().filter(|c| *c == '.').count() >= 1 && w.chars().all(|c| c.is_ascii_digit() || c == '.')).map(|v| format!("NVIDIA {v}"))
            }).unwrap_or_else(|| "NVIDIA".into())
        } else if let Some(m) = &mesa {
            if driver.is_empty() { format!("Mesa {m}") } else { format!("Mesa {m} ({driver})") }
        } else {
            driver.clone()
        };
        let vram_bytes = std::fs::read_to_string(dev.join("mem_info_vram_total")).ok().and_then(|t| t.trim().parse::<u64>().ok())
            .or_else(|| if driver == "nvidia" && sys.nvidia_smi { nvidia_vram() } else { None });
        let boot = std::fs::read_to_string(dev.join("boot_vga")).is_ok_and(|t| t.trim() == "1");
        out.push(Gpu { vendor, device, name, driver: driver_words, vram_bytes, boot });
    }
    out
}

fn nvidia_vram() -> Option<u64> {
    let exe = crate::util::which("nvidia-smi")?;
    let mut child = crate::hostenv::command(&exe).args(["--query-gpu=memory.total", "--format=csv,noheader,nounits"])
        .stdin(std::process::Stdio::null()).stdout(std::process::Stdio::piped()).stderr(std::process::Stdio::null()).spawn().ok()?;
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(3);
    loop {
        if child.try_wait().ok()?.is_some() {
            break;
        }
        if std::time::Instant::now() > deadline {
            let _ = child.kill();
            let _ = child.wait();
            return None;
        }
        std::thread::sleep(std::time::Duration::from_millis(30));
    }
    let mut out = String::new();
    std::io::Read::read_to_string(&mut child.stdout.take()?, &mut out).ok()?;
    out.lines().next()?.trim().parse::<u64>().ok().map(|mib| mib * 1024 * 1024)
}

/// The device the game most likely renders on: the one the firmware started on, else the most memory.
pub fn main_gpu(list: &[Gpu]) -> Option<&Gpu> {
    // a discrete card (memory of its own) wins over the integrated one the firmware may have started on
    let discrete = |g: &&Gpu| g.vram_bytes.is_some_and(|b| b > 2 << 30) || g.vendor == NVIDIA;
    list.iter().filter(discrete).find(|g| g.boot).or_else(|| list.iter().filter(discrete).max_by_key(|g| g.vram_bytes.unwrap_or(0)))
        .or_else(|| list.iter().find(|g| g.boot)).or_else(|| list.first())
}

/// A Steam Deck from DMI: `Some("LCD")` (Jupiter) or `Some("OLED")` (Galileo).
pub fn deck_model(dmi: &std::path::Path) -> Option<&'static str> {
    let read = |n: &str| std::fs::read_to_string(dmi.join(n)).map(|t| t.trim().to_ascii_lowercase()).unwrap_or_default();
    let (board, product) = (read("board_name"), read("product_name"));
    if board == "jupiter" || product == "jupiter" {
        Some("LCD")
    } else if board == "galileo" || product == "galileo" {
        Some("OLED")
    } else {
        None
    }
}

/// The hardware lines on Linux. `runner`: how the game runs (the caller knows the install); `os`: the system's name.
pub fn read_linux(sys: &Sys, os: &str, runner: &str) -> Hardware {
    let list = gpus(sys);
    let deck = deck_model(&sys.dmi);
    let g = main_gpu(&list).cloned().unwrap_or_default();
    let gpu = match deck {
        Some(m) if !g.name.is_empty() => format!("{} (Steam Deck {m})", g.name),
        Some(m) => format!("Steam Deck {m}"),
        None => g.name.clone(),
    };
    let caps = capabilities(g.vendor, &g.name, deck.is_some());
    Hardware { gpu, driver: g.driver, vram: vram_words(g.vram_bytes, deck.is_some()), os: os.to_string(), runner: runner.to_string(), covers: caps.iter().map(|c| c.label().to_string()).collect(), caps }
}

/// The hardware lines on Windows: the first display adapter in the registry.
#[cfg(windows)]
pub fn read_windows(os: &str, runner: &str) -> Hardware {
    use windows_sys::Win32::System::Registry::{RegGetValueW, HKEY_LOCAL_MACHINE, RRF_RT_REG_BINARY, RRF_RT_REG_DWORD, RRF_RT_REG_QWORD, RRF_RT_REG_SZ};
    let wide = |s: &str| s.encode_utf16().chain(std::iter::once(0)).collect::<Vec<u16>>();
    let text = |key: &str, value: &str| -> Option<String> {
        let (k, v) = (wide(key), wide(value));
        let mut buf = vec![0u16; 512];
        let mut len = (buf.len() * 2) as u32;
        // SAFETY: the buffers outlive the call; len is the buffer's size in bytes
        let r = unsafe { RegGetValueW(HKEY_LOCAL_MACHINE, k.as_ptr(), v.as_ptr(), RRF_RT_REG_SZ, std::ptr::null_mut(), buf.as_mut_ptr() as *mut _, &mut len) };
        if r != 0 { return None; }
        let n = (len as usize / 2).saturating_sub(1);
        Some(String::from_utf16_lossy(&buf[..n.min(buf.len())]).trim_end_matches('\0').to_string())
    };
    let number = |key: &str, value: &str| -> Option<u64> {
        let (k, v) = (wide(key), wide(value));
        let mut buf = [0u8; 8];
        let mut len = 8u32;
        // SAFETY: as above
        let r = unsafe { RegGetValueW(HKEY_LOCAL_MACHINE, k.as_ptr(), v.as_ptr(), RRF_RT_REG_QWORD | RRF_RT_REG_DWORD | RRF_RT_REG_BINARY, std::ptr::null_mut(), buf.as_mut_ptr() as *mut _, &mut len) };
        if r != 0 { return None; }
        Some(if len >= 8 { u64::from_le_bytes(buf) } else { u32::from_le_bytes([buf[0], buf[1], buf[2], buf[3]]) as u64 })
    };
    let class = r"SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}";
    let mut best: Option<(String, String, Option<u64>, u16)> = None;
    for i in 0..10 {
        let key = format!("{class}\\{i:04}");
        let Some(name) = text(&key, "DriverDesc") else { continue };
        let driver = text(&key, "DriverVersion").unwrap_or_default();
        let mem = number(&key, "HardwareInformation.qwMemorySize").or_else(|| number(&key, "HardwareInformation.MemorySize"));
        let low = name.to_ascii_lowercase();
        let vendor = if low.contains("nvidia") { NVIDIA } else if low.contains("amd") || low.contains("radeon") { AMD } else if low.contains("intel") { INTEL } else { 0 };
        if best.as_ref().is_none_or(|b| mem.unwrap_or(0) > b.2.unwrap_or(0)) {
            best = Some((name, driver, mem, vendor));
        }
    }
    let (gpu, driver, mem, vendor) = best.unwrap_or_default();
    let caps = capabilities(vendor, &gpu, false);
    Hardware { gpu, driver, vram: vram_words(mem, false), os: os.to_string(), runner: runner.to_string(), covers: caps.iter().map(|c| c.label().to_string()).collect(), caps }
}

#[cfg(test)]
mod tests {
    use super::*;

    const IDS: &str = "# pci.ids (a few lines)\n1002  Advanced Micro Devices, Inc. [AMD/ATI]\n\t163f  VanGogh [AMD Custom GPU 0405]\n\t164e  Raphael\n\t747e  Navi 32 [Radeon RX 7700 XT / 7800 XT]\n\t\t1002 0e3b  RX 7800 XT reference\n10de  NVIDIA Corporation\n\t2783  AD104 [GeForce RTX 4070 SUPER]\n\t2208  GA102 [GeForce RTX 3080 Ti]\n8086  Intel Corporation\n\t56a0  DG2 [Arc A770]\n";

    #[test]
    fn names_come_from_pci_ids_with_the_marketing_name() {
        assert_eq!(pci_name(IDS, AMD, 0x747e).unwrap(), "AMD Radeon RX 7700 XT / 7800 XT");
        assert_eq!(pci_name(IDS, NVIDIA, 0x2783).unwrap(), "NVIDIA GeForce RTX 4070 SUPER");
        assert_eq!(pci_name(IDS, AMD, 0x164e).unwrap(), "AMD Raphael");
        assert_eq!(pci_name(IDS, AMD, 0x163f).unwrap(), "AMD Custom GPU 0405");
        assert_eq!(pci_name(IDS, INTEL, 0x56a0).unwrap(), "Intel Arc A770");
        assert!(pci_name(IDS, NVIDIA, 0x747e).is_none(), "a device id of another vendor");
    }

    #[test]
    fn what_a_pc_can_test_follows_its_card() {
        assert_eq!(capabilities(AMD, "AMD Radeon RX 7700 XT / 7800 XT", false), [Capability::Fsr4]);
        assert_eq!(capabilities(AMD, "AMD Radeon RX 6700 XT", false), [Capability::Fsr4]);
        assert_eq!(capabilities(AMD, "AMD Radeon RX 9070", false), [Capability::Fsr4]);
        assert!(capabilities(AMD, "AMD Radeon RX 580", false).is_empty());
        assert!(capabilities(AMD, "AMD Radeon RX 5700 XT", false).is_empty());
        assert_eq!(capabilities(NVIDIA, "NVIDIA GeForce RTX 4070 SUPER", false), [Capability::Framegen]);
        assert_eq!(capabilities(NVIDIA, "NVIDIA GeForce RTX 5080", false), [Capability::Framegen]);
        assert!(capabilities(NVIDIA, "NVIDIA GeForce RTX 3080 Ti", false).is_empty());
        assert_eq!(capabilities(INTEL, "Intel Arc A770", false), [Capability::Xess]);
        assert!(capabilities(INTEL, "Intel UHD Graphics 770", false).is_empty());
        assert_eq!(capabilities(AMD, "AMD Custom GPU 0405", true), [Capability::Deck]);
    }

    fn tree(cards: &[(&str, &str, &str, &str, Option<u64>, bool)], dmi_board: Option<&str>) -> (crate::library::TempDir, Sys) {
        let t = super::super::testing::tmp("hw");
        let root = t.path();
        for (card, vendor, device, driver, vram, boot) in cards {
            let dev = root.join("drm").join(card).join("device");
            std::fs::create_dir_all(&dev).unwrap();
            std::fs::write(dev.join("vendor"), format!("{vendor}\n")).unwrap();
            std::fs::write(dev.join("device"), format!("{device}\n")).unwrap();
            std::fs::write(dev.join("boot_vga"), if *boot { "1\n" } else { "0\n" }).unwrap();
            if let Some(v) = vram {
                std::fs::write(dev.join("mem_info_vram_total"), v.to_string()).unwrap();
            }
            let drv = root.join("drivers").join(driver);
            std::fs::create_dir_all(&drv).unwrap();
            #[cfg(unix)]
            std::os::unix::fs::symlink(&drv, dev.join("driver")).unwrap();
        }
        // a connector folder is not a card
        std::fs::create_dir_all(root.join("drm/card0-DP-1")).unwrap();
        std::fs::write(root.join("pci.ids"), IDS).unwrap();
        std::fs::create_dir_all(root.join("lib")).unwrap();
        std::fs::write(root.join("lib/libgallium-26.2.5-arch1.1.so"), b"").unwrap();
        std::fs::write(root.join("nvidia-version"), "NVRM version: NVIDIA UNIX Open Kernel Module for x86_64  615.78.08  Release Build  (root@pc)\n").unwrap();
        std::fs::create_dir_all(root.join("dmi")).unwrap();
        if let Some(b) = dmi_board {
            std::fs::write(root.join("dmi/board_name"), format!("{b}\n")).unwrap();
        }
        let sys = Sys { drm: root.join("drm"), pci_ids: vec![root.join("pci.ids")], nvidia_version: root.join("nvidia-version"), lib_dirs: vec![root.join("lib")], dmi: root.join("dmi"), nvidia_smi: false };
        (t, sys)
    }

    #[cfg(unix)]
    #[test]
    fn an_amd_card_beside_an_integrated_one_is_the_one_shown() {
        let (_t, sys) = tree(&[("card0", "0x1002", "0x164e", "amdgpu", Some(512 << 20), true), ("card1", "0x1002", "0x747e", "amdgpu", Some(16 << 30), false)], None);
        let hw = read_linux(&sys, "Arch Linux", "GE-Proton10-3");
        assert_eq!(hw.gpu, "AMD Radeon RX 7700 XT / 7800 XT");
        assert_eq!(hw.driver, "Mesa 26.2.5 (amdgpu)");
        assert_eq!(hw.vram, "16 GB");
        assert_eq!((hw.os.as_str(), hw.runner.as_str()), ("Arch Linux", "GE-Proton10-3"));
        assert_eq!(hw.caps, [Capability::Fsr4]);
        assert_eq!(hw.covers, ["FSR 4"]);
    }

    #[cfg(unix)]
    #[test]
    fn nvidia_says_its_driver_version_and_a_deck_says_so() {
        let (_t, sys) = tree(&[("card0", "0x10de", "0x2783", "nvidia", None, false), ("card1", "0x1002", "0x164e", "amdgpu", Some(512 << 20), true)], None);
        let hw = read_linux(&sys, "Arch Linux", "Proton 10.0");
        assert_eq!(hw.gpu, "NVIDIA GeForce RTX 4070 SUPER");
        assert_eq!(hw.driver, "NVIDIA 615.78.08");
        assert_eq!(hw.vram, "", "not known without nvidia-smi");
        assert_eq!(hw.caps, [Capability::Framegen]);
        let (_t2, deck) = tree(&[("card0", "0x1002", "0x163f", "amdgpu", Some(1 << 30), true)], Some("Jupiter"));
        let hw = read_linux(&deck, "SteamOS 3.7", "Proton 10.0");
        assert_eq!(hw.gpu, "AMD Custom GPU 0405 (Steam Deck LCD)");
        assert_eq!(hw.vram, "1 GB shared");
        assert_eq!(hw.caps, [Capability::Deck]);
        assert_eq!(hw.covers, ["Steam Deck"]);
    }

    /// `cargo test -p kiln-manage -- --ignored this_pcs_hardware --nocapture`: what Volunteer would show on this PC.
    #[cfg(target_os = "linux")]
    #[test]
    #[ignore]
    fn this_pcs_hardware() {
        println!("{:#?}", read_linux(&Sys::default(), &crate::report::os_info().name, "(the game's runner)"));
    }

    #[test]
    fn what_volunteer_sends_is_the_lines_shown() {
        let hw = Hardware { gpu: "g".into(), driver: "d".into(), vram: "v".into(), os: "o".into(), runner: "r".into(), caps: vec![Capability::Fsr4], covers: vec!["FSR 4".into()] };
        assert_eq!(hw.wire(), serde_json::json!({ "gpu": "g", "driver": "d", "vram": "v", "os": "o", "runner": "r", "covers": ["FSR 4"] }));
        // what could not be read is left out; the card, which the relay needs, says so
        let bare = Hardware { os: "Arch Linux".into(), ..Default::default() };
        assert_eq!(bare.wire(), serde_json::json!({ "gpu": "Not known", "os": "Arch Linux", "covers": [] }));
        let en: serde_json::Value = serde_json::from_str(include_str!("../../../../launcher/src/i18n/en.json")).unwrap();
        assert_eq!(en["community"]["tester"]["unknown"], NOT_KNOWN);
    }
}
