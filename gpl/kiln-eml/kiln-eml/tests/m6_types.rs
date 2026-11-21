// SPDX-License-Identifier: GPL-3.0-or-later
//! The SF-dialect startup pass's type session and `game.types` / `game.version`, against ShroudForge's
//! eml-assets-types API rows. Each test names the rows it covers. The
//! expectations are ShroudForge 1.7.9's (lua/bindings/game/types.rs, resolve_type.rs,
//! reflection/traverse_reflected_types.rs, lua/readonly_collections.rs), source-derived: the oracle cannot inject a
//! registry (its game.types runs on an empty one; `oracle_*` below compares that part with it).
#[path = "support/m6_fixture/mod.rs"]
mod fixture;
#[path = "support/m6_types/mod.rs"]
mod reg;

use fixture::TestRoot;
use kfc::{
    container::{KFCFile, KFCWriter},
    guid::{Guid, ResourceId},
    hash::fnv,
    reflection::{TypeIndex, TypeRegistry},
};
use kiln_eml::sf_startup::{SelectedTarget, TypeSession, format_adapter, game};
use mlua::{Lua, Table, Value};
use std::{fs, rc::Rc};

const KFC_VERSION: &str = "m6-kfc-data-7";

/// A synthetic `<stem>.kfc` written with a data version unlike the registry's, read back as the selected index.
fn selected_index(root: &TestRoot, registry: &Rc<TypeRegistry>) -> KFCFile {
    let game = root.0.join("game");
    fs::create_dir_all(&game).unwrap();
    fs::File::create(game.join("enshrouded.kfc")).unwrap();
    fs::File::create(game.join("enshrouded.kfc_resources")).unwrap();
    let mut writer = KFCWriter::<Rc<KFCFile>, _>::new(&game, "enshrouded", registry.clone(), KFC_VERSION).unwrap();
    let id = ResourceId::new(
        Guid::parse(fixture::archive::RESOURCE_GUID).unwrap(),
        fnv(fixture::types::ROOT),
        0,
    );
    let root_type = registry.get(TypeIndex::new(8)).unwrap();
    writer
        .write_resource(
            &id,
            &fixture::types::root_value().to_bytes(registry, root_type).unwrap(),
        )
        .unwrap();
    writer.finalize().unwrap();
    KFCFile::from_path(game.join("enshrouded.kfc"), false).unwrap()
}

struct Vm {
    lua: Lua,
    session: TypeSession,
    _root: TestRoot,
}

/// A VM with `game` (package A), `game2` (package B, same session) and `other` (`game.types` of a second registry).
fn vm() -> Vm {
    let root = TestRoot::new("g05-types");
    let registry = Rc::new(reg::registry());
    let index = selected_index(&root, &registry);
    let lua = Lua::new();
    let session = TypeSession::new(&lua, registry, SelectedTarget::from_index("enshrouded", &index));
    let mut changed = reg::types();
    // Same index and name as m6.Base, another qualified hash: a Type from it is "another registry"'s.
    changed[1].qualified_hash ^= 1;
    let other = TypeSession::new(
        &lua,
        Rc::new(reg::registry_of("other-registry", changed)),
        session.target().clone(),
    );
    lua.globals()
        .set("game", game::create(&lua, &session).unwrap())
        .unwrap();
    lua.globals()
        .set("game2", game::create(&lua, &session).unwrap())
        .unwrap();
    lua.globals()
        .set(
            "other",
            game::create(&lua, &other).unwrap().get::<Table>("types").unwrap(),
        )
        .unwrap();
    Vm {
        lua,
        session,
        _root: root,
    }
}

impl Vm {
    fn run(&self, chunk: &str) {
        if let Err(error) = self.lua.load(chunk).set_name("=g05").exec() {
            panic!("{error}");
        }
    }
}

/// Raised error text (the first line of mlua's callback error).
const PERR: &str = r#"
local function perr(f, ...)
  local ok, e = pcall(f, ...)
  assert(not ok, "expected an error")
  return (tostring(e):match("^[^\n]*"))
end
"#;

