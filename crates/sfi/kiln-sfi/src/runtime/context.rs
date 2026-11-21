// SPDX-License-Identifier: MIT
//! What every runtime family registrar gets besides the package itself.
//!
//! One [`RuntimeContext`] per runtime-create, built on the worker thread by the
//! lifecycle host. It holds kiln-sfi's plugin owner (the `reload_epoch` of every ref
//! the interpreter opens, the gate's owner and every hop's owner), the
//! [`ServiceAdapter`], the hops and the Lua-time watchdog, and the VM's typed-value
//! arena handle when the plan source gives the VM typed values. The registering package's
//! identity is the `Package` the registrar receives, captured immutably by each
//! closure, never a caller's.
//!
//! It never leaves the worker thread, so nothing a producer runs can reach Lua or the
//! services through it:
//! ```compile_fail,E0277
//! fn producer<T: Send>(_: T) {}
//! fn give(cx: std::rc::Rc<kiln_sfi::runtime::RuntimeContext>) { producer(cx) }
//! ```

use super::availability::RuntimeFacts;
use super::services::ServiceAdapter;
use super::values::Values;
use crate::hop::Hops;
use crate::lifecycle::Watchdog;
use kiln_abi::KilnOwner;
use sfi_core::bindings::runtime::RuntimeHost;
use std::cell::RefCell;
use std::marker::PhantomData;
use std::rc::Rc;
use std::thread::ThreadId;

pub struct RuntimeContext {
    owner: KilnOwner,
    services: Rc<ServiceAdapter>,
    hops: Rc<Hops>,
    watchdog: Rc<Watchdog>,
    facts: Rc<RuntimeFacts>,
    /// The registrar's host of the package whose families the installer runs now.
    package_host: RefCell<Option<Rc<dyn RuntimeHost>>>,
    /// This VM's typed-value arena (no VM reference), set before any package is registered.
    values: RefCell<Option<Values>>,
    worker: ThreadId,
    _not_send: PhantomData<*mut ()>,
}

impl RuntimeContext {
    pub(crate) fn new(
        owner: KilnOwner,
        services: Rc<ServiceAdapter>,
        hops: Rc<Hops>,
        watchdog: Rc<Watchdog>,
        facts: Rc<RuntimeFacts>,
    ) -> Self {
        Self {
            owner,
            services,
            hops,
            watchdog,
            facts,
            package_host: RefCell::new(None),
            values: RefCell::new(None),
            worker: std::thread::current().id(),
            _not_send: PhantomData,
        }
    }
    /// kiln-sfi's plugin owner.
    pub fn owner(&self) -> KilnOwner {
        self.owner
    }
    /// The only path to Kiln's services (direct any-thread members).
    pub fn services(&self) -> &Rc<ServiceAdapter> {
        &self.services
    }
    /// Hops, for tick-bound Kiln facilities only. Refused on a core
    /// without `world.lifecycle`.
    pub fn hops(&self) -> &Rc<Hops> {
        &self.hops
    }
    pub fn watchdog(&self) -> &Rc<Watchdog> {
        &self.watchdog
    }
    /// The runtime-create's facts (operation catalog, provider facts, the session
    /// write latch the CAS core and the guarded-write callers report to, effect records).
    pub fn facts(&self) -> &Rc<RuntimeFacts> {
        &self.facts
    }
    /// The registrar's runtime host of the package whose families the installer is running
    /// (`runtime_denial_reason(host, op)`), set by the installer around that package's
    /// family installs and cleared right after. `None` outside that window, and for a
    /// package without a `runtime` namespace. A family captures it into its closures.
    pub fn package_host(&self) -> Option<Rc<dyn RuntimeHost>> {
        self.package_host.borrow().clone()
    }
    pub(crate) fn set_package_host(&self, host: Option<Rc<dyn RuntimeHost>>) {
        *self.package_host.borrow_mut() = host;
    }
    /// The typed-value arena of this context's VM (kiln.eml.values, through the service adapter), for the
    /// families that open values; `None` when the plan source gives the VM no typed values. Each VM has its own
    /// context and arena (a live reload's new VM too). It holds no VM reference, so a family may keep it; the
    /// lifecycle host closes the arena before the VM goes, after which every call answers Kiln's closed error.
    pub fn values(&self) -> Option<Values> {
        self.values.borrow().clone()
    }
    pub(crate) fn set_values(&self, values: Option<Values>) {
        *self.values.borrow_mut() = values;
    }
    /// The worker thread this context was built on (the only thread that runs Lua).
    pub fn worker(&self) -> ThreadId {
        self.worker
    }
}
