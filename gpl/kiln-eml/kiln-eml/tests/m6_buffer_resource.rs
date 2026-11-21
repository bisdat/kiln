// SPDX-License-Identifier: GPL-3.0-or-later
//! The startup pass's Buffer resource port (sfi-core's real Buffer extended through `BufferResourcePort`,
//! attached before the first Buffer exists). ShroudForge 1.7.9 lua/bindings/buffer/mod.rs:345-364 (read_resource) and
//! 457-480 (write_resource), identical at 1.8.0: the receiver is borrowed for the whole call (immutably to read,
//! mutably to write), the closed/readable or closed/writable checks come before the type argument, a read decodes
//! head..tail without moving head and returns exactly one value, a write validates and clones, converts, encodes and
//! appends at tail and returns nothing, and neither has a PATCH gate. Expectations are source-derived; the
//! archive-independent precedence is also in the oracle transcript (m6_assets).
#[path = "support/m6_fixture/mod.rs"]
mod fixture;

use fixture::types;
use kfc::reflection::LookupKey;
use kiln_eml::sf_startup::{
    SelectedTarget, TypeSession, buffer_resource::BufferResources, game, resource_values::ValueObjects,
};
use mlua::Table;
use sfi_core::{
    eml::buffer::Buffer,
    host::{HostContext, Package, PackageSource, Phase, Side, SourceVersion},
    lua::{BufferResourcePort, LuaVm, VmAttachments},
};
use std::rc::Rc;

struct NoFiles;
impl PackageSource for NoFiles {
    fn read_file(&self, path: &str) -> std::io::Result<Vec<u8>> {
        Err(std::io::Error::new(std::io::ErrorKind::NotFound, path.to_owned()))
    }
}

const PRELUDE: &str = r##"
local function perr(f, ...)
  local ok, e = pcall(f, ...)
  assert(not ok, "expected an error")
  return (tostring(e):match("^[^\n]*"))
end
local function eq(got, want)
  assert(got == want, "got: " .. tostring(got) .. ", want: " .. tostring(want))
end
local function ROOT()
  return {child = {base = 7, extra = 11}, fixed = {2, 3}, children = {{base = 13, extra = 17}, {base = 19, extra = 23}},
    maybe = {base = 29, extra = 31}, choice = {type = "m6.Child", value = {base = 37, extra = 41}}, text = "synthetic"}
end
"##;

struct Env {
    vm: LuaVm,
    env: Table,
}
impl Env {
    /// A production startup VM: the port attached first (or not at all), then one package, a `game.types` over the
    /// same registry, and the root value's archive bytes as `ROOT_BYTES`.
    fn new(port: bool) -> Self {
        let registry = Rc::new(types::registry());
        let vm = LuaVm::new(HostContext::new(Phase::Startup, Side::Client, SourceVersion::Sf179)).unwrap();
        let buffer_resources = port.then(|| {
            Rc::new(BufferResources::new(registry.clone(), ValueObjects::new())) as Rc<dyn BufferResourcePort>
        });
        vm.attach(VmAttachments {
            typed_tables: None,
            buffer_resources,
        })
        .unwrap();
        let env = vm
            .add_package(Package::new("g08b", Rc::new(NoFiles)), |_, _, _| Ok(()))
            .unwrap();
        let lua = vm.lua();
        let target = SelectedTarget {
            stem: "enshrouded".into(),
            game_version: types::VERSION.into(),
        };
        let session = TypeSession::new(lua, registry.clone(), target);
        env.set("game", game::create(lua, &session).unwrap()).unwrap();
        let ty = registry.get_by_name(LookupKey::Qualified(types::ROOT)).unwrap();
        let bytes = types::root_value().to_bytes(&registry, ty).unwrap();
        env.set("ROOT_BYTES", lua.create_string(&bytes).unwrap()).unwrap();
        Self { vm, env }
    }
    fn run(&self, chunk: &str) {
        if let Err(error) = self
            .vm
            .lua()
            .load(format!("{PRELUDE}\n{chunk}"))
            .set_name("=g08b")
            .set_environment(self.env.clone())
            .exec()
        {
            panic!("{error}");
        }
    }
}

