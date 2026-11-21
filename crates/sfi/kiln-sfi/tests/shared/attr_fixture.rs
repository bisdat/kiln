// SPDX-License-Identifier: MIT
// the interpreter's runtime.ecs attribute members, the tests' shared fixture.
//! What the attribute tests (tests/runtime_ecs_attributes.rs) share: the ECS fixture (ecs_fixture.rs: the archive
//! registry, kiln.types' catalog, kiln.world's ECS fake, the scripted host, the typed values' codec) with two attribute
//! components added to the registry, the attribute catalog the fake archive serves (support/fake_attributes.rs), its
//! recalculation and kiln.engine's attribute model row, and one VM with two packages: "alpha" (globals `runtime` and
//! `ecs`) and "beta" (global `beta`), each with the ECS family's and the attribute family's members, the component
//! claim and target, the attribute target and the override tracker (one ownership store, one tracker registry).
//!
//! The catalog is ShroudForge-shaped (reflection/build_attribute_catalog.rs, MIT, Copyright (c) 2026 bonsaibauer): a
//! sint32 root "Stamina" (Stamina, Stamina_Min, Stamina_Max) whose component keen::ecs::Stamina carries its definition
//! GUID, a float32 root "Revive" (Revive_Delay, Revive_Base) in keen::ecs::Revive, and two one-attribute roots without a
//! component that share the name "Shared" (an ambiguous name). Its recalculation (the fake's attribute_evaluate) clamps
//! Stamina to Stamina_Max and doubles Revive_Delay into Revive_Base's place only when asked to.
#![allow(dead_code, unused_imports)]

#[path = "ecs_fixture.rs"]
pub mod ecs;
#[path = "../support/fake_attributes.rs"]
pub mod fake_attributes;

pub use ecs::*;
use kiln_abi::services::kiln_caps::{KILN_CAP_AVAILABLE, KILN_CAP_REASON_NONE};
use kiln_abi::services::kiln_eml_archive::KilnEmlArchiveIdentityV1;
use kiln_sfi::lifecycle::{Watchdog, process_clock};
use kiln_sfi::runtime::availability::{FactsConfig, RuntimeFacts};
use kiln_sfi::runtime::ecs::attributes::{self, catalog::Attributes};
use kiln_sfi::runtime::ecs::{Ecs, EcsRuntime, install_into};
use kiln_sfi::runtime::operation_catalog::CATALOG;
use kiln_sfi::runtime::overrides::OverrideTrackers;
use kiln_sfi::runtime::services::ServiceAdapter;
use kiln_sfi::runtime::types::{self, TypesWiring};
use kiln_sfi::runtime::values::{ArenaOptions, ValuesArena};
use kiln_sfi::worker::Host;
use serde_json::{Value as Json, json};
use sfi_core::bindings::runtime::RuntimeProviderFacts;

pub const STAMINA: &str = "keen::ecs::Stamina";
pub const REVIVE: &str = "keen::ecs::Revive";
pub const STAMINA_GUID: &str = "f552f8bc-ada0-4a58-8739-4081ce0d3873";
pub const REVIVE_GUID: &str = "0badf00d-1234-4abc-9def-00112233aabb";

/// A GUID's game-order bytes (the first three groups little-endian).
pub fn guid(text: &str) -> [u8; 16] {
    let hex: Vec<u8> = text
        .replace('-', "")
        .as_bytes()
        .chunks(2)
        .map(|p| u8::from_str_radix(std::str::from_utf8(p).unwrap(), 16).unwrap())
        .collect();
    let mut b = [0u8; 16];
    b[..4].copy_from_slice(&[hex[3], hex[2], hex[1], hex[0]]);
    b[4..6].copy_from_slice(&[hex[5], hex[4]]);
    b[6..8].copy_from_slice(&[hex[7], hex[6]]);
    b[8..].copy_from_slice(&hex[8..]);
    b
}
/// ShroudForge's `definition_root`: FNV-1a over the GUID's bytes.
pub fn root_of(text: &str) -> u32 {
    guid(text).iter().fold(0x811c_9dc5_u32, |h, b| {
        (h ^ u32::from(*b)).wrapping_mul(0x0100_0193)
    })
}
pub fn stamina_root() -> u32 {
    root_of(STAMINA_GUID)
}
pub fn revive_root() -> u32 {
    root_of(REVIVE_GUID)
}
pub const SHARED_ROOT: u32 = 0x5eed_0001;
/// Attribute IDs: a root's first attribute's ID is the root.
pub fn stamina_ids() -> [u32; 3] {
    let r = stamina_root();
    [r, r.wrapping_add(1), r.wrapping_add(2)]
}
pub fn revive_ids() -> [u32; 2] {
    let r = revive_root();
    [r, r.wrapping_add(1)]
}

