//! Save-editor display names from the installed game, never bundled game text.
use super::decode::{Decoder, Offsets, guid_bytes_at, guid_value, scalar, u32_at};
use super::extract::{Src, read_loca_all};
use crate::Result;
use crate::gameart::kfc::fnv1a32;
use crate::gameart::map::markers::clean;
use serde::Serialize;
use std::collections::BTreeMap;
use std::path::Path;

type Tables = BTreeMap<String, BTreeMap<u32, String>>;
type Tags = BTreeMap<String, BTreeMap<u32, Vec<u32>>>;

#[derive(Debug, Serialize)]
pub(crate) struct Names {
    format: u32,
    languages: BTreeMap<String, BTreeMap<String, BTreeMap<u32, String>>>,
}

impl Names {
    pub(crate) fn language_count(&self) -> u64 {
        self.languages.len() as u64
    }
}

fn names_from_tags(tables: &Tables, tags: &Tags) -> Names {
    let english = tables.get("en-us");
    let languages = tables
        .iter()
        .map(|(code, table)| {
            let sections = tags
                .iter()
                .map(|(kind, entries)| {
                    let names = entries
                        .iter()
                        .filter_map(|(id, candidates)| {
                            let read = |t: &BTreeMap<u32, String>| {
                                candidates
                                    .iter()
                                    .filter(|tag| **tag != 0)
                                    .filter_map(|tag| t.get(tag))
                                    .map(|text| clean(text))
                                    .find(|text| !text.trim().is_empty())
                            };
                            read(table).or_else(|| english.and_then(read)).map(|name| (*id, name))
                        })
                        .collect();
                    (kind.clone(), names)
                })
                .collect();
            (code.clone(), sections)
        })
        .collect();
    Names { format: 1, languages }
}