// FINAL: game.version, game.types.version, game.types.count, game.types (per-package table, shared Type cache).
#[test]
fn version_count_and_package_tables() {
    let vm = vm();
    vm.run(&format!(
        r#"
        assert(game.version == "{KFC_VERSION}", game.version)
        assert(game.types.version == "{}", game.types.version)
        assert(game.types.count == {} and math.type(game.types.count) == "integer")
        assert(other.version == "other-registry")
        assert(game.types ~= game2.types, "each package gets its own API table")
        assert(rawequal(game.types.get("m6.Root"), game2.types.get("m6.Root")), "one Type per index per session")
        game.types.count = 1; game.types.version = "x"
        assert(game2.types.count == {})
        assert(game.types.get_by_index(0).index == 0)
        "#,
        fixture::types::VERSION,
        reg::COUNT,
        reg::COUNT,
    ));
    assert_eq!(vm.session.target().game_version, KFC_VERSION);
    assert_eq!(vm.session.registry().version, fixture::types::VERSION);
}

// FINAL: game.types extends an existing game table (the one root table later builders also extend).
#[test]
fn install_keeps_the_game_table() {
    let vm = vm();
    let existing = vm.lua.create_table().unwrap();
    existing.raw_set("assets", "kept").unwrap();
    game::install(&vm.lua, &existing, &vm.session).unwrap();
    vm.lua.globals().set("g3", existing.clone()).unwrap();
    vm.run(r#"assert(g3.assets == "kept" and g3.version == "m6-kfc-data-7" and g3.types.count > 0)"#);
}

// FINAL: game.types.get, get_by_qualified_name, get_by_impact_name, game.types:numeric_argument_coercion.
#[test]
fn exact_names_never_hash_or_number_lookups() {
    let vm = vm();
    vm.run(&format!(
        r#"{PERR}
        local t = game.types
        assert(t.get("m6.Root").qualified_name == "m6.Root")
        assert(t.get_by_qualified_name("m6.Root") == t.get("m6.Root"))
        assert(t.get_by_impact_name("M6_Root") == t.get("m6.Root"))
        -- the registry indexes m6.Aliased under the hash of "{ghost}": a name whose hash matches is not its name
        assert(t.get("{ghost}") == nil and t.get_by_qualified_name("{ghost}") == nil)
        assert(t.get_by_impact_name("{phantom}") == nil)
        -- and SF's get looks names up through that hash index: m6.Aliased itself is not found by name
        assert(t.get("m6.Aliased") == nil)
        assert(t.resolve("m6.Aliased").qualified_name == "m6.Aliased", "resolve compares names in full")
        -- impact name is not the short name, nor the qualified name
        assert(t.get_by_impact_name("Root") == nil and t.get_by_impact_name("m6.Root") == nil)
        -- DS types have no Impact index
        assert(t.get("m6.DsText") ~= nil and t.get_by_impact_name("M6_DsText") == nil)
        -- numbers become decimal text: 4242 names a type, a qualified hash as a number names none
        assert(t.get(4242).qualified_name == "4242")
        assert(t.get({root_hash}) == nil)
        assert(t.get(4242.0) == t.get("4242"), "a float becomes Rust's text of it: 4242, not 4242.0")
        assert(perr(t.get) == "bad argument #1 (string expected, got nil)")
        assert(perr(t.get, {{}}) == "bad argument #1 (string expected, got table)")
        assert(perr(t.get_by_impact_name, true) == "bad argument #1 (string expected, got boolean)")
        "#,
        ghost = reg::GHOST,
        phantom = reg::PHANTOM,
        root_hash = fnv("m6.Root"),
    ));
}

// FINAL: game.types.get_by_qualified_hash, get_by_internal_hash, find_by_hash, numeric coercion.
#[test]
fn hash_domains_refuse_collisions() {
    let vm = vm();
    vm.run(&format!(
        r#"{PERR}
        local t = game.types
        local root = t.get("m6.Root")
        local a, b = t.get_by_qualified_hash({root_hash})
        assert(a == root and b == nil and select('#', t.get_by_qualified_hash({root_hash})) == 2)
        assert(t.get_by_internal_hash(root.internal_hash) == root)
        local n, why = t.get_by_internal_hash({shared})
        assert(n == nil and why == "Ambiguous type hash 0x00000077. Use a qualified name or qualified hash", why)
        n, why = t.get_by_qualified_hash(1)
        assert(n == nil and why == "type hash not found: 0x00000001", why)
        -- string and integral float arguments are SF casts
        assert(t.get_by_qualified_hash("{root_hash}") == root and t.get_by_qualified_hash({root_hash}.0) == root)
        assert(perr(t.get_by_qualified_hash, -1) == "bad argument #1 (32-bit unsigned integer expected, got integer)")
        assert(perr(t.get_by_qualified_hash, 4294967296) == "bad argument #1 (32-bit unsigned integer expected, got integer)")
        assert(perr(t.get_by_qualified_hash, 1.5) == "bad argument #1 (32-bit unsigned integer expected, got number)")
        assert(perr(t.get_by_internal_hash, nil) == "bad argument #1 (32-bit unsigned integer expected, got nil)")
        -- find_by_hash enumerates collisions, qualified by default, never filters DS
        local found = t.find_by_hash({shared}, "internal")
        assert(#found == 2 and found[1] == t.get_by_index({la}) and found[2] == t.get_by_index({lb}))
        local names = t.find_by_hash(root.name_hash, "name")
        assert(#names == 1)
        local bases = t.find_by_hash(t.get("m6.Base").name_hash, "name")
        assert(#bases == 2 and bases[1] == t.get("m6.Base") and bases[2] == t.get_by_index({inner}))
        assert(#t.find_by_hash({root_hash}) == 1 and #t.find_by_hash(t.get("m6.DsText").qualified_hash) == 1)
        assert(#t.find_by_hash(root.impact_hash, "impact") == 1)
        assert(#t.find_by_hash(1, "qualified") == 0)
        assert(perr(t.find_by_hash, 1, "address") == "runtime error: Unknown hash domain 'address'. Expected qualified, internal, name or impact")
        assert(perr(t.find_by_hash, 1, false) == "bad argument #2 (string? expected, got boolean)")
        found[1] = 0
        assert(#t.find_by_hash({shared}, "internal") == 2, "a fresh mutable table each call")
        "#,
        root_hash = fnv("m6.Root"),
        shared = reg::INTERNAL_SHARED,
        la = reg::LAYOUT_A,
        lb = reg::LAYOUT_B,
        inner = reg::INNER_BASE,
    ));
}

// FINAL: game.types.resolve, Type:is_a (selector forms, wrong registry, wrong domain, raise vs nil-reason).
#[test]
fn resolve_selectors_and_wrong_registry() {
    let vm = vm();
    vm.run(&format!(
        r#"{PERR}
        local t = game.types
        local root = t.get("m6.Root")
        assert(t.resolve("m6.Root") == root and t.resolve({root_hash}) == root and t.resolve(root) == root)
        assert(t.resolve({{hash = root.internal_hash, domain = "internal"}}) == root)
        local function reason(...) local v, r = t.resolve(...); assert(v == nil); return r end
        assert(reason("m6.Nope") == "type not found: m6.Nope")
        assert(reason(1) == "type hash not found: 0x00000001")
        assert(reason({{hash = {shared}, domain = "internal"}}) == "Ambiguous type hash 0x00000077. Use a qualified name or qualified hash")
        assert(reason({{hash = 1, domain = "address"}}) == "Unknown hash domain 'address'. Expected qualified, internal, name or impact")
        assert(reason({{hash = 1}}) == "error converting Lua nil to String (expected string or number)")
        assert(reason({root_hash}.0) == "expected a qualified name, unsigned qualified hash, Type, or {{hash, domain}}", "a float is no hash")
        assert(reason(-1) == "expected a qualified name, unsigned qualified hash, Type, or {{hash, domain}}")
        assert(reason(4294967296) == "expected a qualified name, unsigned qualified hash, Type, or {{hash, domain}}")
        assert(reason(true) == "expected a qualified name, unsigned qualified hash, Type, or {{hash, domain}}")
        assert(reason(nil) == "expected a qualified name, unsigned qualified hash, Type, or {{hash, domain}}")
        -- a Type of another registry: same index and identity resolves, a changed identity does not
        assert(t.resolve(other.get("m6.Root")) == root)
        assert(other.get_by_index(1) ~= nil and other.get_by_index(1) ~= t.get("m6.Base"))
        assert(reason(other.get_by_index(1)) == "type belongs to another registry")
        -- is_a raises the same texts
        local child = t.get("m6.Child")
        assert(child:is_a("m6.Base") and child:is_a(child) and child:is_a({base_hash}))
        assert(not t.get("m6.Base"):is_a(child))
        assert(perr(child.is_a, child, "m6.Nope") == "runtime error: type not found: m6.Nope")
        assert(perr(child.is_a, child, other.get_by_index(1)) == "runtime error: type belongs to another registry")
        "#,
        root_hash = fnv("m6.Root"),
        base_hash = fnv("m6.Base"),
        shared = reg::INTERNAL_SHARED,
    ));
}

// FINAL: game.types.get_all (DS included by default), find (prefix, own attribute, primitive spelling, has_ds).
#[test]
fn get_all_and_find_filters() {
    let vm = vm();
    vm.run(&format!(
        r#"{PERR}
        local t = game.types
        local all = t.get_all()
        assert(#all == {count} and all[1] == t.get_by_index(0) and all[{count}] == t.get_by_index({last}))
        assert(#t.get_all(true) == {count} and #t.get_all(nil) == {count})
        local no_ds = t.get_all(false)
        assert(#no_ds == {count} - 4)
        for _, ty in ipairs(no_ds) do assert(not ty.has_ds) end
        assert(perr(t.get_all, 0) == "bad argument #1 (boolean? expected, got integer)")
        assert(#t.find() == {count} and #t.find({{}}) == {count} and #t.find({{unknown = 1}}) == {count})
        assert(#t.find({{prefix = "m6.Ds"}}) == 4)
        assert(#t.find({{prefix = "m6.inner."}}) == 1)
        local tagged = t.find({{attribute = "tag"}})
        assert(#tagged == 1 and tagged[1] == t.get("m6.Child"), "own attributes only, not inherited")
        assert(#t.find({{primitive_type = "Enum"}}) == 1 and #t.find({{primitive_type = "ENUM"}}) == 0)
        assert(#t.find({{primitive_type = "BlobArray"}}) == 1 and #t.find({{primitive_type = "Typedef"}}) == 1)
        assert(#t.find({{has_ds = true}}) == 4 and #t.find({{has_ds = false}}) == {count} - 4)
        -- option values use mlua's conversions: a number prefix is its text, any non-nil has_ds is true
        assert(#t.find({{prefix = 42}}) == 1 and t.find({{prefix = 42}})[1] == t.get("4242"))
        assert(#t.find({{has_ds = 0}}) == 4)
        assert(#t.find({{prefix = "m6.Ds", has_ds = false}}) == 0)
        assert(perr(t.find, "m6") == "bad argument #1 (table? expected, got string)")
        "#,
        count = reg::COUNT,
        last = reg::COUNT - 1,
    ));
}

// FINAL: game.types.get_by_index, numeric coercion (usize casts: negative float maps to 0).
#[test]
fn get_by_index_casts_and_missing_indices() {
    let vm = vm();
    vm.run(&format!(
        r#"{PERR}
        local t = game.types
        assert(t.get_by_index(8) == t.get("m6.Root"))
        assert(t.get_by_index({count}) == nil and select('#', t.get_by_index({count})) == 1)
        assert(t.get_by_index(-1) == nil, "a negative integer is a huge index")
        assert(t.get_by_index(-1.0) == t.get_by_index(0), "a negative integral float saturates to 0")
        assert(t.get_by_index("3") == t.get_by_index(3))
        assert(perr(t.get_by_index, 1.5) == "bad argument #1 (64-bit unsigned integer expected, got number)")
        assert(perr(t.get_by_index) == "bad argument #1 (64-bit unsigned integer expected, got nil)")
        "#,
        count = reg::COUNT,
    ));
}

// FINAL: Type.* fields (identity, hashes, namespace, sizes, primitive, flags, has_ds, default_bytes).
#[test]
fn type_metadata_fields() {
    let vm = vm();
    let child = vm.session.registry().get(TypeIndex::new(2)).unwrap().clone();
    vm.run(&format!(
        r#"
        local c = game.types.get("m6.Child")
        assert(c.index == 2 and c.name == "Child" and c.impact_name == "M6_Child" and c.qualified_name == "m6.Child")
        assert(c.qualified_hash == {qh} and c.internal_hash == {ih} and c.name_hash == {nh} and c.impact_hash == {mh})
        local ns = c.namespace
        assert(type(ns) == "table" and #ns == 1 and ns[1] == "m6" and ns ~= c.namespace, "a fresh table")
        ns[1] = "changed"; assert(c.namespace[1] == "m6")
        assert(c.inner_type == game.types.get("m6.Base"), "the cached base Type")
        assert(c.size == 8 and c.alignment == 4 and c.element_alignment == 4 and c.field_count == 1)
        assert(c.primitive_type == "Struct" and game.types.get("m6.DsChildren").primitive_type == "DsArray")
        assert(c.flags_bits == 0 and #c.flags == 0 and c.has_ds == false)
        local tagged = game.types.get("m6.Tagged")
        assert(tagged.flags_bits == 0x24)
        local flags = tagged.flags
        assert(#flags == 2 and flags[1] == "HAS_BLOB_STRING" and flags[2] == "IS_GPU_UNIFORM" and flags ~= tagged.flags)
        local ds = game.types.get("m6.DsText")
        assert(ds.has_ds and ds.flags[1] == "HAS_DS" and ds.flags_bits == 1)
        assert(c.default_bytes == "\17\0\255\0\128\23\0\0" and #c.default_bytes == 8, "exact bytes: zeros, no UTF-8")
        assert(game.types.get("m6.Base").default_bytes == nil, "absent stays nil, not zeros")
        assert(game.types.get("m6.Fixed").inner_type == game.types.get("m6.UInt32"), "an array's element")
        assert(game.types.get("m6.UInt32").inner_type == nil)
        assert(game.types.get("m6.Broken").inner_type == nil, "a missing base index reads as nil")
        assert(game.types.get("m6.Mode").field_count == 3)
        "#,
        qh = child.qualified_hash,
        ih = child.internal_hash,
        nh = child.name_hash,
        mh = child.impact_hash,
    ));
}

// FINAL: Type.fields (base first, overrides keep their slot, declaring_type), Type.struct_fields (own, cached
// read-only), traversal errors.
#[test]
fn inherited_versus_own_fields() {
    let vm = vm();
    vm.run(&format!(
        r#"{PERR}
        local t = game.types
        local child, base, u32 = t.get("m6.Child"), t.get("m6.Base"), t.get("m6.UInt32")
        local f = child.fields
        assert(#f == 2 and f ~= child.fields, "a fresh dense table per read")
        assert(f[1].name == "base" and f[1].type == u32 and f[1].declaring_type == base and f[1].data_offset == 0)
        assert(f[2].name == "extra" and f[2].declaring_type == child and f[2].data_offset == 4)
        assert(getmetatable(f[1]) == nil and f[2].attributes.unit.value == "cm")
        local tagged = t.get("m6.Tagged").fields
        assert(#tagged == 2 and tagged[1].declaring_type == base and tagged[2].declaring_type == child)
        -- Override redeclares base's field: it keeps base's slot, with the derived owner and offset
        local o = t.get_by_index({over}).fields
        assert(#o == 2 and o[1].name == "base" and o[1].declaring_type == t.get_by_index({over}) and o[1].data_offset == 8)
        assert(o[2].name == "late" and o[2].data_offset == 4)
        -- struct_fields: own fields only, a cached ReadOnlyMap of ReadOnlyMaps
        local sf = child.struct_fields
        assert(type(sf) == "userdata" and sf == child.struct_fields and #sf == 1 and sf.base == nil)
        assert(sf.extra.name == "extra" and sf.extra.type == u32 and sf.extra.data_offset == 4)
        assert(sf.extra.attributes.unit.name == "unit" and sf.extra.attributes.unit.type == nil)
        local keys = {{}}
        for k, v in pairs(t.get_by_index({over}).struct_fields) do keys[#keys + 1] = k .. "@" .. v.data_offset end
        assert(table.concat(keys, ",") == "late@4,base@8", table.concat(keys, ","))
        assert(not pcall(function() sf.extra = 1 end), "read-only")
        assert(t.get("m6.Broken").struct_fields.lost.type == nil, "a field of a missing type reads as nil")
        assert(perr(function() return t.get("m6.Broken").fields end):find("unresolved base type", 1, true))
        assert(perr(function() return t.get("m6.CycleA").fields end):find("cyclic type inheritance", 1, true))
        assert(perr(t.get("m6.CycleA").is_a, t.get("m6.CycleA"), "m6.Base"):find("cyclic type inheritance", 1, true))
        assert(perr(t.get("m6.Broken").is_a, t.get("m6.Broken"), "m6.Base"):find("unresolved base type", 1, true))
        "#,
        over = reg::OVERRIDE,
    ));
}

// FINAL: Type:is_a (self, Struct/Typedef walk only; an array is not its element).
#[test]
fn is_a_walks_bases_not_elements() {
    let vm = vm();
    vm.run(
        r#"
        local t = game.types
        assert(t.get("m6.Tagged"):is_a("m6.Base") and t.get("m6.Tagged"):is_a("m6.Child"))
        assert(t.get("m6.Alias"):is_a("m6.UInt32"), "a typedef's inner type is its base")
        assert(not t.get("m6.Fixed"):is_a("m6.UInt32"), "an array is not its element")
        assert(not t.get("m6.Children"):is_a("m6.Child") and not t.get("m6.MaybeChild"):is_a("m6.Child"))
        assert(not t.get("m6.inner.Base"):is_a("m6.Base"), "same short name is not the same type")
        "#,
    );
}

// FINAL: Type.enum_fields (insertion order, value bits as Lua integer, value_hex), Type.attributes.
#[test]
fn enum_values_and_attributes() {
    let vm = vm();
    vm.run(&format!(
        r#"
        local mode = game.types.get("m6.Mode")
        local e = mode.enum_fields
        assert(e == mode.enum_fields and #e == 3)
        local order = {{}}
        for k, v in pairs(e) do order[#order + 1] = k .. "=" .. v.value_hex end
        assert(table.concat(order, ",") == "Off=0x0000000000000000,On=0x0000000000000001,Big=0x{big:016x}", table.concat(order, ","))
        assert(e.Big.name == "Big" and math.type(e.Big.value) == "integer" and e.Big.value == {big_signed})
        assert(e.On.value == 1 and e.Nope == nil and #e.On == 3)
        local a = game.types.get("m6.Child").attributes
        assert(a == game.types.get("m6.Child").attributes and #a == 1)
        local tag = a.tag
        assert(tag.name == "tag" and tag.value == "child-tag" and tag.type == game.types.get("m6.UInt32"))
        local ns = tag.namespace
        assert(type(ns) == "userdata" and #ns == 2 and ns[1] == "m6" and ns[2] == "attr" and ns[0] == nil and ns[3] == nil)
        local items = {{}}
        for i, v in pairs(ns) do items[#items + 1] = i .. v end
        assert(table.concat(items, ",") == "1m6,2attr")
        local base_only = game.types.get("m6.Base").attributes.base_only
        assert(base_only.type == nil and base_only.value == "" and #base_only.namespace == 0, "missing type index is nil")
        assert(#game.types.get("m6.Tagged").attributes == 0, "attributes are own, not inherited")
        "#,
        big = reg::BIG_ENUM,
        big_signed = reg::BIG_ENUM as i64,
    ));
}

// FINAL: game.types.of (partial: no recognizer yet, so nil for every value; the seam answers through recognizers).
#[test]
fn of_answers_through_recognizers_only() {
    let vm = vm();
    vm.run(
        r#"
        local t = game.types
        for _, v in ipairs({1, "m6.Root", {}, true, t.get("m6.Root"), t.get("m6.Child").struct_fields}) do
          assert(t.of(v) == nil, tostring(v))
        end
        assert(t.of() == nil and select('#', t.of()) == 1)
        "#,
    );
    let marker = vm.lua.create_table().unwrap();
    let pointer = marker.to_pointer();
    vm.session.add_type_recognizer(Rc::new(move |value: &Value| {
        Ok(match value {
            Value::Table(t) if t.to_pointer() == pointer => Some(TypeIndex::new(8)),
            Value::Boolean(true) => Some(TypeIndex::new(999)),
            Value::Boolean(false) => return Err(mlua::Error::runtime("recognizer failed")),
            _ => None,
        })
    }));
    // A later recognizer is asked only when the earlier ones do not know the value: the first answer wins.
    vm.session.add_type_recognizer(Rc::new(move |value: &Value| {
        Ok(match value {
            Value::Table(t) if t.to_pointer() == pointer => Some(TypeIndex::new(2)),
            Value::Integer(7) => Some(TypeIndex::new(2)),
            _ => None,
        })
    }));
    vm.lua.globals().set("marker", marker).unwrap();
    vm.run(
        r#"
        local t = game.types
        assert(t.of(marker) == t.get("m6.Root") and game2.types.of(marker) == t.get("m6.Root"))
        assert(t.of(7) == t.get("m6.Child"), "a value only the later recognizer knows")
        assert(t.of({}) == nil)
        local ok, e = pcall(t.of, true)
        assert(not ok and tostring(e):find("invalid type index 999", 1, true))
        ok, e = pcall(t.of, false)
        assert(not ok and tostring(e):find("recognizer failed", 1, true))
        assert(other.of(marker) == nil, "recognizers belong to their session")
        "#,
    );
}

// Session identity: a Type is cached per index; a session refuses another VM; a Type outliving it errors cleanly.
#[test]
fn session_cache_identity_and_lifetime() {
    let vm = vm();
    let a = vm.session.get_type(&vm.lua, TypeIndex::new(2)).unwrap().unwrap();
    let b = vm.session.get_type(&vm.lua, TypeIndex::new(2)).unwrap().unwrap();
    assert_eq!(a, b);
    assert!(
        vm.session
            .get_type(&vm.lua, TypeIndex::new(reg::COUNT))
            .unwrap()
            .is_none()
    );
    let elsewhere = Lua::new();
    let error = vm
        .session
        .get_type(&elsewhere, TypeIndex::new(2))
        .unwrap_err()
        .to_string();
    assert!(error.contains("another Lua VM"), "{error}");

    let lua = Lua::new();
    let registry = Rc::new(reg::registry());
    let session = TypeSession::new(
        &lua,
        registry,
        SelectedTarget {
            stem: "enshrouded".into(),
            game_version: "v".into(),
        },
    );
    let child = session.get_type(&lua, TypeIndex::new(2)).unwrap().unwrap();
    lua.globals().set("child", child).unwrap();
    drop(session);
    lua.load(r#"assert(child.name == "Child" and child.size == 8)"#)
        .exec()
        .unwrap();
    let error = lua.load("return child.inner_type").exec().unwrap_err().to_string();
    assert!(error.contains("the type session of this Type has ended"), "{error}");
}

// Session identity when Lua runs inside the allocation that builds a Type: the collector calls finalizers from
// create_userdata, and a finalizer may ask for the very index being built. One Type per index must survive (SF holds
// its cache entry across that allocation). Lua's collector is deterministic for one sequence of allocations, and these
// rounds reach that window every run; `inside` shows they did.
#[test]
fn a_finalizer_asking_for_the_type_being_built_keeps_one_type_per_index() {
    let registry = Rc::new(reg::registry());
    let lua = Lua::new();
    let target = SelectedTarget {
        stem: "enshrouded".into(),
        game_version: "v".into(),
    };
    let mut inside = 0;
    for round in 0..40 {
        let session = TypeSession::new(&lua, registry.clone(), target.clone());
        lua.globals()
            .set("game", game::create(&lua, &session).unwrap())
            .unwrap();
        let (during, mismatch): (i64, String) = lua
            .load(
                r#"
                collectgarbage("incremental", 1, 400, 1)
                local t, seen, during = game.types, {}, 0
                local mt = {__gc = function()
                  local i = current
                  if i ~= nil then
                    if building then during = during + 1 end
                    seen[#seen + 1] = {i, t.get_by_index(i)}
                  end
                end}
                for i = 0, t.count - 1 do
                  for _ = 1, 16 do setmetatable({}, mt) end
                  current, building = i, true
                  local ty = t.get_by_index(i)
                  building = false
                  if not rawequal(ty, t.get_by_index(i)) then return during, "index " .. i .. " was not cached" end
                  current = nil
                end
                collectgarbage()
                for _, s in ipairs(seen) do
                  if not rawequal(s[2], t.get_by_index(s[1])) then return during, "two Types for index " .. s[1] end
                end
                return during, ""
                "#,
            )
            .eval()
            .unwrap();
        assert_eq!(mismatch, "", "round {round}: {during} finalizers ran inside a build");
        inside += during;
    }
    assert!(
        inside > 0,
        "no finalizer ran while a Type was built: the test never reached its window"
    );
}

// The pinned parser helpers (kfc-parser container/resource_lookup.rs) on EML's kfc APIs.
#[test]
fn format_adapter_lookups() {
    let root = TestRoot::new("g05-adapter");
    let registry = Rc::new(reg::registry());
    let index = selected_index(&root, &registry);
    let guid = Guid::parse(fixture::archive::RESOURCE_GUID).unwrap();
    let id = format_adapter::resource_id(&registry, "m6.Root", guid, 0).unwrap();
    assert_eq!(id, ResourceId::new(guid, fnv("m6.Root"), 0));
    assert_eq!(
        format_adapter::resource_id_for_type(&registry, TypeIndex::new(8), guid, 0),
        Some(id)
    );
    // m6.Aliased is indexed under GHOST's hash, so its own name builds no identifier (SF: get_by_name)
    assert_eq!(
        format_adapter::resource_id_for_type(&registry, TypeIndex::new(reg::ALIASED), guid, 0),
        None
    );
    assert_eq!(format_adapter::resource_id(&registry, "m6.Nope", guid, 0), None);
    assert_eq!(
        format_adapter::type_for_resource(&registry, &id).unwrap().index,
        TypeIndex::new(8)
    );
    assert!(format_adapter::same_type(
        &id,
        &ResourceId::new(guid, fnv("m6.Root"), 3)
    ));
    assert!(!format_adapter::same_type(
        &id,
        &ResourceId::new(guid, fnv("m6.Child"), 0)
    ));
    assert_eq!(
        format_adapter::resources_by_type(&index, &registry, "m6.Root"),
        vec![id]
    );
    assert!(format_adapter::resources_by_type(&index, &registry, "m6.Child").is_empty());
    assert_eq!(
        format_adapter::resource_type_indices(&index, &registry),
        vec![TypeIndex::new(8)]
    );
    assert_eq!(
        SelectedTarget::from_index("enshrouded", &index).game_version,
        KFC_VERSION
    );
}


/// The transcript of `game.types` on an empty registry, captured with ShroudForge's own mod runtime (1.7.9; a local
/// differential test, not published, which can build no other registry), replayed here through the same shared prelude/runner on an empty session.
/// Byte-identical shared-v2 records: argument casts, raised texts, nil/reason tuples and the API table's keys.
#[test]
fn oracle_empty_registry_transcript_matches_shroudforge() {
    let fixture = include_str!("fixtures/m6/types/oracle/game-types-empty.lua");
    let golden = include_str!("fixtures/m6/types/oracle/sf-1.7.9/game-types-empty.json");
    let ours = sfi_core::conformance::run_fixture(fixture, |lua| {
        let env = sfi_core::conformance::plain_std_environment(lua)?;
        let session = TypeSession::new(
            lua,
            Rc::new(TypeRegistry::default()),
            SelectedTarget {
                stem: "enshrouded".into(),
                game_version: String::new(),
            },
        );
        env.raw_set("game", game::create(lua, &session)?)?;
        Ok(env)
    })
    .unwrap();
    sfi_core::conformance::validate_transcript(&ours).unwrap();
    if let Err(difference) = sfi_core::conformance::compare(golden, &ours) {
        panic!("{difference}\nours: {ours}");
    }
}