/// The registry with the two attribute components (bases: keen::ecs::AttributeComponent).
pub fn attribute_types() -> Vec<T> {
    let mut types = archive_types();
    let base = Some(index_of(ATTRIBUTE_BASE) as u64);
    // the header's root ID and the storage (keen::UInt32x3) as reflected fields, for the plain field writes
    types.push(T {
        qualified: STAMINA,
        primitive: "Struct",
        size: 36,
        inner: base,
        count: 0,
        fields: vec![("rootId", UINT32 as u64, 0), ("dataStorage", 5, 24)],
        flags: 0,
    });
    types.push(T {
        qualified: REVIVE,
        primitive: "Struct",
        size: 32,
        inner: base,
        count: 0,
        fields: Vec::new(),
        flags: 0,
    });
    types
}

/// kiln.types' catalog over `types` (every keen::ecs type but keen::ecs::Unregistered, each row proven).
pub fn catalog_of(types: &[T]) -> EcsCatalog {
    let mut catalog = EcsCatalog {
        stamp: 0x303a_7700_0001,
        ..EcsCatalog::default()
    };
    for t in types
        .iter()
        .filter(|t| t.qualified.starts_with("keen::ecs::") && t.qualified != UNREGISTERED)
    {
        let identity = ECS_TYPE_BASE as u64 + catalog.types.len() as u64;
        let hash = fnv(t.qualified);
        catalog.types.push((t.qualified.to_owned(), t.size, hash));
        catalog.rows.push((hash, t.size, 0, identity));
    }
    catalog
}

/// A reflected attribute layout (build_attribute_catalog.rs `component_layouts`): the header (rootId u32 @0,
/// storageOffset u16 @4, flags u8 @6 with the Initialized bit 1, storageCount u8 @7), the definition GUID @8 and the
/// storage after it.
pub fn layout(name: &str, count: u32, scalar: &str, definition: Option<&str>) -> Json {
    let mut layout = json!({"qualified_name":name,"qualified_hash":fnv(name),"size":24 + count * 4,
        "storage_offset":24,"storage_size":count * 4,
        "header":{"rootId":{"name":"rootId","offset":0,"size":4},"storageOffset":{"name":"storageOffset","offset":4,"size":2},
            "storageCount":{"name":"storageCount","offset":7,"size":1},"flags":{"name":"flags","offset":6,"size":1,"initialized_mask":1}},
        "storage_scalar_type":scalar,"engine_server_only":false,"association":"reflected-attribute-storage"});
    if let Some(guid) = definition {
        layout["definition_offset"] = json!(8);
        layout["definition_guid"] = json!(guid);
    }
    layout
}

