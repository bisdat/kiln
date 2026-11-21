// SPDX-License-Identifier: MIT
//! The archive type registry, read page by page through a
//! [`TypeRegistryPort`] and checked before any Lua sees it.
//!
//! The pages follow `schema/sfi/eml-archive/registry.schema.json` (schema_version 1) and
//! the type records `common.schema.json#/$defs/type` exactly: every listed property, no
//! other, the tagged `u64:`/`bytes:` texts, bounded 32-bit integers. On top of the shape
//! the loader checks what the schema cannot say and what ShroudForge's registry relies on
//! (its `game.types` reads a registry built with these guarantees):
//! - every page carries the first page's pin, versions and count, its own `first`, and
//!   exactly `min(limit, count - first)` types; the pin is the launch's when the port
//!   names one;
//! - a type's `index` is its registry ordinal (ShroudForge looks types up by index as a
//!   position);
//! - qualified hashes are unique, and so are the Impact hashes of types without
//!   `HAS_DS` (the two lookup indexes);
//! - `primitive_type` is one of the 30 names, `flags` are exactly the names of
//!   `flags_bits` (one byte) and `has_ds` its `HAS_DS` bit, an enum value's
//!   `value_hex` is its value, and names are unique within a struct's fields, an enum's
//!   fields and an attribute list;
//! - the provider's resolved `fields` equal the base-first fields computed here, where
//!   those can be computed (a cyclic or unresolved base is reported by `Type.fields` at
//!   use, as ShroudForge does).
//!
//! References to other types (inner, field and attribute types) may name an index the
//! registry does not have: ShroudForge answers those with nil, or with its unresolved
//! base text, at use.
//!
//! Any failure refuses the whole registry: there is no partial registry.

use super::port::{NativeType, NativeTypes, PortError, RegistryPin, TypeRegistryPort};
use super::selectors;
use serde_json::{Map, Value};
use std::collections::{HashMap, HashSet};

/// Types asked for per `registry_metadata` call.
pub const PAGE_LIMIT: u32 = 256;

/// ShroudForge's `PrimitiveType` names, in its order (`primitive_type_to_string`).
pub const PRIMITIVE_TYPES: [&str; 30] = [
    "None",
    "Bool",
    "UInt8",
    "SInt8",
    "UInt16",
    "SInt16",
    "UInt32",
    "SInt32",
    "UInt64",
    "SInt64",
    "Float32",
    "Float64",
    "Enum",
    "Bitmask8",
    "Bitmask16",
    "Bitmask32",
    "Bitmask64",
    "Typedef",
    "Struct",
    "StaticArray",
    "DsArray",
    "DsString",
    "DsOptional",
    "DsVariant",
    "BlobArray",
    "BlobString",
    "BlobOptional",
    "BlobVariant",
    "ObjectReference",
    "Guid",
];

/// The type flag names, in bit order (bit 0 first); `flags` lists the set ones in
/// this order.
pub const TYPE_FLAGS: [&str; 8] = [
    "HAS_DS",
    "HAS_BLOB_ARRAY",
    "HAS_BLOB_STRING",
    "HAS_BLOB_OPTIONAL",
    "HAS_BLOB_VARIANT",
    "IS_GPU_UNIFORM",
    "IS_GPU_STORAGE",
    "IS_GPU_CONSTANT",
];
/// `HAS_DS`.
pub const HAS_DS: u32 = 0x01;

/// An attribute of a type or a struct field.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Attribute {
    /// The attribute map's key.
    pub key: String,
    pub name: String,
    pub namespace: Vec<String>,
    pub type_index: Option<usize>,
    pub value: String,
}

/// One of a struct's own fields.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct StructField {
    pub name: String,
    pub type_index: usize,
    pub data_offset: u64,
    pub attributes: Vec<Attribute>,
}

/// One enum field; `value` keeps all 64 bits.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct EnumField {
    pub name: String,
    pub value: u64,
}

/// One reflected type, as the archive registry describes it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TypeMeta {
    pub index: usize,
    pub name: String,
    pub qualified_name: String,
    pub impact_name: String,
    pub namespace: Vec<String>,
    pub qualified_hash: u32,
    pub internal_hash: u32,
    pub name_hash: u32,
    pub impact_hash: u32,
    pub inner_type: Option<usize>,
    pub size: u32,
    pub alignment: u32,
    pub element_alignment: u32,
    pub field_count: u32,
    pub primitive_type: &'static str,
    pub flags_bits: u8,
    pub default_bytes: Option<Vec<u8>>,
    pub struct_fields: Vec<StructField>,
    pub enum_fields: Vec<EnumField>,
    pub attributes: Vec<Attribute>,
}

