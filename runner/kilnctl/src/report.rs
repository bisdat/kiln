// SPDX-License-Identifier: MIT
//! `kilnctl report`: a support report (kiln_manage::report) of a server instance, a server or client folder, or the
//! client Steam knows. Reads only; the zip goes where --out says (default: the current folder).
use crate::bundle::data_dirs;
use crate::util::Km;
use anyhow::Result;
use kiln_manage::report::{self, ReportInput};
use std::path::PathBuf;

pub struct ReportArgs {
    pub name: Option<String>,
    pub dir: Option<PathBuf>,
    pub out: Option<PathBuf>,
    pub print: bool,
    /// Other mods' files to leave out (paths in the zip, or `other-mods`).
    pub exclude: Vec<String>,
    /// Print what the report holds (each file, its size and replacements) instead of writing the zip.
    pub contents: bool,
}

pub fn run(a: ReportArgs) -> Result<i32> {
    // only other mods' and tools' own files can be left out: support needs Kiln's own logs, the settings and the listings
    if let Some(x) = a.exclude.iter().find(|x| *x != report::EXCLUDE_OTHER_MODS && !report::excludable(x)) {
        anyhow::bail!("{x} cannot be left out: support needs Kiln's own logs, its settings and the folder listings. --exclude takes \
            other mods' files by their path in the zip (game/BuildingTools.log; see --contents) or {}", report::EXCLUDE_OTHER_MODS);
    }
    let mut input = ReportInput {
        tool: format!("kilnctl {}", kiln_manage::VERSION),
        dirs: data_dirs().ok(),
        exclude: a.exclude.clone(),
        ..Default::default()
    };
    if let Some(name) = &a.name {
        let inst = crate::config::load(name)?;
        input.facts.push(("Instance".into(), format!("{} (query port {})", inst.name, inst.query_port)));
        input.facts.push(("Wine".into(), inst.wine.to_config()));
        input.facts.push(("Policy".into(), inst.kiln.policy.as_str().to_string()));
        match kiln_manage::game::inspect(&inst.dir) {
            Ok(mut g) => {
                g.prefix = Some(inst.prefix.clone());
                input.install = Some(g);
            }
            Err(_) => input.game_dir = Some(inst.dir.clone()),
        }
    } else if let Some(d) = &a.dir {
        match kiln_manage::game::inspect(d) {
            Ok(g) => input.install = Some(g),
            Err(_) => input.game_dir = Some(d.clone()),
        }
    } else {
        // reading only: the real client folder needs no --real
        input.install = kiln_manage::game::detect().km()?.into_iter().find(|g| g.kind == kiln_manage::game::GameKind::Client);
        if input.install.is_none() {
            eprintln!("kilnctl: no Enshrouded client in any Steam library (pass an instance name or --dir); the report has the system only");
        }
    }
    let r = report::build(&input);
    if a.contents {
        print!("{}", contents_text(&report::contents(&r)));
        return Ok(0);
    }
    if a.print {
        print!("{}", r.summary);
        return Ok(0);
    }
    let out = a.out.unwrap_or_else(|| PathBuf::from(report::file_name(std::time::SystemTime::now())));
    let size = report::write_zip(&r, &out).km()?;
    print!("{}", r.short);
    println!("\nwrote {} ({}): attach it to your report. Names and paths in it are replaced; summary.txt says what was left out.",
        out.display(), kiln_manage::util::human_size(size));
    Ok(0)
}

/// `--contents`: each file with its size and what was replaced in it, grouped as the launcher's "What's in this report".
fn contents_text(c: &report::Contents) -> String {
    use report::FileGroup;
    use std::fmt::Write as _;
    let mut s = String::new();
    for (g, title) in [(FileGroup::Kiln, "Kiln's logs"), (FileGroup::Mods, "Other mods' files (--exclude can leave these out)"),
        (FileGroup::Settings, "Settings"), (FileGroup::Listings, "Folder listings")] {
        let files: Vec<_> = c.files.iter().filter(|f| f.group == g).collect();
        if files.is_empty() {
            continue;
        }
        let _ = writeln!(s, "{title}:");
        for f in files {
            let replaced = f.replaced.iter().map(|(k, n)| format!("<{k}> x{n}")).collect::<Vec<_>>().join(", ");
            let _ = writeln!(s, "  {}{} ({}){}", f.path, if f.excluded { " [left out]" } else { "" }, kiln_manage::util::human_size(f.size),
                if replaced.is_empty() { String::new() } else { format!(": {replaced}") });
        }
    }
    let _ = writeln!(s, "\nIn the zip: {} before compression.\nNever collected:", kiln_manage::util::human_size(c.size));
    for n in &c.never {
        let _ = writeln!(s, "  - {}", n.text);
    }
    s
}