/// One descriptor as build_attribute_catalog.rs `from_resources` makes it (the fields the members read).
pub fn descriptor(
    name: &str,
    hash: u32,
    root: u32,
    index: usize,
    scalar: &str,
    component: Option<&Json>,
) -> Json {
    let components: Vec<Json> = component
        .map(|c| {
            let mut c = c.clone();
            c["scalar_type_matches"] = json!(true);
            c["integer_representation_compatible"] = json!(scalar != "keen::float32");
            c["association"] = json!("fnv1a-definition-guid-bytes-and-static-array");
            c
        })
        .into_iter()
        .collect();
    let has = !components.is_empty();
    json!({"name":name,"hash":hash,"hash_domain":"attribute-id","name_source":"debugNames","root_hash":root,
        "index":index,"resource_ids":[],"container_ids":[],"scalar_type_hash":7,"scalar_type":scalar,
        "components":components,"calculation_scalar_type":scalar,
        "structure":{"parentIndex":65535,"childIndex":65535,"siblingIndex":65535,"calculation":[131072]},
        "calculation_words":[131072],"calculation":{"encoding":"attribute-command-v1","decoded":true,"instructions":[]},
        "calculation_encoding_verified":true,"calculation_sign_independent":true,
        "storage_resolution":if has {"reflected-definition-guid"} else {"per-entity-initialized-root-id"},
        "storage_writable":has,"write_value_domain":"declared-scalar","calculation_writable":has,
        "calculation_reason":null,"storage_reason":if has {Json::Null} else {json!("no proven component/root association")}})
}

pub fn stamina_layout() -> Json {
    layout(STAMINA, 3, "keen::sint32", Some(STAMINA_GUID))
}
pub fn revive_layout() -> Json {
    layout(REVIVE, 2, "keen::float32", Some(REVIVE_GUID))
}
/// The catalog's descriptors.
pub fn descriptors() -> Vec<Json> {
    let (s, r) = (stamina_ids(), revive_ids());
    let sl = stamina_layout();
    let rl = revive_layout();
    vec![
        descriptor(
            "Stamina",
            s[0],
            stamina_root(),
            0,
            "keen::sint32",
            Some(&sl),
        ),
        descriptor(
            "Stamina_Min",
            s[1],
            stamina_root(),
            1,
            "keen::sint32",
            Some(&sl),
        ),
        descriptor(
            "Stamina_Max",
            s[2],
            stamina_root(),
            2,
            "keen::sint32",
            Some(&sl),
        ),
        descriptor(
            "Revive_Delay",
            r[0],
            revive_root(),
            0,
            "keen::float32",
            Some(&rl),
        ),
        descriptor(
            "Revive_Base",
            r[1],
            revive_root(),
            1,
            "keen::float32",
            Some(&rl),
        ),
        descriptor("Shared", SHARED_ROOT, SHARED_ROOT, 0, "keen::uint32", None),
        descriptor(
            "Shared",
            SHARED_ROOT + 1,
            SHARED_ROOT + 1,
            0,
            "keen::uint32",
            None,
        ),
    ]
}

/// A Stamina component: the header (root, storage offset 24, flags, count 3), the definition GUID and the values.
pub fn stamina(values: [i32; 3], flags: u8) -> (u32, Vec<u8>) {
    component(
        stamina_root(),
        STAMINA_GUID,
        flags,
        &values.map(|v| v as u32),
    )
}
pub fn revive(values: [f32; 2]) -> (u32, Vec<u8>) {
    let mut c = component(revive_root(), REVIVE_GUID, 1, &values.map(f32::to_bits));
    c.0 = fnv(REVIVE);
    c
}
pub fn component(root: u32, definition: &str, flags: u8, words: &[u32]) -> (u32, Vec<u8>) {
    let mut bytes = Vec::new();
    bytes.extend(root.to_le_bytes());
    bytes.extend(24u16.to_le_bytes());
    bytes.push(flags);
    bytes.push(words.len() as u8);
    bytes.extend(guid(definition));
    for w in words {
        bytes.extend(w.to_le_bytes());
    }
    (fnv(STAMINA), bytes)
}

/// The fake's recalculation: Stamina's root clamps Stamina to its third value; every other root keeps its bits.
pub fn recalculate(root: u32, input: &[u32]) -> Result<(Vec<u32>, Vec<Json>), (i32, String)> {
    let mut out = input.to_vec();
    if root == stamina_root() && out.len() == 3 {
        out[0] = (out[0] as i32).min(out[2] as i32) as u32;
        let trace = vec![
            json!({"name":"Stamina","hash":root,"index":0,"before_bits":input,"after_bits":out}),
        ];
        return Ok((out, trace));
    }
    Ok((out, Vec::new()))
}