impl TypeMeta {
    /// The set flags' names, in bit order.
    pub fn flags(&self) -> Vec<&'static str> {
        flag_names(self.flags_bits as u32)
    }
    pub fn has_ds(&self) -> bool {
        self.flags_bits as u32 & HAS_DS != 0
    }
}

fn flag_names(bits: u32) -> Vec<&'static str> {
    TYPE_FLAGS
        .iter()
        .enumerate()
        .filter(|(bit, _)| bits & (1 << bit) != 0)
        .map(|(_, name)| *name)
        .collect()
}

/// Why the registry was refused.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum LoadError {
    /// The port could not read a page.
    Port(PortError),
    /// A page does not follow the schema, or breaks a registry guarantee.
    Malformed(String),
    /// kiln.types disagrees with the archive registry (the cross-check).
    Disagrees(String),
}

impl std::fmt::Display for LoadError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            LoadError::Port(error) => write!(f, "{error}"),
            LoadError::Malformed(why) => write!(f, "malformed registry metadata: {why}"),
            LoadError::Disagrees(why) => {
                write!(f, "kiln.types disagrees with the archive registry: {why}")
            }
        }
    }
}

/// What the kiln.types cross-check found when it did not refuse.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum CrossCheck {
    /// Same count, and every qualified name with the same size.
    Agreed { count: usize },
    /// kiln.types could not be read here; nothing was compared.
    Skipped(String),
}

/// The checked archive registry of one launch.
#[derive(Debug)]
pub struct Registry {
    pin: RegistryPin,
    game_version: String,
    registry_version: String,
    types: Vec<TypeMeta>,
    by_qualified_hash: HashMap<u32, usize>,
    by_impact_hash: HashMap<u32, usize>,
}

impl Registry {
    /// Reads every page through `port` and checks the whole registry (module docs).
    pub fn load(port: &dyn TypeRegistryPort) -> Result<Self, LoadError> {
        let header = Page::parse(&port.page(0, 0).map_err(LoadError::Port)?)?;
        if header.first != 0 || !header.types.is_empty() {
            return Err(malformed(
                "the header page (first 0, limit 0) must start at 0 with no types",
            ));
        }
        if let Some(expected) = port.expected_pin()
            && expected != header.pin
        {
            return Err(malformed(
                "the registry is pinned to another launch identity",
            ));
        }
        let count = usize::try_from(header.count)
            .map_err(|_| malformed("count does not fit this process"))?;
        let mut types = Vec::with_capacity(count.min(1 << 16));
        while types.len() < count {
            let first = types.len() as u64;
            let limit = (count - types.len()).min(PAGE_LIMIT as usize) as u32;
            let page = Page::parse(&port.page(first, limit).map_err(LoadError::Port)?)?;
            if page.pin != header.pin
                || page.game_version != header.game_version
                || page.registry_version != header.registry_version
                || page.count != header.count
            {
                return Err(malformed(format!(
                    "the page at {first} belongs to another registry (pin, versions or count differ)"
                )));
            }
            if page.first != first || page.types.len() != limit as usize {
                return Err(malformed(format!(
                    "asked for {limit} types from {first}, got {} from {}",
                    page.types.len(),
                    page.first
                )));
            }
            types.extend(page.types);
        }
        Self::new(
            header.pin,
            header.game_version,
            header.registry_version,
            types,
        )
    }

