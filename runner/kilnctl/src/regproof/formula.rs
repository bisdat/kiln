// SPDX-License-Identifier: MIT
//! Formula checks (registry proof step 2, BRIEF 6.1): what Kiln.Blocks does for gear (it compares the game's
//! genericWeaponSeed, powerCreepWeapon, armour offset and gradient with the numbers Kiln assumes), generalised to every
//! regmap row with conv `formula`, and checked against the lab: kilnctl computes each formula itself over the raw member
//! values the round-trip suite read back through reflection, so a formula row is proven by a second implementation.
//!
//! The formulas are kiln.content's (native/services/content/registry/formulas.cpp over gear.cpp, and Kiln.Content's
//! GearMath): the same float steps, pinned here by the 10,000-vector hash those two implementations pin as well.
use super::regmap::Row;
use serde::Serialize;
use std::collections::BTreeMap;

/// The items registry's level range (schema/registry/items.json levels.min: 1..105, the game's gems reach 105).
pub const LEVEL_MIN: i32 = 1;
pub const LEVEL_MAX: i32 = 105;

/// The gear formulas' constants: the balance registry's gear.seed, gear.creep, gear.armorOffset, gear.armorGradient.
#[derive(Debug, Clone, Copy, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Constants {
    pub seed: f32,
    pub creep: f32,
    pub armor_offset: f32,
    pub armor_gradient: f32,
}

impl Default for Constants {
    /// The numbers Kiln assumes (gear.h kSeed, kCreep, kArmorOffset, kArmorGradient; Kiln.Blocks checks them).
    fn default() -> Self {
        Constants { seed: 10.0, creep: 0.15, armor_offset: 23.0, armor_gradient: 15.0 }
    }
}

/// The constants from the balance registry's values; None unless all four are there (never assumed).
pub fn constants_of(balance: &BTreeMap<String, f64>) -> Option<Constants> {
    let get = |k: &str| balance.get(k).copied().filter(|v| v.is_finite()).map(|v| v as f32);
    Some(Constants { seed: get("gear.seed")?, creep: get("gear.creep")?, armor_offset: get("gear.armorOffset")?, armor_gradient: get("gear.armorGradient")? })
}

/// int(seed * creep * level + seed), in float as the game computes it.
pub fn base_power(level: i32, c: &Constants) -> i32 {
    let x = c.seed * c.creep * level as f32 + c.seed;
    x as i64 as i32
}

/// The tooltip's damage: int(base_power(level) * dmgMod * speedMod).
pub fn power_of(dmg_mod: f64, speed_mod: f64, level: i32, c: &Constants) -> i32 {
    let x = base_power(level, c) as f32 * dmg_mod as f32 * speed_mod as f32;
    if x <= 0.0 { 0 } else { x as i32 }
}

/// The dmgMod that makes the tooltip show this power (half a point of margin).
pub fn dmg_mod_for(power: i32, level: i32, speed_mod: f64, c: &Constants) -> f64 {
    (power as f64 + 0.5) / (base_power(level, c) as f64 * speed_mod)
}

/// The tooltip's armour of one kind: round((gradient * level + offset) * share).
pub fn armor_of(share: f64, level: i32, c: &Constants) -> i32 {
    ((c.armor_gradient * level as f32 + c.armor_offset) * share as f32).round() as i32
}

pub fn armor_share_for(armor: i32, level: i32, c: &Constants) -> f64 {
    armor as f64 / (c.armor_gradient as f64 * level as f64 + c.armor_offset as f64)
}

/// The inputs a formula takes (the row's paths, in order), or None for a formula kilnctl does not know.
pub fn arity(formula: &str) -> Option<usize> {
    match formula {
        "gear.power" => Some(3),
        "gear.armor" => Some(2),
        _ => None,
    }
}

fn level_of(v: f64) -> Option<i32> {
    (v.is_finite() && v == v.floor() && v >= LEVEL_MIN as f64 && v <= LEVEL_MAX as f64).then_some(v as i32)
}

fn whole(v: f64) -> Option<i32> {
    (v.is_finite() && v == v.floor() && (0.0..=1e9).contains(&v)).then_some(v as i32)
}