pub fn identity() -> KilnEmlArchiveIdentityV1 {
    // SAFETY: an all-zero identity is valid (integers and byte arrays).
    let mut id: KilnEmlArchiveIdentityV1 = unsafe { std::mem::zeroed() };
    id.size = std::mem::size_of::<KilnEmlArchiveIdentityV1>() as u32;
    id.phase = 2;
    id.launch_id = [0x11; 16];
    id.plan_id = [0x22; 16];
    id.generation = 0x303a;
    id.reload_epoch = 2;
    id.registry_id = 0x7e57;
    id.registry_sha256 = [0x33; 32];
    id
}

pub struct AttrSetup {
    pub owner: KilnOwner,
    pub vm: fake_values::TestVm,
    pub arena: ValuesArena,
    pub codec: Rc<RefCell<Codec>>,
    pub host: Rc<Script>,
    /// A second package, "beta" (global `beta`: its runtime table).
    pub beta: Rc<Script>,
    pub ecs: Rc<Ecs>,
    pub attributes: Rc<Attributes>,
    pub trackers: Rc<OverrideTrackers>,
    pub facts: Rc<RuntimeFacts>,
    pub services: Rc<ServiceAdapter>,
    pub session: Rc<Cell<u64>>,
    _wiring: TypesWiring,
}

/// A server VM of package "alpha" (every fact on) over `world`, the catalog, the codec and the fake archive serving
/// [`descriptors`] with [`recalculate`].
pub fn setup(world: EcsWorld) -> AttrSetup {
    setup_with(world, Vec::new())
}

/// [`setup`] with `extra` reflected types after the attribute components.
pub fn setup_with(world: EcsWorld, extra: Vec<T>) -> AttrSetup {
    let owner = support::next_owner();
    fake_services::bind_thread(owner);
    let mut types = attribute_types();
    types.extend(extra);
    fake_services::set_ecs_catalog(owner, Some(catalog_of(&types)));
    fake_services::set_ecs_world(owner, Some(world));
    for row in fake_attributes::ROWS {
        fake_services::set_cap(row, Some((KILN_CAP_AVAILABLE, KILN_CAP_REASON_NONE)));
    }
    let identity = identity();
    fake_attributes::with(|s| {
        s.identity = Some(identity);
        s.catalog = Some(Ok(fake_attributes::catalog_reply(
            &identity,
            &descriptors(),
            &[stamina_layout(), revive_layout()],
        )));
        s.evaluate = Some(Rc::new(recalculate));
        s.model = Some(Ok("attribute-command-v1".into()));
    });
    let mut api = fake_services::api(true);
    api.service = Some(fake_attributes::lookup);
    let api: &'static KilnApi = Box::leak(api);
    let services = Rc::new(ServiceAdapter::new(
        unsafe { Host::read(api) },
        Rc::new(Watchdog::new(process_clock())),
    ));
    let vm = fake_values::vm();
    let arena = vm.arena(ArenaOptions::default()).unwrap();
    let codec = codec();
    let facts = Rc::new(RuntimeFacts::new(
        services.clone(),
        &CATALOG,
        FactsConfig {
            global_patch: false,
            global_export: false,
            game_build: BUILD.into(),
        },
    ));
    let session = Rc::new(Cell::new(FAKE_SESSION));
    let live = session.clone();
    let wiring = types::wire(Port(types), |_, _| {});
    let ecs = Rc::new(
        Ecs::new(
            wiring.handle.session(vm.lua()).unwrap(),
            services.clone(),
            SERVER,
        )
        .with_runtime(EcsRuntime {
            owner,
            values: Some(arena.values()),
            facts: facts.clone(),
            world: Rc::new(move || (SERVER, live.get())),
        }),
    );
    let attributes = Rc::new(Attributes::new(services.clone(), owner, identity));
    attributes::set_vm(vm.lua(), attributes.clone());
    let trackers = OverrideTrackers::new();
    let host = script("alpha");
    let runtime = package(&vm, &host, &ecs, &attributes, &trackers, &session);
    vm.set("ecs", runtime.raw_get::<Table>("ecs").unwrap());
    vm.set("runtime", runtime);
    let beta = script("beta");
    vm.set(
        "beta",
        package(&vm, &beta, &ecs, &attributes, &trackers, &session),
    );
    vm.run::<()>(
        "function perr(f, ...)
           local ok, e = pcall(f, ...)
           assert(not ok, 'expected an error')
           return (tostring(e):match('^[^\\n]*'))
         end",
    )
    .unwrap();
    AttrSetup {
        owner,
        vm,
        arena,
        codec,
        host,
        beta,
        ecs,
        attributes,
        trackers,
        facts,
        services,
        session,
        _wiring: wiring,
    }
}

