// SPDX-License-Identifier: GPL-3.0-or-later
//! Lifetimes of the typed graph: the protected host reader the graph calls, the host-object pins that mirror
//! where ShroudForge keeps a `LuaValue`, the per-node Source borrow flag, and the Source errors an operation returns.
//!
//! # Host objects and pins
//! The graph never holds a Lua value. A host (the MIT frontend's Lua VM, through the C table's adapter of
//! `KilnEmlLuaReaderV1`) answers protected requests with opaque object tokens that live until the outermost service
//! call ends. Where Source keeps a `LuaValue` alive (a ResourceInfo's cached root, an owned or mapped container's
//! cached child, a value a removal hands back), the graph retains the token with [`LuaHost::retain`] as a [`Pin`].
//! Dropping a pin never calls the host: it queues the token, and the arena releases queued tokens once, on its worker,
//! when no service call is in progress (`arena::Scope`), so a release can never run under an outer operation.
//!
//! A node never pins its own proxy and never points at its parent; a parent's slot pins its child's proxy, and the
//! proxy's adopted lease keeps the child node. Lua's collector therefore frees a detached subtree one level per cycle,
//! and nothing forms a cycle through the graph.
//!
//! # Borrows
//! Source wraps each value in mlua userdata and borrows it per metamethod: the structural array operations borrow it
//! mutably for the whole call (validation included), `__newindex`, `__index`, `#`, `__tostring`, `pairs` and cloning
//! borrow it shared, and a variant's `value` setter borrows mutably. [`BorrowFlag`] keeps exactly those modes per
//! node, so a callback may re-enter any other node, and the same node as far as Source allows; a conflict is Source's
//! borrow error as a normal result, never a service lock.
use std::{
    cell::{Cell, RefCell},
    fmt,
    rc::Rc,
    sync::{
        Arc,
        atomic::{AtomicUsize, Ordering},
    },
};

use kiln_abi::{
    KILN_ERR_INTERNAL, KILN_ERR_INVALID_ARGUMENT, KILN_ERR_NOT_FOUND, KILN_ERR_REVOKED, KILN_ERR_UNAVAILABLE,
    KILN_ERR_WRONG_THREAD, KilnResult,
};

/// One protected request to the host (`KilnEmlLuaAccessV1`). Each executes exactly once, on the arena's worker.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Access<'a> {
    /// The object's kind and scalar bits.
    Describe { object: u64 },
    /// `Table::get(key)`: metamethods run once.
    GetFieldValue { object: u64, key: &'a [u8] },
    /// `Table::get::<String>(key)`: the get, then Source's string conversion.
    GetFieldString { object: u64, key: &'a [u8] },
    /// `Table::len()`: `__len` runs on every request, never memoized.
    Length { object: u64 },
    /// `rawgeti(object, index)`, as `sequence_values` reads.
    RawSequenceIndex { object: u64, index: i64 },
    /// Source's `LuaValue::to_string` (`__tostring` runs once); the reply is a STRING object.
    SourceToString { object: u64 },
    /// The frontend userdata for a node: interned weakly by arena and node; a new proxy adopts `provisional`.
    MakeTypedProxy { provisional: u64, arena: u64, node_id: u64 },
}

impl Access<'_> {
    /// The `KILN_LUA_*` operation name, for diagnostics.
    pub fn name(&self) -> &'static str {
        match self {
            Access::Describe { .. } => "DESCRIBE",
            Access::GetFieldValue { .. } => "GET_FIELD_VALUE",
            Access::GetFieldString { .. } => "GET_FIELD_STRING",
            Access::Length { .. } => "LENGTH",
            Access::RawSequenceIndex { .. } => "RAW_SEQUENCE_INDEX",
            Access::SourceToString { .. } => "SOURCE_TO_STRING",
            Access::MakeTypedProxy { .. } => "MAKE_TYPED_PROXY",
        }
    }
}

/// `KILN_LUA_*` kinds.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum HostKind {
    Nil,
    Boolean,
    Integer,
    Number,
    String,
    Table,
    TypedValue,
    KnownUserdata,
    OtherUserdata,
    Other,
}

/// `KILN_LUA_ADAPTER_*`: the neutral identities of userdata the codec accepts.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Adapter {
    None,
    Guid,
    Content,
    Resource,
}

/// `KILN_LUA_LEASE_*`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LeaseDisposition {
    None,
    Adopted,
    Reused,
}

