// SPDX-License-Identifier: GPL-3.0-or-later
//! The typed graph's nodes: ShroudForge's six value wrappers as Rust nodes a host VM reaches through leases,
//! with the same caches, laziness, aliases, clones and dirtiness the mlua value wrappers have.
//!
//! Adapted from ShroudForge 1.7.9, Kiln's fork pin 2df1de68 (MIT, Copyright (c) 2026 bonsaibauer; notice in
//! sf_startup/mod.rs), through the codec's and the wrappers' ports of it:
//! - modules/mod-runtime/lua/resource_values/owned_values.rs:21-556 (owned struct/array/variant, their getters,
//!   setters with the fork fix, `insert`/`remove`/`clear`, iteration, `__tostring`);
//! - modules/mod-runtime/lua/resource_values/mapped_values.rs:29-758 (lazy mapped values, the read cache, dirtiness,
//!   ORIGINAL-length iteration);
//! - modules/mod-runtime/lua/resource_values/mod.rs:51-89 (`is_dirty_lua_value`, `mapped_source_bytes`);
//! - modules/mod-runtime/lua/resource_values/convert_resource_values.rs:23-560 and validate_resource_values.rs:685-735
//!   (`convert_lua_to_value`, `try_clone_lua_value`).
//!
//! A `LuaValue` slot becomes a [`Slot`]: the value ([`Val`]: a scalar or a child node) plus, once the child's frontend
//! userdata exists, the pin that keeps that userdata alive exactly as Source's slot keeps its `LuaValue`. Nodes own
//! their children (`Rc`), never their parent and never their own proxy. Every walk here is bounded by the codec's
//! [`MAX_DEPTH`], kfc's lazy reads run under a panic guard, and type chains are checked before kfc walks them (the codec and the wrappers
//! review fixes). Where Source panics, the text is a Lua error instead.
use std::{
    cell::{Cell, RefCell},
    collections::HashMap,
    panic::AssertUnwindSafe,
    rc::Rc,
};

use kfc::{
    guid::{ContentHash, Guid},
    reflection::{PrimitiveType, TypeRegistry},
    resource::{
        mapped::{MappedArray as KfcArray, MappedStruct as KfcStruct, MappedVariant as KfcVariant},
        value::{Value, Variant},
    },
};
use kiln_abi::{
    KilnOwner,
    services::kiln_eml_values::{
        KILN_VALUE_ARRAY_MAPPED, KILN_VALUE_ARRAY_OWNED, KILN_VALUE_BOOLEAN, KILN_VALUE_INTEGER, KILN_VALUE_NIL,
        KILN_VALUE_NUMBER, KILN_VALUE_STRING, KILN_VALUE_STRUCT_MAPPED, KILN_VALUE_STRUCT_OWNED,
        KILN_VALUE_VARIANT_MAPPED, KILN_VALUE_VARIANT_OWNED,
    },
};

use super::lifetime::{Adapter, BorrowFlag, CheckKind, Fault, Found, Pin, UserdataKind};
use crate::sf_startup::resource_values::{
    MAX_DEPTH, MappedValue, ObjectKind, TypeHandle, check_chain, field_type, inner_type, mapped_to_value, name_of,
    path::TreePath,
};

pub(crate) type MappedStruct = KfcStruct<Rc<[u8]>, Rc<TypeRegistry>>;
pub(crate) type MappedArray = KfcArray<Rc<[u8]>, Rc<TypeRegistry>>;
pub(crate) type MappedVariant = KfcVariant<Rc<[u8]>, Rc<TypeRegistry>>;

/// A Lua value the graph stores: one of Lua's scalars, or a typed child (the wrapper userdata in Source).
#[derive(Clone)]
pub(crate) enum Val {
    Nil,
    Bool(bool),
    Int(i64),
    Num(f64),
    Str(Rc<[u8]>),
    Node(Rc<Node>),
}

impl Val {
    pub(crate) fn str(text: &str) -> Self {
        Val::Str(Rc::from(text.as_bytes()))
    }
    pub(crate) fn node(&self) -> Option<&Rc<Node>> {
        match self {
            Val::Node(node) => Some(node),
            _ => None,
        }
    }
    /// Source's read-cache partition: strings and userdata are cached (mapped_values.rs:175-195).
    fn cacheable(&self) -> bool {
        matches!(self, Val::Str(_) | Val::Node(_))
    }
}

/// A Source `LuaValue` slot: the value and, once its userdata exists, the pin that keeps that userdata.
pub(crate) struct Slot {
    pub(crate) val: Val,
    pub(crate) pin: Option<Pin>,
}

impl Slot {
    pub(crate) fn new(val: Val) -> Self {
        Self { val, pin: None }
    }
}

/// A mapped array entry: a value read (or written) once, or still the original element.
pub(crate) enum Entry {
    Value(Slot),
    Uninit(usize),
}

pub(crate) enum Body {
    /// A scalar node: a removed or returned scalar (a removed nil is a nil node).
    Scalar(Val),
    Struct {
        ty: TypeHandle,
        fields: RefCell<HashMap<String, Slot>>,
    },
    Array {
        ty: TypeHandle,
        inner: TypeHandle,
        items: RefCell<Vec<Slot>>,
    },
    Variant {
        ty: TypeHandle,
        variant: TypeHandle,
        value: RefCell<Slot>,
    },
    MStruct {
        data: MappedStruct,
        cache: RefCell<HashMap<String, Slot>>,
        dirty: Cell<bool>,
    },
    MArray {
        data: MappedArray,
        entries: RefCell<Vec<Entry>>,
        dirty: Cell<bool>,
    },
    MVariant {
        data: MappedVariant,
        value: RefCell<Option<Slot>>,
        dirty: Cell<bool>,
    },
}

/// One graph node. Owned by the slots, leases, iterators and resource records that hold it.
pub struct Node {
    pub(crate) id: u64,
    /// Attribution only: whose call made the node (its owner). Never a lifetime or access gate.
    pub(crate) creator: KilnOwner,
    pub(crate) body: Body,
    pub(crate) borrow: BorrowFlag,
    /// Edits committed on this node (its `receiver_revision`).
    pub(crate) revision: Cell<u64>,
    /// `KILN_VALUE_RESOURCE_ROOT` / `KILN_VALUE_LOADED_SNAPSHOT` for the roots the arena caches.
    pub(crate) flags: Cell<u32>,
}

/// Where a child value lives, to pin its userdata into the same slot after a host call.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum SlotRef {
    Field(String),
    Index(usize),
}

/// What node creation and the few host-facing renderings need from the arena.
pub(crate) trait Env {
    fn owner(&self) -> KilnOwner;
    fn new_id(&self) -> u64;
    /// Source's `to_string` of a variant userdata (no `__tostring`: mlua's default text of its frontend userdata).
    fn userdata_text(&self, node: &Rc<Node>) -> Result<Vec<u8>, Fault>;
}

/// kfc's lazy parser reads under a panic guard: a panic is a Lua error with its text.
pub(crate) fn lazily<R>(read: impl FnOnce() -> Result<R, Fault>) -> Result<R, Fault> {
    std::panic::catch_unwind(AssertUnwindSafe(read)).unwrap_or_else(|panic| Err(Fault::runtime(panic_text(panic))))
}

pub(crate) fn panic_text(panic: Box<dyn std::any::Any + Send>) -> String {
    panic
        .downcast_ref::<&str>()
        .map(|s| s.to_string())
        .or_else(|| panic.downcast_ref::<String>().cloned())
        .unwrap_or_else(|| "no message".into())
}