/// The Kiln value from the game's numbers (power: [dmgMod, speedMod, minLevel]; armor: [share, minLevel]).
pub fn read(formula: &str, inputs: &[f64], c: &Constants) -> Option<f64> {
    match (formula, inputs) {
        ("gear.power", [dm, speed, level]) if dm.is_finite() && speed.is_finite() => Some(power_of(*dm, *speed, level_of(*level)?, c) as f64),
        ("gear.armor", [share, level]) if share.is_finite() => Some(armor_of(*share, level_of(*level)?, c) as f64),
        _ => None,
    }
}

/// The game's value of the first path for a Kiln value, the other inputs as they are.
pub fn write(formula: &str, kiln: f64, inputs: &[f64], c: &Constants) -> Option<f64> {
    let n = whole(kiln)?;
    match (formula, inputs) {
        ("gear.power", [_, speed, level]) => {
            let level = level_of(*level)?;
            (speed.is_finite() && *speed > 0.0 && base_power(level, c) > 0).then(|| dmg_mod_for(n, level, *speed, c))
        }
        ("gear.armor", [_, level]) => {
            let level = level_of(*level)?;
            (c.armor_gradient * level as f32 + c.armor_offset > 0.0).then(|| armor_share_for(n, level, c))
        }
        _ => None,
    }
}

/// One round-trip sample of a formula field: the schema's value before and the probe written, with the raw member values
/// (the row's paths, in order) read through reflection before the write and after it.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct Sample {
    pub entry: String,
    pub read: Option<f64>,
    pub probe: Option<f64>,
    pub raw_before: Vec<f64>,
    pub raw_after: Vec<f64>,
}

#[derive(Debug, Clone, Default, Serialize)]
pub struct RowCheck {
    pub formula: String,
    pub ok: bool,
    pub samples: usize,
    pub problems: Vec<String>,
}

#[derive(Debug, Clone, Default, Serialize)]
pub struct Report {
    pub rows: BTreeMap<String, RowCheck>,
    pub constants: Option<Constants>,
    pub previous: Option<Constants>,
    pub reference: Constants,
    pub notes: Vec<String>,
}

/// Every formula row (`<registry>.<field>`, row) against the new build's constants and the lab's samples.
pub fn check(rows: &[(String, &Row)], now: Option<Constants>, before: Option<Constants>, samples: &BTreeMap<String, Vec<Sample>>) -> Report {
    let reference = Constants::default();
    let mut r = Report { constants: now, previous: before, reference, ..Default::default() };
    if let (Some(n), Some(b)) = (now, before) {
        if n != b {
            r.notes.push(format!("this build changed the gear constants ({}): the registry's formula fields follow them, so the \
                                  tooltip power and armour of every item move with them", changes(&b, &n)));
        }
    }
    if let Some(n) = now {
        if n != reference {
            r.notes.push(format!("the gear constants are not the numbers Kiln assumes ({}): the registry's formulas follow the \
                                  build; the data packs' gear section and item blocks use Kiln's numbers until Kiln follows \
                                  (Kiln.Blocks warns at run time)", changes(&reference, &n)));
        }
    }
    for (q, row) in rows {
        if row.conv != "formula" {
            continue;
        }
        let name = row.formula.clone().unwrap_or_default();
        let mut problems = Vec::new();
        match arity(&name) {
            None => problems.push(format!("the formula {name:?} is not one kilnctl knows (gear.power, gear.armor)")),
            Some(n) if n != row.paths.len() => problems.push(format!("{name} takes {n} members, the row names {}", row.paths.len())),
            _ => {}
        }
        match now {
            None => problems.push("this build's gear constants are not known (the values export or the round-trip suite must \
                                   carry all four): the formula cannot be checked".to_string()),
            Some(c) => {
                if let Some(level) = (LEVEL_MIN..=LEVEL_MAX).find(|l| !answers(&name, *l, &c)) {
                    problems.push(format!("with this build's constants {name} gives no answer at level {level}"));
                }
            }
        }
        let ss = samples.get(q).map(Vec::as_slice).unwrap_or(&[]);
        if ss.is_empty() {
            problems.push("no round-trip sample: the formula is not proven on this build".into());
        }
        if problems.is_empty() {
            let c = now.expect("checked above");
            for s in ss {
                problems.extend(sample_problems(&name, s, row.writable(), &c));
            }
        }
        r.rows.insert(q.clone(), RowCheck { formula: name, ok: problems.is_empty(), samples: ss.len(), problems });
    }
    r
}