/// One reply (`KilnEmlLuaValueV1`).  A nonzero `error_object` means the request raised that Lua error (protected);
/// the other fields are then unspecified. Hosts give every value an object token (scalars included), valid until the
/// outermost service call ends.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct HostValue {
    pub kind: HostKind,
    pub object: u64,
    /// BOOLEAN 0/1, INTEGER two's complement, NUMBER IEEE bits.
    pub bits: u64,
    pub byte_length: u64,
    /// TYPED_VALUE: a value lease of the same arena.
    pub typed_value: u64,
    pub error_object: u64,
    pub adapter: Adapter,
    pub lease: LeaseDisposition,
}

impl HostValue {
    pub fn of(kind: HostKind, object: u64) -> Self {
        Self {
            kind,
            object,
            bits: 0,
            byte_length: 0,
            typed_value: 0,
            error_object: 0,
            adapter: Adapter::None,
            lease: LeaseDisposition::None,
        }
    }
    /// A reply that raised `error_object`.
    pub fn error(error_object: u64) -> Self {
        Self {
            error_object,
            ..Self::of(HostKind::Nil, 0)
        }
    }
}

/// The host refused a request itself (its KilnResult): a transport failure, never a Source error.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Transport(pub KilnResult);

/// The protected reader, as the graph uses it. The C table implements it over `KilnEmlLuaReaderV1`, checking the table's
/// size, version and member before every call. All four run on the arena's worker; `access` may run Lua or trigger
/// the host's collector, which may re-enter the service (`value_close`, identity-only `value_info`).
pub trait LuaHost {
    fn access(&self, access: &Access<'_>) -> Result<HostValue, Transport>;
    /// An immutable object's bytes (a string, an error's text, a known userdata's neutral record). Never re-runs a
    /// getter.
    fn copy(&self, object: u64) -> Result<Vec<u8>, Transport>;
    /// Keeps a temporary object past the current call.
    fn retain(&self, object: u64) -> Result<(), Transport>;
    /// Drops one retain. Never called from a drop or under an outer operation.
    fn release(&self, object: u64);
}

/// The host's any-thread signal (`reader.wake`): runs no Lua.
pub type Waker = Arc<dyn Fn() + Send + Sync>;

/// Host objects whose pins were dropped, released on the worker outside any service call. `retained` counts every
/// object retained and not yet released (live pins plus queued ones); it is shared with the arena's any-thread half.
pub(crate) struct Sink {
    queued: RefCell<Vec<u64>>,
    pub(crate) retained: Arc<AtomicUsize>,
}

impl Sink {
    pub(crate) fn new(retained: Arc<AtomicUsize>) -> Self {
        Self {
            queued: RefCell::new(Vec::new()),
            retained,
        }
    }
    pub(crate) fn take(&self) -> Vec<u64> {
        std::mem::take(&mut *self.queued.borrow_mut())
    }
    /// One queued object was released.
    pub(crate) fn released(&self) {
        self.retained.fetch_sub(1, Ordering::SeqCst);
    }
}

/// One retained host object (`LuaHost::retain`), released once after it is dropped.
pub(crate) struct Pin {
    object: u64,
    sink: Rc<Sink>,
}

impl Pin {
    /// Retains `object` for as long as the pin lives.
    pub(crate) fn retain(host: &dyn LuaHost, sink: &Rc<Sink>, object: u64) -> Result<Self, Transport> {
        host.retain(object)?;
        sink.retained.fetch_add(1, Ordering::SeqCst);
        Ok(Self {
            object,
            sink: sink.clone(),
        })
    }
    pub(crate) fn object(&self) -> u64 {
        self.object
    }
}

impl Drop for Pin {
    fn drop(&mut self) {
        // never the host from here: a drop can happen under an outer operation or a host callback
        self.sink.queued.borrow_mut().push(self.object);
    }
}

impl fmt::Debug for Pin {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "Pin({})", self.object)
    }
}

/// A node's Source borrow state: n > 0 shared borrows, -1 one mutable borrow.
#[derive(Default)]
pub(crate) struct BorrowFlag(Cell<isize>);

pub(crate) struct SharedBorrow<'a>(&'a BorrowFlag);
pub(crate) struct MutBorrow<'a>(&'a BorrowFlag);