fn chain(r#type: &TypeHandle) -> Result<(), Fault> {
    check_chain(r#type).map_err(Fault::from)
}

/// A struct field's chain, checked before kfc maps the field.
fn check_field_chain(data: &MappedStruct, key: &str) -> Result<(), Fault> {
    let Some(field) = data.r#type().get_field_metadata(key) else {
        return Ok(());
    };
    match TypeHandle::try_new(data.r#type().type_registry().clone(), field.r#type) {
        Some(field_type) => chain(&field_type),
        None => Ok(()),
    }
}

pub(crate) fn too_deep() -> Fault {
    Fault::runtime(format!("value nested more than {MAX_DEPTH} levels deep"))
}

impl Node {
    pub(crate) fn new(env: &dyn Env, body: Body) -> Rc<Self> {
        Rc::new(Self {
            id: env.new_id(),
            creator: env.owner(),
            body,
            borrow: BorrowFlag::default(),
            revision: Cell::new(0),
            flags: Cell::new(0),
        })
    }

    pub(crate) fn scalar(env: &dyn Env, val: Val) -> Rc<Self> {
        debug_assert!(val.node().is_none());
        Self::new(env, Body::Scalar(val))
    }

    /// The `KILN_VALUE_*` kind.
    pub(crate) fn kind(&self) -> u32 {
        (match &self.body {
            Body::Scalar(Val::Nil) => KILN_VALUE_NIL,
            Body::Scalar(Val::Bool(_)) => KILN_VALUE_BOOLEAN,
            Body::Scalar(Val::Int(_)) => KILN_VALUE_INTEGER,
            Body::Scalar(Val::Num(_)) => KILN_VALUE_NUMBER,
            Body::Scalar(_) => KILN_VALUE_STRING,
            Body::Struct { .. } => KILN_VALUE_STRUCT_OWNED,
            Body::MStruct { .. } => KILN_VALUE_STRUCT_MAPPED,
            Body::Array { .. } => KILN_VALUE_ARRAY_OWNED,
            Body::MArray { .. } => KILN_VALUE_ARRAY_MAPPED,
            Body::Variant { .. } => KILN_VALUE_VARIANT_OWNED,
            Body::MVariant { .. } => KILN_VALUE_VARIANT_MAPPED,
        }) as u32
    }

