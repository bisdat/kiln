// SPDX-License-Identifier: GPL-3.0-or-later
//! The SF-dialect startup pass's archive catalog and content half of `game.assets`, against ShroudForge's
//! eml-assets-types API rows. Each test names the rows it covers.
//! The expectations are ShroudForge 1.7.9's (lua/bindings/game/assets/{access_file_resources,access_file_content,
//! content,resource}.rs, game/content_id_casts.rs, session/asset_resources.rs), source-derived, plus FINAL's selected
//! ascending part order. Resource.data, create_resource and Buffer:read_resource/write_resource are the asset session (after the value wrappers).
//!
//! Every VM here runs over a real synthetic archive: a `StartupHost` over the synthetic fixture's baseline, its baseline
//! reader, and its one stage lent to the VM (`StagedArchive::lend`). No second writer, no path write.
#[path = "support/m6_fixture/mod.rs"]
mod fixture;

use fixture::{
    TestRoot,
    archive::{self, ArchiveFixture, STEM},
    types,
};
use kfc::{
    container::{KFCFile, KFCReader, KFCWriter},
    guid::{ContentHash, Guid, ResourceId},
    hash::fnv,
    reflection::{LookupKey, TypeIndex, TypeRegistry},
};
use kiln_eml::sf_startup::{
    SelectedTarget, TypeSession,
    game::{self, assets::AssetSession, resource::Resource},
};
use mlua::{AnyUserData, Lua, ObjectLike, Table};
use mod_loader::lua::startup_host::{LentStage, PreparedArchive, StartupHost};
use std::{
    collections::BTreeMap,
    fs,
    path::{Path, PathBuf},
    rc::Rc,
};

fn baseline(f: &ArchiveFixture) -> Rc<KFCFile> {
    for ext in ["kfc", "kfc_resources"] {
        fs::copy(
            f.game.join(format!("{STEM}.{ext}")),
            f.game.join(format!("{STEM}.{ext}.bak")),
        )
        .unwrap();
    }
    Rc::new(KFCFile::from_path(f.game.join(format!("{STEM}.kfc.bak")), false).unwrap())
}
fn guard(f: &ArchiveFixture) -> fs::File {
    let file = fs::File::create(f.game.join(".kiln-eml.lock")).unwrap();
    file.try_lock().unwrap();
    file
}

/// Raised error text (the first line of mlua's callback error).
const PERR: &str = r##"
local function perr(f, ...)
  local ok, e = pcall(f, ...)
  assert(not ok, "expected an error")
  return (tostring(e):match("^[^\n]*"))
end
local function eq(got, want)
  assert(got == want, "got: " .. tostring(got))
end
"##;

fn run(lua: &Lua, chunk: &str) {
    if let Err(error) = lua.load(format!("{PERR}\n{chunk}")).set_name("=g08").exec() {
        panic!("{error}");
    }
}

/// The `buffer` table (the core's Buffer) and two packages' `game` tables over ONE asset session.
fn install(lua: &Lua, types: &TypeSession, assets: &AssetSession) {
    lua.globals()
        .set("buffer", sfi_core::eml::buffer::create(lua).unwrap())
        .unwrap();
    for name in ["game", "game2"] {
        let table = game::create(lua, types).unwrap();
        game::install_assets(lua, &table, assets).unwrap();
        lua.globals().set(name, table).unwrap();
    }
}

/// Runs `body` with a PATCH VM over the fixture's baseline and its lent stage, then hands the lent stage back.
fn with_patch_vm<'g>(
    f: &ArchiveFixture,
    host: &mut StartupHost<'g>,
    body: impl FnOnce(&Lua, &AssetSession),
) -> LentStage<'g> {
    let base = baseline_of(f);
    let lent = host.begin_stage().unwrap().lend();
    let lua = Lua::new();
    let types = TypeSession::new(&lua, f.types.clone(), SelectedTarget::from_index(STEM, &base));
    let assets = AssetSession::new(&types, base, host.reference_reader().unwrap(), Some(lent.writer()));
    install(&lua, &types, &assets);
    body(&lua, &assets);
    lent
}
fn baseline_of(f: &ArchiveFixture) -> Rc<KFCFile> {
    Rc::new(KFCFile::from_path(f.game.join(format!("{STEM}.kfc.bak")), false).unwrap())
}