impl BorrowFlag {
    /// mlua's `UserDataBorrowError` when the node is mutably borrowed.
    pub(crate) fn shared(&self) -> Result<SharedBorrow<'_>, Fault> {
        let n = self.0.get();
        if n < 0 {
            return Err(Fault::Borrow { mutable: false });
        }
        self.0.set(n + 1);
        Ok(SharedBorrow(self))
    }
    /// mlua's `UserDataBorrowMutError` when the node is borrowed at all.
    pub(crate) fn exclusive(&self) -> Result<MutBorrow<'_>, Fault> {
        if self.0.get() != 0 {
            return Err(Fault::Borrow { mutable: true });
        }
        self.0.set(-1);
        Ok(MutBorrow(self))
    }
}

impl Drop for SharedBorrow<'_> {
    fn drop(&mut self) {
        self.0.0.set(self.0.0.get() - 1);
    }
}

impl Drop for MutBorrow<'_> {
    fn drop(&mut self) {
        self.0.0.set(0);
    }
}

/// A Lua error the host returned (`error_object`), retained while an error lease holds it.
#[derive(Debug)]
pub struct HostError {
    /// None when the retain failed: the error is still reported, without `host_error`.
    pub(crate) pin: Option<Pin>,
    /// The host's text of it (`copy`): mlua's Display of the error.
    pub text: Vec<u8>,
}

impl HostError {
    /// The retained host object, 0 when the retain failed.
    pub fn object(&self) -> u64 {
        self.pin.as_ref().map_or(0, Pin::object)
    }
}

/// The class of a userdata Source found where it expected another (its `userdata `TypeId(..)`` label).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum UserdataKind {
    TypedValue,
    KnownUserdata,
    OtherUserdata,
}

/// What an incompatible-type error found.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Found {
    /// Source's text of the value (its `to_string`, or a type name).
    Text(Vec<u8>),
    /// A userdata Source labels by its Rust TypeId: the frontend renders that label. `value_kind` is the
    /// `KILN_VALUE_*` class of a typed value.
    Userdata {
        kind: UserdataKind,
        adapter: Adapter,
        value_kind: Option<u32>,
    },
}

/// One level of a validation or conversion error (the codec's `LuaValidationErrorKind`/`LuaConversionErrorKind`).
#[derive(Debug)]
pub enum CheckKind {
    IncompatibleType { expected: String, found: Found },
    MissingField { field: String, r#type: String },
    InvalidType(String),
    InvalidGuid(String),
    Lua(Box<Fault>),
    Mapping(String),
}

/// A Source error an operation returns as a normal result (SEMANTIC_ERROR), with each level's exact Source text.
#[derive(Debug)]
pub enum Fault {
    /// The complete text of a Source runtime error: `LuaError::generic` texts as they are, and SF panic texts raised as
    /// errors as the codec and the wrappers raise them (`mlua::Error::runtime`, so with mlua's `runtime error: ` prefix).
    Runtime(String),
    OutOfBounds {
        index: u64,
        start: u64,
        end: u64,
    },
    StaticArray,
    InvalidProperty {
        key: Vec<u8>,
        r#type: String,
    },
    /// mlua's borrow errors on the same node: `mutable` is the borrow that was refused.
    Borrow {
        mutable: bool,
    },
    /// The codec's validation error: "at {path}: {kind}".
    Validation {
        path: String,
        kind: CheckKind,
    },
    /// The codec's conversion error: "at {path}: {kind}".
    Conversion {
        path: String,
        kind: CheckKind,
    },
    /// An error the protected reader returned.
    Lua(HostError),
    /// A contained Kiln fault (a panic in Kiln-only code, a host transport failure): never presented as a Source text.
    Internal(String),
}

/// Source renders a userdata of no expected class as `userdata `TypeId(..)``; that text is the frontend's, so a message
/// carries this placeholder in its place (`found_userdata` says which userdata it was).
pub const USERDATA_LABEL: &[u8] = b"userdata `<frontend TypeId>`";

impl Fault {
    /// An SF panic text raised as an error, as the codec and the wrappers raise it (`mlua::Error::runtime`'s Display).
    pub(crate) fn runtime(text: impl AsRef<str>) -> Self {
        Self::Runtime(format!("runtime error: {}", text.as_ref()))
    }
    /// A `LuaError::generic`/`Error::external` text (its Display is the text).
    pub(crate) fn external(text: impl Into<String>) -> Self {
        Self::Runtime(text.into())
    }
    pub(crate) fn internal(text: impl Into<String>) -> Self {
        Self::Internal(text.into())
    }

