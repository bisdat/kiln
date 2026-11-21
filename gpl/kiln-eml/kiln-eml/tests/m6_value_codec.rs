// SPDX-License-Identifier: GPL-3.0-or-later
//! The typed value codec (validator, converter, resource bytes) on synthetic registries, against ShroudForge
//! 1.7.9's modules/mod-runtime/lua/resource_values/{validate_resource_values,convert_resource_values,value_path}.rs
//! (identical at 1.8.0) and FINAL eml-assets-types `game.assets:reflected_scalar_codec`. Expectations are
//! source-derived: the oracle has no registry or archive to run these on. Container userdata come from the TEST-ONLY
//! seam implementation in support/m6_values (the value wrappers brings the real wrappers).
#[path = "support/m6_fixture/mod.rs"]
mod fixture;
#[path = "support/m6_types/mod.rs"]
mod reg;
#[path = "support/m6_values/mod.rs"]
mod values;

use kfc::{
    guid::{ContentHash, Guid},
    reflection::{TypeIndex, TypeRegistry},
    resource::value::Value,
};
use kiln_eml::sf_startup::resource_values::{
    self as rv, TypeHandle, convert_lua_to_value, decode, decode_mapped, encode, validate_and_clone,
};
use mlua::Lua;
use sfi_core::lua::LuaValue;
use std::rc::Rc;
use values::{TestArray, TestContent, TestMapped, TestObjects, TestResource, TestStruct, handle};

struct Env {
    lua: Lua,
    registry: Rc<TypeRegistry>,
}

fn env() -> Env {
    Env {
        lua: Lua::new(),
        registry: values::registry(),
    }
}

impl Env {
    fn ty(&self, index: usize) -> TypeHandle {
        handle(&self.registry, index)
    }
    fn named(&self, name: &str) -> TypeHandle {
        let index = self.registry.iter().find(|t| t.qualified_name == name).unwrap().index;
        TypeHandle::new(self.registry.clone(), index)
    }
    fn lua(&self, chunk: &str) -> LuaValue {
        self.lua
            .load(chunk)
            .set_name("=g06")
            .eval()
            .unwrap_or_else(|e| panic!("{e}"))
    }
    /// The validation error text, or the validated value.
    fn validate(&self, chunk: &str, ty: &TypeHandle) -> Result<LuaValue, String> {
        validate_and_clone(&self.lua(chunk), ty, &self.lua, &TestObjects).map_err(|e| e.to_string())
    }
    fn err(&self, chunk: &str, ty: &TypeHandle) -> String {
        match self.validate(chunk, ty) {
            Ok(_) => panic!("{chunk} validated"),
            Err(e) => e,
        }
    }
    /// validate + convert to a kfc value.
    fn value(&self, chunk: &str, ty: &TypeHandle) -> Value {
        let checked = self.validate(chunk, ty).unwrap_or_else(|e| panic!("{chunk}: {e}"));
        convert_lua_to_value(&checked, ty, &TestObjects).unwrap_or_else(|e| panic!("{chunk}: {e}"))
    }
}

fn structure(fields: Vec<(&str, Value)>) -> Value {
    Value::Struct(Box::new(fields.into_iter().map(|(k, v)| (k.to_owned(), v)).collect()))
}

// Red-first case 1: a nested invalid field names its exact path; the input is not changed.
#[test]
fn nested_invalid_field_reports_exact_path_without_mutation() {
    let e = env();
    let root = e.lua(
        r#"ROOT = {child = {base = 1, extra = 2}, fixed = {2, 3},
            children = {{base = 1, extra = 2}, {base = 3, extra = -1}}, maybe = nil,
            choice = {type = "m6.Child", value = {base = 1, extra = 2}}, text = "t"}
           return ROOT"#,
    );
    let error = validate_and_clone(&root, &e.ty(8), &e.lua, &TestObjects)
        .unwrap_err()
        .to_string();
    assert_eq!(
        error,
        "at children.1.extra: expected `unsigned integer in range [0, 4294967295]`, got `-1`"
    );
    let unchanged: bool = e
        .lua
        .load(
            r#"local n = 0 for _ in pairs(ROOT) do n = n + 1 end
               return n == 5 and ROOT.children[2].extra == -1 and getmetatable(ROOT) == nil
                 and #ROOT.children == 2 and ROOT.child.base == 1"#,
        )
        .eval()
        .unwrap();
    assert!(unchanged);
    // the root path is "."
    assert_eq!(
        e.err("return 4294967296", &e.ty(0)),
        "at .: expected `unsigned integer in range [0, 4294967295]`, got `4294967296`"
    );
}

