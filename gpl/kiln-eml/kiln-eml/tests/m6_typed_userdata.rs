// SPDX-License-Identifier: GPL-3.0-or-later
//! ShroudForge's owned and mapped value wrappers and the typed table operations, in a production sfi-core VM
//! whose `table.insert/remove/clear` dispatcher has the GPL `ValueTables` port attached. Expectations are ShroudForge
//! 1.7.9's at Kiln's fork pin (resource_values/{owned_values,mapped_values,mod}.rs, bindings/extend_table_operations.rs),
//! source-derived: the oracle has no registry or archive to build these values from. Error texts are checked as the
//! shared v2 prelude records them (`v2_transcript_records_source_error_texts`).
#[path = "support/m6_fixture/mod.rs"]
mod fixture;
#[path = "support/m6_types/mod.rs"]
mod reg;
#[path = "support/m6_userdata/mod.rs"]
mod userdata;
#[path = "support/m6_values/mod.rs"]
mod values;

use kfc::{guid::Guid, reflection::TypeIndex};
use kiln_eml::sf_startup::{
    SelectedTarget, TypeSession, game,
    resource_values::{
        self as rv, AssetUserdata, MappedArrayValue, MappedStructValue, StructValue, TypeHandle, TypedObjects,
        ValueObjects, decode, encode, is_dirty_lua_value, mapped_source_bytes, validate_and_clone,
    },
};
use mlua::Table;
use sfi_core::lua::{LuaValue, LuaVm};
use std::rc::Rc;

const ROOT: usize = 8;
const ROOT_TABLE: &str = r#"return {child = {base = 7, extra = 11}, fixed = {2, 3},
    children = {{base = 13, extra = 17}, {base = 19, extra = 23}}, maybe = {base = 29, extra = 31},
    choice = {type = "m6.Child", value = {base = 37, extra = 41}}, text = "synthetic"}"#;

const PERR: &str = r#"
local function perr(f, ...)
  local ok, e = pcall(f, ...)
  assert(not ok, "expected an error")
  return (tostring(e):match("^[^\n]*"))
end
"#;

struct Env {
    vm: LuaVm,
    env: Table,
    registry: Rc<kfc::reflection::TypeRegistry>,
    objects: ValueObjects,
}

fn env() -> Env {
    let (vm, env) = userdata::core_vm();
    Env {
        vm,
        env,
        registry: userdata::registry(),
        objects: userdata::objects(),
    }
}

impl Env {
    fn ty(&self, index: usize) -> TypeHandle {
        userdata::handle(&self.registry, index)
    }
    fn eval(&self, chunk: &str) -> LuaValue {
        self.vm
            .lua()
            .load(chunk)
            .set_name("=g07")
            .set_environment(self.env.clone())
            .eval()
            .unwrap_or_else(|e| panic!("{e}"))
    }
    fn run(&self, chunk: &str) {
        let chunk = format!("{PERR}\n{chunk}");
        if let Err(e) = self
            .vm
            .lua()
            .load(&chunk)
            .set_name("=g07")
            .set_environment(self.env.clone())
            .exec()
        {
            panic!("{e}");
        }
    }
    /// An owned value of `index` from a Lua table (validation), set as global `name`.
    fn owned(&self, name: &str, chunk: &str, index: usize) -> LuaValue {
        let value = validate_and_clone(&self.eval(chunk), &self.ty(index), self.vm.lua(), &self.objects).unwrap();
        self.env.set(name, value.clone()).unwrap();
        value
    }
    /// A mapped value of `index` (encode then decode), set as global `name`; returns it and its bytes.
    fn mapped(&self, name: &str, chunk: &str, index: usize) -> (LuaValue, Vec<u8>) {
        let bytes = encode(&self.eval(chunk), &self.ty(index), self.vm.lua(), &self.objects).unwrap();
        let value = decode(bytes.clone().into(), &self.ty(index), self.vm.lua(), &self.objects).unwrap();
        self.env.set(name, value.clone()).unwrap();
        (value, bytes)
    }
    fn dirty(&self, name: &str) -> bool {
        is_dirty_lua_value(&self.env.get::<LuaValue>(name).unwrap()).unwrap()
    }
    fn reencode(&self, name: &str, index: usize) -> Vec<u8> {
        encode(
            &self.env.get::<LuaValue>(name).unwrap(),
            &self.ty(index),
            self.vm.lua(),
            &self.objects,
        )
        .unwrap()
    }
}

