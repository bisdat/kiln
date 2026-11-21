// SPDX-License-Identifier: GPL-3.0-or-later
//! The codec's synthetic registry plus a dynamic array of optional elements (`m6.OptList`), and a production startup
//! VM (sfi-core `LuaVm`) with the GPL table port attached, as the pass will build it. All values synthetic.
#![allow(dead_code)]
use std::rc::Rc;

use kfc::reflection::{PrimitiveType as P, StructFieldMetadata, TypeFlags as F, TypeIndex, TypeMetadata, TypeRegistry};
use kiln_eml::sf_startup::resource_values::{TypeHandle, ValueObjects, ValueTables};
use sfi_core::{
    host::{HostContext, Package, PackageSource, Phase, Side, SourceVersion},
    lua::{LuaVm, VmAttachments},
};

use crate::values;

pub const OPT_LIST: usize = values::COUNT; // BlobArray of m6.MaybeChild (BlobOptional of m6.Child)
pub const MODE_HOLDER: usize = OPT_LIST + 1; // struct whose one field is m6.Mode, an enum without its value type
pub const MODE_LIST: usize = MODE_HOLDER + 1; // BlobArray of m6.Mode
pub const NODE: usize = MODE_LIST + 1; // m6.Node {kids: m6.NodeList}: a recursive type (as the codec's review test)
pub const NODE_LIST: usize = NODE + 1; // BlobArray of m6.Node
pub const LOOP: usize = NODE_LIST + 1; // BlobOptional of itself (a faulty registry only)
pub const CYCLE_HOLDER: usize = LOOP + 1; // struct {cycle: m6.CycleA @0, looped: m6.Loop @4}
pub const COUNT: usize = CYCLE_HOLDER + 1;

pub fn types() -> Vec<TypeMetadata> {
    let mut types = values::types();
    let mut list = types[4].clone(); // m6.Children: BlobArray of m6.Child
    list.index = TypeIndex::new(OPT_LIST);
    list.name = "OptList".into();
    list.qualified_name = "m6.OptList".into();
    list.impact_name = "M6_OptList".into();
    list.qualified_hash = kfc::hash::fnv("m6.OptList");
    list.impact_hash = kfc::hash::fnv("M6_OptList");
    list.name_hash = kfc::hash::fnv("OptList");
    list.internal_hash = 0xd600_0000;
    list.inner_type = Some(TypeIndex::new(5));
    list.flags = F::HAS_BLOB_ARRAY | F::HAS_BLOB_OPTIONAL;
    assert_eq!(list.primitive_type, P::BlobArray);
    types.push(list);
    // a faulty registry for lazy reads: decoding the field panics in the parser (kfc's enum value-type `expect`)
    let mut holder = types[1].clone(); // m6.Base: one field, `base`, at 0
    holder.index = TypeIndex::new(MODE_HOLDER);
    holder.name = "ModeHolder".into();
    holder.qualified_name = "m6.ModeHolder".into();
    holder.impact_name = "M6_ModeHolder".into();
    holder.qualified_hash = kfc::hash::fnv("m6.ModeHolder");
    holder.impact_hash = kfc::hash::fnv("M6_ModeHolder");
    holder.name_hash = kfc::hash::fnv("ModeHolder");
    holder.internal_hash = 0xd600_0001;
    holder.inner_type = None;
    holder.size = 8;
    holder.struct_fields.get_mut("base").unwrap().r#type = TypeIndex::new(crate::reg::MODE);
    assert_eq!(holder.primitive_type, P::Struct);
    types.push(holder);
    let mut modes = types[4].clone(); // m6.Children: BlobArray
    modes.index = TypeIndex::new(MODE_LIST);
    modes.name = "ModeList".into();
    modes.qualified_name = "m6.ModeList".into();
    modes.impact_name = "M6_ModeList".into();
    modes.qualified_hash = kfc::hash::fnv("m6.ModeList");
    modes.impact_hash = kfc::hash::fnv("M6_ModeList");
    modes.name_hash = kfc::hash::fnv("ModeList");
    modes.internal_hash = 0xd600_0002;
    modes.inner_type = Some(TypeIndex::new(crate::reg::MODE));
    types.push(modes);
    let named = |mut t: TypeMetadata, index: usize, name: &str, inner: Option<usize>| {
        t.index = TypeIndex::new(index);
        t.name = name.into();
        t.qualified_name = format!("m6.{name}");
        t.impact_name = format!("M6_{name}");
        t.qualified_hash = kfc::hash::fnv(&t.qualified_name);
        t.impact_hash = kfc::hash::fnv(&t.impact_name);
        t.name_hash = kfc::hash::fnv(name);
        t.internal_hash = 0xd600_0000 + index as u32;
        t.inner_type = inner.map(TypeIndex::new);
        t.default_value = None;
        t.attributes = Default::default();
        t
    };
    let fields = |t: &mut TypeMetadata, list: &[(&str, usize, u64)]| {
        t.struct_fields.clear();
        for (name, r#type, data_offset) in list {
            t.struct_fields.insert(
                (*name).into(),
                StructFieldMetadata {
                    name: (*name).into(),
                    r#type: TypeIndex::new(*r#type),
                    data_offset: *data_offset,
                    attributes: Default::default(),
                },
            );
        }
        t.field_count = list.len() as u32;
    };
    let mut node = named(types[2].clone(), NODE, "Node", None);
    fields(&mut node, &[("kids", NODE_LIST, 0)]);
    node.size = 8;
    types.push(node);
    types.push(named(types[4].clone(), NODE_LIST, "NodeList", Some(NODE)));
    types.push(named(types[5].clone(), LOOP, "Loop", Some(LOOP)));
    let mut holder = named(types[1].clone(), CYCLE_HOLDER, "CycleHolder", None);
    fields(&mut holder, &[("cycle", crate::reg::CYCLE_A, 0), ("looped", LOOP, 4)]);
    holder.size = 8;
    types.push(holder);
    assert_eq!(types.len(), COUNT);
    types
}

pub fn registry() -> Rc<TypeRegistry> {
    Rc::new(crate::reg::registry_of("m6-userdata", types()))
}

pub fn handle(registry: &Rc<TypeRegistry>, index: usize) -> TypeHandle {
    TypeHandle::new(registry.clone(), TypeIndex::new(index))
}

struct NoFiles;
impl PackageSource for NoFiles {
    fn read_file(&self, path: &str) -> std::io::Result<Vec<u8>> {
        Err(std::io::Error::new(std::io::ErrorKind::NotFound, path.to_owned()))
    }
}

/// A production core VM with `ValueTables` attached and one package whose environment is returned.
pub fn core_vm() -> (LuaVm, mlua::Table) {
    let vm = LuaVm::new(HostContext::new(Phase::Startup, Side::Client, SourceVersion::Sf179)).unwrap();
    vm.attach(VmAttachments {
        typed_tables: Some(Rc::new(ValueTables)),
        buffer_resources: None,
    })
    .unwrap();
    let env = vm
        .add_package(Package::new("g07", Rc::new(NoFiles)), |_, _, _| Ok(()))
        .unwrap();
    (vm, env)
}

pub fn objects() -> ValueObjects {
    ValueObjects::new()
}