// Red-first cases 2 and 3: fields are read with Table::get (metamethods), a failing get is MissingField.
#[test]
fn struct_fields_use_get_and_metamethods() {
    let e = env();
    let child = e.ty(2);
    let v = e.value(
        r#"return setmetatable({extra = 5}, {__index = function(_, k) if k == "base" then return 4 end end})"#,
        &child,
    );
    assert_eq!(v, structure(vec![("base", Value::UInt(4)), ("extra", Value::UInt(5))]));
    // a throwing __index: SF maps any get failure to MissingField at the struct's own path
    assert_eq!(
        e.err(
            r#"return setmetatable({}, {__index = function() error("boom") end})"#,
            &child
        ),
        "at .: missing field `base` in struct `m6.Child`"
    );
    assert_eq!(
        e.err(
            r#"return {child = setmetatable({base = 1}, {__index = function() error("boom") end})}"#,
            &e.ty(8)
        ),
        "at child: missing field `extra` in struct `m6.Child`"
    );
    // a field the table lacks is nil, which a UInt32 refuses; unknown keys are ignored
    assert_eq!(
        e.err("return {extra = 1, other = 9}", &child),
        "at base: expected `unsigned integer in range [0, 4294967295]`, got `nil`"
    );
    // fields are read base first, in the type's order: the first failing field is the base's
    let order: String = {
        e.lua
            .load(
                r#"SEEN = {}
                   return setmetatable({}, {__index = function(_, k) SEEN[#SEEN + 1] = k; return 1 end})"#,
            )
            .eval::<LuaValue>()
            .map(|t| {
                validate_and_clone(&t, &child, &e.lua, &TestObjects).unwrap();
                e.lua.load("return table.concat(SEEN, ',')").eval::<String>().unwrap()
            })
            .unwrap()
    };
    assert_eq!(order, "base,extra");
    // a userdata of the wrong class or type
    let ud = LuaValue::UserData(e.lua.create_userdata(values::Foreign).unwrap());
    let error = validate_and_clone(&ud, &child, &e.lua, &TestObjects)
        .unwrap_err()
        .to_string();
    assert!(
        error.starts_with("at .: expected `m6.Child`, got `userdata `TypeId("),
        "{error}"
    );
    assert!(
        e.err("return 1", &child)
            .starts_with("at .: expected `m6.Child`, got `1`")
    );
}

// Red-first case 4: static array length and element type; __len called as often as SF calls it.
#[test]
fn static_arrays_check_length_and_elements() {
    let e = env();
    let fixed = e.ty(3);
    assert_eq!(
        e.value("return {2, 3}", &fixed),
        Value::Array(vec![Value::UInt(2), Value::UInt(3)])
    );
    assert_eq!(
        e.err("return {1}", &fixed),
        "at .: expected `array of length 2`, got `array of length 1`"
    );
    assert_eq!(
        e.err(r#"return {1, "x"}"#, &fixed),
        "at 1: expected `unsigned integer in range [0, 4294967295]`, got `x`"
    );
    let lens = |chunk: &str, ty: &TypeHandle| -> (bool, i64) {
        let value = e.lua(&format!(
            "LENS = 0 return setmetatable({chunk}, {{__len = function(t) LENS = LENS + 1 return rawlen(t) end}})"
        ));
        let ok = validate_and_clone(&value, ty, &e.lua, &TestObjects).is_ok();
        (ok, e.lua.load("return LENS").eval().unwrap())
    };
    assert_eq!(lens("{2, 3}", &fixed), (true, 2), "capacity, then the length check");
    assert_eq!(lens("{2}", &fixed), (false, 3), "and once more for the message");
    assert_eq!(lens("{}", &e.ty(4)), (true, 1), "a blob array asks once");
    // the sequence stops at the first nil; a negative __len is SF's capacity-overflow panic, raised
    assert_eq!(
        e.value("return {{base=1,extra=2}, nil, {base=3,extra=4}}", &e.ty(4))
            .as_array()
            .unwrap()
            .len(),
        1
    );
    assert!(
        e.err("return setmetatable({}, {__len = function() return -1 end})", &e.ty(4))
            .ends_with("capacity overflow")
    );
    // a huge __len only bounds the reservation
    assert!(
        e.validate(
            "return setmetatable({}, {__len = function() return math.maxinteger end})",
            &e.ty(4)
        )
        .is_ok()
    );
    let error = e.err(
        r#"return setmetatable({}, {__len = function() error("len") end})"#,
        &e.ty(4),
    );
    assert!(
        error.starts_with("at .: runtime error: g06:1: len"),
        "a throwing __len is the error: {error}"
    );
}

// Red-first cases 5 and 6: optionals and variants.
#[test]
fn optionals_and_variants() {
    let e = env();
    let maybe = e.ty(5);
    let choice = e.ty(6);
    assert_eq!(e.value("return nil", &maybe), Value::None);
    assert_eq!(
        e.value("return {base = 1, extra = 2}", &maybe),
        structure(vec![("base", Value::UInt(1)), ("extra", Value::UInt(2))])
    );
    assert_eq!(e.err("return 3", &maybe), "at .: expected `m6.Child`, got `3`");
    let v = e.value(r#"return {type = "m6.Child", value = {base = 1, extra = 2}}"#, &choice);
    let Value::Variant(v) = v else { panic!() };
    assert_eq!(v.type_index, TypeIndex::new(2));
    assert_eq!(v.value.get("extra"), Some(&Value::UInt(2)));
    assert_eq!(
        e.value(r#"return {type = "m6.Base", value = {base = 1}}"#, &choice)
            .as_variant()
            .unwrap()
            .type_index,
        TypeIndex::new(1)
    );
    assert_eq!(
        e.err(r#"return {type = "m6.UInt32", value = 1}"#, &choice),
        "at type: expected `m6.Base`, got `m6.UInt32`"
    );
    assert_eq!(
        e.err(r#"return {type = "m6.Nope", value = {}}"#, &choice),
        "at type: invalid type `m6.Nope`"
    );
    assert_eq!(
        e.err(r#"return {type = "m6.Child", value = {base = 1}}"#, &choice),
        "at value.extra: expected `unsigned integer in range [0, 4294967295]`, got `nil`"
    );
    assert_eq!(
        e.err(r#"return {type = "m6.Child", value = 7}"#, &choice),
        "at value: expected `m6.Child`, got `7`"
    );
    assert!(
        e.err(r#"return {value = {}}"#, &choice)
            .starts_with("at type: error converting Lua nil to String")
    );
    // the type name is looked up through the registry's hash index, as SF does (no exact-name check)
    assert_eq!(
        e.err(&format!(r#"return {{type = "{}", value = {{}}}}"#, reg::GHOST), &choice),
        "at type: expected `m6.Base`, got `m6.Aliased`"
    );
    assert_eq!(e.err("return 1", &choice), "at .: expected `m6.Choice`, got `1`");
}

// Red-first case 7: inherited fields come from the base, by the derived struct's own validation.
#[test]
fn inherited_fields_and_typedefs() {
    let e = env();
    let tagged = e.named("m6.Tagged");
    assert_eq!(
        e.value("return {base = 1, extra = 2}", &tagged),
        structure(vec![("base", Value::UInt(1)), ("extra", Value::UInt(2))])
    );
    assert_eq!(
        e.err("return {extra = 2}", &tagged),
        "at base: expected `unsigned integer in range [0, 4294967295]`, got `nil`"
    );
    let alias = e.ty(reg::ALIAS);
    assert_eq!(e.value("return 7", &alias), Value::UInt(7));
    assert_eq!(
        e.err("return -1", &alias),
        "at .: expected `unsigned integer in range [0, 4294967295]`, got `-1`"
    );
    // name_of shows a typedef's target
    assert_eq!(rv::name_of(&alias), "m6.UInt32");
}

// Red-first case 8: integer and float boundaries, enum and scalar forms (FINAL game.assets:reflected_scalar_codec).
#[test]
fn scalar_boundaries() {
    let e = env();
    let ok = |chunk: &str, index: usize| e.value(&format!("return {chunk}"), &e.ty(index));
    let bad = |chunk: &str, index: usize| e.err(&format!("return {chunk}"), &e.ty(index));
    assert_eq!(ok("255", values::U8), Value::UInt(255));
    assert_eq!(
        bad("256", values::U8),
        "at .: expected `unsigned integer in range [0, 255]`, got `256`"
    );
    assert_eq!(
        bad("-1", values::U8),
        "at .: expected `unsigned integer in range [0, 255]`, got `-1`"
    );
    assert_eq!(ok("3.0", values::U8), Value::UInt(3));
    assert_eq!(
        bad("3.5", values::U8),
        "at .: expected `unsigned integer in range [0, 255]`, got `3.5`"
    );
    assert_eq!(ok("-128", values::S8), Value::SInt(-128));
    assert_eq!(
        bad("128", values::S8),
        "at .: expected `signed integer in range [-128, 127]`, got `128`"
    );
    assert_eq!(ok("65535", values::U16), Value::UInt(65535));
    assert_eq!(ok("-32768", values::S16), Value::SInt(-32768));
    assert_eq!(
        bad("2147483648", values::S32),
        "at .: expected `signed integer in range [-2147483648, 2147483647]`, got `2147483648`"
    );
    // UInt64 takes an Integer's bits: -1 is u64::MAX
    assert_eq!(ok("-1", values::U64), Value::UInt(u64::MAX));
    assert_eq!(ok("math.mininteger", values::U64), Value::UInt(1 << 63));
    // a float compares with f64 bounds, then the VALIDATOR stores it as a Lua Integer (`n as i64`): 2^64 passes the
    // u64 bound check and saturates to i64::MAX, which converts as that (SF's own validate-then-convert order)
    assert_eq!(ok("2.0^64", values::U64), Value::UInt(i64::MAX as u64));
    assert_eq!(ok("2.0^63", values::U64), Value::UInt(i64::MAX as u64));
    assert_eq!(ok("2.0^62", values::U64), Value::UInt(1 << 62));
    assert_eq!(
        bad("2.0^65", values::U64),
        format!(
            "at .: expected `unsigned integer in range [0, {}]`, got `36893488147419103000`",
            u64::MAX
        )
    );
    assert_eq!(ok("math.mininteger", values::S64), Value::SInt(i64::MIN));
    assert_eq!(
        ok("2.0^63", values::S64),
        Value::SInt(i64::MAX),
        "f64 2^63 equals i64::MAX as f64; the cast saturates"
    );
    assert_eq!(ok("1", values::F32), Value::Float(1.0));
    assert_eq!(
        ok("0/0", values::F64).as_f64().map(f64::is_nan),
        Some(true),
        "no finite policy here"
    );
    assert_eq!(bad("'1'", values::F64), "at .: expected `float`, got `1`");
    assert_eq!(
        bad("'1'", values::U8),
        "at .: expected `unsigned integer in range [0, 255]`, got `1`",
        "no string coercion"
    );
    assert_eq!(ok("true", values::BOOL), Value::Bool(true));
    assert_eq!(bad("1", values::BOOL), "at .: expected `boolean`, got `1`");
    assert_eq!(ok("nil", values::NONE), Value::None);
    assert_eq!(bad("false", values::NONE), "at .: expected `nil`, got `false`");
    // enums: a name, or an Integer naming a value; the validated value is the name
    assert_eq!(ok("'Blue'", values::COLOR), Value::String("Blue".into()));
    assert_eq!(ok("7", values::COLOR), Value::String("Blue".into()));
    assert_eq!(
        bad("2", values::COLOR),
        r#"at .: expected `one of ["Red", "Green", "Blue"]`, got `2`"#
    );
    // a Float is no enum value, even integral (mlua shows it as 7)
    assert_eq!(
        bad("7.0", values::COLOR),
        r#"at .: expected `one of ["Red", "Green", "Blue"]`, got `7`"#
    );
    assert_eq!(
        bad("'blue'", values::COLOR),
        r#"at .: expected `one of ["Red", "Green", "Blue"]`, got `blue`"#
    );
    // GUIDs: Guid is SF's Guid cast (strings, Content, Resource); ObjectReference parses as a content hash
    let guid = "01234567-89ab-cdef-0123-456789abcdef";
    assert_eq!(
        ok(&format!("'{guid}'"), values::GUID),
        Value::Guid(Guid::parse(guid).unwrap())
    );
    assert_eq!(bad("'nope'", values::GUID), "at .: guid expected, got string");
    assert_eq!(bad("{}", values::GUID), "at .: guid expected, got table");
    assert_eq!(
        ok(&format!("'{guid}'"), values::REF),
        Value::Guid(Guid::parse(guid).unwrap())
    );
    assert_eq!(bad("'nope'", values::REF), "at .: invalid guid `nope`");
    assert_eq!(bad("1", values::REF), "at .: expected `object reference`, got `1`");
    // blob strings keep the Lua bytes until conversion, which is lossy UTF-8
    let text = e.ty(7);
    let checked = e.validate(r#"return "a\255b""#, &text).unwrap();
    assert_eq!(checked.as_string().unwrap().as_bytes().as_ref(), b"a\xffb");
    assert_eq!(
        convert_lua_to_value(&checked, &text, &TestObjects).unwrap(),
        Value::String("a\u{fffd}b".into())
    );
}

// Content, Resource and owned/mapped wrappers through the seam.
#[test]
fn seam_userdata_inputs() {
    let e = env();
    let hash = ContentHash::new(5, 6, 7, 8);
    let guid = Guid::parse("01234567-89ab-cdef-0123-456789abcdef").unwrap();
    e.lua.globals().set("CONTENT", TestContent(hash)).unwrap();
    e.lua.globals().set("RESOURCE", TestResource(guid)).unwrap();
    assert_eq!(
        e.value("return CONTENT", &e.ty(values::GUID)),
        Value::Guid(hash.into_guid())
    );
    assert_eq!(e.value("return RESOURCE", &e.ty(values::GUID)), Value::Guid(guid));
    assert_eq!(e.value("return RESOURCE", &e.ty(values::REF)), Value::Guid(guid));
    assert!(
        e.err("return CONTENT", &e.ty(values::REF))
            .starts_with("at .: expected `object reference`, got `userdata `TypeId(")
    );
    // a Content where a struct is expected is its {size, hash0, hash1, hash2}
    let mut hash_type = reg::types();
    let mut keen = hash_type[1].clone();
    keen.index = TypeIndex::new(hash_type.len());
    keen.qualified_name = "m6.ContentHash".into();
    keen.qualified_hash = kfc::hash::fnv("m6.ContentHash");
    keen.impact_hash = kfc::hash::fnv("M6_ContentHash");
    keen.struct_fields.clear();
    for (i, name) in ["size", "hash0", "hash1", "hash2"].into_iter().enumerate() {
        keen.struct_fields.insert(
            name.into(),
            kfc::reflection::StructFieldMetadata {
                name: name.into(),
                r#type: TypeIndex::new(0),
                data_offset: 4 * i as u64,
                attributes: Default::default(),
            },
        );
    }
    hash_type.push(keen);
    let registry = Rc::new(reg::registry_of("hash", hash_type));
    let ty = TypeHandle::new(registry.clone(), TypeIndex::new(registry.len() - 1));
    let checked = validate_and_clone(&e.lua("return CONTENT"), &ty, &e.lua, &TestObjects).unwrap();
    assert_eq!(
        convert_lua_to_value(&checked, &ty, &TestObjects).unwrap(),
        structure(vec![
            ("size", Value::UInt(5)),
            ("hash0", Value::UInt(6)),
            ("hash1", Value::UInt(7)),
            ("hash2", Value::UInt(8))
        ])
    );
    // a wrapper of the exact type is deep-cloned (a new object), one of another type is refused
    let child = e.ty(2);
    let original = e.validate("return {base = 1, extra = 2}", &child).unwrap();
    let copy = validate_and_clone(&original, &child, &e.lua, &TestObjects).unwrap();
    assert_ne!(original, copy);
    assert_eq!(
        copy.as_userdata().unwrap().borrow::<TestStruct>().unwrap().fields.len(),
        2
    );
    let error = validate_and_clone(&original, &e.ty(1), &e.lua, &TestObjects)
        .unwrap_err()
        .to_string();
    assert_eq!(error, "at .: expected `m6.Base`, got `m6.Child`");
    // an array wrapper where a struct is expected is a foreign userdata to the struct check
    let array = e.validate("return {2, 3}", &e.ty(3)).unwrap();
    assert!(
        validate_and_clone(&array, &child, &e.lua, &TestObjects)
            .unwrap_err()
            .to_string()
            .starts_with("at .: expected `m6.Child`, got `userdata `TypeId(")
    );
    // a wrapper nested in a table is cloned in place
    e.lua.globals().set("C", original.clone()).unwrap();
    let root = e.validate(
        r#"return {child = C, fixed = {2, 3}, children = {C}, choice = {type = "m6.Child", value = C}, text = ""}"#,
        &e.ty(8),
    );
    let root = root.unwrap();
    let fields = &root.as_userdata().unwrap().borrow::<TestStruct>().unwrap().fields;
    assert_ne!(fields["child"], original, "deep copy");
    // the converter: wrong-class and plain-table inputs
    let converted = convert_lua_to_value(&e.lua("return {}"), &child, &TestObjects)
        .unwrap_err()
        .to_string();
    assert!(
        // the address reads 0x... on Linux and bare hex on Windows
        converted.starts_with("at .: expected `m6.Child` but found `table: "),
        "{converted}"
    );
    let converted = convert_lua_to_value(&original, &e.ty(1), &TestObjects)
        .unwrap_err()
        .to_string();
    assert_eq!(converted, "at .: expected `m6.Base` but found `m6.Child`");
    // try_clone_lua_value: SF panics on an unknown userdata; here it is an error
    let foreign = LuaValue::UserData(e.lua.create_userdata(values::Foreign).unwrap());
    assert_eq!(
        rv::try_clone_lua_value(&foreign, &e.lua, &TestObjects)
            .unwrap_err()
            .to_string(),
        "runtime error: unexpected user data type in clone_lua_value"
    );
}

// DS kinds: SF's validator and converter hit todo!(); here a Lua error with that text.
#[test]
fn ds_kinds_are_errors_not_panics() {
    let e = env();
    for ds in [9, 10, 11, 12] {
        assert_eq!(
            e.err("return {}", &e.ty(ds)),
            "at .: runtime error: not yet implemented",
            "validate {ds}"
        );
        let error = convert_lua_to_value(&LuaValue::Nil, &e.ty(ds), &TestObjects)
            .unwrap_err()
            .to_string();
        assert_eq!(error, "at .: runtime error: not yet implemented", "convert {ds}");
    }
}

fn scalars_table() -> &'static str {
    r#"return {u8 = 200, s8 = -5, u16 = 60000, s16 = -300, s32 = -70000, u32 = 4000000000, u64 = -2,
        s64 = math.mininteger, f32 = 0.5, f64 = -1.25, b = true, color = "Green", flags = {"B", "A"},
        guid = "01234567-89ab-cdef-0123-456789abcdef", ref = "fedcba98-7654-3210-fedc-ba9876543210", alias = 9}"#
}

// Red-first case 9: resource bytes round-trip; decoding gives the values SF hands a mod.
#[test]
fn resource_bytes_round_trip() {
    let e = env();
    let scalars = e.ty(values::SCALARS);
    let bytes = encode(&e.lua(scalars_table()), &scalars, &e.lua, &TestObjects).unwrap();
    assert_eq!(bytes.len(), values::SCALARS_SIZE as usize);
    assert_eq!(&bytes[0..2], &[200, 0xfb]);
    assert_eq!(bytes[50], 0b1001, "bits A (0) and B (3)");
    assert_eq!(u64::from_le_bytes(bytes[16..24].try_into().unwrap()), u64::MAX - 1);
    // decode: scalars to Lua as SF does (UInt64 bits read negative), the struct through the seam's mapped wrapper
    let decoded = decode(bytes.clone().into(), &scalars, &e.lua, &TestObjects).unwrap();
    let mapped = decoded.as_userdata().unwrap().borrow::<TestMapped>().unwrap().0.clone();
    let rv::MappedValue::Struct(s) = &mapped else { panic!() };
    let field = |name: &str| rv::value_to_lua(&s.get(name).unwrap().unwrap(), &e.lua, &TestObjects).unwrap();
    assert_eq!(field("u64"), LuaValue::Integer(-2));
    assert_eq!(field("s64"), LuaValue::Integer(i64::MIN));
    assert_eq!(field("u8"), LuaValue::Integer(200));
    assert_eq!(field("f32"), LuaValue::Number(0.5));
    assert_eq!(field("color").to_string().unwrap(), "Green");
    assert_eq!(
        field("guid").to_string().unwrap(),
        "01234567-89ab-cdef-0123-456789abcdef"
    );
    assert_eq!(
        field("ref").to_string().unwrap(),
        "fedcba98-7654-3210-fedc-ba9876543210"
    );
    let flags = field("flags");
    let flags = flags
        .as_userdata()
        .unwrap()
        .borrow::<TestArray>()
        .unwrap()
        .items
        .iter()
        .map(|v| v.to_string().unwrap())
        .collect::<Vec<_>>();
    assert_eq!(flags, ["A", "B"], "bits in bit order, by name");
    // the decoded wrapper re-encodes to the same bytes
    assert_eq!(encode(&decoded, &scalars, &e.lua, &TestObjects).unwrap(), bytes);
    // The fixture's Root from a plain table encodes exactly as the fixture's own value
    let root = e.ty(8);
    let from_table = encode(
        &e.lua(
            r#"return {child = {base = 7, extra = 11}, fixed = {2, 3},
                children = {{base = 13, extra = 17}, {base = 19, extra = 23}}, maybe = {base = 29, extra = 31},
                choice = {type = "m6.Child", value = {base = 37, extra = 41}}, text = "synthetic"}"#,
        ),
        &root,
        &e.lua,
        &TestObjects,
    )
    .unwrap();
    let expected = fixture::types::root_value()
        .to_bytes(&e.registry, e.registry.get(TypeIndex::new(8)).unwrap())
        .unwrap();
    assert_eq!(from_table, expected);
    let again = decode(from_table.into(), &root, &e.lua, &TestObjects).unwrap();
    assert_eq!(encode(&again, &root, &e.lua, &TestObjects).unwrap(), expected);
}

// Decoding: unknown enum values are decimal text, optional absent is nil; padding is physical, not semantic.
#[test]
fn decode_semantics_and_padding() {
    let e = env();
    let scalars = e.ty(values::SCALARS);
    let mut bytes = encode(&e.lua(scalars_table()), &scalars, &e.lua, &TestObjects).unwrap();
    bytes[values::PADDING] = 0xee;
    bytes[49] = 5; // a Color value no field names
    let decoded = decode(bytes.clone().into(), &scalars, &e.lua, &TestObjects).unwrap();
    let rv::MappedValue::Struct(s) = &decoded.as_userdata().unwrap().borrow::<TestMapped>().unwrap().0 else {
        panic!()
    };
    assert_eq!(
        rv::value_to_lua(&s.get("color").unwrap().unwrap(), &e.lua, &TestObjects)
            .unwrap()
            .to_string()
            .unwrap(),
        "5"
    );
    bytes[49] = 1;
    let decoded = decode(bytes.clone().into(), &scalars, &e.lua, &TestObjects).unwrap();
    let reencoded = encode(&decoded, &scalars, &e.lua, &TestObjects).unwrap();
    assert_ne!(reencoded, bytes, "padding is not carried");
    assert_eq!(reencoded[values::PADDING], 0);
    let mut cleared = bytes.clone();
    cleared[values::PADDING] = 0;
    assert_eq!(reencoded, cleared, "every semantic byte is");
    // an absent optional decodes to nil
    let maybe = e.ty(5);
    let none = encode(&LuaValue::Nil, &maybe, &e.lua, &TestObjects).unwrap();
    assert_eq!(
        decode(none.into(), &maybe, &e.lua, &TestObjects).unwrap(),
        LuaValue::Nil
    );
}

// Red-first case 10: truncated or malformed input and registry faults are errors; no panic escapes.
#[test]
fn malformed_input_never_panics() {
    let e = env();
    let root = e.ty(8);
    let expected = fixture::types::root_value()
        .to_bytes(&e.registry, e.registry.get(TypeIndex::new(8)).unwrap())
        .unwrap();
    for cut in [0, 1, 7, 20, expected.len() / 2, expected.len() - 1] {
        let error = decode_mapped(expected[..cut].to_vec().into(), &root).err();
        let walked = decode(expected[..cut].to_vec().into(), &root, &e.lua, &TestObjects)
            .and_then(|v| encode(&v, &root, &e.lua, &TestObjects).map(|_| ()));
        assert!(
            error.is_some() || walked.is_err(),
            "cut {cut} decoded and re-encoded whole"
        );
    }
    // a blob offset pointing far outside the data
    let mut far = expected.clone();
    far[16..20].copy_from_slice(&0x7fff_fff0u32.to_le_bytes());
    let _ = decode(far.into(), &root, &e.lua, &TestObjects).and_then(|v| encode(&v, &root, &e.lua, &TestObjects));
    // the parser's own panics: an enum without its value type (kfc expect) and a struct field of a missing type
    let mode = e.named("m6.Mode");
    let error = encode(&e.lua("return 'On'"), &mode, &e.lua, &TestObjects).unwrap_err();
    assert!(error.starts_with("serializing failed: "), "{error}");
    let error = decode_mapped(vec![0; 8].into(), &mode).unwrap_err();
    assert!(error.starts_with("decoding failed: "), "{error}");
    let broken = e.named("m6.Broken");
    // mapping is lazy: the decode itself succeeds; reading the missing field is the wrapper's and errors there
    assert!(decode(vec![0; 8].into(), &broken, &e.lua, &TestObjects).is_ok());
    assert_eq!(
        e.err("return {lost = 1}", &broken),
        "at .: runtime error: field type must exist in the type registry"
    );
    let error = convert_lua_to_value(&e.lua("return 2"), &e.named("m6.Mode"), &TestObjects)
        .unwrap_err()
        .to_string();
    assert_eq!(error, r#"at .: expected `one of ["Off", "On", "Big"]` but found `2`"#);
}


/// m6.Node {kids: m6.NodeList} with m6.NodeList a blob array of m6.Node (a recursive type), m6.Loop an optional of
/// itself and m6.Self a typedef of itself (the last two only a faulty registry has).
fn recursive_registry() -> (Rc<TypeRegistry>, usize) {
    let mut types = values::types();
    let n = types.len();
    let named = |mut t: kfc::reflection::TypeMetadata, index: usize, name: &str, inner: Option<usize>| {
        t.index = TypeIndex::new(index);
        t.name = name.into();
        t.qualified_name = format!("m6.{name}");
        t.qualified_hash = kfc::hash::fnv(&t.qualified_name);
        t.impact_hash = kfc::hash::fnv(&format!("M6_{name}"));
        t.inner_type = inner.map(TypeIndex::new);
        t.default_value = None;
        t.attributes = Default::default();
        t
    };
    let mut node = named(types[2].clone(), n, "Node", None);
    node.struct_fields.clear();
    node.struct_fields.insert(
        "kids".into(),
        kfc::reflection::StructFieldMetadata {
            name: "kids".into(),
            r#type: TypeIndex::new(n + 1),
            data_offset: 0,
            attributes: Default::default(),
        },
    );
    node.field_count = 1;
    let list = named(types[4].clone(), n + 1, "NodeList", Some(n));
    let looped = named(types[5].clone(), n + 2, "Loop", Some(n + 2));
    let selfish = named(types[reg::ALIAS].clone(), n + 3, "Self", Some(n + 3));
    types.extend([node, list, looped, selfish]);
    (Rc::new(reg::registry_of("recursive", types)), n)
}

/// Runs `f` on a thread with a 1 MiB stack (a Windows main thread's; the DLL is a release build), so a missing depth
/// bound aborts the run. Unoptimized frames are several times larger, so a debug build gets 8 MiB, which an unbounded
/// walk overflows just the same.
fn on_small_stack<R: Send + 'static>(f: impl FnOnce() -> R + Send + 'static) -> R {
    std::thread::Builder::new()
        .stack_size(if cfg!(debug_assertions) { 8 << 20 } else { 1 << 20 })
        .spawn(f)
        .unwrap()
        .join()
        .unwrap()
}

// Review: SF has no depth bound; a self-referencing table of a recursive type, or a deep chain, overflowed the
// stack and aborted the process. Here both are an error at the level that passes MAX_DEPTH.
#[test]
fn deep_and_self_referencing_values_are_errors_not_stack_overflows() {
    on_small_stack(|| {
        let e = env();
        let (registry, n) = recursive_registry();
        let node = TypeHandle::new(registry, TypeIndex::new(n));
        let deep = |chunk: &str| {
            validate_and_clone(&e.lua(chunk), &node, &e.lua, &TestObjects)
                .unwrap_err()
                .to_string()
        };
        let error = deep("local t = {} t.kids = {t} return t");
        assert!(error.starts_with("at kids.0.kids.0."), "{error}");
        assert!(
            error.ends_with(": runtime error: value nested more than 128 levels deep"),
            "{error}"
        );
        assert_eq!(error.matches("kids").count(), rv::MAX_DEPTH / 2, "{error}");
        let error = deep("local t = {kids = {}} for i = 1, 100000 do t = {kids = {t}} end return t");
        assert!(error.ends_with("value nested more than 128 levels deep"), "{error}");
        // within the bound a recursive value validates and converts: 63 nodes and 63 lists, plus the last empty list
        let ok = e.lua("local t = {kids = {}} for i = 1, 62 do t = {kids = {t}} end return t");
        let checked = validate_and_clone(&ok, &node, &e.lua, &TestObjects).unwrap();
        convert_lua_to_value(&checked, &node, &TestObjects).unwrap();
        let too_deep = e.lua("local t = {kids = {}} for i = 1, 64 do t = {kids = {t}} end return t");
        assert!(validate_and_clone(&too_deep, &node, &e.lua, &TestObjects).is_err());
    });
}

// The converter has the same bound: wrappers linked into a cycle (the wrappers' could be) end in an error, not an overflow.
#[test]
fn converter_depth_is_bounded() {
    on_small_stack(|| {
        let e = env();
        let (registry, n) = recursive_registry();
        let node = TypeHandle::new(registry.clone(), TypeIndex::new(n));
        let list = TypeHandle::new(registry, TypeIndex::new(n + 1));
        let kids = e
            .lua
            .create_userdata(TestArray {
                r#type: list,
                items: vec![],
            })
            .unwrap();
        let parent = e
            .lua
            .create_userdata(TestStruct {
                r#type: node.clone(),
                fields: [("kids".to_owned(), LuaValue::UserData(kids.clone()))].into(),
            })
            .unwrap();
        kids.borrow_mut::<TestArray>()
            .unwrap()
            .items
            .push(LuaValue::UserData(parent.clone()));
        let error = convert_lua_to_value(&LuaValue::UserData(parent), &node, &TestObjects)
            .unwrap_err()
            .to_string();
        assert!(
            error.ends_with(": runtime error: value nested more than 128 levels deep"),
            "{error}"
        );
        kids.borrow_mut::<TestArray>().unwrap().items.clear();
    });
}

// Review: a faulty registry with a cyclic inheritance or inner-type chain made kfc's own walks (iter_fields,
// is_sub_type, eager optional mapping) loop forever or recurse without end. Each is an error now.
#[test]
fn cyclic_type_chains_are_errors_not_hangs() {
    let e = env();
    let cycle = e.ty(reg::CYCLE_A);
    assert_eq!(
        e.err("return {}", &cycle),
        "at .: runtime error: cyclic type chain at `m6.CycleA`"
    );
    assert_eq!(
        e.err(r#"return {type = "m6.CycleB", value = {}}"#, &e.ty(6)),
        "at type: runtime error: cyclic type chain at `m6.CycleB`"
    );
    let (registry, n) = recursive_registry();
    let looped = TypeHandle::new(registry.clone(), TypeIndex::new(n + 2));
    assert_eq!(
        decode_mapped(vec![0; 16].into(), &looped).unwrap_err(),
        "runtime error: cyclic type chain at `m6.Loop`"
    );
    let selfish = TypeHandle::new(registry, TypeIndex::new(n + 3));
    assert_eq!(rv::name_of(&selfish), "m6.Self");
    assert!(
        validate_and_clone(&e.lua("return 1"), &selfish, &e.lua, &TestObjects)
            .unwrap_err()
            .to_string()
            .ends_with("value nested more than 128 levels deep")
    );
}

// Review: the codec takes a wrapper only when the seam's class matches the expected kind as well as its type, so
// a wrapper whose class and type disagree (a struct wrapper claiming an array type) is refused, never cloned or
// converted as the array it claims to be.
#[test]
fn a_wrapper_of_the_wrong_class_is_refused_even_with_the_right_type() {
    let e = env();
    let fixed = e.ty(3);
    let liar = LuaValue::UserData(
        e.lua
            .create_userdata(TestStruct {
                r#type: fixed.clone(),
                fields: Default::default(),
            })
            .unwrap(),
    );
    let error = validate_and_clone(&liar, &fixed, &e.lua, &TestObjects)
        .unwrap_err()
        .to_string();
    assert!(
        error.starts_with("at .: expected `m6.Fixed`, got `userdata `TypeId("),
        "{error}"
    );
    let error = convert_lua_to_value(&liar, &fixed, &TestObjects)
        .unwrap_err()
        .to_string();
    assert!(
        error.starts_with("at .: expected `m6.Fixed` but found `userdata `TypeId("),
        "{error}"
    );
}
