// SPDX-License-Identifier: MIT
//! Compatibility: core's old package/host paths name the SINGLE moved definitions.
use std::any::TypeId;

fn same<A: ?Sized + 'static, B: ?Sized + 'static>() {
    assert_eq!(
        TypeId::of::<A>(),
        TypeId::of::<B>(),
        "{}",
        std::any::type_name::<A>()
    );
}

#[test]
fn old_core_paths_are_the_moved_types() {
    use sfi_core::{host, package};
    use sfi_package::{package as pure, source};
    same::<host::SourceVersion, source::SourceVersion>();
    same::<package::SourceVersion, source::SourceVersion>();
    same::<host::SourceKind, source::SourceKind>();
    same::<host::SourceMetadata, source::SourceMetadata>();
    same::<host::SourceEntry, source::SourceEntry>();
    same::<host::DiskSource, source::DiskSource>();
    same::<host::ProfileOverlaySource, source::ProfileOverlaySource>();
    same::<host::Package, source::Package>();
    same::<dyn host::PackageSource, dyn source::PackageSource>();
    same::<package::Package, pure::Package>();
    same::<package::ParseOptions, pure::ParseOptions>();
    same::<package::Manifest, pure::Manifest>();
    same::<package::manifest::Capability, pure::manifest::Capability>();
    same::<package::document::Document, pure::document::Document>();
    same::<package::edit::EditorPackage, pure::edit::EditorPackage>();
    same::<package::discover::PackageRecord, pure::discover::PackageRecord>();
    same::<package::discover::Registry, pure::discover::Registry>();
    same::<package::plan::Plan, pure::plan::Plan>();
    same::<package::profiles::ProfileCatalog, pure::profiles::ProfileCatalog>();
    assert_eq!(package::MAX_FILE_BYTES, pure::MAX_FILE_BYTES);
}

#[test]
fn host_runtime_types_stay_in_core_and_compose_with_moved_sources() {
    use sfi_core::host::{
        DiskSource, HostContext, Package, PackageSource, Phase, Side, SourceVersion,
    };
    use std::rc::Rc;
    let context = HostContext::new(Phase::Startup, Side::Client, SourceVersion::Sf179);
    assert_eq!(context.version, sfi_package::source::SourceVersion::Sf179);
    let root = std::env::temp_dir();
    let source: Rc<dyn PackageSource> = Rc::new(DiskSource::new(&root).unwrap());
    let descriptor = Package::new("facade", source);
    let record_side: &sfi_package::source::Package = &descriptor;
    assert_eq!(record_side.id(), "facade");
}