    fn new(
        pin: RegistryPin,
        game_version: String,
        registry_version: String,
        types: Vec<(TypeMeta, Vec<ResolvedField>)>,
    ) -> Result<Self, LoadError> {
        let mut by_qualified_hash = HashMap::new();
        let mut by_impact_hash = HashMap::new();
        for (ordinal, (ty, _)) in types.iter().enumerate() {
            if ty.index != ordinal {
                return Err(malformed(format!(
                    "type {} has index {} at registry ordinal {ordinal}",
                    ty.qualified_name, ty.index
                )));
            }
            if by_qualified_hash
                .insert(ty.qualified_hash, ordinal)
                .is_some()
            {
                return Err(malformed(format!(
                    "duplicate qualified hash 0x{:08x} ({})",
                    ty.qualified_hash, ty.qualified_name
                )));
            }
            if !ty.has_ds() && by_impact_hash.insert(ty.impact_hash, ordinal).is_some() {
                return Err(malformed(format!(
                    "duplicate Impact hash 0x{:08x} ({})",
                    ty.impact_hash, ty.qualified_name
                )));
            }
        }
        let (types, resolved): (Vec<_>, Vec<_>) = types.into_iter().unzip();
        let registry = Self {
            pin,
            game_version,
            registry_version,
            types,
            by_qualified_hash,
            by_impact_hash,
        };
        for (ty, given) in registry.types.iter().zip(resolved) {
            if let Ok(fields) = selectors::fields(&registry, ty.index) {
                let computed: Vec<ResolvedField> = fields
                    .into_iter()
                    .map(|(owner, field)| ResolvedField {
                        field: field.clone(),
                        declaring_type: owner,
                    })
                    .collect();
                if computed != given {
                    return Err(malformed(format!(
                        "the resolved fields of {} do not follow its bases' fields",
                        ty.qualified_name
                    )));
                }
            }
        }
        Ok(registry)
    }

    pub fn pin(&self) -> &RegistryPin {
        &self.pin
    }
    /// The selected archive's game version text (`game.version`).
    pub fn game_version(&self) -> &str {
        &self.game_version
    }
    /// The registry's own version text (`game.types.version`).
    pub fn registry_version(&self) -> &str {
        &self.registry_version
    }
    pub fn len(&self) -> usize {
        self.types.len()
    }
    pub fn is_empty(&self) -> bool {
        self.types.is_empty()
    }
    pub fn get(&self, index: usize) -> Option<&TypeMeta> {
        self.types.get(index)
    }
    pub fn iter(&self) -> impl Iterator<Item = &TypeMeta> {
        self.types.iter()
    }
    /// The type whose qualified hash is the FNV-1a of `name` and whose qualified name is
    /// exactly `name` (the hash index, then the exact name).
    pub fn by_qualified_name(&self, name: &str) -> Option<&TypeMeta> {
        self.by_qualified_hash
            .get(&fnv1a32(name.as_bytes()))
            .map(|&at| &self.types[at])
            .filter(|ty| ty.qualified_name == name)
    }
    /// As [`Self::by_qualified_name`] for Impact names; types with `HAS_DS` have no
    /// Impact entry.
    pub fn by_impact_name(&self, name: &str) -> Option<&TypeMeta> {
        self.by_impact_hash
            .get(&fnv1a32(name.as_bytes()))
            .map(|&at| &self.types[at])
            .filter(|ty| ty.impact_name == name)
    }

    /// The kiln.types cross-check: the same count, and every
    /// qualified name kiln.types reports once, with the archive's size. A native table
    /// that cannot be read skips the check; a disagreement refuses.
    pub fn cross_check(&self, native: &dyn NativeTypes) -> Result<CrossCheck, LoadError> {
        let natives: Vec<NativeType> = match native.types() {
            Ok(natives) => natives,
            Err(why) => return Ok(CrossCheck::Skipped(why)),
        };
        if natives.len() != self.types.len() {
            return Err(LoadError::Disagrees(format!(
                "kiln.types has {} types, the archive registry {}",
                natives.len(),
                self.types.len()
            )));
        }
        let sizes: HashMap<&[u8], u32> = self
            .types
            .iter()
            .map(|ty| (ty.qualified_name.as_bytes(), ty.size))
            .collect();
        let mut seen = HashSet::new();
        for native in &natives {
            let name = String::from_utf8_lossy(&native.qualified_name);
            if !seen.insert(native.qualified_name.as_slice()) {
                return Err(LoadError::Disagrees(format!(
                    "kiln.types lists {name} twice"
                )));
            }
            match sizes.get(native.qualified_name.as_slice()) {
                None => {
                    return Err(LoadError::Disagrees(format!(
                        "the archive registry has no type {name}"
                    )));
                }
                Some(&size) if size != native.size => {
                    return Err(LoadError::Disagrees(format!(
                        "{name} is {} bytes in kiln.types and {size} in the archive registry",
                        native.size
                    )));
                }
                Some(_) => {}
            }
        }
        Ok(CrossCheck::Agreed {
            count: natives.len(),
        })
    }
}