// Red-first: a get returns the stored object (aliases share edits); an assignment validates and clones.
#[test]
fn nested_aliases_versus_clones() {
    let e = env();
    e.owned("R", ROOT_TABLE, ROOT);
    e.run(
        r#"
        local c = R.child
        assert(rawequal(c, R.child), "a get returns the same wrapper")
        c.base = 70
        assert(R.child.base == 70, "an alias sees the edit")
        R.child = c
        assert(not rawequal(R.child, c), "an assignment clones")
        c.base = 71
        assert(R.child.base == 70, "the clone does not follow its source")
        local kids = R.children
        local first = kids[1]
        assert(rawequal(first, kids[1]))
        kids[1] = first
        assert(not rawequal(kids[1], first))
        -- variant: value is the stored wrapper, assignment validates against the selected variant type
        local v = R.choice
        assert(v.type == "m6.Child" and rawequal(v.value, R.choice.value))
        v.value = {base = 1, extra = 2}
        assert(v.value.extra == 2)
        "#,
    );
    let (_, bytes) = e.mapped("M", ROOT_TABLE, ROOT);
    e.run(
        r#"
        local c = M.child
        assert(rawequal(c, M.child), "mapped reads cache wrappers")
        assert(M.child.base == 7 and M.fixed[2] == 3 and M.text == "synthetic")
        assert(rawequal(M.text, M.text))
        c.extra = 99
        assert(M.child.extra == 99)
        "#,
    );
    assert!(e.dirty("M"), "a nested edit makes the root dirty");
    assert_eq!(
        mapped_source_bytes(&e.env.get::<LuaValue>("M").unwrap())
            .unwrap()
            .unwrap(),
        bytes,
        "originals are kept"
    );
    let edited = e.reencode("M", ROOT);
    assert_ne!(edited, bytes);
    let decoded = decode(edited.into(), &e.ty(ROOT), e.vm.lua(), &e.objects).unwrap();
    e.env.set("D", decoded).unwrap();
    e.run("assert(D.child.extra == 99 and D.child.base == 7 and D.children[2].extra == 23)");
}

// Red-first: the fork's setter fix: a field takes its FIELD type, an element its ELEMENT type (1.8 refuses these).
#[test]
fn fork_setter_validates_field_and_element_types() {
    let e = env();
    e.owned("C", "return {base = 1, extra = 2}", 2);
    e.owned("R", ROOT_TABLE, ROOT);
    e.mapped("M", ROOT_TABLE, ROOT);
    e.run(
        r#"
        C.base = 5
        assert(C.base == 5)
        assert(perr(function() C.base = -1 end) == "at .: expected `unsigned integer in range [0, 4294967295]`, got `-1`")
        assert(perr(function() C.nope = 1 end) == "attempt to set invalid property 'nope' on m6.Child")
        R.children[1] = {base = 3, extra = 4}
        assert(R.children[1].base == 3)
        assert(perr(function() R.children[1] = 5 end) == "at .: expected `m6.Child`, got `5`")
        M.children[2] = {base = 8, extra = 9}
        assert(M.children[2].extra == 9)
        M.child = {base = 1, extra = 1}
        assert(perr(function() M.nope = 1 end) == "attempt to set invalid property 'nope' on m6.Root")
        -- out of range assignment
        assert(perr(function() R.children[3] = {base = 1, extra = 1} end) == "position out of bounds: expected index in range [1, 2], got 3")
        assert(perr(function() M.fixed[0] = 1 end) == "position out of bounds: expected index in range [1, 2], got 0")
        "#,
    );
}