    /// The class and declared type of a typed node (SF `classify`; a variant's declared base type); None for scalars.
    pub(crate) fn classify(&self) -> Option<(ObjectKind, &TypeHandle)> {
        Some(match &self.body {
            Body::Scalar(_) => return None,
            Body::Struct { ty, .. } => (ObjectKind::Struct, ty),
            Body::MStruct { data, .. } => (ObjectKind::Struct, data.r#type()),
            Body::Array { ty, .. } => (ObjectKind::Array, ty),
            Body::MArray { data, .. } => (ObjectKind::Array, data.r#type()),
            Body::Variant { ty, .. } => (ObjectKind::Variant, ty),
            Body::MVariant { data, .. } => (ObjectKind::Variant, data.base_type()),
        })
    }

    pub(crate) fn is_array(&self) -> bool {
        matches!(self.body, Body::Array { .. } | Body::MArray { .. })
    }

    /// A variant's selected type.
    pub(crate) fn variant_type(&self) -> Option<&TypeHandle> {
        match &self.body {
            Body::Variant { variant, .. } => Some(variant),
            Body::MVariant { data, .. } => Some(data.variant_type()),
            _ => None,
        }
    }

    pub(crate) fn scalar_val(&self) -> Option<&Val> {
        match &self.body {
            Body::Scalar(val) => Some(val),
            _ => None,
        }
    }

    /// Source's `#`: a struct's field count (owned: the map; mapped: through bases), an array's current length
    /// (mapped as `i32`). None for variants (no `__len`).
    pub(crate) fn lua_len(&self) -> Result<Option<i64>, Fault> {
        Ok(Some(match &self.body {
            Body::Struct { fields, .. } => fields.borrow().len() as i64,
            Body::MStruct { data, .. } => lazily(|| Ok(data.len()))? as i64,
            Body::Array { items, .. } => items.borrow().len() as i64,
            Body::MArray { entries, .. } => entries.borrow().len() as i32 as i64,
            _ => return Ok(None),
        }))
    }

    /// An array's current logical length.
    pub(crate) fn logical_len(&self) -> Option<usize> {
        match &self.body {
            Body::Array { items, .. } => Some(items.borrow().len()),
            Body::MArray { entries, .. } => Some(entries.borrow().len()),
            _ => None,
        }
    }

    /// A mapped array's original length.
    pub(crate) fn source_len(&self) -> Option<usize> {
        match &self.body {
            Body::MArray { data, .. } => Some(data.len()),
            _ => None,
        }
    }

    pub(crate) fn is_static_array(&self) -> bool {
        match &self.body {
            Body::Array { ty, .. } => ty.primitive_type == PrimitiveType::StaticArray,
            Body::MArray { data, .. } => data.r#type().primitive_type == PrimitiveType::StaticArray,
            _ => false,
        }
    }

    /// SF `mapped_source_bytes`: a mapped struct's original bytes, None for anything else.
    pub(crate) fn source_bytes(&self) -> Option<Vec<u8>> {
        match &self.body {
            Body::MStruct { data, .. } => Some(data.data().as_ref().to_vec()),
            _ => None,
        }
    }

    // ---- getters (Source `__index` / field getters) ----

    /// A child read as Source's getter reads it, caching what Source caches. Errors are Source's. `None` means the
    /// selector names nothing this node's Lua surface has (a variant field other than `type`/`value`).
    pub(crate) fn get(&self, env: &dyn Env, at: &SlotRef) -> Result<Option<Val>, Fault> {
        Ok(Some(match (&self.body, at) {
            (Body::Struct { fields, .. }, SlotRef::Field(key)) => {
                fields.borrow().get(key).map_or(Val::Nil, |slot| slot.val.clone())
            }
            (Body::MStruct { data, cache, .. }, SlotRef::Field(key)) => mstruct_get(data, cache, key, env)?,
            (Body::Array { items, .. }, SlotRef::Index(index)) => {
                if *index == 0 {
                    Val::Nil
                } else {
                    items.borrow().get(index - 1).map_or(Val::Nil, |slot| slot.val.clone())
                }
            }
            (Body::MArray { data, entries, .. }, SlotRef::Index(index)) => {
                if *index == 0 {
                    Val::Nil
                } else {
                    marray_get(data, entries, index - 1, env)?
                }
            }
            (Body::Variant { .. } | Body::MVariant { .. }, SlotRef::Field(key)) if key == "type" => {
                let variant = self.variant_type().expect("a variant has a selected type");
                Val::str(&variant.qualified_name)
            }
            (Body::Variant { value, .. }, SlotRef::Field(key)) if key == "value" => value.borrow().val.clone(),
            (Body::MVariant { data, value, .. }, SlotRef::Field(key)) if key == "value" => {
                let mut slot = value.borrow_mut();
                match slot.as_ref() {
                    Some(slot) => slot.val.clone(),
                    None => {
                        let val = mapped_to_val(&MappedValue::Struct(data.value().clone()), env)?;
                        *slot = Some(Slot::new(val.clone()));
                        val
                    }
                }
            }
            _ => return Ok(None),
        }))
    }

    /// Pins `pin` (the userdata of `child`) into the slot at `at` when that slot still holds `child` unpinned;
    /// otherwise the pin comes back (the caller keeps it with its result).
    pub(crate) fn pin_slot(&self, at: &SlotRef, child: &Rc<Node>, pin: Pin) -> Result<(), Pin> {
        let place = |slot: &mut Slot, pin: Pin| -> Result<(), Pin> {
            match (&slot.val, &slot.pin) {
                (Val::Node(node), None) if Rc::ptr_eq(node, child) => {
                    slot.pin = Some(pin);
                    Ok(())
                }
                _ => Err(pin),
            }
        };
        match (&self.body, at) {
            (Body::Struct { fields, .. }, SlotRef::Field(key)) => match fields.borrow_mut().get_mut(key) {
                Some(slot) => place(slot, pin),
                None => Err(pin),
            },
            (Body::MStruct { cache, .. }, SlotRef::Field(key)) => match cache.borrow_mut().get_mut(key) {
                Some(slot) => place(slot, pin),
                None => Err(pin),
            },
            (Body::Array { items, .. }, SlotRef::Index(index)) if *index > 0 => {
                match items.borrow_mut().get_mut(index - 1) {
                    Some(slot) => place(slot, pin),
                    None => Err(pin),
                }
            }
            (Body::MArray { entries, .. }, SlotRef::Index(index)) if *index > 0 => {
                match entries.borrow_mut().get_mut(index - 1) {
                    Some(Entry::Value(slot)) => place(slot, pin),
                    _ => Err(pin),
                }
            }
            (Body::Variant { value, .. }, SlotRef::Field(key)) if key == "value" => place(&mut value.borrow_mut(), pin),
            (Body::MVariant { value, .. }, SlotRef::Field(key)) if key == "value" => {
                match value.borrow_mut().as_mut() {
                    Some(slot) => place(slot, pin),
                    None => Err(pin),
                }
            }
            _ => Err(pin),
        }
    }

    /// Whether the slot at `at` already pins `child`'s userdata.
    pub(crate) fn slot_pinned(&self, at: &SlotRef, child: &Rc<Node>) -> bool {
        let pinned = |slot: &Slot| matches!(&slot.val, Val::Node(n) if Rc::ptr_eq(n, child)) && slot.pin.is_some();
        match (&self.body, at) {
            (Body::Struct { fields, .. }, SlotRef::Field(key)) => fields.borrow().get(key).is_some_and(pinned),
            (Body::MStruct { cache, .. }, SlotRef::Field(key)) => cache.borrow().get(key).is_some_and(pinned),
            (Body::Array { items, .. }, SlotRef::Index(i)) if *i > 0 => items.borrow().get(i - 1).is_some_and(pinned),
            (Body::MArray { entries, .. }, SlotRef::Index(i)) if *i > 0 => {
                matches!(entries.borrow().get(i - 1), Some(Entry::Value(slot)) if pinned(slot))
            }
            (Body::Variant { value, .. }, SlotRef::Field(k)) if k == "value" => pinned(&value.borrow()),
            (Body::MVariant { value, .. }, SlotRef::Field(k)) if k == "value" => {
                value.borrow().as_ref().is_some_and(pinned)
            }
            _ => false,
        }
    }

    // ---- setters (Source `__newindex` and the variant `value` setter) ----

    /// The type a setter validates against, after Source's own checks (unknown property, range). The fork fix: the
    /// FIELD's type for structs, the ELEMENT type for arrays.
    pub(crate) fn set_target(&self, at: &SlotRef) -> Result<Option<TypeHandle>, Fault> {
        Ok(Some(match (&self.body, at) {
            (Body::Struct { ty, fields }, SlotRef::Field(key)) => {
                let known = fields.borrow().contains_key(key);
                match ty.get_field_metadata(key) {
                    Some(_) if known => field_type(ty, key)?,
                    _ => return Err(invalid_property(key, ty)),
                }
            }
            (Body::MStruct { data, .. }, SlotRef::Field(key)) => match data.r#type().get_field_metadata(key) {
                Some(_) => field_type(data.r#type(), key)?,
                None => return Err(invalid_property(key, data.r#type())),
            },
            (Body::Array { inner, items, .. }, SlotRef::Index(index)) => {
                check_range(*index, 1, items.borrow().len())?;
                inner.clone()
            }
            (Body::MArray { data, entries, .. }, SlotRef::Index(index)) => {
                check_range(*index, 1, entries.borrow().len())?;
                data.element_type().clone()
            }
            (Body::Variant { variant, .. }, SlotRef::Field(key)) if key == "value" => variant.clone(),
            (Body::MVariant { data, .. }, SlotRef::Field(key)) if key == "value" => data.variant_type().clone(),
            _ => return Ok(None),
        }))
    }

    /// Commits a validated value (the old slot and its pin drop).
    pub(crate) fn set_commit(&self, at: &SlotRef, val: Val) -> Result<(), Fault> {
        match (&self.body, at) {
            (Body::Struct { fields, .. }, SlotRef::Field(key)) => {
                fields.borrow_mut().insert(key.clone(), Slot::new(val));
            }
            (Body::MStruct { cache, dirty, .. }, SlotRef::Field(key)) => {
                cache.borrow_mut().insert(key.clone(), Slot::new(val));
                dirty.set(true);
            }
            (Body::Array { items, .. }, SlotRef::Index(index)) => {
                // Source indexes the Vec again after validation: a re-entrant edit that shrank it panics there
                let mut items = items.borrow_mut();
                let len = items.len();
                let slot = items.get_mut(index - 1).ok_or_else(|| stale_index(len, index - 1))?;
                *slot = Slot::new(val);
            }
            (Body::MArray { entries, dirty, .. }, SlotRef::Index(index)) => {
                let mut entries = entries.borrow_mut();
                let len = entries.len();
                let entry = entries.get_mut(index - 1).ok_or_else(|| stale_index(len, index - 1))?;
                *entry = Entry::Value(Slot::new(val));
                dirty.set(true);
            }
            (Body::Variant { value, .. }, SlotRef::Field(_)) => *value.borrow_mut() = Slot::new(val),
            (Body::MVariant { value, dirty, .. }, SlotRef::Field(_)) => {
                *value.borrow_mut() = Some(Slot::new(val));
                dirty.set(true);
            }
            _ => return Err(Fault::Internal("set_commit on a node without that slot".into())),
        }
        Ok(())
    }

    // ---- structural array operations (Source `insert`/`remove`/`clear`) ----

    /// insert's checks before validation: range [1, len+1], then static. Returns the element type.
    pub(crate) fn insert_target(&self, index: usize) -> Result<TypeHandle, Fault> {
        let len = self.logical_len().unwrap_or(0);
        check_range(index, 1, len + 1)?;
        check_static(self)?;
        match &self.body {
            Body::Array { inner, .. } => Ok(inner.clone()),
            Body::MArray { data, .. } => Ok(data.element_type().clone()),
            _ => Err(Fault::Internal("insert on a non-array".into())),
        }
    }

    pub(crate) fn insert_commit(&self, index: usize, val: Val) -> Result<(), Fault> {
        match &self.body {
            Body::Array { items, .. } => {
                let mut items = items.borrow_mut();
                if index - 1 > items.len() {
                    return Err(stale_insert(index - 1, items.len()));
                }
                items.insert(index - 1, Slot::new(val));
            }
            Body::MArray { entries, dirty, .. } => {
                let mut entries = entries.borrow_mut();
                if index - 1 > entries.len() {
                    return Err(stale_insert(index - 1, entries.len()));
                }
                entries.insert(index - 1, Entry::Value(Slot::new(val)));
                dirty.set(true);
            }
            _ => return Err(Fault::Internal("insert on a non-array".into())),
        }
        Ok(())
    }

    /// remove's checks and the element's materialization (a mapped element is read, and cached, as a mod would read
    /// it, so a removed wrapper keeps its identity). `None`: Source removes nothing (position 0, or len+1).
    pub(crate) fn remove_prepare(&self, index: usize, env: &dyn Env) -> Result<Option<Val>, Fault> {
        if index == 0 {
            return Ok(None);
        }
        let len = self.logical_len().unwrap_or(0);
        check_range(index, 1, len + 1)?;
        check_static(self)?;
        if index > len {
            return Ok(None);
        }
        Ok(Some(match &self.body {
            Body::Array { items, .. } => items.borrow()[index - 1].val.clone(),
            Body::MArray { data, entries, .. } => marray_get(data, entries, index - 1, env)?,
            _ => return Err(Fault::Internal("remove on a non-array".into())),
        }))
    }

    /// Removes the prepared element and returns its slot (its pin moves with it).
    pub(crate) fn remove_commit(&self, index: usize, expected: &Val) -> Result<Slot, Fault> {
        let same = |slot: &Slot| match (&slot.val, expected) {
            (Val::Node(a), Val::Node(b)) => Rc::ptr_eq(a, b),
            _ => true,
        };
        match &self.body {
            Body::Array { items, .. } => {
                let mut items = items.borrow_mut();
                if index > items.len() || !same(&items[index - 1]) {
                    return Err(stale_index(items.len(), index - 1));
                }
                Ok(items.remove(index - 1))
            }
            Body::MArray { entries, dirty, .. } => {
                let mut entries = entries.borrow_mut();
                if index > entries.len() {
                    return Err(stale_index(entries.len(), index - 1));
                }
                let slot = match entries.remove(index - 1) {
                    Entry::Value(slot) if same(&slot) => slot,
                    Entry::Value(slot) => {
                        entries.insert(index - 1, Entry::Value(slot));
                        return Err(stale_index(entries.len(), index - 1));
                    }
                    Entry::Uninit(original) => {
                        entries.insert(index - 1, Entry::Uninit(original));
                        return Err(stale_index(entries.len(), index - 1));
                    }
                };
                dirty.set(true);
                Ok(slot)
            }
            _ => Err(Fault::Internal("remove on a non-array".into())),
        }
    }

    /// Source `clear`: static first (an empty static array errors too); a mapped array is dirty even when empty.
    pub(crate) fn clear(&self) -> Result<(), Fault> {
        check_static(self)?;
        match &self.body {
            Body::Array { items, .. } => items.borrow_mut().clear(),
            Body::MArray { entries, dirty, .. } => {
                entries.borrow_mut().clear();
                dirty.set(true);
            }
            _ => return Err(Fault::Internal("clear on a non-array".into())),
        }
        Ok(())
    }
}

fn invalid_property(key: &str, ty: &TypeHandle) -> Fault {
    Fault::InvalidProperty {
        key: key.as_bytes().to_vec(),
        r#type: name_of(ty),
    }
}

/// Rust's index panic text, raised as an error (RULES 6).
fn stale_index(len: usize, index: usize) -> Fault {
    Fault::runtime(format!(
        "index out of bounds: the len is {len} but the index is {index}"
    ))
}

fn stale_insert(index: usize, len: usize) -> Fault {
    Fault::runtime(format!("insertion index (is {index}) should be <= len (is {len})"))
}

/// 1-based `index` in `[start, end]` (sfi-core's text).
pub(crate) fn check_range(index: usize, start: usize, end: usize) -> Result<(), Fault> {
    if index == 0 || index > end {
        Err(Fault::OutOfBounds {
            index: index as u64,
            start: start as u64,
            end: end as u64,
        })
    } else {
        Ok(())
    }
}

fn check_static(node: &Node) -> Result<(), Fault> {
    if node.is_static_array() {
        Err(Fault::StaticArray)
    } else {
        Ok(())
    }
}

/// MappedStructValue::get_field: the cache, else a lazy decode, cached when strings or userdata.
fn mstruct_get(
    data: &MappedStruct,
    cache: &RefCell<HashMap<String, Slot>>,
    key: &str,
    env: &dyn Env,
) -> Result<Val, Fault> {
    if let Some(slot) = cache.borrow().get(key) {
        return Ok(slot.val.clone());
    }
    let decoded = lazily(|| {
        check_field_chain(data, key)?;
        match data.get(key).map_err(|e| Fault::Runtime(e.to_string()))? {
            Some(value) => mapped_to_val(&value, env).map(Some),
            None => Ok(None),
        }
    })?;
    match decoded {
        Some(val) => {
            if val.cacheable() {
                cache.borrow_mut().insert(key.to_string(), Slot::new(val.clone()));
            }
            Ok(val)
        }
        None => Ok(Val::Nil),
    }
}

/// MappedArrayValue::get_field: every element is cached once read, scalars included.
fn marray_get(data: &MappedArray, entries: &RefCell<Vec<Entry>>, key: usize, env: &dyn Env) -> Result<Val, Fault> {
    let original = match entries.borrow().get(key) {
        None => return Ok(Val::Nil),
        Some(Entry::Value(slot)) => return Ok(slot.val.clone()),
        Some(Entry::Uninit(original)) => *original,
    };
    let decoded = lazily(
        || match data.get(original).map_err(|e| Fault::Runtime(e.to_string()))? {
            Some(value) => mapped_to_val(&value, env).map(Some),
            None => Ok(None),
        },
    )?;
    let Some(val) = decoded else { return Ok(Val::Nil) };
    if let Some(entry @ Entry::Uninit(_)) = entries.borrow_mut().get_mut(key) {
        *entry = Entry::Value(Slot::new(val.clone()));
    }
    Ok(val)
}

/// The codec's `value_to_lua`: decoded values as graph values (containers as mapped nodes, bitmasks as owned arrays).
pub(crate) fn mapped_to_val(value: &MappedValue, env: &dyn Env) -> Result<Val, Fault> {
    Ok(match value {
        MappedValue::None => Val::Nil,
        MappedValue::Bool(b) => Val::Bool(*b),
        MappedValue::UInt8(v) => Val::Int(*v as i64),
        MappedValue::SInt8(v) => Val::Int(*v as i64),
        MappedValue::UInt16(v) => Val::Int(*v as i64),
        MappedValue::SInt16(v) => Val::Int(*v as i64),
        MappedValue::UInt32(v) => Val::Int(*v as i64),
        MappedValue::SInt32(v) => Val::Int(*v as i64),
        MappedValue::UInt64(v) => Val::Int(*v as i64),
        MappedValue::SInt64(v) => Val::Int(*v),
        MappedValue::Float32(v) => Val::Num(*v as f64),
        MappedValue::Float64(v) => Val::Num(*v),
        MappedValue::Enum(v) => match v.name() {
            Some(name) => Val::str(name),
            None => Val::str(&v.value().to_string()),
        },
        MappedValue::Bitmask(v) => {
            let bits = v
                .iter()
                .map(|bit| {
                    Slot::new(match bit.name() {
                        Some(name) => Val::str(name),
                        None => Val::str(&bit.value().to_string()),
                    })
                })
                .collect();
            let ty = v.r#type().clone();
            let inner = inner_type(&ty, "array type must have an inner type")?;
            Val::Node(Node::new(
                env,
                Body::Array {
                    ty,
                    inner,
                    items: RefCell::new(bits),
                },
            ))
        }
        MappedValue::Struct(v) => {
            chain(v.r#type())?;
            Val::Node(Node::new(
                env,
                Body::MStruct {
                    data: v.clone(),
                    cache: Default::default(),
                    dirty: Cell::new(false),
                },
            ))
        }
        MappedValue::Array(v) => {
            chain(v.r#type())?;
            // one entry per element: kfc refused a stored count the bytes cannot hold before mapping it (eml-array-count)
            let entries = (0..v.len()).map(Entry::Uninit).collect();
            Val::Node(Node::new(
                env,
                Body::MArray {
                    data: v.clone(),
                    entries: RefCell::new(entries),
                    dirty: Cell::new(false),
                },
            ))
        }
        MappedValue::Variant(v) => {
            chain(v.base_type())?;
            chain(v.variant_type())?;
            Val::Node(Node::new(
                env,
                Body::MVariant {
                    data: v.clone(),
                    value: RefCell::new(None),
                    dirty: Cell::new(false),
                },
            ))
        }
        MappedValue::String(s) => Val::str(s.as_str().map_err(|e| Fault::Runtime(e.to_string()))?),
        MappedValue::Optional(v) => match v.value() {
            Some(inner) => mapped_to_val(inner, env)?,
            None => Val::Nil,
        },
        MappedValue::Guid(guid) => Val::str(&guid.to_string()),
        MappedValue::Reference(reference) => Val::str(&reference.guid().to_string()),
    })
}

// ---- clone, dirtiness ----

/// SF `try_clone_lua_value`: typed nodes deep-cloned per class (shared borrows, as `ud.borrow`), scalars as they are.
pub(crate) fn clone_val(val: &Val, env: &dyn Env, depth: usize) -> Result<Val, Fault> {
    match val {
        Val::Node(node) if node.scalar_val().is_none() => Ok(Val::Node(clone_node(node, env, depth)?)),
        // a scalar node is never stored in a slot; its value is
        Val::Node(node) => Ok(node.scalar_val().cloned().unwrap_or(Val::Nil)),
        other => Ok(other.clone()),
    }
}

fn clone_slot(slot: &Slot, env: &dyn Env, depth: usize) -> Result<Slot, Fault> {
    Ok(Slot::new(clone_val(&slot.val, env, depth)?))
}

pub(crate) fn clone_node(node: &Rc<Node>, env: &dyn Env, depth: usize) -> Result<Rc<Node>, Fault> {
    if depth >= MAX_DEPTH {
        return Err(too_deep());
    }
    let _borrow = node.borrow.shared()?;
    let next = depth + 1;
    let body = match &node.body {
        Body::Scalar(val) => Body::Scalar(val.clone()),
        Body::Struct { ty, fields } => {
            // snapshot first: a clone never holds a field map across nested clones
            let entries: Vec<(String, Val)> = fields
                .borrow()
                .iter()
                .map(|(k, s)| (k.clone(), s.val.clone()))
                .collect();
            let mut cloned = HashMap::with_capacity(entries.len());
            for (key, val) in entries {
                cloned.insert(key, Slot::new(clone_val(&val, env, next)?));
            }
            Body::Struct {
                ty: ty.clone(),
                fields: RefCell::new(cloned),
            }
        }
        Body::Array { ty, inner, items } => {
            let vals: Vec<Val> = items.borrow().iter().map(|s| s.val.clone()).collect();
            let mut cloned = Vec::with_capacity(vals.len());
            for val in vals {
                cloned.push(Slot::new(clone_val(&val, env, next)?));
            }
            Body::Array {
                ty: ty.clone(),
                inner: inner.clone(),
                items: RefCell::new(cloned),
            }
        }
        Body::Variant { ty, variant, value } => {
            let val = value.borrow().val.clone();
            Body::Variant {
                ty: ty.clone(),
                variant: variant.clone(),
                value: RefCell::new(Slot::new(clone_val(&val, env, next)?)),
            }
        }
        // mapped clones keep the original data and dirtiness, deep-clone cached values, keep uninitialized entries
        Body::MStruct { data, cache, dirty } => {
            let entries: Vec<(String, Val)> = cache.borrow().iter().map(|(k, s)| (k.clone(), s.val.clone())).collect();
            let mut cloned = HashMap::with_capacity(entries.len());
            for (key, val) in entries {
                cloned.insert(key, Slot::new(clone_val(&val, env, next)?));
            }
            Body::MStruct {
                data: data.clone(),
                cache: RefCell::new(cloned),
                dirty: Cell::new(dirty.get()),
            }
        }
        Body::MArray { data, entries, dirty } => {
            let snapshot: Vec<Result<Val, usize>> = entries
                .borrow()
                .iter()
                .map(|e| match e {
                    Entry::Value(slot) => Ok(slot.val.clone()),
                    Entry::Uninit(i) => Err(*i),
                })
                .collect();
            let mut cloned = Vec::with_capacity(snapshot.len());
            for entry in snapshot {
                cloned.push(match entry {
                    Ok(val) => Entry::Value(Slot::new(clone_val(&val, env, next)?)),
                    Err(original) => Entry::Uninit(original),
                });
            }
            Body::MArray {
                data: data.clone(),
                entries: RefCell::new(cloned),
                dirty: Cell::new(dirty.get()),
            }
        }
        Body::MVariant { data, value, dirty } => {
            let val = value.borrow().as_ref().map(|s| s.val.clone());
            let cloned = match val {
                Some(val) => Some(clone_slot(&Slot::new(val), env, next)?),
                None => None,
            };
            Body::MVariant {
                data: data.clone(),
                value: RefCell::new(cloned),
                dirty: Cell::new(dirty.get()),
            }
        }
    };
    Ok(Node::new(env, body))
}

/// SF `is_dirty_lua_value`: owned nodes always, mapped ones when edited, recursively through their caches.
pub(crate) fn is_dirty(val: &Val, depth: usize) -> Result<bool, Fault> {
    let Val::Node(node) = val else { return Ok(false) };
    if depth >= MAX_DEPTH {
        return Err(too_deep());
    }
    let _borrow = node.borrow.shared()?;
    match &node.body {
        Body::Scalar(_) => Ok(false),
        Body::Struct { .. } | Body::Array { .. } | Body::Variant { .. } => Ok(true),
        Body::MStruct { cache, dirty, .. } => {
            if dirty.get() {
                return Ok(true);
            }
            let vals: Vec<Val> = cache.borrow().values().map(|s| s.val.clone()).collect();
            for val in vals {
                if is_dirty(&val, depth + 1)? {
                    return Ok(true);
                }
            }
            Ok(false)
        }
        Body::MArray { entries, dirty, .. } => {
            if dirty.get() {
                return Ok(true);
            }
            let vals: Vec<Val> = entries
                .borrow()
                .iter()
                .filter_map(|e| match e {
                    Entry::Value(slot) => Some(slot.val.clone()),
                    Entry::Uninit(_) => None,
                })
                .collect();
            for val in vals {
                if is_dirty(&val, depth + 1)? {
                    return Ok(true);
                }
            }
            Ok(false)
        }
        Body::MVariant { value, dirty, .. } => {
            if dirty.get() {
                return Ok(true);
            }
            let val = value.borrow().as_ref().map(|s| s.val.clone());
            match val {
                Some(val) => is_dirty(&val, depth + 1),
                None => Ok(false),
            }
        }
    }
}

// ---- iteration (Source `pairs`) ----

/// One `pairs` iteration: owned structs walk the type's fields, mapped structs the mapped keys, owned arrays the
/// CURRENT length, mapped arrays the ORIGINAL length through the live cache (SF quirk, kept).
pub(crate) struct Iter {
    pub(crate) node: Rc<Node>,
    keys: Vec<String>,
    next: Cell<usize>,
}

impl Iter {
    pub(crate) fn begin(node: &Rc<Node>) -> Result<Option<Self>, Fault> {
        let keys = match &node.body {
            Body::Struct { ty, .. } => {
                chain(ty)?;
                ty.iter_fields().map(|f| f.name.clone()).collect()
            }
            Body::MStruct { data, .. } => lazily(|| Ok(data.iter_keys().map(str::to_owned).collect()))?,
            Body::Array { .. } | Body::MArray { .. } => Vec::new(),
            _ => return Ok(None),
        };
        Ok(Some(Self {
            node: node.clone(),
            keys,
            next: Cell::new(0),
        }))
    }

    /// The next (key, value, where the value lives); key Nil at the end.
    pub(crate) fn step(&self, env: &dyn Env) -> Result<(Val, Val, Option<SlotRef>), Fault> {
        let i = self.next.get();
        match &self.node.body {
            Body::Struct { fields, .. } => {
                let Some(key) = self.keys.get(i) else {
                    return Ok((Val::Nil, Val::Nil, None));
                };
                self.next.set(i + 1);
                let val = fields.borrow().get(key).map_or(Val::Nil, |s| s.val.clone());
                Ok((Val::str(key), val, Some(SlotRef::Field(key.clone()))))
            }
            Body::MStruct { data, cache, .. } => {
                let Some(key) = self.keys.get(i) else {
                    return Ok((Val::Nil, Val::Nil, None));
                };
                self.next.set(i + 1);
                let val = mstruct_get(data, cache, key, env)?;
                Ok((Val::str(key), val, Some(SlotRef::Field(key.clone()))))
            }
            Body::Array { items, .. } => {
                let val = match items.borrow().get(i) {
                    Some(slot) => slot.val.clone(),
                    None => return Ok((Val::Nil, Val::Nil, None)),
                };
                self.next.set(i + 1);
                Ok((Val::Int(i as i64 + 1), val, Some(SlotRef::Index(i + 1))))
            }
            Body::MArray { data, entries, .. } => {
                if i >= data.len() {
                    return Ok((Val::Nil, Val::Nil, None));
                }
                let val = marray_get(data, entries, i, env)?;
                self.next.set(i + 1);
                Ok((Val::Int(i as i64 + 1), val, Some(SlotRef::Index(i + 1))))
            }
            _ => Ok((Val::Nil, Val::Nil, None)),
        }
    }
}

// ---- __tostring ----

/// mlua's `String::to_str` (mlua 0.11.6 string.rs:305-318), as Source's `to_string` applies it: the bytes when they are
/// UTF-8, else mlua's `FromLuaConversionError` text.
pub(crate) fn utf8_text(bytes: &[u8]) -> Result<&[u8], Fault> {
    std::str::from_utf8(bytes)
        .map(str::as_bytes)
        .map_err(|e| Fault::external(format!("error converting Lua string to &str ({e})")))
}

/// Source's `LuaValue::to_string` of a graph value: mlua 0.11.6's `Value::to_string` (value.rs:154-187), which SF's
/// `__tostring` bodies and incompatible-type texts call. A number is Rust's `Display` of the f64 (`1`, `0.5`,
/// `0.30000000000000004`, `NaN`), not Lua's `%.14g`; a string must be UTF-8; a nested typed value goes through its own
/// `__tostring`.
pub(crate) fn val_text(val: &Val, env: &dyn Env, depth: usize) -> Result<Vec<u8>, Fault> {
    Ok(match val {
        Val::Nil => b"nil".to_vec(),
        Val::Bool(b) => b.to_string().into_bytes(),
        Val::Int(i) => i.to_string().into_bytes(),
        Val::Num(n) => n.to_string().into_bytes(),
        Val::Str(s) => utf8_text(s)?.to_vec(),
        Val::Node(node) => match node_text(node, env, depth)? {
            Some(text) => text,
            None => match node.scalar_val() {
                Some(val) => val_text(val, env, depth)?,
                None => env.userdata_text(node)?,
            },
        },
    })
}

/// SF `push_display`: strings quoted (each value through `to_string`, so a string that is not UTF-8 is an error).
fn push_display(out: &mut Vec<u8>, val: &Val, env: &dyn Env, depth: usize) -> Result<(), Fault> {
    if let Val::Str(s) = val {
        out.push(b'"');
        out.extend_from_slice(utf8_text(s)?);
        out.push(b'"');
    } else {
        out.extend(val_text(val, env, depth)?);
    }
    Ok(())
}

/// A struct's or array's `__tostring`; None for nodes Source gives none (variants, scalars).
pub(crate) fn node_text(node: &Rc<Node>, env: &dyn Env, depth: usize) -> Result<Option<Vec<u8>>, Fault> {
    if depth >= MAX_DEPTH {
        return Err(too_deep());
    }
    let next = depth + 1;
    let _borrow = match &node.body {
        Body::Struct { .. } | Body::MStruct { .. } | Body::Array { .. } | Body::MArray { .. } => {
            node.borrow.shared()?
        }
        _ => return Ok(None),
    };
    let mut out = Vec::new();
    match &node.body {
        Body::Struct { fields, .. } => {
            // SF prints its field map's own order (a hash map's: unstable in both)
            let entries: Vec<(String, Val)> = fields
                .borrow()
                .iter()
                .map(|(k, s)| (k.clone(), s.val.clone()))
                .collect();
            out.push(b'{');
            for (i, (key, val)) in entries.iter().enumerate() {
                if i > 0 {
                    out.push(b',');
                }
                out.push(b'"');
                out.extend_from_slice(key.as_bytes());
                out.extend_from_slice(b"\":");
                push_display(&mut out, val, env, next)?;
            }
            out.push(b'}');
        }
        Body::MStruct { data, cache, .. } => {
            chain(data.r#type())?;
            let keys: Vec<String> = data.r#type().iter_fields().map(|f| f.name.clone()).collect();
            out.push(b'{');
            for (i, key) in keys.iter().enumerate() {
                if i > 0 {
                    out.push(b',');
                }
                let cached = cache.borrow().get(key).map(|s| s.val.clone());
                // uncached fields decode for the text only, never into the cache
                let val = match cached {
                    Some(val) => val,
                    None => lazily(|| {
                        check_field_chain(data, key)?;
                        match data.get(key).map_err(|e| Fault::Runtime(e.to_string()))? {
                            Some(value) => mapped_to_val(&value, env),
                            None => Err(Fault::runtime("expected a valid field in struct")),
                        }
                    })?,
                };
                out.push(b'"');
                out.extend_from_slice(key.as_bytes());
                out.extend_from_slice(b"\":");
                push_display(&mut out, &val, env, next)?;
            }
            out.push(b'}');
        }
        Body::Array { items, .. } => {
            let vals: Vec<Val> = items.borrow().iter().map(|s| s.val.clone()).collect();
            out.push(b'[');
            for (i, val) in vals.iter().enumerate() {
                if i > 0 {
                    out.push(b',');
                }
                push_display(&mut out, val, env, next)?;
            }
            out.push(b']');
        }
        Body::MArray { data, entries, .. } => {
            let snapshot: Vec<Result<Val, usize>> = entries
                .borrow()
                .iter()
                .map(|e| match e {
                    Entry::Value(slot) => Ok(slot.val.clone()),
                    Entry::Uninit(i) => Err(*i),
                })
                .collect();
            out.push(b'[');
            for (i, entry) in snapshot.into_iter().enumerate() {
                if i > 0 {
                    out.push(b',');
                }
                let val = match entry {
                    Ok(val) => val,
                    Err(original) => {
                        lazily(
                            || match data.get(original).map_err(|e| Fault::Runtime(e.to_string()))? {
                                Some(value) => mapped_to_val(&value, env),
                                None => Err(Fault::runtime("expected a valid field in struct")),
                            },
                        )?
                    }
                };
                push_display(&mut out, &val, env, next)?;
            }
            out.push(b']');
        }
        _ => unreachable!(),
    }
    Ok(Some(out))
}

// ---- conversion to kfc values (encode side) ----

/// The codec's `Converter` over graph values: SF `convert_lua_to_value` of an already validated value.
pub(crate) struct Converter<'a> {
    pub(crate) path: TreePath,
    env: &'a dyn Env,
    depth: usize,
}

impl<'a> Converter<'a> {
    pub(crate) fn convert(val: &Val, ty: &TypeHandle, env: &'a dyn Env) -> Result<Value, Fault> {
        let mut converter = Self {
            path: TreePath::new(),
            env,
            depth: 0,
        };
        let result = std::panic::catch_unwind(AssertUnwindSafe(|| converter.process(val, ty)))
            .unwrap_or_else(|panic| Err(CheckKind::Lua(Box::new(Fault::Runtime(panic_text(panic))))));
        result.map_err(|kind| Fault::Conversion {
            path: converter.path.to_string(),
            kind,
        })
    }

    fn process(&mut self, val: &Val, ty: &TypeHandle) -> Result<Value, CheckKind> {
        if self.depth >= MAX_DEPTH {
            return Err(lua(too_deep()));
        }
        self.depth += 1;
        let result = self.process_level(val, ty);
        self.depth -= 1;
        result
    }

    fn incompatible(&self, expected: &str, val: &Val) -> CheckKind {
        match val_text(val, self.env, self.depth) {
            Ok(found) => CheckKind::IncompatibleType {
                expected: expected.to_owned(),
                found: Found::Text(found),
            },
            Err(fault) => lua(fault),
        }
    }

    fn process_level(&mut self, val: &Val, ty: &TypeHandle) -> Result<Value, CheckKind> {
        // a scalar node converts as its value
        if let Val::Node(node) = val
            && let Some(scalar) = node.scalar_val()
        {
            let scalar = scalar.clone();
            return self.process_level(&scalar, ty);
        }
        Ok(match ty.primitive_type {
            PrimitiveType::None => match val {
                Val::Nil => Value::None,
                _ => return Err(self.incompatible("nil", val)),
            },
            PrimitiveType::Bool => match val {
                Val::Bool(b) => (*b).into(),
                _ => return Err(self.incompatible("boolean", val)),
            },
            PrimitiveType::UInt8 => self.uint(val, u8::MAX as u64)?.into(),
            PrimitiveType::SInt8 => self.sint(val, i8::MIN as i64, i8::MAX as i64)?.into(),
            PrimitiveType::UInt16 => self.uint(val, u16::MAX as u64)?.into(),
            PrimitiveType::SInt16 => self.sint(val, i16::MIN as i64, i16::MAX as i64)?.into(),
            PrimitiveType::UInt32 => self.uint(val, u32::MAX as u64)?.into(),
            PrimitiveType::SInt32 => self.sint(val, i32::MIN as i64, i32::MAX as i64)?.into(),
            PrimitiveType::UInt64 => self.uint(val, u64::MAX)?.into(),
            PrimitiveType::SInt64 => self.sint(val, i64::MIN, i64::MAX)?.into(),
            PrimitiveType::Float32 | PrimitiveType::Float64 => match val {
                Val::Int(i) => (*i as f64).into(),
                Val::Num(n) => (*n).into(),
                _ => return Err(self.incompatible("float", val)),
            },
            PrimitiveType::Enum => {
                match val {
                    Val::Str(s) => {
                        if let Some(field) = ty.enum_fields.get(&*String::from_utf8_lossy(s)) {
                            return Ok(Value::String(field.name.clone()));
                        }
                    }
                    Val::Int(i) => {
                        let i = *i as u64;
                        if let Some(field) = ty.enum_fields.values().find(|f| f.value == i) {
                            return Ok(Value::String(field.name.clone()));
                        }
                    }
                    _ => {}
                }
                let names = ty
                    .enum_fields
                    .keys()
                    .map(|k| format!("\"{k}\""))
                    .collect::<Vec<_>>()
                    .join(", ");
                return Err(self.incompatible(&format!("one of [{names}]"), val));
            }
            PrimitiveType::Bitmask8
            | PrimitiveType::Bitmask16
            | PrimitiveType::Bitmask32
            | PrimitiveType::Bitmask64
            | PrimitiveType::StaticArray
            | PrimitiveType::BlobArray => self.container(val, ty, ObjectKind::Array)?,
            PrimitiveType::Typedef => {
                let inner = ty.inner_type().ok_or_else(|| runtime("invalid typedef type"))?;
                self.process(val, &inner)?
            }
            PrimitiveType::Struct => self.container(val, ty, ObjectKind::Struct)?,
            PrimitiveType::DsArray | PrimitiveType::DsString | PrimitiveType::DsOptional | PrimitiveType::DsVariant => {
                return Err(runtime("not yet implemented"));
            }
            PrimitiveType::BlobString => match val {
                Val::Str(s) => Value::String(String::from_utf8_lossy(s).into_owned()),
                _ => return Err(self.incompatible("string", val)),
            },
            PrimitiveType::BlobOptional => match val {
                Val::Nil => Value::None,
                _ => {
                    let inner = ty.inner_type().ok_or_else(|| runtime("invalid optional type"))?;
                    self.process(val, &inner)?
                }
            },
            PrimitiveType::BlobVariant => self.container(val, ty, ObjectKind::Variant)?,
            PrimitiveType::ObjectReference => self.guid(val, "object reference")?,
            PrimitiveType::Guid => self.guid(val, "guid")?,
        })
    }

    fn uint(&self, val: &Val, max: u64) -> Result<u64, CheckKind> {
        match val {
            Val::Int(i) if *i as u64 <= max => Ok(*i as u64),
            Val::Num(n) if n.fract() == 0.0 && *n >= 0.0 && *n <= max as f64 => Ok(*n as u64),
            _ => Err(self.incompatible(&format!("unsigned integer in range [0, {max}]"), val)),
        }
    }

    fn sint(&self, val: &Val, min: i64, max: i64) -> Result<i64, CheckKind> {
        match val {
            Val::Int(i) if *i >= min && *i <= max => Ok(*i),
            Val::Num(n) if n.fract() == 0.0 && *n >= min as f64 && *n <= max as f64 => Ok(*n as i64),
            _ => Err(self.incompatible(&format!("signed integer in range [{min}, {max}]"), val)),
        }
    }

    fn guid(&self, val: &Val, expected: &str) -> Result<Value, CheckKind> {
        match val {
            Val::Str(s) => {
                let text = String::from_utf8_lossy(s).into_owned();
                let guid = Guid::parse(&text).ok_or(CheckKind::InvalidGuid(text))?;
                Ok(Value::Guid(guid))
            }
            _ => Err(self.incompatible(expected, val)),
        }
    }

    fn container(&mut self, val: &Val, ty: &TypeHandle, kind: ObjectKind) -> Result<Value, CheckKind> {
        let Val::Node(node) = val else {
            return Err(self.incompatible(&name_of(ty), val));
        };
        match node.classify() {
            Some((found, object_type)) if found == kind => {
                if **object_type != **ty {
                    return Err(CheckKind::IncompatibleType {
                        expected: name_of(ty),
                        found: Found::Text(name_of(object_type).into_bytes()),
                    });
                }
                let node = node.clone();
                self.node(&node)
            }
            _ => Err(CheckKind::IncompatibleType {
                expected: name_of(ty),
                found: Found::Userdata {
                    kind: UserdataKind::TypedValue,
                    adapter: Adapter::None,
                    value_kind: Some(node.kind()),
                },
            }),
        }
    }

    fn node(&mut self, node: &Rc<Node>) -> Result<Value, CheckKind> {
        let _borrow = node.borrow.shared().map_err(lua)?;
        match &node.body {
            Body::Struct { ty, fields } => {
                chain(ty).map_err(lua)?;
                let entries: Vec<(String, Val)> = fields
                    .borrow()
                    .iter()
                    .map(|(k, s)| (k.clone(), s.val.clone()))
                    .collect();
                let mut result: Vec<(String, Value)> = Vec::new();
                for (key, val) in entries {
                    self.path.push(&key);
                    let field_type = ty
                        .get_field_type(&key)
                        .ok_or_else(|| runtime("field type must be present"))?;
                    let converted = self.process(&val, &field_type)?;
                    insert_ordered(&mut result, key, converted);
                    self.path.pop();
                }
                Ok(Value::Struct(Box::new(result.into_iter().collect())))
            }
            Body::Array { ty, items, .. } => {
                let element = ty
                    .inner_type()
                    .ok_or_else(|| runtime("array type must have an inner type"))?;
                let vals: Vec<Val> = items.borrow().iter().map(|s| s.val.clone()).collect();
                let mut result = Vec::with_capacity(vals.len());
                for (i, val) in vals.iter().enumerate() {
                    self.path.push_index(i);
                    result.push(self.process(val, &element)?);
                    self.path.pop();
                }
                Ok(result.into())
            }
            Body::Variant { variant, value, .. } => {
                let val = value.borrow().val.clone();
                self.variant(variant, &val)
            }
            Body::MStruct { data, cache, .. } => {
                let mut result: Vec<(String, Value)> = Vec::new();
                for field in data.r#type().iter_fields() {
                    let key = field.name.as_str();
                    self.path.push(key);
                    let cached = cache.borrow().get(key).map(|s| s.val.clone());
                    let value = match cached {
                        Some(val) => {
                            let field_type = field_type(data.r#type(), key).map_err(|e| lua(e.into()))?;
                            self.process(&val, &field_type)?
                        }
                        None => {
                            check_field_chain(data, key).map_err(lua)?;
                            let original = data
                                .get(key)
                                .map_err(|e| CheckKind::Mapping(e.to_string()))?
                                .ok_or_else(|| runtime("expected a valid field in struct"))?;
                            // SF: kfc's Value::from, unbounded on crafted bytes; the codec's bounded walk
                            mapped_to_value(original, data.data().len()).map_err(CheckKind::Mapping)?
                        }
                    };
                    insert_ordered(&mut result, key.to_owned(), value);
                    self.path.pop();
                }
                Ok(Value::Struct(Box::new(result.into_iter().collect())))
            }
            Body::MArray { data, entries, .. } => {
                let snapshot: Vec<Result<Val, usize>> = entries
                    .borrow()
                    .iter()
                    .map(|e| match e {
                        Entry::Value(slot) => Ok(slot.val.clone()),
                        Entry::Uninit(i) => Err(*i),
                    })
                    .collect();
                let mut result = Vec::with_capacity(snapshot.len());
                for entry in snapshot {
                    match entry {
                        Err(original) => {
                            let original = data
                                .get(original)
                                .map_err(|e| CheckKind::Mapping(e.to_string()))?
                                .ok_or_else(|| runtime("expected a value in the array"))?;
                            result.push(mapped_to_value(original, data.data().len()).map_err(CheckKind::Mapping)?);
                        }
                        // SF `expect`s here (no path entry): its panic text, raised
                        Ok(val) => match self.process(&val, data.element_type()) {
                            Ok(value) => result.push(value),
                            Err(error) => {
                                return Err(runtime(&format!(
                                    "failed to convert modified value: {}",
                                    debug_kind(&error)
                                )));
                            }
                        },
                    }
                }
                Ok(result.into())
            }
            Body::MVariant { data, value, .. } => {
                let val = value.borrow().as_ref().map(|s| s.val.clone());
                match val {
                    Some(val) => self.variant(data.variant_type(), &val),
                    None => {
                        mapped_to_value(MappedValue::Variant(data.clone()), data.value().data().len())
                            .map_err(CheckKind::Mapping)
                    }
                }
            }
            Body::Scalar(_) => Err(runtime("convert_object called with a userdata of no value class")),
        }
    }

    fn variant(&mut self, variant: &TypeHandle, val: &Val) -> Result<Value, CheckKind> {
        let value = self
            .process(val, variant)?
            .into_struct()
            .ok_or_else(|| runtime("variant value must be convertible to struct"))?;
        Ok(Variant {
            type_index: variant.index(),
            value,
        }
        .into())
    }
}

fn insert_ordered(fields: &mut Vec<(String, Value)>, key: String, value: Value) {
    match fields.iter_mut().find(|(k, _)| *k == key) {
        Some(slot) => slot.1 = value,
        None => fields.push((key, value)),
    }
}

fn lua(fault: Fault) -> CheckKind {
    CheckKind::Lua(Box::new(fault))
}

fn runtime(text: &str) -> CheckKind {
    lua(Fault::runtime(text))
}

/// SF's `{error:?}` of a `LuaConversionErrorKind` (derive(Debug)) for the shapes it can render exactly.
fn debug_kind(kind: &CheckKind) -> String {
    match kind {
        CheckKind::IncompatibleType { expected, found } => {
            let found: &[u8] = match found {
                Found::Text(text) => text,
                Found::Userdata { .. } => super::lifetime::USERDATA_LABEL,
            };
            let found = String::from_utf8_lossy(found).into_owned();
            format!("IncompatibleType {{ expected: {expected:?}, found: {found:?} }}")
        }
        CheckKind::InvalidGuid(guid) => format!("InvalidGuid({guid:?})"),
        CheckKind::Lua(fault) => format!("Lua({:?})", fault.text()),
        other => format!("{:?}", String::from_utf8_lossy(&other.message_for_debug())),
    }
}

impl CheckKind {
    fn message_for_debug(&self) -> Vec<u8> {
        match self {
            CheckKind::Mapping(text) => text.clone().into_bytes(),
            CheckKind::MissingField { field, r#type } => format!("MissingField {field} {type}").into_bytes(),
            CheckKind::InvalidType(name) => name.clone().into_bytes(),
            _ => Vec::new(),
        }
    }
}

/// A neutral `KilnContentHash` record (16 bytes, LE) as a content hash.
pub(crate) fn content_hash(record: &[u8]) -> Option<ContentHash> {
    let word = |i: usize| -> Option<u32> { Some(u32::from_le_bytes(record.get(i * 4..i * 4 + 4)?.try_into().ok()?)) };
    if record.len() != 16 {
        return None;
    }
    Some(ContentHash::new(word(0)?, word(1)?, word(2)?, word(3)?))
}

/// A neutral `KilnAssetKey` record (24 bytes) as its GUID (game byte order).
pub(crate) fn resource_guid(record: &[u8]) -> Option<Guid> {
    if record.len() != 24 {
        return None;
    }
    let bytes: [u8; 16] = record[..16].try_into().ok()?;
    Some(Guid::from(bytes))
}