/// FNV-1a over bytes, 32 bits (offset basis 0x811c9dc5, prime 0x01000193): the
/// registry's qualified and Impact name hash.
pub fn fnv1a32(bytes: &[u8]) -> u32 {
    bytes.iter().fold(0x811c_9dc5_u32, |hash, &byte| {
        (hash ^ u32::from(byte)).wrapping_mul(0x0100_0193)
    })
}

fn malformed(why: impl Into<String>) -> LoadError {
    LoadError::Malformed(why.into())
}

// ---- the page's shape (registry.schema.json, common.schema.json) ----

/// A resolved field as the page lists it (`fields`), for the consistency check.
#[derive(Debug, PartialEq, Eq)]
struct ResolvedField {
    field: StructField,
    declaring_type: usize,
}

struct Page {
    pin: RegistryPin,
    game_version: String,
    registry_version: String,
    count: u64,
    first: u64,
    types: Vec<(TypeMeta, Vec<ResolvedField>)>,
}

type Fields<'a> = &'a Map<String, Value>;

/// `value` is an object with exactly `keys` (all required, no others).
fn object<'a>(value: &'a Value, what: &str, keys: &[&str]) -> Result<Fields<'a>, LoadError> {
    let map = value
        .as_object()
        .ok_or_else(|| malformed(format!("{what} is not an object")))?;
    if let Some(missing) = keys.iter().find(|key| !map.contains_key(**key)) {
        return Err(malformed(format!("{what} has no {missing}")));
    }
    if let Some(extra) = map.keys().find(|key| !keys.contains(&key.as_str())) {
        return Err(malformed(format!("{what} has an unknown property {extra}")));
    }
    Ok(map)
}

fn text(map: Fields<'_>, key: &str, what: &str) -> Result<String, LoadError> {
    map[key]
        .as_str()
        .map(str::to_owned)
        .ok_or_else(|| malformed(format!("{what}.{key} is not a string")))
}

fn texts(map: Fields<'_>, key: &str, what: &str) -> Result<Vec<String>, LoadError> {
    map[key]
        .as_array()
        .and_then(|items| {
            items
                .iter()
                .map(|item| item.as_str().map(str::to_owned))
                .collect()
        })
        .ok_or_else(|| malformed(format!("{what}.{key} is not an array of strings")))
}