// FINAL: Buffer:write_resource (validate + clone, convert, encode, append at tail; zero results; head unchanged; a
// Type or a qualified name; no PATCH gate: no asset session or stage exists here) and Buffer:read_resource (head..tail,
// one result, head unchanged, the same bytes decode again).
#[test]
fn write_appends_the_encoded_value_and_read_decodes_head_to_tail_without_moving_head() {
    let e = Env::new(true);
    e.run(
        r##"
        local b = buffer.create()
        b:write_u8(0xAA)
        eq(select("#", b:write_resource("m6.Root", ROOT())), 0)
        eq(b:head(), 0); eq(b:tail(), 1 + #ROOT_BYTES)
        b:skip(1)
        eq(b:to_string(), ROOT_BYTES)
        local n = select("#", b:read_resource(game.types.get("m6.Root")))
        eq(n, 1)
        local v = b:read_resource("m6.Root")
        eq(b:head(), 1)
        eq(v.child.base, 7); eq(v.text, "synthetic"); eq(#v.children, 2); eq(v.choice.value.extra, 41)
        local w = b:read_resource("m6.Root")
        assert(not rawequal(v, w), "each read decodes a new value")
        -- a scalar type reads one number; an empty view of a struct type is a decoding error, raised
        local s = buffer.create()
        s:write_u32(0x01020304)
        eq(s:read_resource("m6.UInt32"), 0x01020304)
        eq(s:head(), 0)
        s:skip(4)
        assert(perr(s.read_resource, s, "m6.UInt32") ~= nil)
        -- a decoded value written back gives the same bytes
        local c = buffer.create()
        c:write_resource("m6.Root", v)
        eq(c:to_string(), ROOT_BYTES)
        "##,
    );
}

// The check order of both pins: closed (both), then writable (write only; a read-only buffer still reads), then the
// type argument, then the value; a failed write leaves the buffer as it was.
#[test]
fn closed_writable_type_and_value_precedence_and_a_failed_write_changes_nothing() {
    let e = Env::new(true);
    let read_only = {
        let mut b = Buffer::wrap(
            e.vm.lua()
                .load("return ROOT_BYTES")
                .set_environment(e.env.clone())
                .eval::<mlua::String>()
                .unwrap()
                .as_bytes()
                .to_vec(),
        );
        b.set_writable(false).unwrap();
        b
    };
    e.env.set("RO", read_only).unwrap();
    e.run(
        r##"
        local closed = buffer.create()
        closed:close()
        eq(perr(closed.read_resource, closed, "m6.Nope"), "buffer is closed")
        eq(perr(closed.write_resource, closed, "m6.Nope", {}), "buffer is closed")
        eq(perr(RO.write_resource, RO, "m6.Nope", {}), "buffer is not writable")
        eq(RO:read_resource("m6.Root").child.extra, 11)
        local b = buffer.create()
        b:write_u8(1)
        eq(perr(b.read_resource, b, "m6.Nope"), "type 'm6.Nope' not found")
        eq(perr(b.write_resource, b, "m6.Nope", ROOT()), "type 'm6.Nope' not found")
        eq(perr(b.write_resource, b, true, ROOT()), "bad argument #1 (string or Type expected, got boolean)")
        local t = ROOT(); t.children[2].extra = -1
        local err = perr(b.write_resource, b, "m6.Root", t)
        assert(err:find("at children.1.extra: expected `unsigned integer in range [0, 4294967295]`, got `-1`", 1, true), err)
        eq(b:tail(), 1); eq(b:head(), 0); eq(b:to_string(), "\1")
        "##,
    );
}

// The receiver stays mutably borrowed across validation (SF holds its `&mut Buffer` through the metamethod
// callbacks of the input table, here an array's __len): a metamethod that touches the same buffer gets the borrow error, and nothing was
// appended; another buffer is free.
#[test]
fn the_receiver_stays_borrowed_across_metamethod_callbacks() {
    let e = Env::new(true);
    e.run(
        r##"
        local b, other = buffer.create(), buffer.create()
        local t = ROOT()
        -- the validator calls an input array's __len (SF's call order); it touches both buffers
        t.children = setmetatable(t.children, {__len = function()
          other:write_u8(1)
          b:write_u8(9)
          return 2
        end})
        local err = perr(b.write_resource, b, "m6.Root", t)
        assert(err:find("borrow", 1, true), err)
        eq(b:tail(), 0)
        assert(other:tail() > 0, "the other buffer was written by the metamethod")
        t.children = setmetatable(ROOT().children, {__len = function() return b:tail() + 2 end})
        err = perr(b.write_resource, b, "m6.Root", t)
        assert(err:find("borrow", 1, true), err)
        eq(b:tail(), 0)
        -- the same value without the metamethod writes
        b:write_resource("m6.Root", ROOT())
        eq(b:to_string(), ROOT_BYTES)
        "##,
    );
}

// Without the port the core Buffer has no resource methods at all (no dummy success), and every ordinary method stays.
#[test]
fn without_the_port_the_methods_are_absent() {
    let e = Env::new(false);
    e.run(
        r##"
        local b = buffer.create()
        eq(b.read_resource, nil); eq(b.write_resource, nil)
        b:write_u8(3)
        eq(b:read_u8(), 3)
        "##,
    );
}

// Values from the port are the startup pass's typed wrappers: a decoded value edits (dirty) like a Resource's data and
// serializes again, and a Type of the same registry is accepted where a name is.
#[test]
fn decoded_values_are_the_pass_wrappers() {
    let e = Env::new(true);
    e.run(
        r##"
        local b = buffer.create()
        b:write_resource(game.types.get("m6.Root"), ROOT())
        local v = b:read_resource("m6.Root")
        v.child.extra = 404
        v.children[1].base = 5
        local c = buffer.create()
        c:write_resource("m6.Root", v)
        local w = c:read_resource("m6.Root")
        eq(w.child.extra, 404); eq(w.children[1].base, 5); eq(w.children[2].base, 19)
        eq(perr(c.write_resource, c, "m6.Child", v), "at .: expected `m6.Child`, got `m6.Root`")
        "##,
    );
}

