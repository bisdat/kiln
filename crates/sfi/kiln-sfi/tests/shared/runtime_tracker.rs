// SPDX-License-Identifier: MIT
//! The override tracker's bodies, shared by kiln-sfi's tests/runtime_21.rs and any other
//! test package that includes them. They drive `runtime.overrides.new` as
//! [\`OverrideTrackers\`] installs it, in a real Lua VM, with Lua targets over a fake storage and a
//! settable live session. Expected texts and rules are ShroudForge's (MIT, Copyright (c) 2026
//! bonsaibauer), identical in both pins: modules/mod-runtime/lua/ingame/overrides/
//! track_runtime_overrides.lua:1-210 and track_runtime_overrides.rs:1-268 (its own tests' cases
//! are kept here), plus Kiln's session rule for restores (tracker.lua "Kiln:" lines).
use kiln_sfi::runtime::overrides::OverrideTrackers;
use mlua::{Lua, Table};
use std::cell::Cell;
use std::rc::Rc;

/// A VM with \`first\` and \`second\` (two mods' \`runtime.overrides\`), a settable live
/// session (starts at 1) and the Lua helpers below.
pub struct Rig {
    pub lua: Lua,
    pub trackers: Rc<OverrideTrackers>,
    pub session: Rc<Cell<u64>>,
}