/// A write of the formula has an answer at this level (power: a base above 0; armour: a scale above 0).
fn answers(formula: &str, level: i32, c: &Constants) -> bool {
    match formula {
        "gear.power" => base_power(level, c) > 0,
        "gear.armor" => c.armor_gradient * level as f32 + c.armor_offset > 0.0,
        _ => true,
    }
}

fn changes(a: &Constants, b: &Constants) -> String {
    let mut v = Vec::new();
    for (k, x, y) in [("seed", a.seed, b.seed), ("creep", a.creep, b.creep), ("armour offset", a.armor_offset, b.armor_offset),
                      ("armour gradient", a.armor_gradient, b.armor_gradient)] {
        if x != y {
            v.push(format!("{k} {x} -> {y}"));
        }
    }
    v.join(", ")
}

fn show(v: Option<f64>) -> String {
    v.map(|x| x.to_string()).unwrap_or_else(|| "nothing".into())
}

/// The game keeps a float: a written member is the solve within float precision.
fn close(a: f64, b: f64) -> bool {
    (a - b).abs() <= 1e-5 * a.abs().max(1.0)
}

fn sample_problems(name: &str, s: &Sample, writable: bool, c: &Constants) -> Vec<String> {
    let (e, n) = (&s.entry, arity(name).unwrap_or(0));
    let mut p = Vec::new();
    if s.raw_before.len() != n {
        p.push(format!("{e}: {} raw values before the write, {name} takes {n}", s.raw_before.len()));
        return p;
    }
    let v = read(name, &s.raw_before, c);
    if v.is_none() || v != s.read {
        p.push(format!("{e}: the formula over the raw members gives {}, the schema read {}", show(v), show(s.read)));
    }
    let Some(probe) = s.probe else {
        if writable {
            p.push(format!("{e}: no probe was written"));
        }
        return p;
    };
    if s.raw_after.len() != n {
        p.push(format!("{e}: {} raw values after the write, {name} takes {n}", s.raw_after.len()));
        return p;
    }
    let now = read(name, &s.raw_after, c);
    if now != Some(probe) {
        p.push(format!("{e}: wrote {probe}, the formula over the raw members now gives {}", show(now)));
    }
    match write(name, probe, &s.raw_after, c) {
        Some(w) if close(w, s.raw_after[0]) => {}
        w => p.push(format!("{e}: for {probe} the formula writes {}, the game holds {}", show(w), s.raw_after[0])),
    }
    p
}

#[cfg(test)]
mod tests {
    use super::*;

    fn splitmix(s: &mut u64) -> u64 {
        *s = s.wrapping_add(0x9e3779b97f4a7c15);
        let mut z = *s;
        z = (z ^ (z >> 30)).wrapping_mul(0xbf58476d1ce4e5b9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94d049bb133111eb);
        z ^ (z >> 31)
    }
    fn fnv(h: &mut u64, b: &[u8]) {
        for x in b {
            *h = (*h ^ *x as u64).wrapping_mul(0x100000001b3);
        }
    }