fn lowercase_hex(digits: &str) -> bool {
    digits
        .bytes()
        .all(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f'))
}

fn tagged_u64(value: &Value, what: &str) -> Result<u64, LoadError> {
    value
        .as_str()
        .and_then(|text| text.strip_prefix("u64:"))
        .filter(|digits| digits.len() == 16 && lowercase_hex(digits))
        .and_then(|digits| u64::from_str_radix(digits, 16).ok())
        .ok_or_else(|| malformed(format!("{what} is not a u64: text")))
}

fn tagged_index(value: &Value, what: &str) -> Result<usize, LoadError> {
    usize::try_from(tagged_u64(value, what)?)
        .map_err(|_| malformed(format!("{what} does not fit this process")))
}

fn optional_index(value: &Value, what: &str) -> Result<Option<usize>, LoadError> {
    match value {
        Value::Null => Ok(None),
        other => tagged_index(other, what).map(Some),
    }
}

fn bounded_u32(value: &Value, what: &str) -> Result<u32, LoadError> {
    value
        .as_u64()
        .and_then(|number| u32::try_from(number).ok())
        .ok_or_else(|| malformed(format!("{what} is not an integer in [0, 4294967295]")))
}

fn sha256_text(map: Fields<'_>, key: &str) -> Result<String, LoadError> {
    let value = text(map, key, "pin")?;
    if value.len() == 64 && lowercase_hex(&value) {
        Ok(value)
    } else {
        Err(malformed(format!("pin.{key} is not a lowercase SHA-256")))
    }
}

fn uuid_text(map: Fields<'_>, key: &str) -> Result<String, LoadError> {
    let value = text(map, key, "pin")?;
    let groups: Vec<&str> = value.split('-').collect();
    let lengths: Vec<usize> = groups.iter().map(|g| g.len()).collect();
    if lengths == [8, 4, 4, 4, 12] && groups.iter().all(|g| lowercase_hex(g)) {
        Ok(value)
    } else {
        Err(malformed(format!("pin.{key} is not a lowercase UUID")))
    }
}

fn pin(value: &Value) -> Result<RegistryPin, LoadError> {
    let map = object(
        value,
        "pin",
        &[
            "launch_id",
            "plan_id",
            "generation",
            "reload_epoch",
            "plan_sha256",
            "archive_sha256",
            "registry_id",
            "registry_sha256",
        ],
    )?;
    Ok(RegistryPin {
        launch_id: uuid_text(map, "launch_id")?,
        plan_id: uuid_text(map, "plan_id")?,
        generation: tagged_u64(&map["generation"], "pin.generation")?,
        reload_epoch: tagged_u64(&map["reload_epoch"], "pin.reload_epoch")?,
        plan_sha256: sha256_text(map, "plan_sha256")?,
        archive_sha256: sha256_text(map, "archive_sha256")?,
        registry_id: tagged_u64(&map["registry_id"], "pin.registry_id")?,
        registry_sha256: sha256_text(map, "registry_sha256")?,
    })
}

fn attributes(value: &Value, what: &str) -> Result<Vec<Attribute>, LoadError> {
    let items = value
        .as_array()
        .ok_or_else(|| malformed(format!("{what}.attributes is not an array")))?;
    let mut keys = HashSet::new();
    let mut attributes = Vec::with_capacity(items.len());
    for item in items {
        let map = object(
            item,
            "attribute",
            &["key", "name", "namespace", "type_index", "value"],
        )?;
        let attribute = Attribute {
            key: text(map, "key", "attribute")?,
            name: text(map, "name", "attribute")?,
            namespace: texts(map, "namespace", "attribute")?,
            type_index: optional_index(&map["type_index"], "attribute.type_index")?,
            value: text(map, "value", "attribute")?,
        };
        if !keys.insert(attribute.key.clone()) {
            return Err(malformed(format!(
                "{what} lists attribute {} twice",
                attribute.key
            )));
        }
        attributes.push(attribute);
    }
    Ok(attributes)
}

fn struct_field(value: &Value, what: &str, resolved: bool) -> Result<ResolvedField, LoadError> {
    let mut keys = vec!["name", "type_index", "data_offset", "attributes"];
    if resolved {
        keys.push("declaring_type");
    }
    let map = object(value, what, &keys)?;
    let name = text(map, "name", what)?;
    Ok(ResolvedField {
        field: StructField {
            type_index: tagged_index(&map["type_index"], "field.type_index")?,
            data_offset: tagged_u64(&map["data_offset"], "field.data_offset")?,
            attributes: attributes(&map["attributes"], &format!("field {name}"))?,
            name,
        },
        declaring_type: if resolved {
            tagged_index(&map["declaring_type"], "field.declaring_type")?
        } else {
            0
        },
    })
}

const TYPE_KEYS: [&str; 20] = [
    "index",
    "name",
    "qualified_name",
    "impact_name",
    "namespace",
    "hashes",
    "inner_type",
    "size",
    "alignment",
    "element_alignment",
    "field_count",
    "primitive_type",
    "flags_bits",
    "flags",
    "has_ds",
    "default_bytes",
    "struct_fields",
    "fields",
    "enum_fields",
    "attributes",
];

fn type_meta(value: &Value) -> Result<(TypeMeta, Vec<ResolvedField>), LoadError> {
    let map = object(value, "type", &TYPE_KEYS)?;
    let qualified_name = text(map, "qualified_name", "type")?;
    let what = format!("type {qualified_name}");
    let hashes = object(
        &map["hashes"],
        "type.hashes",
        &["qualified", "internal", "name", "impact"],
    )?;
    let hash = |key: &str| bounded_u32(&hashes[key], &format!("{what}.hashes.{key}"));
    let primitive = text(map, "primitive_type", &what)?;
    let primitive_type = PRIMITIVE_TYPES
        .iter()
        .copied()
        .find(|name| *name == primitive)
        .ok_or_else(|| malformed(format!("{what} has an unknown primitive type {primitive}")))?;
    let flags_bits = bounded_u32(&map["flags_bits"], &format!("{what}.flags_bits"))?;
    let flags_bits = u8::try_from(flags_bits)
        .map_err(|_| malformed(format!("{what}.flags_bits is more than one byte")))?;
    if texts(map, "flags", &what)? != flag_names(flags_bits as u32) {
        return Err(malformed(format!(
            "{what}.flags do not name its flags_bits"
        )));
    }
    if map["has_ds"].as_bool() != Some(flags_bits as u32 & HAS_DS != 0) {
        return Err(malformed(format!("{what}.has_ds is not its HAS_DS bit")));
    }
    let default_bytes = match &map["default_bytes"] {
        Value::Null => None,
        other => Some(
            other
                .as_str()
                .and_then(|text| text.strip_prefix("bytes:"))
                .filter(|digits| digits.len() % 2 == 0 && lowercase_hex(digits))
                .map(|digits| {
                    (0..digits.len())
                        .step_by(2)
                        .map(|at| u8::from_str_radix(&digits[at..at + 2], 16).unwrap())
                        .collect::<Vec<u8>>()
                })
                .ok_or_else(|| malformed(format!("{what}.default_bytes is not a bytes: text")))?,
        ),
    };
    let list = |key: &str| {
        map[key]
            .as_array()
            .ok_or_else(|| malformed(format!("{what}.{key} is not an array")))
    };
    let mut struct_fields = Vec::new();
    for item in list("struct_fields")? {
        let field = struct_field(item, "struct field", false)?.field;
        if struct_fields
            .iter()
            .any(|f: &StructField| f.name == field.name)
        {
            return Err(malformed(format!(
                "{what} lists field {} twice",
                field.name
            )));
        }
        struct_fields.push(field);
    }
    let resolved = list("fields")?
        .iter()
        .map(|item| struct_field(item, "resolved field", true))
        .collect::<Result<Vec<_>, _>>()?;
    let mut enum_fields: Vec<EnumField> = Vec::new();
    for item in list("enum_fields")? {
        let entry = object(item, "enum field", &["name", "value", "value_hex"])?;
        let field = EnumField {
            name: text(entry, "name", "enum field")?,
            value: tagged_u64(&entry["value"], "enum field value")?,
        };
        if text(entry, "value_hex", "enum field")? != format!("0x{:016x}", field.value) {
            return Err(malformed(format!(
                "{what}: enum field {} has a value_hex that is not its value",
                field.name
            )));
        }
        if enum_fields.iter().any(|f| f.name == field.name) {
            return Err(malformed(format!(
                "{what} lists enum field {} twice",
                field.name
            )));
        }
        enum_fields.push(field);
    }
    let meta = TypeMeta {
        index: tagged_index(&map["index"], &format!("{what}.index"))?,
        name: text(map, "name", &what)?,
        impact_name: text(map, "impact_name", &what)?,
        namespace: texts(map, "namespace", &what)?,
        qualified_hash: hash("qualified")?,
        internal_hash: hash("internal")?,
        name_hash: hash("name")?,
        impact_hash: hash("impact")?,
        inner_type: optional_index(&map["inner_type"], &format!("{what}.inner_type"))?,
        size: bounded_u32(&map["size"], &format!("{what}.size"))?,
        alignment: bounded_u32(&map["alignment"], &format!("{what}.alignment"))?,
        element_alignment: bounded_u32(
            &map["element_alignment"],
            &format!("{what}.element_alignment"),
        )?,
        field_count: bounded_u32(&map["field_count"], &format!("{what}.field_count"))?,
        primitive_type,
        flags_bits,
        default_bytes,
        struct_fields,
        enum_fields,
        attributes: attributes(&map["attributes"], &what)?,
        qualified_name,
    };
    Ok((meta, resolved))
}

impl Page {
    fn parse(bytes: &[u8]) -> Result<Self, LoadError> {
        let value: Value = serde_json::from_slice(bytes)
            .map_err(|e| malformed(format!("the page is not JSON: {e}")))?;
        let map = object(
            &value,
            "page",
            &[
                "schema_version",
                "pin",
                "game_version",
                "registry_version",
                "count",
                "first",
                "types",
            ],
        )?;
        if map["schema_version"] != 1 {
            return Err(malformed("schema_version is not 1"));
        }
        Ok(Self {
            pin: pin(&map["pin"])?,
            game_version: text(map, "game_version", "page")?,
            registry_version: text(map, "registry_version", "page")?,
            count: tagged_u64(&map["count"], "count")?,
            first: tagged_u64(&map["first"], "first")?,
            types: map["types"]
                .as_array()
                .ok_or_else(|| malformed("types is not an array"))?
                .iter()
                .map(type_meta)
                .collect::<Result<_, _>>()?,
        })
    }
}