/// The same ids as items.json, skills.nodes and conhecimento.json: itemId, skill node id and recipeId.
/// Knowledge flags without a player-facing name have no loca tag and deliberately keep the editor's old label.
pub(crate) fn extract(game_dir: &Path) -> Result<Names> {
    let s = Src::open(game_dir)?;
    let tables = read_loca_all(&s)?;
    let o = Offsets::new(&s.types);
    let mut items = BTreeMap::new();
    let mut item_guids = BTreeMap::new();
    let id_off = o.need("keen::ItemInfo", "itemId")?;
    let name_off = o.need("keen::ItemInfo", "name")?;
    for (ix, key) in s.of_type("keen::ItemInfo")? {
        if key.part != 0 {
            continue;
        }
        let b = s.kfc.read(ix)?;
        if let (Some(id), Some(guid)) = (u32_at(&b, id_off), guid_bytes_at(&b, name_off)) {
            let tag = fnv1a32(&guid);
            items.insert(id, vec![tag]);
            item_guids.insert(key.guid_text(), vec![tag]);
        }
    }
    let dec = Decoder::new(&s.types);
    let tree = dec.decode("keen::SkillTreeResource", &s.one("keen::SkillTreeResource")?, 0)?;
    let skills =
        tree["nodes"].as_array().into_iter().flatten().filter_map(|node| Some((scalar(&node["id"])? as u32, vec![scalar(&node["name"])? as u32]))).collect();
    let mut knowledge = items.clone();
    let recipes = dec.decode("keen::RecipeRegistryResource", &s.one("keen::RecipeRegistryResource")?, 0)?;
    for recipe in recipes["recipes"].as_array().into_iter().flatten() {
        let Some(id) = scalar(&recipe["recipeId"]) else { continue };
        let mut tags = Vec::new();
        if let Some(guid) = guid_value(&recipe["recipeName"]).and_then(|g| crate::gameart::modded::guid_bytes(&g)) {
            tags.push(fnv1a32(&guid));
        }
        // Unnamed recipes display the produced item's name, just as the existing editor tables do.
        if let Some(output) = recipe["output"].as_array().and_then(|a| a.first()) {
            let output_tags =
                scalar(&output["item"]).and_then(|id| items.get(&(id as u32))).or_else(|| output["itemRef"].as_str().and_then(|guid| item_guids.get(guid)));
            if let Some(output_tags) = output_tags {
                tags.extend(output_tags);
            }
        }
        knowledge.insert(id as u32, tags);
    }
    Ok(names_from_tags(&tables, &BTreeMap::from([("items".into(), items), ("skills".into(), skills), ("knowledge".into(), knowledge)])))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::gameart::map::markers::loca_table;

    fn fixture(entries: &[(u32, &str)]) -> Vec<u8> {
        let mut b = vec![0u8; 8 + entries.len() * 24];
        b[..4].copy_from_slice(&8i32.to_le_bytes());
        b[4..8].copy_from_slice(&(entries.len() as u32).to_le_bytes());
        for (i, (id, text)) in entries.iter().enumerate() {
            let o = 8 + i * 24;
            b[o..o + 4].copy_from_slice(&id.to_le_bytes());
            let at = b.len();
            b[o + 4..o + 8].copy_from_slice(&((at - o - 4) as i32).to_le_bytes());
            b[o + 8..o + 12].copy_from_slice(&(text.len() as u32).to_le_bytes());
            b.extend_from_slice(text.as_bytes());
        }
        b
    }

    fn tags() -> Tags {
        BTreeMap::from([
            ("items".into(), BTreeMap::from([(123, vec![7])])),
            ("skills".into(), BTreeMap::from([(456, vec![42])])),
            ("knowledge".into(), BTreeMap::from([(789, vec![99, 7])])),
        ])
    }

    #[test]
    fn editor_names_extract_every_language_and_decimal_editor_ids() {
        // Synthetic loca content, parsed with the same reader as the installed game's tables.
        let codes = ["en-us", "de-de", "fr-fr", "it-it", "es-es", "ru-ru", "pl-pl", "ja-jp", "ko-kr", "pt-br", "zh-cn", "zh-tw", "th-th", "tr-tr", "uk-ua"];
        let tables = codes
            .iter()
            .map(|code| {
                let text = format!("<b>Test {code}</b>");
                ((*code).into(), loca_table(&fixture(&[(7, &text), (42, "Test skill")]), 0, 4, 24).unwrap())
            })
            .collect();
        let names = names_from_tags(&tables, &tags());
        assert_eq!(names.languages.len(), codes.len());
        for code in codes {
            assert_eq!(names.languages[code]["items"][&123], format!("Test {code}"));
            assert_eq!(names.languages[code]["skills"][&456], "Test skill");
            assert_eq!(names.languages[code]["knowledge"][&789], format!("Test {code}"));
        }
        let v = serde_json::to_value(names).unwrap();
        assert_eq!(v["languages"]["zh-cn"]["items"]["123"], "Test zh-cn");
    }

    #[test]
    fn editor_names_missing_tag_uses_english_then_omits_unknowns() {
        let tables = BTreeMap::from([
            ("en-us".into(), loca_table(&fixture(&[(7, "English item"), (42, "English skill")]), 0, 4, 24).unwrap()),
            ("zh-cn".into(), loca_table(&fixture(&[(7, "测试物品"), (42, "<b></b>")]), 0, 4, 24).unwrap()),
        ]);
        let mut tags = tags();
        tags.get_mut("items").unwrap().insert(999, vec![1000]);
        let names = names_from_tags(&tables, &tags);
        assert_eq!(names.languages["zh-cn"]["skills"][&456], "English skill");
        assert_eq!(names.languages["zh-cn"]["knowledge"][&789], "测试物品");
        assert!(!names.languages["zh-cn"]["items"].contains_key(&999));
    }

    #[test]
    fn editor_names_missing_english_does_not_use_an_unrelated_language() {
        let tables = BTreeMap::from([("de-de".into(), BTreeMap::from([(7, "Test".into())]))]);
        let names = names_from_tags(&tables, &tags());
        assert_eq!(names.languages["de-de"]["items"][&123], "Test");
        assert!(names.languages["de-de"]["skills"].is_empty());
    }
}