    /// native/services/content/tests/registry_formulas_test.cpp and Kiln.Blocks.Tests GearAgreementTests pin the same hash:
    /// the port gives gear.cpp's numbers bit for bit.
    #[test]
    fn the_port_gives_the_native_and_managed_numbers() {
        let g = Constants::default();
        let (mut s, mut h) = (0x6b696c6e2d726567u64, 0xcbf29ce484222325u64);
        for _ in 0..10_000 {
            let level = 1 + (splitmix(&mut s) % 55) as i32;
            let power = (splitmix(&mut s) % 2000) as i32;
            let speed = 0.5 + (splitmix(&mut s) % 200) as f64 / 100.0;
            let share = (splitmix(&mut s) % 1000) as f64 / 1000.0;
            let armor = (splitmix(&mut s) % 500) as i32;
            let bp = base_power(level, &g);
            let dm = dmg_mod_for(power, level, speed, &g);
            let back = power_of(dm, speed, level, &g);
            let ar = armor_of(share, level, &g);
            let r#as = armor_share_for(armor, level, &g) as f32;
            fnv(&mut h, &bp.to_le_bytes());
            fnv(&mut h, &dm.to_le_bytes());
            fnv(&mut h, &back.to_le_bytes());
            fnv(&mut h, &ar.to_le_bytes());
            fnv(&mut h, &r#as.to_le_bytes());
        }
        assert_eq!(h, 0xa40975297674a479, "gear vectors: fnv64 {h:#018x}");
    }

    #[test]
    fn formulas_by_name_as_kiln_content_takes_them() {
        let g = Constants::default();
        assert_eq!((arity("gear.power"), arity("gear.armor"), arity("gear.other")), (Some(3), Some(2), None));
        assert_eq!(base_power(10, &Constants { seed: 20.0, ..g }), 2 * base_power(10, &g));
        assert_eq!(armor_of(1.0, 2, &Constants { armor_gradient: 30.0, ..g }), 83);
        assert_eq!(armor_of(1.0, 2, &g), 53);
        assert_eq!(read("gear.power", &[dmg_mod_for(42, 5, 1.0, &g), 1.0, 5.0], &g), Some(42.0));
        assert_eq!(write("gear.power", 42.0, &[0.0, 1.0, 5.0], &g), Some(dmg_mod_for(42, 5, 1.0, &g)));
        let share = write("gear.armor", 60.0, &[0.0, 3.0], &g).unwrap();
        assert_eq!(read("gear.armor", &[share, 3.0], &g), Some(60.0));
        assert_eq!(read("gear.power", &[1.0], &g), None);
        assert_eq!(write("gear.power", 42.0, &[0.0, 0.0, 5.0], &g), None, "speed 0 gives no dmgMod");
        assert_eq!(write("gear.power", 42.0, &[0.0, 1.0, 106.0], &g), None, "a level outside the field's range");
        assert_eq!(write("gear.power", 42.5, &[0.0, 1.0, 5.0], &g), None, "a power is a whole number");
        let dm = write("gear.power", 900.0, &[0.0, 1.0, 105.0], &g).unwrap();
        assert_eq!(read("gear.power", &[dm, 1.0, 105.0], &g), Some(900.0));
        let mut b = BTreeMap::new();
        assert_eq!(constants_of(&b), None, "no constants: unknown, never assumed");
        b.insert("gear.seed".to_string(), 12.0);
        b.insert("gear.creep".to_string(), 0.2);
        b.insert("gear.armorOffset".to_string(), 20.0);
        assert_eq!(constants_of(&b), None, "every constant or none");
        b.insert("gear.armorGradient".to_string(), 16.0);
        assert_eq!(constants_of(&b), Some(Constants { seed: 12.0, creep: 0.2, armor_offset: 20.0, armor_gradient: 16.0 }));
    }

    fn power_row() -> Row {
        serde_json::from_str(r#"{"paths": ["hit.mod", "hit.pace", "range.min"], "conv": "formula", "formula": "gear.power", "read": "now", "backend": "live"}"#).unwrap()
    }

    /// A sample as a correct lab would report it: power 30 at level 4, speed 1.25, written to 44.
    fn good(c: &Constants) -> Sample {
        let before = dmg_mod_for(30, 4, 1.25, c);
        let after = dmg_mod_for(44, 4, 1.25, c) as f32 as f64; // the game keeps a float
        Sample { entry: "game:a".into(), read: Some(30.0), probe: Some(44.0), raw_before: vec![before, 1.25, 4.0], raw_after: vec![after, 1.25, 4.0] }
    }

    fn one(field: &str, s: Vec<Sample>) -> BTreeMap<String, Vec<Sample>> {
        BTreeMap::from([(field.to_string(), s)])
    }

    #[test]
    fn a_formula_row_is_proven_by_the_labs_raw_values() {
        let g = Constants::default();
        let row = power_row();
        let rows = vec![("gadgets.power".to_string(), &row)];
        let r = check(&rows, Some(g), Some(g), &one("gadgets.power", vec![good(&g)]));
        let c = &r.rows["gadgets.power"];
        assert!(c.ok, "{c:?}");
        assert_eq!((c.samples, c.formula.as_str()), (1, "gear.power"));
        assert!(r.notes.is_empty(), "{:?}", r.notes);
        // the schema read disagrees with the formula over the raw members
        let mut bad = good(&g);
        bad.read = Some(31.0);
        let r = check(&rows, Some(g), Some(g), &one("gadgets.power", vec![bad]));
        assert!(!r.rows["gadgets.power"].ok);
        assert!(r.rows["gadgets.power"].problems[0].contains("game:a"), "{:?}", r.rows["gadgets.power"]);
        // the member written is not what the formula gives for the probe
        let mut off = good(&g);
        off.raw_after[0] *= 1.01;
        assert!(!check(&rows, Some(g), Some(g), &one("gadgets.power", vec![off])).rows["gadgets.power"].ok);
        // a sample without the raw values proves nothing
        let mut thin = good(&g);
        thin.raw_after.clear();
        assert!(!check(&rows, Some(g), Some(g), &one("gadgets.power", vec![thin])).rows["gadgets.power"].ok);
    }

    #[test]
    fn a_formula_row_fails_closed() {
        let g = Constants::default();
        let row = power_row();
        let rows = vec![("gadgets.power".to_string(), &row)];
        let no_samples = check(&rows, Some(g), Some(g), &BTreeMap::new());
        assert!(!no_samples.rows["gadgets.power"].ok);
        assert!(no_samples.rows["gadgets.power"].problems.iter().any(|p| p.contains("no round-trip sample")));
        let no_constants = check(&rows, None, Some(g), &one("gadgets.power", vec![good(&g)]));
        assert!(!no_constants.rows["gadgets.power"].ok);
        let mut unknown = power_row();
        unknown.formula = Some("gear.other".into());
        let rows_u = vec![("gadgets.power".to_string(), &unknown)];
        assert!(!check(&rows_u, Some(g), Some(g), &one("gadgets.power", vec![good(&g)])).rows["gadgets.power"].ok);
        let mut short = power_row();
        short.paths.pop();
        let rows_s = vec![("gadgets.power".to_string(), &short)];
        assert!(!check(&rows_s, Some(g), Some(g), &one("gadgets.power", vec![good(&g)])).rows["gadgets.power"].ok);
        // constants with which the formula gives no answer at some level of the field's range
        let zero = Constants { seed: 0.0, ..g };
        let r = check(&rows, Some(zero), Some(g), &one("gadgets.power", vec![good(&zero)]));
        assert!(!r.rows["gadgets.power"].ok);
        // a row that is not a formula is not checked here
        let plain: Row = serde_json::from_str(r#"{"paths": ["a"], "conv": "int", "backend": "live"}"#).unwrap();
        assert!(check(&[("gadgets.size".to_string(), &plain)], Some(g), Some(g), &BTreeMap::new()).rows.is_empty());
    }

    #[test]
    fn changed_constants_are_followed_and_reported() {
        let g = Constants::default();
        let moved = Constants { seed: 11.0, ..g };
        let row = power_row();
        let rows = vec![("gadgets.power".to_string(), &row)];
        let r = check(&rows, Some(moved), Some(g), &one("gadgets.power", vec![good(&moved)]));
        assert!(r.rows["gadgets.power"].ok, "{:?}", r.rows["gadgets.power"]);
        assert!(r.notes.iter().any(|n| n.contains("seed 10 -> 11") && n.contains("this build changed")), "{:?}", r.notes);
        assert!(r.notes.iter().any(|n| n.contains("Kiln assumes")), "{:?}", r.notes);
        assert_eq!(r.constants, Some(moved));
    }
}