/// \`store[name]\` is the storage of world \`world()\`; \`target(name, opts)\` is a target over it
/// whose \`write\` is a compare-and-swap (\`snapshot.value\` must still be there) and is logged in
/// \`writes\`; \`busy\` refuses writes as Kiln's gate does; \`external(name, value)\` is a foreign
/// writer. Claims count \`acquired\` / \`released\`.
const HELPERS: &str = r#"
worlds, writes, busy, acquired, released = {}, {}, false, 0, 0
function world() local s = session() worlds[s] = worlds[s] or {} return worlds[s] end
function external(name, value) world()[name] = value end
function value(name) return world()[name] end
function target(name, opts)
    opts = opts or {}
    return {
        key = opts.key, shared = opts.shared,
        claim = opts.claim or {
            acquire = function()
                if opts.refuse then return false, opts.refuse end
                acquired = acquired + 1
                return true
            end,
            release = function() released = released + 1 end,
        },
        read = function()
            if opts.unreadable then return nil, opts.unreadable end
            return {value = world()[name]}
        end,
        normalize = opts.normalize,
        write = function(snapshot, v)
            if opts.raise then error(opts.raise) end
            if busy then return false, nil, "Scene gate is busy" end
            if opts.refusal then return false, nil, opts.refusal end
            local store = world()
            if store[name] ~= snapshot.value then return false, nil, "stale snapshot" end
            store[name] = v
            writes[#writes + 1] = name .. "=" .. tostring(v) .. "@" .. session()
            if opts.no_actual then return true, nil end
            return true, v, nil, opts.canonical
        end,
    }
end
function joined(t) return table.concat(t, ",") end
"#;

pub fn rig() -> Rig {
    let lua = Lua::new();
    let trackers = OverrideTrackers::new();
    let session = Rc::new(Cell::new(1u64));
    for owner in ["first", "second"] {
        let table = lua.create_table().unwrap();
        let live = session.clone();
        trackers
            .install(&lua, &table, owner, Rc::new(move || live.get()))
            .unwrap();
        lua.globals().set(owner, table).unwrap();
    }
    let live = session.clone();
    lua.globals()
        .set(
            "session",
            lua.create_function(move |_, ()| Ok(live.get())).unwrap(),
        )
        .unwrap();
    lua.load(HELPERS).exec().unwrap();
    Rig {
        lua,
        trackers,
        session,
    }
}

impl Rig {
    pub fn run(&self, source: &str) {
        self.lua
            .load(source)
            .exec()
            .unwrap_or_else(|e| panic!("{e}"));
    }
    pub fn get<T: mlua::FromLua>(&self, expression: &str) -> T {
        self.lua
            .load(format!("return {expression}"))
            .eval()
            .unwrap_or_else(|e| panic!("{expression}: {e}"))
    }
}

/// \`new\` gives independent trackers with ShroudForge's initial fields; ignores arguments; the
/// tracker is a plain mutable table.
pub fn new_trackers_start_with_shroudforge_s_fields() {
    let rig = rig();
    rig.run(
        r#"
        local t = first.new("ignored", 1)
        assert(type(t) == "table" and getmetatable(t) == nil)
        assert(next(t.owned) == nil and next(t.blocked) == nil and next(t.details) == nil)
        assert(t.writes == 0 and t.waiting_count == 0)
        assert(t.touched == nil and t.session == nil and t.fault == nil)
        local u = first.new()
        assert(u ~= t and u.owned ~= t.owned)
        t.owned.mine = true
        assert(u.owned.mine == nil, "public tables are per tracker")
        "#,
    );
}

/// Fresh, no-op and reused entries: a no-op releases the fresh claim and keeps no entry; a
/// write keeps the first baseline; desired sees that baseline on every later apply.
pub fn fresh_no_op_and_reused_entries() {
    let rig = rig();
    rig.run(
        r#"
        external("hp", 10)
        local t = first.new()
        t:begin_update()
        assert(t.session == 1)
        local ok, why, fresh = t:apply("hp", "health", target("hp"), function(s, base) return s.value end)
        assert(ok == true and why == nil and fresh == true, "no-op")
        assert(next(t.owned) == nil and acquired == 1 and released == 1)
        assert(t.touched.hp)
        ok, why, fresh = t:apply("hp", "health", target("hp"), function(s, base) return base * 2 end)
        assert(ok and fresh == true and value("hp") == 20 and t.writes == 1)
        local entry = t.owned.hp
        assert(entry.setting == "health" and entry.baseline == 10 and entry.applied == 20)
        local seen
        ok, why, fresh = t:apply("hp", "health", target("hp"), function(s, base) seen = base return base * 3 end)
        assert(ok and fresh == false and seen == 10 and value("hp") == 30)
        assert(t.owned.hp == entry and entry.baseline == 10 and entry.applied == 30 and t.writes == 2)
        assert(acquired == 3 and released == 1, "a reused entry keeps its first claim")
        "#,
    );
}

/// A desired callback that raises re-raises after releasing a fresh claim; one that returns
/// nil (and a normalize that does) refuses with the setting's prefix and waits.
pub fn a_desired_callback_that_raises_or_returns_nil() {
    let rig = rig();
    rig.run(
        r#"
        external("hp", 10)
        local t = first.new()
        t:begin_update()
        local ok, err = pcall(function()
            return t:apply("hp", "health", target("hp"), function() error("desired broke", 0) end)
        end)
        assert(not ok and tostring(err):find("desired broke", 1, true), tostring(err))
        assert(next(t.owned) == nil and released == 1 and t.fault == nil)
        local a, why, fresh = t:apply("hp", "health", target("hp"), function() return nil, "not yet" end)
        assert(a == false and why == "health: not yet" and fresh == nil)
        assert(t.waiting_count == 1 and t.details["health: not yet"] and released == 2)
        a, why = t:apply("hp", "health", target("hp", {normalize = function() return nil, "out of range" end}),
            function() return 5 end)
        assert(a == false and why == "health: out of range" and released == 3)
        ok, err = pcall(function()
            return t:apply("hp", "health", target("hp", {normalize = function() error("norm broke", 0) end}),
                function() return 5 end)
        end)
        assert(not ok and tostring(err):find("norm broke", 1, true) and released == 4)
        assert(value("hp") == 10 and #writes == 0)
        "#,
    );
}

/// An external write over an owned value releases the entry and blocks the key (never taking
/// the foreign value as a baseline); disabling the setting clears the block, and the next
/// apply binds again over the new value.
pub fn an_external_write_blocks_and_a_toggle_rebinds() {
    let rig = rig();
    rig.run(
        r#"
        external("hp", 10)
        local t = first.new()
        t:begin_update()
        assert(t:apply("hp", "health", target("hp"), function() return 20 end))
        external("hp", 15)
        t:begin_update()
        local ok, why = t:apply("hp", "health", target("hp"), function() return 20 end)
        local blocked = "health: an external change was preserved. Toggle off and on to bind again"
        assert(ok == false and why == blocked and t.blocked.hp.reason == blocked)
        assert(t.owned.hp == nil and value("hp") == 15 and released == 1)
        ok, why = t:apply("hp", "health", target("hp"), function() return 20 end)
        assert(ok == false and why == blocked and value("hp") == 15)
        t:restore_disabled(function(setting) return setting ~= "health" end)
        assert(t.blocked.hp == nil and value("hp") == 15, "clearing a block writes nothing")
        assert(t:apply("hp", "health", target("hp"), function(s, base) return base + 1 end))
        assert(t.owned.hp.baseline == 15 and value("hp") == 16)
        "#,
    );
}

/// Writer outcomes that leave the value unknown latch the tracker's fault: a raising writer, a
/// reason containing "uncertain" (any case), no confirmed value, and a value other than the
/// canonical one. A plain refusal only waits. A latched fault refuses before touching the key
/// (ShroudForge's fault-before-touched quirk), and restores only wait.
pub fn uncertain_outcomes_latch_and_refuse_before_touching() {
    let rig = rig();
    rig.run(
        r#"
        external("a", 1) external("b", 1) external("c", 1) external("d", 1) external("e", 1)
        local function fresh() local t = first.new() t:begin_update() return t end
        local t = fresh()
        local ok, why = t:apply("a", "A", target("a", {refusal = "Scene gate is busy"}), function() return 2 end)
        assert(ok == false and why == "Scene gate is busy" and t.fault == nil and t.owned.a == nil)
        t = fresh()
        ok, why = t:apply("a", "A", target("a", {raise = "engine fault"}), function() return 2 end)
        assert(ok == false and why:find("^Uncertain runtime write: .*engine fault") and t.fault == why)
        assert(t.owned.a.uncertain == true, "the entry is kept for inspection")
        ok, why = t:apply("b", "B", target("b"), function() return 2 end)
        assert(ok == false and why == t.fault and t.touched.b == nil, "fault before touched")
        t:restore_all()
        assert(value("a") == 1 and t.details[t.fault])
        t = fresh()
        ok, why = t:apply("c", "C", target("c", {refusal = "write outcome UNCERTAIN"}), function() return 2 end)
        assert(ok == false and t.fault == "write outcome UNCERTAIN" and t.owned.c.uncertain)
        t = fresh()
        ok, why = t:apply("d", "D", target("d", {no_actual = true}), function() return 2 end)
        assert(why == "Uncertain runtime write: no confirmed value was returned" and t.owned.d.uncertain)
        t = fresh()
        ok, why = t:apply("e", "E", target("e", {canonical = 3}), function() return 2 end)
        assert(why == "Original calculation returned an unexpected value for E" and t.fault == why)
        assert(t.owned.e.applied == 2 and not t.owned.e.uncertain)
        local cleared = t:reset()
        assert(cleared and t.fault == nil and next(t.owned) == nil and value("e") == 2,
            "reset forgets without restoring")
        "#,
    );
}

/// Restores: a confirmed baseline write releases; a busy gate keeps the entry, claim and
/// adapter for a later try; a value already at baseline, or changed by someone else, releases
/// without writing (the foreign value wins); an uncertain entry waits. A manual release of the
/// claim does not end ownership: the tracker still restores through its claim.
pub fn restores_write_only_over_their_own_last_value() {
    let rig = rig();
    rig.run(
        r#"
        for _, k in ipairs({"a", "b", "c", "d"}) do external(k, 1) end
        local t = first.new()
        t:begin_update()
        for _, k in ipairs({"a", "b", "c", "d"}) do assert(t:apply(k, k, target(k), function() return 5 end)) end
        external("b", 1)          -- someone put the baseline back
        external("c", 7)          -- a foreign writer
        busy = true
        local before = #writes
        t:restore_all()
        assert(#writes == before and t.owned.a and t.owned.d and t.owned.b == nil and t.owned.c == nil)
        assert(value("c") == 7 and value("b") == 1, "never over a foreign writer")
        assert(t.details["Scene gate is busy"] and t.waiting_count == 2)
        t.owned.a.claim:release()  -- a manual release is not a restore
        busy = false
        t:restore_all()
        assert(value("a") == 1 and value("d") == 1 and next(t.owned) == nil)
        assert(#writes == before + 2)
        "#,
    );
}

/// \`restore_disabled\` restores the disabled settings and clears their blocks; \`restore_unused\`
/// the keys not touched this update; \`reset\` refuses while shared values are owned; the summary
/// sorts details and puts the fault last.
pub fn restore_variants_reset_and_summary_order() {
    let rig = rig();
    rig.run(
        r#"
        for _, k in ipairs({"a", "b", "c"}) do external(k, 1) end
        local t = first.new()
        t:begin_update()
        assert(t:apply("a", "A", target("a"), function() return 2 end))
        assert(t:apply("b", "B", target("b"), function() return 2 end))
        assert(t:apply("c", "C", target("c", {shared = true}), function() return 2 end))
        assert(t:has_ownership() and t:has_shared_ownership())
        t:restore_disabled(function(setting) return setting ~= "A" end)
        assert(value("a") == 1 and value("b") == 2 and t.owned.a == nil)
        t:begin_update()
        assert(t:apply("c", "C", target("c", {shared = true}), function() return 2 end))
        t:restore_unused()
        assert(value("b") == 1 and value("c") == 2 and t.owned.b == nil and t.owned.c)
        local ok, why = t:reset()
        assert(ok == false and why == "Shared values must be restored before ownership can be reset")
        t:restore_all()
        assert(value("c") == 1 and not t:has_ownership())
        assert(t:reset() == true)
        t:begin_update()
        t:waiting("zeta") t:waiting("alpha") t:waiting("zeta")
        t.fault = "alpha"
        assert(t:summary("Runtime API cleanup is pending") ==
            "Runtime API cleanup is pending | writes=0 | alpha | zeta | alpha", t:summary("x"))
        assert(t.waiting_count == 3)
        t.fault = nil
        assert(t:summary("m") == "m | writes=0 | alpha | zeta")
        t:begin_update()
        assert(t:summary("m") == "m | writes=0")
        "#,
    );
}

/// Two trackers of one mod cannot own one key; another mod's tracker can (cross-mod exclusion is
/// the ownership store's, as in ShroudForge). A missing claim and a refused claim wait.
pub fn one_tracker_per_key_within_a_mod_and_claims_are_required() {
    let rig = rig();
    rig.run(
        r#"
        external("hp", 1)
        local t, u, v = first.new(), first.new(), second.new()
        for _, x in ipairs({t, u, v}) do x:begin_update() end
        assert(t:apply("hp", "health", target("hp"), function() return 2 end))
        local ok, why = u:apply("hp", "health", target("hp"), function() return 3 end)
        assert(ok == false and why == "health: another override tracker already owns this field")
        assert(u.touched.hp and value("hp") == 2)
        local fake = target("hp", {key = "other"})
        fake.claim = nil
        ok, why = u:apply("x", "X", fake, function() return 3 end)
        assert(ok == false and why == "X: an API ownership claim is required" and u.touched.other)
        ok, why = u:apply("y", "Y", target("hp", {refuse = "runtime.ecs.write is currently unavailable"}),
            function() return 3 end)
        assert(ok == false and why == "Y: runtime.ecs.write is currently unavailable")
        assert(v:apply("hp", "health", target("hp"), function(s) return s.value end))
        "#,
    );
}

/// ShroudForge's world rule in \`begin_update\`: a new nonzero session drops non-shared ownership
/// without writing, latches the fault while shared values are owned, and session 0 is not a
/// world end. Kiln's rule for restores called without \`begin_update\`: never into another nonzero
/// session (non-shared: dropped unwritten; shared: waits and is kept).
pub fn a_world_change_never_restores_into_the_new_session() {
    let rig = rig();
    rig.run(
        r#"
        local t = first.new()
        t:begin_update()
        external("hp", 1) external("e", 1)
        assert(t:apply("hp", "health", target("hp"), function() return 2 end))
        assert(t:apply("e", "E", target("e"), function() return 2 end))
        tracked = t
        local p = first.new()
        p:begin_update()
        external("p", 1)
        assert(p:apply("p", "P", target("p"), function() return 2 end))
        plain = p
        local z = first.new()
        z:begin_update()
        external("sh", 1)
        assert(z:apply("sh", "SH", target("sh", {shared = true}), function() return 2 end))
        zero = z
        "#,
    );
    rig.session.set(0);
    rig.run(
        r#"
        tracked:begin_update()
        assert(tracked.session == 1 and tracked.owned.hp, "0 is not a world end")
        local fresh = first.new()
        fresh:begin_update()
        assert(fresh.session == nil, "a fresh tracker learns no session from 0")
        zero:restore_all()   -- session 0 is no world end: ShroudForge's own restore rules apply
        local changed = "World changed before shared values were restored. Restart the game before writing again"
        assert(not zero.details[changed] and zero.owned.sh == nil, "released, not held for a world change")
        "#,
    );
    rig.session.set(2);
    rig.run(
        r#"
        external("hp", 2)   -- the new world happens to hold the applied value
        local r0, before = released, #writes
        tracked:restore_all()
        assert(#writes == before and value("hp") == 2, "never restored into session 2")
        assert(next(tracked.owned) == nil and released == r0 + 2, "non-shared ownership is dropped")
        plain:begin_update()
        assert(next(plain.owned) == nil and plain.fault == nil and plain.session == 2 and #writes == before,
            "begin_update in a new session resets without writing")
        "#,
    );
    rig.session.set(1);
    rig.run(
        r#"
        local t = first.new()
        t:begin_update()
        external("s", 1) external("n", 1)
        assert(t:apply("s", "S", target("s", {shared = true}), function() return 2 end))
        assert(t:apply("n", "N", target("n"), function() return 2 end))
        shared_tracker = t
        "#,
    );
    rig.session.set(3);
    rig.run(
        r#"
        local t = shared_tracker
        external("s", 2) external("n", 2)
        local before = #writes
        t:restore_all()
        local changed = "World changed before shared values were restored. Restart the game before writing again"
        assert(#writes == before and t.owned.s and t.details[changed], "shared waits and is kept")
        assert(t.owned.n == nil, "non-shared is dropped unwritten")
        t:begin_update()
        assert(t.fault == changed and t.owned.s and t.session == 3)
        local ok, why = t:apply("x", "X", target("x"), function() return 1 end)
        assert(ok == false and why == changed)
        "#,
    );
    rig.session.set(1);
    rig.run(
        r#"
        local t = first.new()
        t:begin_update()
        external("q", 1)
        assert(t:apply("q", "Q", target("q"), function() return 2 end))
        t:restore_all()
        assert(value("q") == 1 and next(t.owned) == nil, "the same session restores")
        "#,
    );
}

/// The cleanup port: \`begin_update\` + \`restore_all\` per tracker of that mod only, ShroudForge's
/// pending and inspection texts, a busy writer retried by a later cleanup, the same registry for a
/// rebuilt environment, nothing for an unknown mod.
pub fn cleanup_is_mod_scoped_retryable_and_answers_shroudforge_s_texts() {
    let rig = rig();
    rig.run(
        r#"
        external("one", 1) external("two", 1)
        local a = first.new() a:begin_update()
        assert(a:apply("one", "one", target("one"), function() return 2 end))
        local b = second.new() b:begin_update()
        assert(b:apply("two", "two", target("two"), function() return 2 end))
        busy = true
        "#,
    );
    let pending = rig.trackers.cleanup("first").unwrap_err();
    assert_eq!(
        pending,
        "Runtime API cleanup is pending | writes=0 | Scene gate is busy"
    );
    assert_eq!(rig.get::<i64>("value('one')"), 2);
    rig.run("busy = false");
    assert_eq!(rig.trackers.cleanup("first"), Ok(()));
    assert_eq!(rig.get::<i64>("value('one')"), 1);
    assert_eq!(
        rig.get::<i64>("value('two')"),
        2,
        "the other mod is untouched"
    );
    assert_eq!(rig.trackers.cleanup("second"), Ok(()));
    assert_eq!(rig.get::<i64>("value('two')"), 1);
    assert_eq!(rig.trackers.cleanup("unregistered"), Ok(()));
    // A rebuilt environment of the same mod keeps the registry (and its ownership).
    let again = rig.lua.create_table().unwrap();
    let live = rig.session.clone();
    rig.trackers
        .install(&rig.lua, &again, "first", Rc::new(move || live.get()))
        .unwrap();
    let first: Table = rig.get("first");
    assert_eq!(
        again.raw_get::<mlua::Function>("new").unwrap(),
        first.raw_get::<mlua::Function>("new").unwrap()
    );
    rig.run(
        r#"
        external("three", 1)
        local c = first.new() c:begin_update()
        assert(not c:apply("three", "three", target("three", {raise = "engine fault"}), function() return 2 end))
        "#,
    );
    let inspection = rig.trackers.cleanup("first").unwrap_err();
    assert!(
        inspection.starts_with("Runtime API cleanup requires inspection: Uncertain runtime write:")
            && inspection.contains("engine fault"),
        "{inspection}"
    );
    assert_eq!(rig.get::<i64>("value('three')"), 1, "nothing written");
}

/// Before any install and after the VM is gone, cleanup has nothing to do.
pub fn cleanup_without_a_vm_does_nothing() {
    let trackers = OverrideTrackers::new();
    assert_eq!(trackers.cleanup("first"), Ok(()));
    let rig = rig();
    let trackers = rig.trackers.clone();
    drop(rig);
    assert_eq!(trackers.cleanup("first"), Ok(()));
}