/// A PATCH VM over a fresh fixture (`body`), then the lent stage prepared and handed to `after`.
fn patch_vm(tag: &str, body: impl FnOnce(&Lua, &AssetSession), after: impl FnOnce(&PreparedArchive<'_>)) {
    let f = ArchiveFixture::new(tag);
    let base = baseline(&f);
    let guard = guard(&f);
    let mut host = StartupHost::open(f.game.as_std_path(), STEM, f.types.clone(), base, &guard).unwrap();
    let lent = with_patch_vm(&f, &mut host, body);
    after(&lent.prepare().unwrap());
}

const GUID: &str = archive::RESOURCE_GUID;

// FINAL: game.assets.get_resource_parts (dense, ascending part order under the compatibility rule; reference keys
// only; unsorted storage fixture), Resource.part.
#[test]
fn resource_parts_are_sorted_ascending_from_unsorted_storage() {
    let f = ArchiveFixture::new("g08-parts");
    let base = baseline(&f);
    // The fixture's precondition: storage order is NOT ascending, else this test would prove nothing.
    let stored: Vec<u32> = base
        .resources()
        .keys()
        .iter()
        .filter(|id| id.guid() == Guid::parse(GUID).unwrap())
        .map(|id| id.part_index())
        .collect();
    let mut sorted = stored.clone();
    sorted.sort();
    assert_ne!(stored, sorted, "fixture storage must be unsorted: {stored:?}");
    let guard = guard(&f);
    let mut host = StartupHost::open(f.game.as_std_path(), STEM, f.types.clone(), base, &guard).unwrap();
    drop(with_patch_vm(&f, &mut host, |lua, _| {
        run(
            lua,
            &format!(
                r##"
                local a, root = game.assets, game.types.get("m6.Root")
                local parts = a.get_resource_parts("{GUID}", root)
                assert(#parts == 3, #parts)
                for i, want in ipairs({{0, 3, 9}}) do assert(parts[i].part == want, i .. ":" .. parts[i].part) end
                -- a mutable plain table, a new one per call
                assert(getmetatable(parts) == nil and parts ~= a.get_resource_parts("{GUID}", "m6.Root"))
                parts[1] = nil
                assert(#a.get_resource_parts("{GUID}", root) == 3)
                -- another type with the same GUID: none; an unknown GUID: none
                assert(#a.get_resource_parts("{GUID}", "m6.Child") == 0)
                assert(#a.get_resource_parts("{}", root) == 0)
                "##,
                archive::CREATED_GUID
            ),
        );
    }));
}

// FINAL: game.assets.get_resource (cache, then reference; part nil -> 0; one nil when absent; Guid cast of string,
// Resource and Content; type errors), Resource.guid/type/part/source/loaded/session_id.
#[test]
fn get_resource_fields_casts_and_errors() {
    patch_vm(
        "g08-get",
        |lua, _assets| {
            run(
                lua,
                &format!(
                    r##"
                local a, root = game.assets, game.types.get("m6.Root")
                local r = a.get_resource("{GUID}", root)
                assert(r.part == 0 and math.type(r.part) == "integer")
                assert(r.guid == "{GUID}", r.guid)
                assert(rawequal(r.type, root))
                assert(r.source == "kfc-file" and r.loaded == false and r.session_id == nil)
                assert(a.get_resource("{GUID}", "m6.Root", nil).part == 0)
                assert(a.get_resource("{GUID}", "m6.Root", 9).part == 9)
                assert(a.get_resource("{GUID}", "m6.Root", "3").part == 3, "numeric strings coerce as u32")
                -- absent: exactly one nil (unknown part, other type, unknown GUID)
                assert(select("#", a.get_resource("{GUID}", root, 4)) == 1 and a.get_resource("{GUID}", root, 4) == nil)
                assert(a.get_resource("{GUID}", "m6.Child") == nil)
                assert(a.get_resource("{created}", root) == nil)
                -- a Resource casts to its base GUID
                assert(a.get_resource(r, root, 3).part == 3)
                -- read-only fields
                assert(perr(function() r.part = 1 end) ~= nil and r.part == 0)
                -- errors
                eq(perr(a.get_resource, "{GUID}", "m6.Missing"), "type 'm6.Missing' not found")
                eq(perr(a.get_resource, "not-a-guid", root), "bad argument #1 (guid expected, got string)")
                eq(perr(a.get_resource, 12, root), "bad argument #1 (guid expected, got integer)")
                eq(perr(a.get_resource, "{GUID}", true), "bad argument #2 (string or Type expected, got boolean)")
                assert(perr(a.get_resource, "{GUID}", root, -1) ~= nil)
                -- a number names a type by its decimal text, as SF's String cast does
                eq(perr(a.get_resource, "{GUID}", 42), "type '42' not found")
                "##,
                    created = archive::CREATED_GUID
                ),
            );
        },
        |_| {},
    );
}

// FINAL: game.assets.get_all_resources / get_resources_by_type / get_resource_types (reference catalog only, arguments
// ignored, Types from the session cache).
#[test]
fn enumeration_is_reference_only_and_ignores_arguments() {
    patch_vm(
        "g08-enum",
        |lua, _assets| {
            run(
                lua,
                r##"
            local a, root = game.assets, game.types.get("m6.Root")
            assert(#a.get_all_resources() == 3 and #a.get_all_resources(1, {}, "x") == 3)
            for _, r in ipairs(a.get_all_resources()) do assert(rawequal(r.type, root)) end
            assert(#a.get_resources_by_type(root) == 3 and #a.get_resources_by_type("m6.Root") == 3)
            assert(#a.get_resources_by_type("m6.Child") == 0)
            eq(perr(a.get_resources_by_type, "m6.Nope"), "type 'm6.Nope' not found")
            eq(perr(a.get_resources_by_type), "bad argument #1 (string or Type expected, got nil)")
            local ts = a.get_resource_types("ignored")
            assert(#ts == 1 and rawequal(ts[1], root) and rawequal(ts[1], game2.types.get("m6.Root")))
            "##,
            );
        },
        |_| {},
    );
}

// FINAL: game.assets.get_resource_types (types the registry does not know are filtered), get_all_resources (every
// reference key, whatever its type), Resource.type (nil when the registry has no matching type).
#[test]
fn unknown_archive_types_are_filtered_from_types_but_listed_as_resources() {
    let root = TestRoot::new("g08-filter");
    // The archive is written with a registry that has one more type; the session's registry does not know it.
    let registry = Rc::new(types::registry());
    let mut wider = types::registry().into_inner();
    let mut extra = wider[0].clone();
    extra.index = TypeIndex::new(wider.len());
    extra.qualified_name = "m6.Unknown".into();
    extra.qualified_hash = fnv("m6.Unknown");
    extra.impact_name = "M6_Unknown".into();
    extra.impact_hash = fnv("M6_Unknown");
    extra.name = "Unknown".into();
    extra.name_hash = fnv("Unknown");
    extra.internal_hash = 0xa600_00ff;
    wider.push(extra);
    // TypeRegistry::extend is private; its serde constructor builds the same indexes (as the fixture does).
    let wider: TypeRegistry =
        serde_json::from_value(serde_json::json!({"version": types::VERSION, "types": wider})).unwrap();
    let dir = root.0.join("game");
    fs::create_dir_all(&dir).unwrap();
    fs::File::create(dir.join(format!("{STEM}.kfc"))).unwrap();
    fs::File::create(dir.join(format!("{STEM}.kfc_resources"))).unwrap();
    let mut writer = KFCWriter::<Rc<KFCFile>, _>::new(&dir, STEM, Rc::new(wider), types::VERSION).unwrap();
    let ty = registry.get_by_name(LookupKey::Qualified(types::ROOT)).unwrap();
    let bytes = types::root_value().to_bytes(&registry, ty).unwrap();
    writer.write_resource(&archive::resource_id(0), &bytes).unwrap();
    let unknown = ResourceId::new(Guid::parse(GUID).unwrap(), fnv("m6.Unknown"), 0);
    assert!(registry.get_by_hash(LookupKey::Qualified(fnv("m6.Unknown"))).is_none());
    writer.write_resource(&unknown, &[0; 4]).unwrap();
    writer.finalize().unwrap();
    let index = Rc::new(KFCFile::from_path(dir.join(format!("{STEM}.kfc")), false).unwrap());
    let reader = KFCReader::new(&dir, STEM).unwrap().into_cursor().unwrap();
    let lua = Lua::new();
    let session = TypeSession::new(&lua, registry, SelectedTarget::from_index(STEM, &index));
    let assets = AssetSession::new(&session, index, reader, None);
    install(&lua, &session, &assets);
    run(
        &lua,
        r##"
        local a = game.assets
        local ts = a.get_resource_types()
        assert(#ts == 1 and ts[1].qualified_name == "m6.Root", #ts)
        local all = a.get_all_resources()
        assert(#all == 2)
        local untyped = 0
        for _, r in ipairs(all) do
          if r.type == nil then untyped = untyped + 1; assert(select("#", r.type) == 1) end
        end
        assert(untyped == 1)
        "##,
    );
}

// FINAL: game.assets.get_content (reference catalog ONLY, unlike EML), get_all_contents (reference only),
// create_content (content-derived identity, dedup, immediate staged write), Content:read_data (staged bytes first).
#[test]
fn new_content_is_readable_but_not_found_by_lookup() {
    let new = ContentHash::from_data(archive::NEW_CONTENT);
    patch_vm(
        "g08-newcontent",
        |lua, _assets| {
            run(
                lua,
                &format!(
                    r##"
                local a = game.assets
                local c = a.create_content(buffer.wrap("{text}"))
                assert(c.guid == "{new}" and c.size == {size})
                assert(a.get_content(c.guid) == nil and select("#", a.get_content(c)) == 1, "SF: reference only")
                assert(#a.get_all_contents() == 3)
                assert(c:read_data():to_string() == "{text}", "a new Content reads its own staged bytes")
                local again = a.create_content(buffer.wrap("{text}"))
                assert(again ~= c and again.guid == c.guid, "same identity, a new userdata")
                local other = game2.assets.create_content(buffer.wrap("{text}"))
                assert(other.guid == c.guid, "one session for every package")
                "##,
                    text = std::str::from_utf8(archive::NEW_CONTENT).unwrap(),
                    new = new,
                    size = archive::NEW_CONTENT.len(),
                ),
            );
        },
        |prepared| {
            // The write went to the stage at once, through the one lent writer: the prepared archive has it, once.
            assert_eq!(prepared.read_content(&new).unwrap().unwrap(), archive::NEW_CONTENT);
            assert_eq!(prepared.index().contents().len(), 4);
        },
    );
}

// The paired old-dialect witness: EML's get_content finds a content created in the same pass (has_content checks its
// new contents); SF's does not. Same fixture, existing discover -> runner -> mod-loader-lua run.
#[test]
fn eml_witness_get_content_finds_new_content() {
    let f = ArchiveFixture::new("g08-eml-witness");
    f.package(
        "witness",
        r##"
        local c = game.assets.create_content(buffer.wrap("m6 created content"))
        assert(game.assets.get_content(c.guid) ~= nil, "EML finds new content")
        assert(#game.assets.get_all_contents() == 3, "EML lists the reference catalog only")
        "##,
        &["patch"],
    );
    let out = f.run_packages(&["witness"]);
    assert_eq!(out.error.as_deref(), None, "{out:?}");
    assert_eq!(out.mods[0].state, "loaded", "{out:?}");
}

// FINAL: game.assets.get_content (string, table and userdata casts; one nil when absent; malformed raises),
// Content.guid/size/hash0/hash1/hash2, Content:read_data (owned read-only Buffer, a new one per call).
#[test]
fn reference_content_lookup_fields_and_read_only_buffer() {
    let blob = ContentHash::from_data(archive::BLOBS[1]);
    patch_vm(
        "g08-content",
        |lua, _assets| {
            run(
                lua,
                &format!(
                    r##"
                local a = game.assets
                local c = a.get_content("{blob}")
                assert(c.guid == "{blob}" and c.size == {size})
                assert(c.hash0 == {h0} and c.hash1 == {h1} and c.hash2 == {h2})
                assert(a.get_content({{size = {size}, hash0 = {h0}, hash1 = {h1}, hash2 = {h2}}}).guid == c.guid)
                assert(a.get_content(c).guid == c.guid, "userdata with the four words")
                assert(select("#", a.get_content("{none}")) == 1 and a.get_content("{none}") == nil)
                eq(perr(a.get_content, "zz"), "bad argument #1 (keen::ContentHash or guid expected, got string)")
                eq(perr(a.get_content, {{size = 1}}), "bad argument #1 (keen::ContentHash or guid expected, got table)")
                -- SF reads all four words before it checks any (content_id_casts.rs:75-83): a later read error wins
                local late = setmetatable({{}}, {{__index = function(_, k) if k == "hash2" then error("hash2 read", 0) end end}})
                eq(perr(a.get_content, late), "runtime error: hash2 read")
                eq(perr(a.get_content), "bad argument #1 (keen::ContentHash or guid expected, got nil)")
                assert(#a.get_all_contents() == 3)
                local b = c:read_data()
                assert(b:to_string() == "{text}" and b:head() == 0 and b:tail() == {size})
                eq(perr(b.write_u8, b, 1), "buffer is not writable")
                assert(b:read_u8() == {first} and b:head() == 1, "readable, own cursor")
                assert(c:read_data():head() == 0, "a new Buffer per call")
                eq(perr(c.read_data, "x"), "attempt to call method on invalid receiver (Content expected, got string)")
                "##,
                    blob = blob,
                    none = "00000000-0000-0000-0000-000000000001",
                    size = archive::BLOBS[1].len(),
                    h0 = blob.hash0(),
                    h1 = blob.hash1(),
                    h2 = blob.hash2(),
                    text = std::str::from_utf8(archive::BLOBS[1]).unwrap(),
                    first = archive::BLOBS[1][0],
                ),
            )
        },
        |_| {},
    );
}

// Content fields are read-only (mlua's default setter refusal).
#[test]
fn content_fields_are_read_only() {
    patch_vm(
        "g08-content-ro",
        |lua, _assets| {
            let content: AnyUserData = lua
                .load(format!(
                    "return game.assets.get_content('{}')",
                    ContentHash::from_data(archive::BLOBS[0])
                ))
                .eval()
                .unwrap();
            assert!(content.set("size", 0).is_err());
            assert!(content.set("guid", "x").is_err());
        },
        |_| {},
    );
}

// FINAL: game.assets.create_content (reads the unread view head..tail without advancing; read-only Buffers accepted;
// argument before PATCH; closed buffer error; returns a Content).
#[test]
fn create_content_reads_the_unread_view() {
    let tail = &archive::NEW_CONTENT[3..];
    patch_vm(
        "g08-view",
        |lua, _assets| {
            run(
                lua,
                &format!(
                    r##"
                local a = game.assets
                local b = buffer.wrap("{text}")
                b:head(3)
                local c = a.create_content(b)
                assert(c.guid == "{tail}" and b:head() == 3 and b:tail() == {len}, "no cursor moves")
                local ro = a.get_content(a.get_all_contents()[1].guid):read_data()
                assert(a.create_content(ro).size == ro:capacity(), "a read-only Buffer is accepted")
                eq(perr(a.create_content, "{text}"), "bad argument #1 (Buffer expected, got string)")
                eq(perr(a.create_content), "bad argument #1 (Buffer expected, got nil)")
                local closed = buffer.wrap("x"); closed:close()
                eq(perr(a.create_content, closed), "buffer is closed")
                local empty = a.create_content(buffer.create())
                assert(empty.size == 0 and empty:read_data():capacity() == 0)
                "##,
                    text = std::str::from_utf8(archive::NEW_CONTENT).unwrap(),
                    tail = ContentHash::from_data(tail),
                    len = archive::NEW_CONTENT.len(),
                ),
            );
        },
        |prepared| {
            assert_eq!(
                prepared.read_content(&ContentHash::from_data(tail)).unwrap().unwrap(),
                tail
            );
        },
    );
}

// FINAL: game.assets.create_content gate (requires PATCH; the argument is cast first), game.assets:archive_transaction
// (in-game / patch=false VMs have no writer).
#[test]
fn create_content_without_patch_is_refused_after_the_argument() {
    let f = ArchiveFixture::new("g08-nopatch");
    let base = baseline(&f);
    let guard = guard(&f);
    let host = StartupHost::open(f.game.as_std_path(), STEM, f.types.clone(), base.clone(), &guard).unwrap();
    let lua = Lua::new();
    let types = TypeSession::new(&lua, f.types.clone(), SelectedTarget::from_index(STEM, &base));
    let assets = AssetSession::new(&types, base, host.reference_reader().unwrap(), None);
    install(&lua, &types, &assets);
    run(
        &lua,
        r##"
        local a = game.assets
        eq(perr(a.create_content, 1), "bad argument #1 (Buffer expected, got integer)")
        eq(perr(a.create_content, buffer.wrap("x")), "game.assets.write is unavailable")
        assert(#a.get_all_contents() == 3 and #a.get_all_resources() == 3, "reads stay available")
        "##,
    );
}

// The single stage: once the lent stage is taken back (prepared) or dropped, the VM's handle refuses to write and no
// other writer exists; a failed write leaves no cached copy, so the next call tries the stage again.
#[test]
fn writes_after_the_stage_is_taken_back_are_refused() {
    let f = ArchiveFixture::new("g08-closed");
    let base = baseline(&f);
    let guard = guard(&f);
    let mut host = StartupHost::open(f.game.as_std_path(), STEM, f.types.clone(), base.clone(), &guard).unwrap();
    let lent = host.begin_stage().unwrap().lend();
    let lua = Lua::new();
    let types = TypeSession::new(&lua, f.types.clone(), SelectedTarget::from_index(STEM, &base));
    let assets = AssetSession::new(&types, base, host.reference_reader().unwrap(), Some(lent.writer()));
    install(&lua, &types, &assets);
    run(
        &lua,
        r##"assert(game.assets.create_content(buffer.wrap("before")).size == 6)"##,
    );
    let writer = lent.writer();
    assert!(writer.is_open());
    drop(lent);
    assert!(!writer.is_open(), "dropping the lent stage closes every handle");
    run(
        &lua,
        r##"
        local a = game.assets
        local e = perr(a.create_content, buffer.wrap("after"))
        assert(e == "the staged archive writer is closed", e)
        assert(perr(a.create_content, buffer.wrap("after")) == e, "nothing was cached by the failed write")
        local c = a.create_content(buffer.wrap("before"))
        assert(c:read_data():to_string() == "before", "content staged before the close is still cached")
        "##,
    );
}

/// Every file in the game folder except the stages and the install guard (the test's own lock; native Windows refuses
/// to read a locked range), cleanup records included: what a generation may leave behind outside its stage.
fn live_files(f: &ArchiveFixture) -> BTreeMap<String, Vec<u8>> {
    fn visit(root: &Path, path: &Path, out: &mut BTreeMap<String, Vec<u8>>) {
        for e in fs::read_dir(path).unwrap().flatten() {
            let name = e.file_name().to_string_lossy().into_owned();
            if name.starts_with(".kiln-eml-stage-") || name == ".kiln-eml.lock" {
                continue;
            }
            if e.file_type().unwrap().is_dir() {
                visit(root, &e.path(), out);
            } else {
                let key = e
                    .path()
                    .strip_prefix(root)
                    .unwrap()
                    .to_string_lossy()
                    .replace('\\', "/");
                out.insert(key, fs::read(e.path()).unwrap());
            }
        }
    }
    let mut out = BTreeMap::new();
    visit(f.game.as_std_path(), f.game.as_std_path(), &mut out);
    out
}
/// The one stage of a fresh fixture.
fn stage_dir(f: &ArchiveFixture) -> PathBuf {
    let stages: Vec<PathBuf> = fs::read_dir(&f.game)
        .unwrap()
        .flatten()
        .map(|e| e.path())
        .filter(|p| p.file_name().unwrap().to_string_lossy().starts_with(".kiln-eml-stage-"))
        .collect();
    assert_eq!(stages.len(), 1, "{stages:?}");
    stages.into_iter().next().unwrap()
}
/// A cleaned stage: gone, or kept empty on non-Windows builds (no removal through a handle there, never by path).
fn reclaimed(stage: &Path) -> bool {
    !stage.exists() || (cfg!(not(windows)) && fs::read_dir(stage).is_ok_and(|mut d| d.next().is_none()))
}

/// Writers that outlive the moment the stage is taken back: a coroutine suspended before it and a finalizer that runs
/// after it (kept reachable until `LATE_CHECK` drops it, so the collector cannot run it early).
const LATE_WRITERS: &str = r##"
late = {}
local function try(tag, text)
  local ok, e = pcall(game.assets.create_content, buffer.wrap(text))
  late[tag] = ok and "written" or tostring(e):match("^[^\n]*")
end
co = coroutine.create(function()
  coroutine.yield()
  try("coroutine", "from a coroutine")
end)
assert(coroutine.resume(co))
keep = setmetatable({}, {__gc = function() try("finalizer", "from a finalizer") end})
staged = game.assets.create_content(buffer.wrap("staged before"))
"##;
const LATE_CHECK: &str = r##"
assert(coroutine.resume(co))
keep = nil
collectgarbage()
collectgarbage()
eq(late.coroutine, "the staged archive writer is closed")
eq(late.finalizer, "the staged archive writer is closed")
eq(perr(game.assets.create_content, buffer.wrap("late")), "the staged archive writer is closed")
assert(staged:read_data():to_string() == "staged before", "the pass's own copy stays readable")
"##;

/// A PATCH VM over `host`'s lent stage that the TEST keeps (unlike `with_patch_vm`, the VM outlives the stage).
fn lent_vm<'g>(f: &ArchiveFixture, host: &mut StartupHost<'g>) -> (Lua, AssetSession, LentStage<'g>) {
    let base = baseline_of(f);
    let lent = host.begin_stage().unwrap().lend();
    let lua = Lua::new();
    let types = TypeSession::new(&lua, f.types.clone(), SelectedTarget::from_index(STEM, &base));
    let assets = AssetSession::new(&types, base, host.reference_reader().unwrap(), Some(lent.writer()));
    install(&lua, &types, &assets);
    run(&lua, LATE_WRITERS);
    (lua, assets, lent)
}

// The single stage's lifetime (review): dropping the lent stage aborts it AT ONCE, while the VM still holds its
// handles, a suspended coroutine and a pending finalizer. Nothing of the generation stays in the game folder (stage
// files, scratch record), and none of the VM's late writers reaches anything.
#[test]
fn a_dropped_lent_stage_is_aborted_at_once_while_the_vm_lives() {
    let f = ArchiveFixture::new("g08-lent-drop");
    let base = baseline(&f);
    let guard = guard(&f);
    let before = live_files(&f);
    let mut host = StartupHost::open(f.game.as_std_path(), STEM, f.types.clone(), base, &guard).unwrap();
    let (lua, _assets, lent) = lent_vm(&f, &mut host);
    let stage = stage_dir(&f);
    assert!(!reclaimed(&stage), "the stage holds the pass's bytes before the abort");
    drop(lent);
    assert!(
        reclaimed(&stage),
        "the stage was not aborted when the lent stage was dropped"
    );
    assert_eq!(
        live_files(&f),
        before,
        "the aborted generation left something in the game folder"
    );
    run(&lua, LATE_CHECK);
    assert!(
        reclaimed(&stage) && live_files(&f) == before,
        "a late Lua writer reached the game folder"
    );
}

// The writer is taken back from every handle: a VM that outlives preparation AND publication cannot write into the
// prepared or the published generation. The published archive holds what the pass staged, and no later Lua write
// changes a byte of it. The baseline reader is closed first, as the pass composition/kiln mode must do (SF's commit_assets does).
#[test]
fn a_lent_stage_taken_back_and_published_refuses_every_late_lua_write() {
    let f = ArchiveFixture::new("g08-lent-publish");
    let base = baseline(&f);
    let guard = guard(&f);
    let mut host = StartupHost::open(f.game.as_std_path(), STEM, f.types.clone(), base, &guard).unwrap();
    let (lua, assets, lent) = lent_vm(&f, &mut host);
    let staged = ContentHash::from_data(b"staged before");
    assets.close_reader();
    let prepared = lent.prepare().unwrap();
    assert_eq!(prepared.read_content(&staged).unwrap().unwrap(), b"staged before");
    let _published = prepared.publish().unwrap();
    let after = live_files(&f);
    run(&lua, LATE_CHECK);
    assert_eq!(live_files(&f), after, "a late Lua write changed the published archive");
    let live = archive::semantics(&f.game, STEM, &f.types).unwrap();
    assert_eq!(live.contents[&staged.to_string()], b"staged before");
    assert_eq!(
        live.contents.len(),
        4,
        "the late writers' contents never reached the archive"
    );
}

// The lent stage prepares exactly as a stage does: without the held install guard it is refused, and the refusal
// aborts it (nothing left), with the VM still holding its handles.
#[test]
fn a_lent_stage_is_not_prepared_without_its_held_guard_and_is_aborted() {
    let f = ArchiveFixture::new("g08-lent-guard");
    let base = baseline(&f);
    let guard = guard(&f);
    let before = live_files(&f);
    let mut host = StartupHost::open(f.game.as_std_path(), STEM, f.types.clone(), base, &guard).unwrap();
    let (lua, _assets, lent) = lent_vm(&f, &mut host);
    let stage = stage_dir(&f);
    let writer = lent.writer();
    guard.unlock().unwrap();
    assert!(lent.prepare().is_err(), "a released guard is no authority to prepare");
    assert!(!writer.is_open());
    assert!(reclaimed(&stage), "the refused stage was not aborted");
    assert_eq!(live_files(&f), before);
    run(&lua, LATE_CHECK);
    assert!(reclaimed(&stage) && live_files(&f) == before);
}

// Two packages' game.assets share one resource cache: the same ResourceInfo behind both Resource userdata.
#[test]
fn two_packages_share_one_resource_info() {
    patch_vm(
        "g08-shared",
        |lua, assets| {
            let a: AnyUserData = lua
                .load(format!("return game.assets.get_resource('{GUID}', 'm6.Root', 3)"))
                .eval()
                .unwrap();
            let b: AnyUserData = lua
                .load("return game2.assets.get_resources_by_type('m6.Root')[1]")
                .eval()
                .unwrap();
            let parts: Table = lua
                .load(format!("return game2.assets.get_resource_parts('{GUID}', 'm6.Root')"))
                .eval()
                .unwrap();
            let c: AnyUserData = parts.get(2).unwrap();
            let (a, c) = (a.borrow::<Resource>().unwrap(), c.borrow::<Resource>().unwrap());
            assert!(
                Rc::ptr_eq(a.info(), c.info()),
                "part 3 from two packages and two functions"
            );
            let b = b.borrow::<Resource>().unwrap();
            let again: AnyUserData = lua
                .load("return game.assets.get_all_resources()")
                .eval::<Table>()
                .unwrap()
                .sequence_values::<AnyUserData>()
                .map(Result::unwrap)
                .find(|r| r.borrow::<Resource>().unwrap().info().resource_id == b.info().resource_id)
                .unwrap();
            assert!(Rc::ptr_eq(again.borrow::<Resource>().unwrap().info(), b.info()));
            assert_eq!(
                assets.cached_resources(),
                3,
                "the cache holds each fetched resource once"
            );
        },
        |_| {},
    );
}

// The baseline reader closes before publication (SF closes its cursor in commit_assets); afterwards reference bytes
// are refused with a Lua error, staged bytes are still served from the session's copy.
#[test]
fn closing_the_reader_refuses_reference_reads_only() {
    let blob = ContentHash::from_data(archive::BLOBS[0]);
    patch_vm(
        "g08-reader",
        |lua, assets| {
            run(lua, r##"staged = game.assets.create_content(buffer.wrap("staged"))"##);
            let c = format!("game.assets.get_content('{blob}')");
            run(lua, &format!("held = {c}; assert(held:read_data():capacity() > 0)"));
            assets.close_reader();
            run(
                lua,
                r##"
            local e = perr(held.read_data, held)
            assert(e == "asset reader is closed after publication", e)
            assert(staged:read_data():to_string() == "staged")
            assert(#game.assets.get_all_contents() == 3, "metadata needs no reader")
            "##,
            );
        },
        |_| {},
    );
}

// Guid casts read Content userdata too (base GUID = the content hash's GUID form), as SF's Guid cast does.
#[test]
fn a_content_userdata_casts_to_its_guid() {
    patch_vm(
        "g08-guidcast",
        |lua, _assets| {
            run(
                lua,
                r##"
            local a = game.assets
            local c = a.get_all_contents()[1]
            assert(a.get_resource(c, "m6.Root") == nil)
            assert(#a.get_resource_parts(c, "m6.Root") == 0)
            "##,
            );
        },
        |_| {},
    );
}

// ---------------------------------------------------------------------------------------------------------------------
// Resource.data, create_resource and the dirty-resource serialization into the one stage.
// ---------------------------------------------------------------------------------------------------------------------

/// A Root value in a mod's table shape, and the same value as the archive bytes it must serialize to.
const NEW_ROOT: &str = r##"{child = {base = 1, extra = 2}, fixed = {3, 4}, children = {{base = 5, extra = 6}},
    maybe = {base = 9, extra = 10}, choice = {type = "m6.Child", value = {base = 7, extra = 8}}, text = "replaced"}"##;
fn new_root(edit: impl FnOnce(&mut kfc::resource::value::Value)) -> Vec<u8> {
    use kfc::resource::value::{Value, Variant};
    let mut value = types::structure([
        ("child", types::child(1, 2)),
        ("fixed", Value::Array(vec![Value::UInt(3), Value::UInt(4)])),
        ("children", Value::Array(vec![types::child(5, 6)])),
        ("maybe", types::child(9, 10)),
        (
            "choice",
            Value::Variant(Box::new(Variant {
                type_index: TypeIndex::new(2),
                value: types::child(7, 8).into_struct().unwrap(),
            })),
        ),
        ("text", Value::String("replaced".into())),
    ]);
    edit(&mut value);
    root_bytes(&value)
}
fn root_bytes(value: &kfc::resource::value::Value) -> Vec<u8> {
    let registry = types::registry();
    let ty = registry.get_by_name(LookupKey::Qualified(types::ROOT)).unwrap();
    value.to_bytes(&registry, ty).unwrap()
}
fn field<'a>(value: &'a mut kfc::resource::value::Value, path: &[&str]) -> &'a mut kfc::resource::value::Value {
    use kfc::resource::value::Value;
    let mut value = value;
    for key in path {
        value = match value {
            Value::Struct(fields) => fields.get_mut(*key).unwrap(),
            Value::Array(items) => &mut items[key.parse::<usize>().unwrap()],
            _ => panic!("no {key}"),
        };
    }
    value
}
fn root_id(guid: &str, part: u32) -> ResourceId {
    ResourceId::new(Guid::parse(guid).unwrap(), fnv(types::ROOT), part)
}
/// Every file in the one stage directory, by name (what the pass has written so far).
fn stage_files(f: &ArchiveFixture) -> BTreeMap<String, Vec<u8>> {
    fs::read_dir(stage_dir(f))
        .unwrap()
        .flatten()
        .filter(|e| e.file_type().unwrap().is_file())
        .map(|e| {
            (
                e.file_name().to_string_lossy().into_owned(),
                fs::read(e.path()).unwrap(),
            )
        })
        .collect()
}
/// A PATCH VM over the fixture's lent stage (no late writers); the test keeps the VM.
fn data_vm<'g>(f: &ArchiveFixture, host: &mut StartupHost<'g>) -> (Lua, AssetSession, LentStage<'g>) {
    let base = baseline_of(f);
    let lent = host.begin_stage().unwrap().lend();
    let lua = Lua::new();
    let types = TypeSession::new(&lua, f.types.clone(), SelectedTarget::from_index(STEM, &base));
    let assets = AssetSession::new(&types, base, host.reference_reader().unwrap(), Some(lent.writer()));
    install(&lua, &types, &assets);
    lua.globals().set("NEW_ROOT_SOURCE", NEW_ROOT).unwrap();
    run(&lua, "NEW = function() return load('return ' .. NEW_ROOT_SOURCE)() end");
    (lua, assets, lent)
}
/// A PATCH VM (`body`), whose dirty resources the session then serializes into the taken-back stage and prepares.
fn serialized_vm(
    tag: &str,
    body: impl FnOnce(&Lua, &AssetSession, &ArchiveFixture),
    after: impl FnOnce(&PreparedArchive<'_>, usize, &Lua),
) {
    let f = ArchiveFixture::new(tag);
    let base = baseline(&f);
    let guard = guard(&f);
    let mut host = StartupHost::open(f.game.as_std_path(), STEM, f.types.clone(), base, &guard).unwrap();
    let (lua, assets, lent) = data_vm(&f, &mut host);
    body(&lua, &assets, &f);
    let (prepared, applied) = assets.prepare(lent).unwrap();
    after(&prepared, applied, &lua);
}

// FINAL: Resource.data (lazy mapped value, one cached value per record for every package), nested edits mark the
// record dirty and reach the stage only when the session serializes (SF commit_asset_changes.rs:11-33), unedited and
// merely read records are not written.
#[test]
fn resource_data_is_lazy_shared_and_nested_edits_reach_the_stage_only_at_preparation() {
    serialized_vm(
        "g08b-nested",
        |lua, _assets, f| {
            let before = stage_files(f);
            run(
                lua,
                &format!(
                    r##"
                local a, root = game.assets, "m6.Root"
                local r = a.get_resource("{GUID}", root, 0)
                local d = r.data
                assert(rawequal(d, r.data) and rawequal(d, game2.assets.get_resource("{GUID}", root, 0).data))
                eq(d.child.base, 7); eq(d.child.extra, 11); eq(d.text, "synthetic"); eq(#d.children, 2)
                eq(d.choice.type, "m6.Child"); eq(d.choice.value.base, 37)
                d.child.extra = 404
                d.children[2].base = 505
                -- read, not edited
                eq(a.get_resource("{GUID}", root, 3).data.maybe.extra, 31)
                "##
                ),
            );
            assert_eq!(
                stage_files(f),
                before,
                "a nested edit wrote to the stage before serialization"
            );
        },
        |prepared, applied, _| {
            assert_eq!(applied, 1, "only the edited record is serialized");
            let mut want = types::root_value();
            *field(&mut want, &["child", "extra"]) = kfc::resource::value::Value::UInt(404);
            *field(&mut want, &["children", "1", "base"]) = kfc::resource::value::Value::UInt(505);
            assert_eq!(
                prepared.read_resource(&archive::resource_id(0)).unwrap().unwrap(),
                root_bytes(&want)
            );
            for part in [3, 9] {
                assert_eq!(
                    prepared.read_resource(&archive::resource_id(part)).unwrap().unwrap(),
                    root_bytes(&types::root_value())
                );
            }
        },
    );
}

// FINAL: Resource.data setter (PATCH, then the record's type, then validation and a deep copy; a failed validation
// leaves the record's value and dirtiness as they were), the replaced value serialized whole.
#[test]
fn explicit_data_replacement_validates_first_and_a_failure_keeps_the_record() {
    serialized_vm(
        "g08b-replace",
        |lua, _assets, _f| {
            run(
                lua,
                &format!(
                    r##"
                local a = game.assets
                local r = a.get_resource("{GUID}", "m6.Root", 3)
                local before = r.data
                local e = perr(function() local t = NEW(); t.child.base = "x"; r.data = t end)
                assert(e:find("at child.base: expected `unsigned integer in range [0, 4294967295]`, got `x`", 1, true), e)
                assert(rawequal(r.data, before), "a failed replacement keeps the cached value")
                assert(perr(function() r.data = 5 end):find("expected `m6.Root`", 1, true))
                local untouched = a.get_resource("{GUID}", "m6.Root", 9)
                eq(untouched.data.text, "synthetic")
                assert(perr(function() local t = NEW(); t.fixed = {{1}}; untouched.data = t end) ~= nil)
                local t = NEW()
                r.data = t
                assert(not rawequal(r.data, before) and not rawequal(r.data, t))
                t.text = "edited after the assignment"
                eq(r.data.text, "replaced"); eq(r.data.child.base, 1)
                "##
                ),
            );
        },
        |prepared, applied, _| {
            assert_eq!(applied, 1, "a failed replacement does not make its record dirty");
            assert_eq!(
                prepared.read_resource(&archive::resource_id(3)).unwrap().unwrap(),
                new_root(|_| {})
            );
            assert_eq!(
                prepared.read_resource(&archive::resource_id(9)).unwrap().unwrap(),
                root_bytes(&types::root_value())
            );
        },
    );
}

// FINAL: game.assets.create_resource (explicit overload: GUID and part, the part required once a GUID is given;
// random overload: a new v4 GUID, part 0), new records are `asset-transaction` with no session, found by
// get_resource from every package but never enumerated (get_all_resources, get_resources_by_type and
// get_resource_parts list reference keys only, FINAL archive_parts_sorted's staged-only part 7), the selected
// duplicate rule (reference catalog AND cache, an archive key never fetched included; SF checks its cache only), its
// order (duplicate before type and validation), and a validation failure caches nothing.
#[test]
fn create_resource_identities_duplicates_and_reference_only_enumeration() {
    let created = archive::CREATED_GUID;
    const OTHER: &str = "0f0e0d0c-0b0a-0908-0706-050403020100";
    serialized_vm(
        "g08b-create",
        |lua, _assets, _f| {
            run(
                lua,
                &format!(
                    r##"
                local a, root = game.assets, game.types.get("m6.Root")
                -- an archive key nobody fetched yet (the cache is empty): refused, unlike SF
                eq(perr(a.create_resource, NEW(), root, "{GUID}", 9), "resource with GUID {GUID} already exists")
                local input = NEW()
                local r = a.create_resource(input, root, "{created}", 0)
                eq(r.source, "asset-transaction"); eq(r.loaded, false); eq(r.part, 0); eq(r.guid, "{created}")
                assert(rawequal(r.type, root) and r.session_id == nil)
                input.text = "the record holds a copy"
                eq(r.data.text, "replaced")
                local again = game2.assets.get_resource("{created}", "m6.Root")
                assert(again and rawequal(again.data, r.data) and again.source == "asset-transaction")
                eq(#a.get_all_resources(), 3); eq(#a.get_resources_by_type(root), 3)
                eq(#a.get_resource_parts("{created}", root), 0)
                -- a staged-only part of an archive GUID
                eq(a.create_resource(NEW(), "m6.Root", "{GUID}", 7).part, 7)
                local parts = a.get_resource_parts("{GUID}", root)
                eq(#parts, 3); eq(parts[1].part, 0); eq(parts[2].part, 3); eq(parts[3].part, 9)
                eq(a.get_resource("{GUID}", root, 7).source, "asset-transaction")
                -- duplicates: a cached archive key and a created one; checked before type and value
                eq(perr(a.create_resource, NEW(), root, "{GUID}", 9), "resource with GUID {GUID} already exists")
                eq(perr(a.create_resource, NEW(), root, "{created}", 0), "resource with GUID {created} already exists")
                eq(perr(a.create_resource, {{child = 1}}, root, "{GUID}", 3), "resource with GUID {GUID} already exists")
                -- the explicit overload needs its part
                assert(perr(a.create_resource, NEW(), root, "{other}"):find("bad argument #4", 1, true))
                eq(perr(a.create_resource, NEW(), "m6.Nope", "{other}", 0), "type 'm6.Nope' not found")
                eq(perr(a.create_resource, NEW(), root, "not-a-guid", 0), "bad argument #3 (guid expected, got string)")
                -- validation, and nothing cached by its failure
                local t = NEW(); t.child.base = "x"
                local e = perr(a.create_resource, t, root, "{other}", 0)
                assert(e:find("at child.base: expected", 1, true), e)
                eq(a.get_resource("{other}", root, 0), nil)
                -- a random identity
                local x = a.create_resource(NEW(), root)
                eq(x.part, 0); eq(x.source, "asset-transaction")
                assert(#x.guid == 36 and x.guid ~= "{GUID}" and x.guid ~= "{created}", x.guid)
                assert(a.get_resource(x.guid, root) ~= nil)
                assert(a.create_resource(NEW(), "m6.Root").guid ~= x.guid)
                eq(#a.get_all_resources(), 3)
                X1 = x.guid
                -- a created record edits like any other
                r.data.child.extra = 99
                "##,
                    other = OTHER
                ),
            );
        },
        |prepared, applied, lua| {
            assert_eq!(applied, 4, "four created records");
            assert_eq!(
                prepared.read_resource(&root_id(created, 0)).unwrap().unwrap(),
                new_root(|v| *field(v, &["child", "extra"]) = kfc::resource::value::Value::UInt(99))
            );
            assert_eq!(
                prepared.read_resource(&root_id(GUID, 7)).unwrap().unwrap(),
                new_root(|_| {})
            );
            let x: String = lua.globals().get("X1").unwrap();
            assert_eq!(
                prepared.read_resource(&root_id(&x, 0)).unwrap().unwrap(),
                new_root(|_| {})
            );
            assert!(prepared.read_resource(&root_id(OTHER, 0)).unwrap().is_none());
            assert_eq!(prepared.index().resources().len(), 7);
        },
    );
}

// FINAL: create_resource and the Resource.data setter require PATCH, refused after the arguments are cast but before
// the value is validated; nested edits and reads need no PATCH (a PATCH-less VM is never serialized).
#[test]
fn create_resource_and_data_replacement_without_patch_are_refused_nested_edits_are_not() {
    let f = ArchiveFixture::new("g08b-nopatch");
    let base = baseline(&f);
    let guard = guard(&f);
    let host = StartupHost::open(f.game.as_std_path(), STEM, f.types.clone(), base.clone(), &guard).unwrap();
    let lua = Lua::new();
    let types = TypeSession::new(&lua, f.types.clone(), SelectedTarget::from_index(STEM, &base));
    let assets = AssetSession::new(&types, base, host.reference_reader().unwrap(), None);
    install(&lua, &types, &assets);
    run(
        &lua,
        &format!(
            r##"
        local a = game.assets
        eq(perr(a.create_resource, {{}}, "m6.Nope"), "type 'm6.Nope' not found")
        eq(perr(a.create_resource, {{child = "not validated"}}, "m6.Root"), "game.assets.write is unavailable")
        eq(perr(a.create_resource, {{}}, "m6.Root", "{GUID}", 0), "game.assets.write is unavailable")
        local r = a.get_resource("{GUID}", "m6.Root", 0)
        eq(perr(function() r.data = 5 end), "game.assets.write is unavailable")
        r.data.child.extra = 12
        eq(r.data.child.extra, 12)
        eq(#a.get_all_resources(), 3)
        "##
        ),
    );
}

// The single writer at serialization: the session takes the writer back from every Lua handle BEFORE it serializes
// (SF takes its writer first, commit_asset_changes.rs:12; the serializer itself creates no Lua value, so no finalizer
// runs inside it), and a finalizer or coroutine that runs after that cannot slip a content, a resource or a
// replacement into the generation: each is refused with the closed writer's text.
#[test]
fn serialization_takes_the_writer_back_first_late_writers_change_nothing() {
    let f = ArchiveFixture::new("g08b-late");
    let base = baseline(&f);
    let guard = guard(&f);
    let mut host = StartupHost::open(f.game.as_std_path(), STEM, f.types.clone(), base, &guard).unwrap();
    let (lua, assets, lent) = data_vm(&f, &mut host);
    run(
        &lua,
        &format!(
            r##"
        late = {{}}
        local function try(tag, f)
          local ok, e = pcall(f)
          late[tag] = ok and "written" or tostring(e):match("^[^\n]*")
        end
        local a = game.assets
        local r = a.get_resource("{GUID}", "m6.Root", 0)
        r.data.child.extra = 1
        -- finalizers kept reachable until the stage is taken back, then collected
        keep = {{}}
        for i = 1, 200 do
          keep[i] = setmetatable({{}}, {{__gc = function()
            try("content", function() a.create_content(buffer.wrap("late " .. i)) end)
            try("resource", function() a.create_resource(NEW(), "m6.Root") end)
            try("replace", function() a.get_resource("{GUID}", "m6.Root", 9).data = NEW() end)
          end}})
        end
        co = coroutine.create(function()
          coroutine.yield()
          try("coroutine", function() a.create_content(buffer.wrap("from a coroutine")) end)
        end)
        assert(coroutine.resume(co))
        "##
        ),
    );
    let (prepared, applied) = assets.prepare(lent).unwrap();
    run(
        &lua,
        r##"
        assert(coroutine.resume(co))
        keep = nil
        collectgarbage(); collectgarbage()
        for _, tag in ipairs({"content", "resource", "replace", "coroutine"}) do
          eq(late[tag], "the staged archive writer is closed")
        end
        "##,
    );
    assert_eq!(applied, 1);
    assert_eq!(
        prepared.index().contents().len(),
        3,
        "no late content reached the generation"
    );
    assert_eq!(
        prepared.index().resources().len(),
        3,
        "no late resource reached the generation"
    );
    assert_eq!(
        prepared.read_resource(&archive::resource_id(9)).unwrap().unwrap(),
        root_bytes(&types::root_value())
    );
}

/// A registry with a recursive type behind a scalar (m6.DeepRoot {mark, node: m6.Node {kids: m6.NodeList}, text}) and
/// resources of crafted bytes: the hostile shapes of unread decoded data that a review found unbounded in kfc's
/// `Value::from` (a node that is its own child, children that share their grandchildren, a stored count of 2^32-1),
/// next to well-formed ones. All synthetic.
mod deep {
    use super::*;
    use kfc::{
        reflection::{PrimitiveType as P, StructFieldMetadata, TypeFlags as F, TypeMetadata},
        resource::value::Value,
    };
    pub const ROOT: &str = "m6.DeepRoot";

    fn metadata(index: usize, name: &str, primitive_type: P, size: u32) -> TypeMetadata {
        let qualified_name = format!("m6.{name}");
        TypeMetadata {
            index: TypeIndex::new(index),
            name: name.into(),
            impact_name: format!("M6_{name}"),
            qualified_hash: fnv(&qualified_name),
            internal_hash: 0xa700_0000 + index as u32,
            name_hash: fnv(name),
            impact_hash: fnv(&format!("M6_{name}")),
            qualified_name,
            namespace: vec!["m6".into()],
            inner_type: None,
            size,
            alignment: 4,
            element_alignment: 4,
            field_count: 0,
            primitive_type,
            flags: F::NONE,
            struct_fields: Default::default(),
            enum_fields: Default::default(),
            default_value: None,
            attributes: Default::default(),
        }
    }
    fn fields(t: &mut TypeMetadata, specs: &[(&str, usize, u64)]) {
        for &(name, index, data_offset) in specs {
            t.struct_fields.insert(
                name.into(),
                StructFieldMetadata {
                    name: name.into(),
                    r#type: TypeIndex::new(index),
                    data_offset,
                    attributes: Default::default(),
                },
            );
        }
        t.field_count = specs.len() as u32;
    }
    pub fn registry() -> TypeRegistry {
        of(sane())
    }
    fn of(types: Vec<TypeMetadata>) -> TypeRegistry {
        serde_json::from_value(serde_json::json!({"version": types::VERSION, "types": types})).unwrap()
    }
    fn sane() -> Vec<TypeMetadata> {
        let mut t = vec![
            metadata(0, "UInt32", P::UInt32, 4),
            metadata(1, "Node", P::Struct, 8),
            metadata(2, "NodeList", P::BlobArray, 8),
            metadata(3, "Text", P::BlobString, 8),
            metadata(4, "DeepRoot", P::Struct, 20),
        ];
        fields(&mut t[1], &[("kids", 2, 0)]);
        t[1].flags = F::HAS_BLOB_ARRAY;
        t[2].inner_type = Some(TypeIndex::new(1));
        t[2].flags = F::HAS_BLOB_ARRAY;
        t[3].flags = F::HAS_BLOB_STRING;
        fields(&mut t[4], &[("mark", 0, 0), ("node", 1, 4), ("text", 3, 12)]);
        t[4].flags = F::HAS_BLOB_ARRAY | F::HAS_BLOB_STRING;
        t
    }
    pub const CYCLE_ROOT: &str = "m6.CycleRoot";
    pub const LOOP_ROOT: &str = "m6.LoopRoot";
    pub const VARIANT_ROOT: &str = "m6.VariantRoot";
    /// The deep registry plus two faulty type chains, each held by a struct field below an otherwise sane root
    /// ({mark, holder}): a struct whose bases are a cycle (m6.CycleA, m6.CycleB: kfc walks the bases to list its fields),
    /// the same struct as a variant's value (its type comes from the bytes) and an optional of itself (m6.Loop: kfc maps
    /// an optional eagerly, one recursion per offset in the bytes).
    pub fn faulty_registry() -> TypeRegistry {
        let mut t = sane();
        t.extend([
            metadata(5, "CycleA", P::Struct, 0),
            metadata(6, "CycleB", P::Struct, 0),
            metadata(7, "CycleHolder", P::Struct, 0),
            metadata(8, "CycleRoot", P::Struct, 4),
            metadata(9, "Loop", P::BlobOptional, 8),
            metadata(10, "LoopHolder", P::Struct, 8),
            metadata(11, "LoopRoot", P::Struct, 12),
            metadata(12, "CycleVariant", P::BlobVariant, 12),
            metadata(13, "VariantHolder", P::Struct, 12),
            metadata(14, "VariantRoot", P::Struct, 16),
        ]);
        t[5].inner_type = Some(TypeIndex::new(6));
        t[6].inner_type = Some(TypeIndex::new(5));
        fields(&mut t[7], &[("cycle", 5, 0)]);
        fields(&mut t[8], &[("mark", 0, 0), ("holder", 7, 4)]);
        t[9].inner_type = Some(TypeIndex::new(9));
        t[9].flags = F::HAS_BLOB_OPTIONAL;
        fields(&mut t[10], &[("looped", 9, 0)]);
        t[10].flags = F::HAS_BLOB_OPTIONAL;
        fields(&mut t[11], &[("mark", 0, 0), ("holder", 10, 4)]);
        t[11].flags = F::HAS_BLOB_OPTIONAL;
        // a variant over a sane base whose bytes name m6.CycleA as the value's type
        t[12].inner_type = Some(TypeIndex::new(1));
        t[12].flags = F::HAS_BLOB_VARIANT;
        fields(&mut t[13], &[("choice", 12, 0)]);
        t[13].flags = F::HAS_BLOB_VARIANT;
        fields(&mut t[14], &[("mark", 0, 0), ("holder", 13, 4)]);
        t[14].flags = F::HAS_BLOB_VARIANT;
        of(t)
    }
    pub fn guid(n: u8) -> String {
        format!("00000000-0000-0000-0000-0000000000{n:02x}")
    }
    pub fn id(n: u8) -> ResourceId {
        id_of(ROOT, n)
    }
    pub fn id_of(root: &str, n: u8) -> ResourceId {
        ResourceId::new(Guid::parse(&guid(n)).unwrap(), fnv(root), 0)
    }
    pub fn words(words: &[u32]) -> Vec<u8> {
        words.iter().flat_map(|w| w.to_le_bytes()).collect()
    }
    /// A chain of `depth` nodes, each the only child of the one before (no shared bytes).
    pub fn chain(depth: usize) -> Value {
        let mut node = types::structure([("kids", Value::Array(vec![]))]);
        for _ in 1..depth {
            node = types::structure([("kids", Value::Array(vec![node]))]);
        }
        node
    }
    pub fn root(mark: u64, node: Value, text: &str) -> Value {
        types::structure([
            ("mark", Value::UInt(mark)),
            ("node", node),
            ("text", Value::String(text.into())),
        ])
    }
    pub fn bytes(value: &Value) -> Vec<u8> {
        let registry = registry();
        value
            .to_bytes(&registry, registry.get_by_name(LookupKey::Qualified(ROOT)).unwrap())
            .unwrap()
    }
    /// The node at 4 is its own child: its kids array (relative offset 0, one element) starts at itself.
    pub fn looped() -> Vec<u8> {
        words(&[1, 0, 1, 0, 0])
    }
    /// `levels` pairs of nodes after the root, both nodes of a pair pointing at the same next pair: 2^levels values in
    /// 20 + 16 * levels bytes, never deeper than `levels` + 1 nodes.
    pub fn shared(levels: u32) -> Vec<u8> {
        let mut out = words(&[1, 16, 2, 0, 0]);
        for level in 0..levels {
            let last = level + 1 == levels;
            out.extend(words(&[16, if last { 0 } else { 2 }, 8, if last { 0 } else { 2 }]));
        }
        out
    }
    /// A kids array whose stored count is 2^32-1 and whose elements lie past the end of the bytes.
    pub fn huge() -> Vec<u8> {
        words(&[1, 1000, u32::MAX, 0, 0])
    }
    /// The deep registry plus arrays for the stored-count check (eml-array-count): m6.Words (BlobArray of UInt32),
    /// m6.ZeroWords and m6.Empties (BlobArrays of a UInt32 and of a struct registered with size 0, as only a faulty
    /// registry has them), m6.Fixed3 (a StaticArray of 3 UInt32) and m6.FixedMany (a StaticArray claiming a million
    /// UInt32 in 8 bytes).
    pub fn counted_registry() -> TypeRegistry {
        let mut t = sane();
        t.extend([
            metadata(5, "Words", P::BlobArray, 8),
            metadata(6, "ZeroWord", P::UInt32, 0),
            metadata(7, "ZeroWords", P::BlobArray, 8),
            metadata(8, "Empty", P::Struct, 0),
            metadata(9, "Empties", P::BlobArray, 8),
            metadata(10, "Fixed3", P::StaticArray, 12),
            metadata(11, "FixedMany", P::StaticArray, 8),
        ]);
        for (array, element) in [(5, 0), (7, 6), (9, 8), (10, 0), (11, 0)] {
            t[array].inner_type = Some(TypeIndex::new(element));
        }
        for array in [5, 7, 9] {
            t[array].flags = F::HAS_BLOB_ARRAY;
        }
        t[10].field_count = 3;
        t[11].field_count = 1_000_000;
        of(t)
    }
    /// kfc's mapping of `bytes` as the array type `m6.<name>`: its length, or the mapping error's text.
    pub fn array_len(registry: &Rc<TypeRegistry>, name: &str, bytes: &[u8]) -> Result<usize, String> {
        use kfc::resource::mapped::MappedValue;
        let ty = registry.get_by_name(LookupKey::Qualified(&format!("m6.{name}"))).unwrap();
        let data: Rc<[u8]> = bytes.into();
        match MappedValue::from_bytes(registry, ty, &data) {
            Ok(MappedValue::Array(array)) => Ok(array.len()),
            Ok(other) => panic!("m6.{name} mapped as {other:?}"),
            Err(error) => Err(error.to_string()),
        }
    }
    /// A text whose bytes are not UTF-8.
    pub fn bad_text() -> Vec<u8> {
        let mut out = words(&[1, 0, 0, 8, 2]);
        out.extend([0xff, 0xfe, 0, 0]);
        out
    }
}

/// An ArchiveFixture-shaped game folder over the deep registry, holding `resources` (no contents).
fn deep_fixture(tag: &str, resources: &[(ResourceId, Vec<u8>)]) -> ArchiveFixture {
    deep_fixture_in(tag, deep::registry(), resources)
}
fn deep_fixture_in(tag: &str, registry: TypeRegistry, resources: &[(ResourceId, Vec<u8>)]) -> ArchiveFixture {
    let root = TestRoot::new(tag);
    let game = root.0.join("game");
    fs::create_dir_all(&game).unwrap();
    fs::File::create(game.join(format!("{STEM}.kfc"))).unwrap();
    fs::File::create(game.join(format!("{STEM}.kfc_resources"))).unwrap();
    let types = Rc::new(registry);
    let mut writer = KFCWriter::<Rc<KFCFile>, _>::new(&game, STEM, types.clone(), types::VERSION).unwrap();
    for (id, bytes) in resources {
        writer.write_resource(id, bytes).unwrap();
    }
    writer.finalize().unwrap();
    ArchiveFixture {
        root,
        game,
        types,
        original: archive::PhysicalSet(BTreeMap::new()),
    }
}

/// Marks resource `n` dirty through a scalar field (its node stays unread), serializes, and expects the attempt to fail
/// with `want`, the stage aborted and the game folder untouched.
fn unread_data_fails_serialization(tag: &str, bytes: Vec<u8>, want: &str) {
    unread_root_fails_serialization(tag, deep::registry(), deep::ROOT, bytes, want);
}
/// The same over `registry`, for a resource of type `root` (whose first field is the scalar `mark`).
fn unread_root_fails_serialization(tag: &str, registry: TypeRegistry, root: &str, bytes: Vec<u8>, want: &str) {
    let f = deep_fixture_in(tag, registry, &[(deep::id_of(root, 1), bytes)]);
    let base = baseline(&f);
    let guard = guard(&f);
    let before = live_files(&f);
    let mut host = StartupHost::open(f.game.as_std_path(), STEM, f.types.clone(), base, &guard).unwrap();
    let (lua, assets, lent) = data_vm(&f, &mut host);
    let stage = stage_dir(&f);
    run(
        &lua,
        &format!(
            r##"
        local r = game.assets.get_resource("{}", "{root}")
        eq(r.data.mark, 1)
        r.data.mark = 2
        "##,
            deep::guid(1)
        ),
    );
    let started = std::time::Instant::now();
    let error = match assets.prepare(lent) {
        Ok(_) => panic!("{tag}: the hostile resource serialized"),
        Err(error) => format!("{error:#}"),
    };
    assert!(error.contains(want), "{tag}: {error}");
    assert!(
        error.contains(&deep::guid(1)),
        "{tag}: the error names the resource: {error}"
    );
    assert!(
        started.elapsed() < std::time::Duration::from_secs(30),
        "{tag}: {:?}",
        started.elapsed()
    );
    assert!(reclaimed(&stage), "{tag}: the failed attempt's stage was not aborted");
    assert_eq!(live_files(&f), before, "{tag}");
}

// A dirty root whose unread part is a node that is its own child. kfc's Value::from recursed until the
// worker's stack overflowed (the process aborted; SF does the same); the serialization now refuses it at the codec's depth.
#[test]
fn a_self_referencing_unread_node_is_refused_at_serialization_not_a_stack_overflow() {
    unread_data_fails_serialization("g08b-looped", deep::looped(), "value nested more than 128 levels deep");
}

// N1, width: children that share their grandchildren (2^41 values in 660 bytes, 42 deep, under the depth bound) are
// refused by the value budget instead of expanding for hours.
#[test]
fn unread_children_that_share_their_grandchildren_are_refused_by_the_value_budget() {
    unread_data_fails_serialization(
        "g08b-shared",
        deep::shared(40),
        "unread value expands to more than 70816 values",
    );
}

// N1, size: a stored count of 2^32-1 no longer reserves it up front (kfc's Vec::with_capacity asked for ~128 GiB and
// aborted); kfc refuses the count its bytes cannot hold (eml-array-count) before the walk sees an element.
#[test]
fn an_unread_array_count_of_four_billion_is_a_parser_error_not_an_allocation() {
    unread_data_fails_serialization(
        "g08b-huge",
        deep::huge(),
        "array of 4294967295 elements (element size 8) at offset 1004 does not fit in 20 bytes of data",
    );
}

// Its other half: a faulty cyclic type chain below an unread field. kfc lists a struct's
// fields by walking its base chain, and the bounded walk handed it a struct whose bases are a cycle, so the serialization
// looped forever. The wrappers' chain check now runs on every struct, field and array type before the walk lets kfc list or map
// it.
#[test]
fn a_cyclic_struct_chain_below_an_unread_field_is_refused_not_a_hang() {
    unread_root_fails_serialization(
        "g08b-cycle",
        deep::faulty_registry(),
        deep::CYCLE_ROOT,
        deep::words(&[1]),
        "cyclic type chain at `m6.CycleA`",
    );
    // the same struct as a variant's value, its type named by the bytes (hash, relative offset, size)
    unread_root_fails_serialization(
        "g08b-cycle-variant",
        deep::faulty_registry(),
        deep::VARIANT_ROOT,
        deep::words(&[1, fnv("m6.CycleA"), 4, 0]),
        "cyclic type chain at `m6.CycleA`",
    );
}

// The same for an optional of itself below an unread field: kfc maps an optional eagerly, one recursion per offset in
// the bytes, so 100 000 chained offsets overflowed the worker's stack (the process aborted) inside one kfc call, where
// the walk's depth bound never saw them.
#[test]
fn an_optional_of_itself_below_an_unread_field_is_refused_not_a_stack_overflow() {
    let mut words = vec![1];
    words.extend(std::iter::repeat_n(4, 100_000));
    words.push(0);
    unread_root_fails_serialization(
        "g08b-loop",
        deep::faulty_registry(),
        deep::LOOP_ROOT,
        deep::words(&words),
        "cyclic type chain at `m6.Loop`",
    );
}

// Size, on a large source: the bounded walk reserves no array room from a stored count. Before, a
// 1 MiB source whose array claims 2^32-1 elements had its whole value budget (8.45 M values, 270 MB) reserved at once:
// Linux overcommits that, Windows commits it or aborts the process.
#[test]
fn an_unread_array_count_reserves_no_room_up_front_in_a_large_source() {
    use kiln_eml::sf_startup::resource_values::{self as rv, TypeHandle};
    let registry = Rc::new(deep::registry());
    let ty = registry.get_by_name(LookupKey::Qualified(deep::ROOT)).unwrap();
    // the kids array's elements lie past the end of the bytes: the walk fails at the first one
    let mut bytes = deep::words(&[1, 0xffff_0000, u32::MAX, 0, 0]);
    bytes.resize(bytes.len() + (1 << 20), 0);
    let source: Rc<[u8]> = bytes.into();
    let mapped = rv::decode_mapped(source.clone(), &TypeHandle::new(registry.clone(), ty.index)).unwrap();
    let (result, largest) = largest_allocation::during(|| rv::mapped_to_value(mapped, source.len()));
    assert!(result.is_err(), "{result:?}");
    assert!(largest < 1 << 20, "the walk asked for {largest} bytes at once");
}

// The same bound once kfc's stored-count check (eml-array-count) lets the count through: the kids array claims 2^17
// elements and its bytes hold them all, but the first element's own array lies past the end, so the walk fails at it.
// Without the 4096 cap the walk reserved room for every claimed element (2^17 values, megabytes) before reading one.
#[test]
fn an_unread_array_count_that_fits_its_bytes_reserves_at_most_the_cap_up_front() {
    use kiln_eml::sf_startup::resource_values::{self as rv, TypeHandle};
    let registry = Rc::new(deep::registry());
    let ty = registry.get_by_name(LookupKey::Qualified(deep::ROOT)).unwrap();
    // the kids start at byte 20 (the node at 4 plus 16) and 2^17 elements of 8 bytes fill the 1 MiB after them exactly
    let count = (1u32 << 20) / 8;
    let mut bytes = deep::words(&[1, 16, count, 0, 0]);
    bytes.resize(bytes.len() + (1 << 20), 0);
    // the first kid's own kids: one element far past the end, refused by kfc when the walk maps it
    bytes[20..24].copy_from_slice(&0xffff_0000u32.to_le_bytes());
    bytes[24..28].copy_from_slice(&1u32.to_le_bytes());
    let source: Rc<[u8]> = bytes.into();
    let mapped = rv::decode_mapped(source.clone(), &TypeHandle::new(registry.clone(), ty.index)).unwrap();
    let (result, largest) = largest_allocation::during(|| rv::mapped_to_value(mapped, source.len()));
    let error = result.unwrap_err();
    assert!(error.contains("array of 1 elements (element size 8)"), "{error}");
    assert!(largest < 1 << 20, "the walk asked for {largest} bytes at once");
}

// eml-array-count: kfc mapped a blob array's stored count (or a static array's registered one) without looking at the
// bytes, and every reader then made one entry per element up front, so 2^32-1 in a few bytes asked for ~100 GiB at once
// (ShroudForge does the same and aborts). A count is now refused unless the bytes from the array's start hold that many
// elements of its element size (the registered size, never less than the element kind reads, never 0). Valid data,
// whose elements are written to their full size, always fits; a count of 0 reads nothing and always passes.
#[test]
fn a_stored_array_count_must_fit_the_bytes_after_the_array() {
    let registry = Rc::new(deep::counted_registry());
    let len = |name: &str, bytes: &[u8]| deep::array_len(&registry, name, bytes);
    let refused = |name: &str, bytes: &[u8], want: &str| {
        let error = len(name, bytes).unwrap_err();
        assert!(error.contains(want), "{name}: {error}");
    };
    let w = deep::words;
    // three words exactly fill the bytes after the 8-byte header; a fourth does not fit
    assert_eq!(len("Words", &w(&[8, 3, 7, 8, 9])), Ok(3));
    refused(
        "Words",
        &w(&[8, 4, 7, 8, 9]),
        "array of 4 elements (element size 4) at offset 8 does not fit in 20 bytes of data",
    );
    refused("Words", &w(&[8, u32::MAX, 7]), "array of 4294967295 elements (element size 4)");
    // a count of 0 is never checked (an empty array's offset is 0, and kfc never reads it); one element past the end is
    assert_eq!(len("Words", &w(&[1000, 0])), Ok(0));
    refused("Words", &w(&[1000, 1]), "array of 1 elements (element size 4) at offset 1000 does not fit in 8 bytes");
    // a UInt32 registered with size 0 still takes the 4 bytes kfc reads
    assert_eq!(len("ZeroWords", &w(&[8, 4, 1, 2, 3, 4])), Ok(4));
    refused("ZeroWords", &w(&[8, 5, 1, 2, 3, 4]), "array of 5 elements (element size 4)");
    // a struct registered with size 0 takes at least one byte: no free elements
    assert_eq!(len("Empties", &[w(&[8, 2]), vec![0, 0]].concat()), Ok(2));
    refused("Empties", &w(&[8, 1_000_000]), "array of 1000000 elements (element size 1) at offset 8");
    // a static array's count comes from the registry and is checked the same way
    assert_eq!(len("Fixed3", &w(&[1, 2, 3])), Ok(3));
    refused("FixedMany", &w(&[1, 2]), "array of 1000000 elements (element size 4) at offset 0 does not fit in 8 bytes");
}

// eml-array-count, kfc's own Value::from (EML's conversions; kiln-eml's own code uses the bounded walk): it reserved the
// stored count whole before reading an element. The count is now refused before any room is reserved.
#[test]
fn kfc_value_from_bytes_refuses_a_count_beyond_the_bytes_before_reserving_it() {
    let registry = deep::registry();
    let ty = registry.get_by_name(LookupKey::Qualified(deep::ROOT)).unwrap();
    let bytes = deep::words(&[1, 1000, 1_000_000, 0, 0]);
    let (result, largest) = largest_allocation::during(|| kfc::resource::value::Value::from_bytes(&registry, ty, &bytes));
    let error = result.unwrap_err().to_string();
    assert!(
        error.contains("array of 1000000 elements (element size 8) at offset 1004 does not fit in 20 bytes of data"),
        "{error}"
    );
    assert!(largest < 1 << 16, "kfc asked for {largest} bytes at once");
}

// eml-array-count, the value wrappers: MappedArrayValue made one lazy entry per stored element when Lua read the field, so a
// mod's bytes claiming 2^32-1 kids asked for ~100 GiB (the process aborted). Reading the field is now a Lua error.
#[test]
fn a_mapped_array_count_beyond_its_bytes_is_a_lua_error_not_an_allocation() {
    for count in [1_000_000, u32::MAX] {
        let f = deep_fixture(&format!("count-{count}"), &[(deep::id(1), deep::words(&[1, 1000, count, 0, 0]))]);
        let base = baseline(&f);
        let guard = guard(&f);
        let mut host = StartupHost::open(f.game.as_std_path(), STEM, f.types.clone(), base, &guard).unwrap();
        let (lua, _assets, _lent) = data_vm(&f, &mut host);
        run(
            &lua,
            &format!(
                r##"
        local r = game.assets.get_resource("{}", "{}")
        eq(r.data.mark, 1)
        local ok, e = pcall(function() return #r.data.node.kids end)
        assert(not ok, "the kids array was made, length " .. tostring(e))
        local want = "array of {count} elements (element size 8) at offset 1004 does not fit in 20 bytes of data"
        assert(tostring(e):find(want, 1, true), tostring(e))
        "##,
                deep::guid(1),
                deep::ROOT
            ),
        );
    }
}

/// The largest single allocation the current thread asks for while `during` runs (the reserve test above).
mod largest_allocation {
    use std::{
        alloc::{GlobalAlloc, Layout, System},
        cell::Cell,
    };

    pub struct Tracking;

    thread_local! {
        static WATCHING: Cell<bool> = const { Cell::new(false) };
        static LARGEST: Cell<usize> = const { Cell::new(0) };
    }

    fn note(size: usize) {
        // const-initialized cells without a destructor: reading them never allocates
        let _ = WATCHING.try_with(|watching| {
            if watching.get() {
                let _ = LARGEST.try_with(|largest| largest.set(largest.get().max(size)));
            }
        });
    }

    // SAFETY: every call goes unchanged to the system allocator.
    unsafe impl GlobalAlloc for Tracking {
        unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
            note(layout.size());
            unsafe { System.alloc(layout) }
        }
        unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
            note(layout.size());
            unsafe { System.alloc_zeroed(layout) }
        }
        unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
            note(new_size);
            unsafe { System.realloc(ptr, layout, new_size) }
        }
        unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
            unsafe { System.dealloc(ptr, layout) }
        }
    }

    /// `f`'s result and the largest single allocation it asked for on this thread.
    pub fn during<R>(f: impl FnOnce() -> R) -> (R, usize) {
        LARGEST.with(|largest| largest.set(0));
        WATCHING.with(|watching| watching.set(true));
        let result = f();
        WATCHING.with(|watching| watching.set(false));
        (result, LARGEST.with(Cell::get))
    }
}

#[global_allocator]
static ALLOCATOR: largest_allocation::Tracking = largest_allocation::Tracking;

// An error while the dirty records are serialized discards the attempt before publication (SF's commit fails the start
// the same way, before finalize and commit): an unread text that is not UTF-8, met only by the serializer.
#[test]
fn a_serialization_error_aborts_the_stage_and_leaves_the_game_folder() {
    unread_data_fails_serialization("g08b-badtext", deep::bad_text(), "at text: ");
}

// ShroudForge-compatible for sane data: the bounded walk gives kfc's own Value::from on the fixture root and on a 50-node
// chain (100 levels), and a dirty sane root serializes to exactly kfc's re-encoding of its edited value.
#[test]
fn unread_sane_data_converts_exactly_as_kfc_does() {
    use kfc::resource::value::Value;
    use kiln_eml::sf_startup::resource_values::{self as rv, TypeHandle};
    let check = |registry: TypeRegistry, name: &str, value: &Value| {
        let registry = Rc::new(registry);
        let ty = registry.get_by_name(LookupKey::Qualified(name)).unwrap();
        let bytes: Rc<[u8]> = value.to_bytes(&registry, ty).unwrap().into();
        let handle = TypeHandle::new(registry.clone(), ty.index);
        let mapped = rv::decode_mapped(bytes.clone(), &handle).unwrap();
        let ours = rv::mapped_to_value(mapped.clone(), bytes.len()).unwrap();
        assert_eq!(ours, Value::from(mapped).unwrap(), "{name}");
    };
    check(types::registry(), types::ROOT, &types::root_value());
    check(deep::registry(), deep::ROOT, &deep::root(5, deep::chain(50), "fifty"));
    let sane = deep::root(1, deep::chain(3), "ok");
    let f = deep_fixture("g08b-sane", &[(deep::id(1), deep::bytes(&sane))]);
    let base = baseline(&f);
    let guard = guard(&f);
    let mut host = StartupHost::open(f.game.as_std_path(), STEM, f.types.clone(), base, &guard).unwrap();
    let (lua, assets, lent) = data_vm(&f, &mut host);
    run(
        &lua,
        &format!(
            r##"game.assets.get_resource("{}", "m6.DeepRoot").data.mark = 2"##,
            deep::guid(1)
        ),
    );
    let (prepared, applied) = assets.prepare(lent).unwrap();
    assert_eq!(applied, 1);
    assert_eq!(
        prepared.read_resource(&deep::id(1)).unwrap().unwrap(),
        deep::bytes(&deep::root(2, deep::chain(3), "ok"))
    );
}

// The order inside the facade: LentStage::prepare_with closes every Lua handle BEFORE the serializer runs on the
// taken-back stage (a write through a handle there is refused), and a serializer error aborts the stage at once.
#[test]
fn prepare_with_closes_the_lent_writer_before_the_serializer_and_aborts_on_its_error() {
    let f = ArchiveFixture::new("g08b-takeback");
    let base = baseline(&f);
    let guard = guard(&f);
    let mut host = StartupHost::open(f.game.as_std_path(), STEM, f.types.clone(), base, &guard).unwrap();
    let lent = host.begin_stage().unwrap().lend();
    let handle = lent.writer();
    handle
        .write_content(&ContentHash::from_data(b"through the handle"), b"through the handle")
        .unwrap();
    let edited = new_root(|_| {});
    let prepared = lent
        .prepare_with(|stage| {
            assert!(
                !handle.is_open(),
                "the serializer ran while a Lua handle could still write"
            );
            assert!(handle.write_content(&ContentHash::from_data(b"late"), b"late").is_err());
            stage.write_resource(&archive::resource_id(3), &edited)?;
            Ok(())
        })
        .unwrap();
    assert_eq!(
        prepared.read_resource(&archive::resource_id(3)).unwrap().unwrap(),
        edited
    );
    assert!(
        prepared
            .read_content(&ContentHash::from_data(b"late"))
            .unwrap()
            .is_none()
    );
    assert_eq!(
        prepared
            .read_content(&ContentHash::from_data(b"through the handle"))
            .unwrap()
            .unwrap(),
        b"through the handle"
    );
    drop(prepared);
    drop(host);

    let f = ArchiveFixture::new("g08b-takeback-fail");
    let base = baseline(&f);
    let guard = crate::guard(&f);
    let before = live_files(&f);
    let mut host = StartupHost::open(f.game.as_std_path(), STEM, f.types.clone(), base, &guard).unwrap();
    let lent = host.begin_stage().unwrap().lend();
    let handle = lent.writer();
    let stage = stage_dir(&f);
    let error = lent
        .prepare_with(|_| anyhow::bail!("the serializer failed"))
        .err()
        .unwrap();
    assert!(error.to_string().contains("the serializer failed"), "{error:#}");
    assert!(!handle.is_open());
    assert!(reclaimed(&stage));
    assert_eq!(live_files(&f), before);
}

// The AssetUserdata seam (the wrappers' ValueObjects): the session's own objects answer a Resource with its id's GUID and a
// Content with its hash, and any other userdata (a Type) with neither, as SF's Guid cast does.
#[test]
fn the_sessions_typed_objects_answer_resource_and_content_userdata() {
    use kiln_eml::sf_startup::resource_values::TypedObjects;
    patch_vm(
        "g08b-assetud",
        |lua, assets| {
            let objects = assets.objects();
            let r: AnyUserData = lua
                .load(format!("return game.assets.get_resource('{GUID}', 'm6.Root', 3)"))
                .eval()
                .unwrap();
            let c: AnyUserData = lua.load("return game.assets.get_all_contents()[1]").eval().unwrap();
            let t: AnyUserData = lua.load("return game.types.get('m6.Root')").eval().unwrap();
            let hash = *c
                .borrow::<kiln_eml::sf_startup::game::content::Content>()
                .unwrap()
                .guid();
            assert_eq!(objects.resource_guid(&r).unwrap(), Guid::parse(GUID));
            assert_eq!(objects.content_hash(&r).unwrap(), None);
            assert_eq!(objects.content_hash(&c).unwrap(), Some(hash));
            assert_eq!(objects.resource_guid(&c).unwrap(), None);
            assert_eq!(objects.resource_guid(&t).unwrap(), None);
            assert_eq!(objects.content_hash(&t).unwrap(), None);
        },
        |_| {},
    );
}

/// The transcript of `game.assets` on an empty registry and archive without PATCH, captured with ShroudForge's own
/// mod runtime (1.7.9; a local differential test, not published, which can build no other state), replayed through the same shared prelude/runner
/// on an empty session: byte-identical shared-v2 records of the casts, raised texts, single nils and the refusal order.
#[test]
fn oracle_empty_archive_transcript_matches_shroudforge() {
    let root = TestRoot::new("g08-oracle");
    let registry = Rc::new(TypeRegistry::default());
    let dir = root.0.join("game");
    fs::create_dir_all(&dir).unwrap();
    fs::File::create(dir.join(format!("{STEM}.kfc"))).unwrap();
    fs::File::create(dir.join(format!("{STEM}.kfc_resources"))).unwrap();
    KFCWriter::<Rc<KFCFile>, _>::new(&dir, STEM, registry.clone(), "")
        .unwrap()
        .finalize()
        .unwrap();
    let fixture = include_str!("fixtures/m6/assets/oracle/game-assets-empty.lua");
    let golden = include_str!("fixtures/m6/assets/oracle/sf-1.7.9/game-assets-empty.json");
    let ours = sfi_core::conformance::run_fixture(fixture, |lua| {
        let env = sfi_core::conformance::plain_std_environment(lua)?;
        let index = Rc::new(KFCFile::from_path(dir.join(format!("{STEM}.kfc")), false).unwrap());
        let reader = KFCReader::new(&dir, STEM).unwrap().into_cursor().unwrap();
        let session = TypeSession::new(lua, registry, SelectedTarget::from_index(STEM, &index));
        let assets = AssetSession::new(&session, index, reader, None);
        let game = game::create(lua, &session)?;
        game::install_assets(lua, &game, &assets)?;
        env.raw_set("game", game)?;
        env.raw_set("buffer", sfi_core::eml::buffer::create(lua)?)?;
        Ok(env)
    })
    .unwrap();
    sfi_core::conformance::validate_transcript(&ours).unwrap();
    if let Err(difference) = sfi_core::conformance::compare(golden, &ours) {
        panic!("{difference}\nours: {ours}");
    }
}