    /// The complete Source Display text of this level.
    pub fn message(&self) -> Vec<u8> {
        match self {
            Fault::Runtime(text) | Fault::Internal(text) => text.clone().into_bytes(),
            Fault::OutOfBounds { index, start, end } => {
                format!("position out of bounds: expected index in range [{start}, {end}], got {index}").into_bytes()
            }
            Fault::StaticArray => b"attempt to modify static array".to_vec(),
            Fault::InvalidProperty { key, r#type } => {
                let mut text = b"attempt to set invalid property '".to_vec();
                text.extend_from_slice(key);
                text.extend_from_slice(format!("' on {type}").as_bytes());
                text
            }
            Fault::Borrow { mutable: false } => b"error borrowing userdata".to_vec(),
            Fault::Borrow { mutable: true } => b"error mutably borrowing userdata".to_vec(),
            Fault::Validation { path, kind } => {
                let mut text = format!("at {path}: ").into_bytes();
                text.extend(kind.message(false));
                text
            }
            Fault::Conversion { path, kind } => {
                let mut text = format!("at {path}: ").into_bytes();
                text.extend(kind.message(true));
                text
            }
            Fault::Lua(error) => error.text.clone(),
        }
    }

    /// Message as lossy text, for diagnostics and tests.
    pub fn text(&self) -> String {
        String::from_utf8_lossy(&self.message()).into_owned()
    }

    /// The fault with any `Internal` level moved to the root: a Kiln fault aborts the whole operation and is never
    /// nested under a Source level (RC-10).
    pub(crate) fn lift_internal(self) -> Fault {
        fn find(fault: &Fault) -> Option<String> {
            match fault {
                Fault::Internal(text) => Some(text.clone()),
                Fault::Validation {
                    kind: CheckKind::Lua(inner),
                    ..
                }
                | Fault::Conversion {
                    kind: CheckKind::Lua(inner),
                    ..
                } => find(inner),
                _ => None,
            }
        }
        match find(&self) {
            Some(text) if !matches!(self, Fault::Internal(_)) => Fault::Internal(text),
            _ => self,
        }
    }
}

impl CheckKind {
    pub(crate) fn message(&self, conversion: bool) -> Vec<u8> {
        match self {
            CheckKind::IncompatibleType { expected, found } => {
                let found: &[u8] = match found {
                    Found::Text(text) => text,
                    Found::Userdata { .. } => USERDATA_LABEL,
                };
                let mut text = if conversion {
                    format!("expected `{expected}` but found `").into_bytes()
                } else {
                    format!("expected `{expected}`, got `").into_bytes()
                };
                text.extend_from_slice(found);
                text.push(b'`');
                text
            }
            CheckKind::MissingField { field, r#type } => {
                format!("missing field `{field}` in struct `{type}`").into_bytes()
            }
            CheckKind::InvalidType(name) => format!("invalid type `{name}`").into_bytes(),
            CheckKind::InvalidGuid(guid) => format!("invalid guid `{guid}`").into_bytes(),
            CheckKind::Lua(fault) => fault.message(),
            CheckKind::Mapping(text) => text.clone().into_bytes(),
        }
    }
}

impl From<mlua::Error> for Fault {
    /// The codec's helpers return mlua errors; their Display is the level's text.
    fn from(error: mlua::Error) -> Self {
        Fault::Runtime(error.to_string())
    }
}

impl fmt::Display for Fault {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.text())
    }
}

/// A service call the graph refuses at the transport level (a KilnResult), never a Source error.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Refusal {
    /// A malformed request, or a token of the wrong kind or class for the operation.
    InvalidArgument,
    /// A token this process never issued.
    NotFound,
    /// A closed lease or a closed arena.
    Revoked,
    /// A worker-only operation off the arena's creator worker.
    WrongThread,
    /// The phase or source cannot serve the request (no resource source, no default bytes).
    Unavailable,
    /// A contained provider fault.
    Internal,
}

impl Refusal {
    pub fn code(self) -> KilnResult {
        match self {
            Refusal::InvalidArgument => KILN_ERR_INVALID_ARGUMENT,
            Refusal::NotFound => KILN_ERR_NOT_FOUND,
            Refusal::Revoked => KILN_ERR_REVOKED,
            Refusal::WrongThread => KILN_ERR_WRONG_THREAD,
            Refusal::Unavailable => KILN_ERR_UNAVAILABLE,
            Refusal::Internal => KILN_ERR_INTERNAL,
        }
    }
}