/// One package's runtime table on `host`: `ecs` (the ECS family's members and the attribute family's),
/// `overrides` (the component claim and target, the attribute target, the tracker) and `world.session_id`.
pub fn package(
    vm: &fake_values::TestVm,
    host: &Rc<Script>,
    ecs: &Rc<Ecs>,
    attributes: &Rc<Attributes>,
    trackers: &Rc<OverrideTrackers>,
    session: &Rc<Cell<u64>>,
) -> Table {
    let lua = vm.lua();
    let runtime = lua.create_table().unwrap();
    let table = lua.create_table().unwrap();
    install_into(lua, &table, host.clone(), ecs.clone()).unwrap();
    attributes::install_into(lua, &table, host.clone(), ecs.clone(), attributes.clone()).unwrap();
    runtime.raw_set("ecs", table).unwrap();
    let overrides = lua.create_table().unwrap();
    runtime.raw_set("overrides", overrides.clone()).unwrap();
    let world = lua.create_table().unwrap();
    let live = session.clone();
    world
        .raw_set(
            "session_id",
            lua.create_function(move |_, ()| Ok(live.get())).unwrap(),
        )
        .unwrap();
    runtime.raw_set("world", world).unwrap();
    let h: Rc<dyn sfi_core::bindings::runtime::RuntimeHost> = host.clone();
    kiln_sfi::runtime::overrides::targets::component::install_into(lua, &runtime, h, ecs.clone())
        .unwrap();
    kiln_sfi::runtime::overrides::targets::attribute::install_into(lua, &runtime).unwrap();
    let live = session.clone();
    trackers
        .install(
            lua,
            &overrides,
            host.owner.id(),
            Rc::new(move || live.get()),
        )
        .unwrap();
    runtime
}

impl AttrSetup {
    pub fn eval<R: FromLuaMulti>(&self, code: &str) -> R {
        self.vm
            .run::<R>(code)
            .unwrap_or_else(|e| panic!("{code}: {e}"))
    }
    /// `(nil, reason)` (or `(false, reason)`) of a call that refuses.
    pub fn reason(&self, call: &str) -> String {
        let (value, reason): (Value, Option<String>) = self.eval(&format!("return {call}"));
        assert!(
            matches!(value, Value::Nil | Value::Boolean(false)),
            "{call}: {value:?}"
        );
        reason.unwrap_or_else(|| panic!("{call}: no reason"))
    }
    pub fn raised(&self, call: &str) -> String {
        self.eval(&format!("return perr(function() return {call} end)"))
    }
    pub fn facts(&self, f: impl FnOnce(&mut RuntimeProviderFacts)) {
        f(&mut self.host.facts.borrow_mut());
    }
    pub fn world<R>(&self, f: impl FnOnce(&mut EcsWorld) -> R) -> R {
        fake_services::ecs_world(self.owner, f).expect("an ECS world")
    }
    /// The handle of entity `id` (through resolve).
    pub fn handle(&self, id: u32) -> i64 {
        self.eval(&format!("return (ecs.resolve({id}))"))
    }
    pub fn calls(&self, member: &str) -> u64 {
        fake_services::counters(self.owner).calls(member)
    }
}
