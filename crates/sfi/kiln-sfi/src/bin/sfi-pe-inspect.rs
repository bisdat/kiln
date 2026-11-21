// SPDX-License-Identifier: MIT
//! Read-only corpus diagnostic. No DLL load, no runtime activation, no implicit
//! trusted reviews/system modules. Verdict output is deliberately not a gate
//! proof or a launch-plan admission token.
use kiln_sfi::{
    native_contracts::{AdmissionVerdict, NativeContracts},
    pe_gate::{self, Image},
};
use std::{
    fs,
    io::{self, Read},
    path::{Path, PathBuf},
};

fn collect(root: &Path, at: &Path, output: &mut Vec<(String, Vec<u8>)>) -> io::Result<()> {
    if output.len() > 1024 || at.components().count() > root.components().count() + 128 {
        return Err(io::Error::other(
            "corpus image-count/directory-depth budget exceeded",
        ));
    }
    let mut entries = fs::read_dir(at)?.collect::<Result<Vec<_>, _>>()?;
    entries.sort_by_key(|e| e.file_name());
    for entry in entries {
        let kind = entry.file_type()?;
        if kind.is_symlink() {
            continue;
        }
        if kind.is_dir() {
            collect(root, &entry.path(), output)?;
        } else if kind.is_file()
            && entry
                .path()
                .extension()
                .is_some_and(|e| e.eq_ignore_ascii_case("dll"))
        {
            let mut bytes = Vec::new();
            fs::File::open(entry.path())?
                .take((pe_gate::MAX_IMAGE_BYTES + 1) as u64)
                .read_to_end(&mut bytes)?;
            if bytes.len() > pe_gate::MAX_IMAGE_BYTES {
                return Err(io::Error::other("corpus image byte budget exceeded"));
            }
            let relative = entry
                .path()
                .strip_prefix(root)
                .map_err(io::Error::other)?
                .to_str()
                .ok_or_else(|| io::Error::other("non-UTF-8 corpus path"))?
                .replace('\\', "/");
            if output.len() >= 1024
                || output.iter().map(|(_, bytes)| bytes.len()).sum::<usize>() + bytes.len()
                    > 256 * 1024 * 1024
            {
                return Err(io::Error::other(
                    "corpus image-count/aggregate-byte budget exceeded",
                ));
            }
            output.push((relative, bytes));
        }
    }
    Ok(())
}
fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args: Vec<PathBuf> = std::env::args_os().skip(1).map(PathBuf::from).collect();
    if args.is_empty() {
        return Err("usage: sfi-pe-inspect <corpus directory>...".into());
    }
    let registry = NativeContracts::shipped()?;
    let mut counts = [0usize; 4];
    for root in args {
        let mut owned = Vec::new();
        collect(&root, &root, &mut owned)?;
        let images: Vec<_> = owned
            .iter()
            .map(|(path, bytes)| Image { path, bytes })
            .collect();
        for image in &images {
            // This host-side corpus classification cannot prove Windows system
            // or API-set binding. The executable loader uses closure-bound permits.
            let report = registry.advisory(image.path, &images);
            let label = match report.verdict {
                AdmissionVerdict::Pass => {
                    counts[0] += 1;
                    "pass"
                }
                AdmissionVerdict::AdmittedWithEnforcement => {
                    counts[1] += 1;
                    "admitted-with-enforcement"
                }
                AdmissionVerdict::NeedsContract => {
                    counts[2] += 1;
                    "needs-contract"
                }
                AdmissionVerdict::Refused => {
                    counts[3] += 1;
                    "refused"
                }
            };
            let reason = report.reasons.join("; ");
            println!("{label}\t{}\t{reason}", root.join(image.path).display());
        }
    }
    eprintln!(
        "verdicts: pass={} admitted-with-enforcement={} needs-contract={} refused={}",
        counts[0], counts[1], counts[2], counts[3]
    );
    Ok(())
}