// Red-first: mapped arrays read lazily after insert/remove; pairs walks the ORIGINAL length, # the current one.
#[test]
fn mapped_lazy_reads_after_insert_and_remove() {
    let e = env();
    e.mapped("M", ROOT_TABLE, ROOT);
    e.run(
        r#"
        local kids = M.children
        local first = kids[1]
        table.insert(kids, 1, {base = 1, extra = 2})
        assert(#kids == 3 and kids[1].base == 1)
        assert(rawequal(kids[2], first), "the read element moved with its slot")
        assert(kids[3].base == 19, "an unread element is read lazily after the insert")
        local removed = table.remove(kids, 2)
        assert(rawequal(removed, first), "remove returns the existing object")
        assert(#kids == 2)
        local seen = 0
        for i, v in pairs(kids) do seen = i end
        assert(seen == 2)
        table.insert(kids, {base = 5, extra = 6})
        table.insert(kids, {base = 7, extra = 8})
        seen = 0
        for i in pairs(kids) do seen = i end
        assert(#kids == 4 and seen == 2, "pairs walks the original mapped length: " .. seen)
        "#,
    );
    let bytes = e.reencode("M", ROOT);
    let decoded = decode(bytes.into(), &e.ty(ROOT), e.vm.lua(), &e.objects).unwrap();
    e.env.set("D", decoded).unwrap();
    e.run(
        r#"
        local kids = D.children
        assert(#kids == 4 and kids[1].base == 1 and kids[2].base == 19 and kids[3].base == 5 and kids[4].extra == 8)
        "#,
    );
}

// Red-first: bounds and static checks in SF's order; returned nil versus an actual removal; dirtiness.
#[test]
fn static_empty_and_bounds_ordering() {
    let e = env();
    e.owned("R", ROOT_TABLE, ROOT);
    e.mapped("M", ROOT_TABLE, ROOT);
    e.run(
        r#"
        for _, root in ipairs({R, M}) do
          local fixed = root.fixed
          assert(table.remove(fixed, 0) == nil, "position 0 returns nil before the static check")
          assert(perr(table.remove, fixed, 4) == "position out of bounds: expected index in range [1, 3], got 4", "bounds before static")
          assert(perr(table.remove, fixed, 1) == "attempt to modify static array")
          assert(perr(table.remove, fixed) == "attempt to modify static array")
          assert(perr(table.remove, fixed, 3) == "attempt to modify static array", "len+1 is in range, then static")
          assert(perr(table.insert, fixed, 1) == "attempt to modify static array")
          assert(perr(table.insert, fixed, 9, 1) == "position out of bounds: expected index in range [1, 3], got 9")
          assert(perr(table.clear, fixed) == "attempt to modify static array")
          assert(#fixed == 2)
          local kids = root.children
          assert(table.remove(kids, 3) == nil and #kids == 2, "len+1 on a dynamic array: nil, no effect")
          assert(perr(table.insert, kids, 0, {base = 1, extra = 1}) == "position out of bounds: expected index in range [1, 3], got 0")
          table.clear(kids)
          assert(#kids == 0)
          assert(table.remove(kids) == nil, "pop of an empty array is nil")
          assert(table.remove(kids, 1) == nil)
          assert(perr(table.clear, root.child) == "runtime error: 'clear' is not supported for userdata `\"userdata\"`")
        end
        "#,
    );
    // mapped: a no-op remove does not dirty, clearing an empty array does
    e.mapped("N", "return {child = {base = 1, extra = 1}, fixed = {1, 1}, children = {}, choice = {type = 'm6.Base', value = {base = 1}}, text = ''}", ROOT);
    e.run("assert(table.remove(N.children) == nil and table.remove(N.children, 1) == nil and table.remove(N.fixed, 0) == nil)");
    assert!(!e.dirty("N"), "no-op removes leave a mapped value clean");
    e.run("local _ = N.child.base; local _ = N.children; local _ = N.choice.value");
    assert!(!e.dirty("N"), "reads leave it clean");
    e.run("table.clear(N.children)");
    assert!(e.dirty("N"), "clear marks dirty even when empty");
    // review: an actual removal and a variant assignment mark a mapped value dirty by themselves (what is left, or
    // what is assigned, is a clean mapped value)
    e.mapped("P", ROOT_TABLE, ROOT);
    e.mapped("Q", ROOT_TABLE, ROOT);
    e.mapped("S", ROOT_TABLE, ROOT);
    e.run("assert(table.remove(P.children, 1).base == 13 and #P.children == 1)");
    assert!(e.dirty("P"), "a removal marks a mapped array dirty");
    e.run("Q.choice.value = S.choice.value; assert(Q.choice.value.extra == 41)");
    assert!(!e.dirty("S") && e.dirty("Q"), "a variant assignment marks the mapped variant dirty");
    assert!(e.dirty("R"), "owned values are always dirty");
    // an optional element: removing a nil element is an actual removal that returns nil
    e.owned("L", "return {{base = 1, extra = 1}}", userdata::OPT_LIST);
    e.run(
        r#"
        table.insert(L, nil)
        table.insert(L, {base = 2, extra = 2})
        assert(#L == 3 and L[2] == nil and L[3].base == 2)
        assert(table.remove(L, 2) == nil)
        assert(#L == 2 and L[2].base == 2, "the nil element was removed")
        "#,
    );
}

// Borrow/re-entrant errors: the receiver stays borrowed through validation.
#[test]
fn reentrant_access_during_insert_is_a_borrow_error() {
    let e = env();
    e.owned("R", ROOT_TABLE, ROOT);
    e.mapped("M", ROOT_TABLE, ROOT);
    e.run(
        r#"
        for _, root in ipairs({R, M}) do
          local kids = root.children
          -- `#kids` inside the element's validation hits the held receiver borrow; SF maps a failing field get to
          -- MissingField (with the receiver free, the get would answer 2 and the insert succeed)
          local sneaky = setmetatable({extra = 1}, {__index = function() return #kids end})
          assert(perr(table.insert, kids, sneaky) == "at .: missing field `base` in struct `m6.Child`")
          assert(#kids == 2, "nothing was inserted")
          kids[1] = sneaky
          assert(kids[1].base == 2, "an assignment holds no borrow across validation")
        end
        "#,
    );
}

// The core dispatcher: unknown userdata falls back to Lua's own functions; arguments convert before recognition.
#[test]
fn core_dispatcher_fallbacks_and_argument_order() {
    let e = env();
    e.owned("R", ROOT_TABLE, ROOT);
    e.env
        .set("FOREIGN", e.vm.lua().create_userdata(values::Foreign).unwrap())
        .unwrap();
    e.run(
        r#"
        local kids = R.children
        assert(perr(table.insert, kids, "x", {base = 1, extra = 1}) == "bad argument #2 (64-bit unsigned integer expected, got string)")
        assert(perr(table.insert, FOREIGN, "x", 1) == "bad argument #2 (64-bit unsigned integer expected, got string)", "converted before recognition")
        -- Lua's own functions answer an unknown userdata (mlua names it by its __name)
        local m = perr(table.insert, FOREIGN, 1)
        assert(m == "runtime error: bad argument #1 to 'table.insert' (table expected, got Foreign)", m)
        m = perr(table.remove, FOREIGN)
        assert(m == "runtime error: bad argument #1 to 'table.remove' (table expected, got Foreign)", m)
        assert(perr(table.clear, FOREIGN) == "runtime error: 'clear' is not supported for userdata `\"userdata\"`")
        table.insert(kids, 1, {base = 1, extra = 1}, "ignored", "extras")
        assert(#kids == 3 and kids[1].base == 1)
        assert(perr(table.remove, kids, 1, 2) == "runtime error: wrong number of arguments to 'remove'")
        assert(perr(table.insert, kids) == "runtime error: wrong number of arguments to 'insert'")
        local t = {1, 2}
        table.insert(t, 3); assert(#t == 3 and table.remove(t) == 3)
        table.clear(t); assert(next(t) == nil)
        "#,
    );
}

// Metamethods: lengths, pairs order, tostring, unknown keys, string keys of numbers.
#[test]
fn len_pairs_tostring_and_indexing() {
    let e = env();
    e.owned("C", "return {base = 1, extra = 2}", 2);
    e.owned("A", "return {2, 3}", 3);
    e.mapped("M", ROOT_TABLE, ROOT);
    e.run(
        r#"
        assert(#C == 2 and C.nope == nil)
        local keys = {}
        for k, v in pairs(C) do keys[#keys + 1] = k .. "=" .. v end
        assert(table.concat(keys, ",") == "base=1,extra=2", "the type's field order, base first")
        assert(tostring(A) == "[2,3]")
        assert(A[0] == nil and A[3] == nil and A["1"] == 2)
        assert(#M == 6 and math.type(#M.children) == "integer")
        keys = {}
        for k in pairs(M) do keys[#keys + 1] = k end
        assert(table.concat(keys, ",") == "child,fixed,children,maybe,choice,text", table.concat(keys, ","))
        assert(tostring(M.child) == '{"base":7,"extra":11}')
        assert(tostring(M.fixed) == "[2,3]")
        assert(tostring(M.choice):find("userdata") or tostring(M.choice):find("VariantValue"))
        assert(M.nope == nil)
        local c = {}
        for k, v in pairs(M.children[1]) do c[#c + 1] = k end
        assert(table.concat(c, ",") == "base,extra")
        "#,
    );
    // owned struct tostring quotes strings (one field, so the map order does not matter)
    e.owned("T", "return {base = 4}", 1);
    e.run(r#"assert(tostring(T) == '{"base":4}')"#);
}

// game.types.of through the session recognizer; SF type_of reports a variant's declared type.
#[test]
fn type_of_recognizes_the_six_classes() {
    let e = env();
    let lua = e.vm.lua();
    let session = TypeSession::new(
        lua,
        e.registry.clone(),
        SelectedTarget {
            stem: "enshrouded".into(),
            game_version: String::new(),
        },
    );
    session.add_type_recognizer(rv::recognizer());
    e.env.set("game", game::create(lua, &session).unwrap()).unwrap();
    e.owned("R", ROOT_TABLE, ROOT);
    e.mapped("M", ROOT_TABLE, ROOT);
    e.run(
        r#"
        local t = game.types
        for _, root in ipairs({R, M}) do
          assert(t.of(root) == t.get("m6.Root"))
          assert(t.of(root.child) == t.get("m6.Child"))
          assert(t.of(root.children) == t.get("m6.Children"))
          assert(t.of(root.fixed) == t.get("m6.Fixed"))
          assert(t.of(root.choice) == t.get("m6.Choice"), "a variant's declared type")
          assert(t.of(root.text) == nil and t.of({}) == nil)
        end
        "#,
    );
    let r: LuaValue = e.env.get("R").unwrap();
    assert_eq!(rv::type_of(&r).unwrap(), Some(TypeIndex::new(ROOT)));
}

// Clones of mapped values keep their cache and dirtiness; owned clone is deep.
#[test]
fn cloning_keeps_state_and_is_deep() {
    let e = env();
    e.mapped("M", ROOT_TABLE, ROOT);
    e.run("M.child.base = 100");
    let m: LuaValue = e.env.get("M").unwrap();
    let copy = validate_and_clone(&m, &e.ty(ROOT), e.vm.lua(), &e.objects).unwrap();
    e.env.set("K", copy.clone()).unwrap();
    e.run(
        r#"
        assert(not rawequal(K, M) and K.child.base == 100 and not rawequal(K.child, M.child))
        K.child.base = 5
        assert(M.child.base == 100)
        "#,
    );
    assert!(
        copy.as_userdata().unwrap().borrow::<MappedStructValue>().is_ok(),
        "a mapped clone stays mapped"
    );
    assert!(e.dirty("K"));
    let kids: LuaValue = e.eval("return M.children");
    assert!(kids.as_userdata().unwrap().borrow::<MappedArrayValue>().is_ok());
    let owned = e.owned("O", ROOT_TABLE, ROOT);
    assert!(owned.as_userdata().unwrap().borrow::<StructValue>().is_ok());
}

// The same error texts as the shared v2 prelude records them in a production VM (not a v1 userdata-name check).
#[test]
fn v2_transcript_records_source_error_texts() {
    let e = env();
    e.owned("R", ROOT_TABLE, ROOT);
    let fixture = r#"
        local kids = R.children
        T.call("static", table.remove, R.fixed, 1)
        T.call("bounds", table.remove, R.fixed, 4)
        T.call("element", table.insert, kids, 5)
        T.call("property", function() R.nope = 1 end)
        T.call("clear", table.clear, R.child)
        T.call("pop", table.remove, kids)
        T.value("left", #kids)
    "#;
    let env = e.env.clone();
    let transcript = sfi_core::conformance::run_fixture_in_vm(e.vm.lua(), fixture, move |_| Ok(env)).unwrap();
    sfi_core::conformance::validate_transcript(&transcript).unwrap();
    let records: serde_json::Value = serde_json::from_str(&transcript).unwrap();
    let message = |label: &str| -> String {
        let record = records["records"]
            .as_array()
            .unwrap()
            .iter()
            .find(|r| r["label"] == label)
            .unwrap();
        record["message"]
            .as_str()
            .unwrap_or_default()
            .lines()
            .next()
            .unwrap_or_default()
            .to_owned()
    };
    assert_eq!(message("static"), "attempt to modify static array");
    assert_eq!(
        message("bounds"),
        "position out of bounds: expected index in range [1, 3], got 4"
    );
    assert_eq!(message("element"), "at .: expected `m6.Child`, got `5`");
    assert!(
        message("property").ends_with("attempt to set invalid property 'nope' on m6.Root"),
        "{}",
        message("property")
    );
    assert_eq!(
        message("clear"),
        "runtime error: 'clear' is not supported for userdata `\"userdata\"`"
    );
    let pop = records["records"]
        .as_array()
        .unwrap()
        .iter()
        .find(|r| r["label"] == "pop")
        .unwrap();
    assert_eq!(pop["ok"], true);
    assert_eq!(
        records["records"]
            .as_array()
            .unwrap()
            .iter()
            .find(|r| r["label"] == "left")
            .unwrap()["values"][0]["v"],
        "1"
    );
}

// Review: kfc maps lazily, so a faulty registry's parser panics surface in the wrappers' reads. Each is
// a Lua error with the panic's text, never a panic through the Lua call.
#[test]
fn faulty_registry_lazy_reads_are_errors_not_panics() {
    let e = env();
    let lua = e.vm.lua();
    let read = |name: &str, index: usize, bytes: Vec<u8>| {
        let value = decode(bytes.into(), &e.ty(index), lua, &e.objects).unwrap();
        e.env.set(name, value).unwrap();
    };
    read("B", reg::BROKEN, vec![0; 8]); // its base type is missing: the field count walks into it
    read("H", userdata::MODE_HOLDER, vec![0; 8]);
    let mut list = vec![8, 0, 0, 0, 2, 0, 0, 0];
    list.extend([0; 16]);
    read("L", userdata::MODE_LIST, list);
    e.run(
        r#"
        local unwrap = "runtime error: called `Option::unwrap()` on a `None` value"
        local inner = "runtime error: invalid type handle: inner type is None"
        assert(perr(function() return #B end) == unwrap)
        assert(perr(function() return H.base end) == inner)
        assert(perr(tostring, H) == inner)
        assert(perr(function() for k, v in pairs(H) do end end) == inner)
        assert(#L == 2)
        assert(perr(function() return L[1] end) == inner)
        assert(perr(tostring, L) == inner)
        assert(perr(function() for i, v in pairs(L) do end end) == inner)
        assert(perr(table.remove, L) == inner)
        assert(#L == 2, "a failed read removes nothing")
        H.base = "On"
        assert(H.base == "On", "a written field reads from the cache")
        "#,
    );
    assert!(e.dirty("H") && !e.dirty("L"));
}

// Review: the production seam answers "neither" for a Resource or Content until the asset session's userdata are wired (no silent
// acceptance), and asks the wired `AssetUserdata` once they are.
#[test]
fn asset_userdata_seam_answers_neither_until_wired() {
    struct Wired;
    impl AssetUserdata for Wired {
        fn resource_guid(&self, ud: &mlua::AnyUserData) -> mlua::Result<Option<Guid>> {
            values::TestObjects.resource_guid(ud)
        }
        fn content_hash(&self, ud: &mlua::AnyUserData) -> mlua::Result<Option<kfc::guid::ContentHash>> {
            values::TestObjects.content_hash(ud)
        }
    }
    let e = env();
    let lua = e.vm.lua();
    let guid = Guid::parse("01234567-89ab-cdef-0123-456789abcdef").unwrap();
    let resource = LuaValue::UserData(lua.create_userdata(values::TestResource(guid)).unwrap());
    let unwired = ValueObjects::new();
    let wired = ValueObjects::with_assets(Rc::new(Wired));
    for objects in [&unwired, &ValueObjects::default()] {
        let error = validate_and_clone(&resource, &e.ty(values::GUID), lua, objects)
            .unwrap_err()
            .to_string();
        assert!(error.starts_with("at .: guid expected, got "), "{error}");
        assert!(validate_and_clone(&resource, &e.ty(values::REF), lua, objects).is_err());
    }
    let bytes = encode(&resource, &e.ty(values::GUID), lua, &wired).unwrap();
    assert_eq!(
        bytes,
        encode(
            &LuaValue::String(lua.create_string(guid.to_string()).unwrap()),
            &e.ty(values::GUID),
            lua,
            &wired
        )
        .unwrap()
    );
}

// Review: a Lua finalizer that runs inside a mapped read's allocation (in generational mode Lua 5.4 ends each minor
// collection with the pending finalizers) and reads the same wrapper finds its cache borrowed. As in SF, that RefCell
// panic aborts the finalizer only: mlua turns it into the finalizer's error, which Lua reports as a warning. The read,
// the cache and the process are unharmed (on x64 about 2 in 7 of these finalizers hit a borrowed cache).
#[test]
fn finalizers_during_mapped_reads_leave_the_values_intact() {
    let e = env();
    let bytes = encode(&e.eval(ROOT_TABLE), &e.ty(ROOT), e.vm.lua(), &e.objects).unwrap();
    let (ty, objects) = (e.ty(ROOT), e.objects.clone());
    let fresh = e
        .vm
        .lua()
        .create_function(move |lua, ()| decode(bytes.clone().into(), &ty, lua, &objects).map_err(mlua::Error::runtime))
        .unwrap();
    e.env.set("FRESH", fresh).unwrap();
    e.run(
        r#"
        collectgarbage("generational", 1, 100) -- minor collections, each ending with the pending finalizers
        ENTERED, DONE = 0, 0
        local mt = {__gc = function() ENTERED = ENTERED + 1; local _ = CUR.child; local _ = CUR.children[2]; DONE = DONE + 1 end}
        for i = 1, 300 do
          CUR = FRESH()
          for j = 1, 8 do setmetatable({}, mt) end
          local c = CUR.child
          assert(c.base == 7 and rawequal(c, CUR.child))
          local kids = CUR.children
          assert(kids[2].extra == 23 and rawequal(kids[2], CUR.children[2]))
        end
        collectgarbage()
        collectgarbage()
        assert(ENTERED > 0)
        "#,
    );
    let entered: i64 = e.env.get("ENTERED").unwrap();
    let done: i64 = e.env.get("DONE").unwrap();
    eprintln!(
        "finalizers: {entered} ran, {} aborted on a borrowed cache",
        entered - done
    );
}

/// Runs `f` on a thread with a 1 MiB stack (a Windows main thread's; the DLL is a release build), so a missing depth
/// bound aborts the run (the codec's review test does the same). A debug build gets 8 MiB.
fn on_small_stack<R: Send + 'static>(f: impl FnOnce() -> R + Send + 'static) -> R {
    std::thread::Builder::new()
        .stack_size(if cfg!(debug_assertions) { 8 << 20 } else { 1 << 20 })
        .spawn(f)
        .unwrap()
        .join()
        .unwrap()
}

// Review, for the wrappers: assignments clone, so a wrapper never contains itself, but each insert of a chain
// into a new node clones the chain one level deeper, and a mapped value of self-referencing bytes reads as deep as a
// mod walks it. The wrappers' own walks (deep clone, dirtiness) stop at MAX_DEPTH like the value codec; unbounded they
// overflowed the stack. `tostring` recurses through Lua, whose C-call limit ends it.
#[test]
fn deep_wrapper_walks_are_errors_not_stack_overflows() {
    on_small_stack(|| {
        let e = env();
        let node = e.ty(userdata::NODE);
        let (ty, objects) = (node.clone(), e.objects.clone());
        let new =
            e.vm.lua()
                .create_function(move |lua, value: LuaValue| {
                    validate_and_clone(&value, &ty, lua, &objects).map_err(mlua::Error::external)
                })
                .unwrap();
        e.env.set("NEW", new).unwrap();
        e.run(
            r#"
            local x, depth = NEW({kids = {}}), 0
            local ok, err = pcall(function()
              for i = 1, 20000 do
                local y = NEW({kids = {}})
                table.insert(y.kids, x)
                x, depth = y, i
              end
            end)
            assert(not ok and tostring(err):find("value nested more than 128 levels deep", 1, true), tostring(err))
            DEPTH, DEEP = depth, x
            "#,
        );
        let depth: i64 = e.env.get("DEPTH").unwrap();
        assert!((60..=64).contains(&depth), "{depth}");
        let deep: LuaValue = e.env.get("DEEP").unwrap();
        assert!(is_dirty_lua_value(&deep).unwrap(), "within the bound");
        // a self-referencing mapped node: its kids array's one element is the node itself, at offset 0
        let looped = decode(vec![0, 0, 0, 0, 1, 0, 0, 0].into(), &node, e.vm.lua(), &e.objects).unwrap();
        e.env.set("M", looped.clone()).unwrap();
        e.run(
            r#"
            local n = M
            for i = 1, 30000 do n = n.kids[1] end
            assert(rawequal(n.kids, n.kids) and #n.kids == 1)
            assert(perr(tostring, M))
            "#,
        );
        let deep_error = |error: String| assert!(error.contains("value nested more than 128 levels deep"), "{error}");
        deep_error(is_dirty_lua_value(&looped).unwrap_err().to_string());
        deep_error(
            validate_and_clone(&looped, &node, e.vm.lua(), &e.objects)
                .unwrap_err()
                .to_string(),
        );
        deep_error(encode(&looped, &node, e.vm.lua(), &e.objects).unwrap_err());
    });
}

// Review, for the wrappers: mapping is lazy, so a faulty registry's cyclic chain inside a resource is first met
// by a wrapper's read: kfc's base-chain walks (`#`, fields, pairs) looped forever and its eager optional mapping
// recursed until the stack overflowed. Each is refused with the codec's error, when the wrapper is made or the field read.
#[test]
fn cyclic_chains_inside_a_value_are_errors_not_hangs() {
    on_small_stack(|| {
        let e = env();
        // `looped` at 4 is an optional of itself whose every blob offset points 4 bytes on: 100 000 levels of mapping
        let mut bytes = vec![0u8; 4];
        for _ in 0..100_000 {
            bytes.extend([4, 0, 0, 0]);
        }
        let holder = decode(bytes.into(), &e.ty(userdata::CYCLE_HOLDER), e.vm.lua(), &e.objects).unwrap();
        e.env.set("H", holder.clone()).unwrap();
        e.run(
            r#"
            local cycle = "runtime error: cyclic type chain at `m6.CycleA`"
            local looped = "runtime error: cyclic type chain at `m6.Loop`"
            assert(#H == 2)
            assert(perr(function() return H.cycle end) == cycle)
            assert(perr(function() return H.looped end) == looped)
            assert(perr(function() for k, v in pairs(H) do end end) == cycle)
            assert(perr(tostring, H) == cycle)
            "#,
        );
        let error = encode(&holder, &e.ty(userdata::CYCLE_HOLDER), e.vm.lua(), &e.objects).unwrap_err();
        assert!(error.contains("cyclic type chain at `m6.CycleA`"), "{error}");
    });
}

