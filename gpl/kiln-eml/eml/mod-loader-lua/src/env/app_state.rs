use std::{cell::{Cell, RefCell, RefMut}, collections::{HashMap, hash_map::{self, Entry}}, rc::Rc};

use bitflags::bitflags;
use mod_loader::ModEnvironment;
use once_cell::unsync::OnceCell;
use kfc::{container::{KFCCursor, KFCFile, KFCReader}, guid::{ContentHash, ResourceId}, reflection::{LookupKey, TypeHandle, TypeIndex, TypeRegistry}, resource::value::Value};

use crate::{RunArgs, alias::{MappedValue, PathBuf}, cache::CacheDiff, env::{Type, game::value::{is_dirty_lua_value, walk_writes}, value::{convert_lua_to_value, convert_value_to_lua, validate_and_clone_lua_value}, writes}, log::warn, lua::{LuaError, LuaValue}};

pub struct AppState {
    env: ModEnvironment,
    config: AppConfig,

    registered_dlls: RefCell<Vec<PathBuf>>,

    type_registry: Rc<TypeRegistry>,

    ref_file: Rc<KFCFile>,
    reader: RefCell<Option<KFCCursor<KFCReader>>>,
    // kiln-eml: write to a private stage; publish only after successful finalization.
    writer: RefCell<Option<crate::transaction::PatchWriter>>,

    // NOTE: the `Vec<u8>` must always be treated as immutable
    new_contents: RefCell<HashMap<ContentHash, Vec<u8>>>,
    resources: RefCell<HashMap<ResourceId, Rc<ResourceInfo>>>,
    types: RefCell<HashMap<TypeIndex, LuaValue>>,

    // kiln-eml: who wrote which resource in this run (env/writes.rs), for the EML dialect's change journal
    writes: RefCell<writes::Record>,
}

pub struct AppConfig {
    skip_cache: bool,
    is_server: bool,

    feature_flags: AppFeatures,
    export_dir: PathBuf,
}

bitflags! {
    pub struct AppFeatures: u32 {
        const PATCH = 1 << 0;
        const EXPORT = 1 << 1;
        const RUNTIME_DLL = 1 << 2;

        const ALL = !0;
    }
}

impl AppConfig {
    // kiln-eml: the run's configuration, shared by AppState::new and AppState::hosted (unchanged from EML's `new`).
    fn new(env: &ModEnvironment, args: RunArgs) -> Self {
        let file_name = &args.file_name;
        let options = args.options;
        let export_dir = options.export_dir
            .and_then(|d| {
                d.canonicalize()
                    .map_err(|e| {
                        warn!(
                            error = %e,
                            "Failed to canonicalize export directory, using default instead"
                        );
                    })
                    .ok()
            })
            .and_then(|d| {
                PathBuf::from_path_buf(d)
                    .map_err(|_| {
                        warn!(
                            "Export directory is not valid UTF-8, using default instead"
                        );
                    })
                    .ok()
            })
            .unwrap_or_else(|| env.game_dir().join("export"));

        let skip_cache = options.skip_cache;
        let is_server = options.is_server
            .unwrap_or_else(|| file_name.to_lowercase().contains("server"));

        let mut feature_flags = AppFeatures::empty();

        if options.patch {
            feature_flags |= AppFeatures::PATCH;
        }

        if options.export {
            feature_flags |= AppFeatures::EXPORT;
        }

        if options.runtime {
            feature_flags |= AppFeatures::RUNTIME_DLL;
        }

        AppConfig {
            skip_cache,
            export_dir,

            is_server,

            feature_flags,
        }
    }
}

impl AppState {

    // kiln-eml: preparation helpers retain their step, path and source error.
    pub fn new(
        env: ModEnvironment,
        args: RunArgs,
        cache_diff: &CacheDiff,
    ) -> anyhow::Result<Self> {
        let game_dir = env.game_dir();
        let cache_dir = env.cache_dir();
        let file_name = args.file_name.clone();
        let file_name = &file_name;
        let skip_cache = args.options.skip_cache;

        // prepare configuration

        let config = AppConfig::new(&env, args);

        // load and attach to resources

        let (type_registry, is_dirty) = crate::load::load_type_registry(
            game_dir,
            cache_dir,
            file_name,
        )?;
        crate::load::export_lua_definitions(
            cache_dir,
            &type_registry,
            skip_cache || is_dirty || cache_diff.build_id_changed(),
        );
        let type_registry = Rc::new(type_registry);

        let kfc_path = game_dir
            .join(file_name)
            .with_extension("kfc");
        let bak_path = crate::load::create_backup(&kfc_path)?;

        let ref_file = crate::load::load_kfc_file(&bak_path)?;
        let reader = crate::load::create_reader(game_dir, file_name)?;
        let writer = crate::load::create_writer(game_dir, file_name, &type_registry, &ref_file)?;

        Ok(Self::with_inputs(env, config, type_registry, ref_file, reader, writer))
    }

    /// kiln-eml: a run over inputs a `StartupHost` captured and validated (startup_host.rs): its type registry, its
    /// baseline, a reader of that baseline and the host's one staged writer. Nothing here opens, captures or seals a
    /// baseline, creates a writer or exports type definitions: the host's caller owns those, once per generation.
    pub(crate) fn hosted(
        env: ModEnvironment,
        args: RunArgs,
        type_registry: Rc<TypeRegistry>,
        ref_file: Rc<KFCFile>,
        reader: KFCCursor<KFCReader>,
        writer: crate::transaction::PatchWriter,
    ) -> Self {
        let config = AppConfig::new(&env, args);
        Self::with_inputs(env, config, type_registry, ref_file, reader, writer)
    }

    fn with_inputs(
        env: ModEnvironment,
        config: AppConfig,
        type_registry: Rc<TypeRegistry>,
        ref_file: Rc<KFCFile>,
        reader: KFCCursor<KFCReader>,
        writer: crate::transaction::PatchWriter,
    ) -> Self {
        Self {
            env,
            config,

            registered_dlls: RefCell::new(Vec::new()),

            type_registry,

            ref_file,
            reader: RefCell::new(Some(reader)),
            writer: RefCell::new(Some(writer)),

            new_contents: RefCell::new(HashMap::new()),
            resources: RefCell::new(HashMap::new()),
            types: RefCell::new(HashMap::new()),

            writes: RefCell::new(writes::Record::default()),
        }
    }

    /// kiln-eml: `package`'s entrypoint begins; what it writes is its own until the guard is dropped (env/writes.rs).
    pub(crate) fn begin_writer(&self, package: &str) -> writes::Entrypoint {
        writes::Record::begin(&self.writes, package)
    }

    /// kiln-eml: the running entrypoint created `id` (`explicit`: with its own GUID and part), or the content `hash`.
    pub(crate) fn note_created(&self, id: ResourceId, explicit: bool) {
        writes::Record::created(&self.writes, id, explicit);
    }
    pub(crate) fn note_content(&self, hash: ContentHash) {
        writes::Record::content(&self.writes, hash);
    }

    /// kiln-eml: who wrote what in this run, per writer (env/writes.rs), from each resource's value as it will be
    /// written: call it once the mods ran, before their resources are serialized.
    pub(crate) fn write_record(&self) -> mlua::Result<writes::WriteRecord> {
        let mut trees = Vec::new();
        let mut path = String::new();
        for info in self.resources.borrow().values() {
            if let Some(value) = info.value.borrow().as_ref() {
                let mut out = Vec::new();
                walk_writes(value, &mut path, &mut out)?;
                if !out.is_empty() {
                    trees.push((info.resource_id, out));
                }
            }
        }
        Ok(writes::collect(&self.writes.borrow(), trees))
    }
    #[inline]
    pub fn env(&self) -> &ModEnvironment {
        &self.env
    }

    #[inline]
    #[allow(unused)]
    pub fn skip_cache(&self) -> bool {
        self.config.skip_cache
    }

    #[inline]
    pub fn is_server(&self) -> bool {
        self.config.is_server
    }

    #[inline]
    pub fn is_client(&self) -> bool {
        !self.config.is_server
    }

    #[inline]
    pub fn export_dir(&self) -> &PathBuf {
        &self.config.export_dir
    }

    #[inline]
    pub fn has_feature(&self, feature: AppFeatures) -> bool {
        self.config.feature_flags.contains(feature)
    }

    #[inline]
    pub fn register_dll(&self, path: PathBuf) {
        self.registered_dlls.borrow_mut().push(path);
    }

    #[inline]
    pub fn registered_dlls(&self) -> Vec<PathBuf> {
        self.registered_dlls.borrow().clone()
    }

    #[inline]
    pub fn kfc_file(&self) -> &KFCFile {
        &self.ref_file
    }

    #[inline]
    pub fn type_registry(&self) -> &Rc<TypeRegistry> {
        &self.type_registry
    }

    #[inline]
    pub fn reader(&self) -> RefMut<KFCCursor<KFCReader>> {
        RefMut::map(self.reader.borrow_mut(), |reader| reader.as_mut().expect("KFC reader closed for publication"))
    }

    pub fn close_reader(&self) { self.reader.borrow_mut().take(); }

    #[inline]
    pub fn take_writer(
        &self,
    ) -> crate::transaction::PatchWriter {
        self.writer.borrow_mut()
            .take()
            .expect("KFCWriter is no longer available")
    }

    pub fn get_cached_resources(
        &self,
    ) -> Vec<Rc<ResourceInfo>> {
        self.resources.borrow()
            .values()
            .cloned()
            .collect()
    }

    pub fn get_resource_info(
        &self,
        guid: &ResourceId,
    ) -> Option<Rc<ResourceInfo>> {
        match self.resources.borrow_mut().entry(*guid) {
            Entry::Occupied(entry) => {
                Some(entry.get().clone())
            },
            Entry::Vacant(entry) => {
                if !self.ref_file.resources().contains_key(guid) {
                    return None;
                }

                let info = ResourceInfo {
                    resource_id: *guid,
                    original_value: OnceCell::new(),
                    value: RefCell::default(),
                    is_dirty: Cell::new(false),
                };
                let info = Rc::new(info);

                entry.insert(info.clone());

                Some(info)
            }
        }
    }

    pub fn add_resource(
        &self,
        value: &LuaValue,
        guid: &ResourceId,
        lua: &mlua::Lua,
    ) -> mlua::Result<()> {
        let mut resources = self.resources.borrow_mut();

        if resources.contains_key(guid) {
            return Err(LuaError::generic(format!(
                "resource with GUID {guid} already exists"
            )));
        }

        let r#type = TypeHandle::new(
            self.type_registry.clone(),
            self.type_registry.get_by_hash(LookupKey::Qualified(guid.type_hash()))
                .ok_or_else(|| LuaError::type_not_found(guid.type_hash()))?
                .index,
        );
        let value = validate_and_clone_lua_value(
            value,
            &r#type,
            lua,
        ).map_err(LuaError::external)?;

        let info = Rc::new(ResourceInfo {
            resource_id: *guid,
            original_value: OnceCell::new(),
            value: RefCell::new(Some(value)),
            is_dirty: Cell::new(true),
        });

        resources.insert(*guid, info);

        Ok(())
    }

    pub fn create_resource(
        &self,
        value: &LuaValue,
        type_index: TypeIndex,
        lua: &mlua::Lua,
    ) -> mlua::Result<ResourceId> {
        let r#type = self.type_registry.get(type_index)
            .ok_or_else(|| LuaError::generic(format!(
                "type with index {type_index} not found"
            )))?;
        let resources = self.resources.borrow_mut();
        let mut guid: ResourceId;

        loop {
            let uuid = uuid::Uuid::new_v4().into_bytes();

            guid = ResourceId::new(
                uuid.into(),
                r#type.qualified_hash,
                0,
            );

            if !resources.contains_key(&guid) {
                break;
            }
        }

        drop(resources);

        self.add_resource(value, &guid, lua)?;

        Ok(guid)
    }

    pub fn has_content(&self, guid: &ContentHash) -> bool {
        self.new_contents.borrow().contains_key(guid)
            || self.kfc_file().contents().contains_key(guid)
    }

    pub fn get_content(
        &self,
        guid: &ContentHash,
    ) -> mlua::Result<Option<Vec<u8>>> {
        if let Some(value) = self.new_contents.borrow().get(guid) {
            // OPTIMIZE: cloning here can be quite expensive
            Ok(Some(value.clone()))
        } else {
            // OPTIMIZE: consider using a reader for this
            Ok(self.reader().read_content(guid)?)
        }
    }

    pub fn create_content(
        &self,
        data: &[u8],
    ) -> mlua::Result<ContentHash> {
        let guid = ContentHash::from_data(data);

        if let hash_map::Entry::Vacant(e) = self.new_contents.borrow_mut().entry(guid) {
            self.writer.borrow_mut()
                .as_mut()
                .ok_or_else(|| LuaError::generic(
                    "cannot create content: KFCWriter is no longer available"
                ))?
                .write_content(&guid, data)?;
            failpoint!("stage.content-written", std::path::Path::new("stage/content"));

            // TODO: attach a reader to the writer to avoid cloning
            e.insert(data.to_vec());
        }

        Ok(guid)
    }

    pub fn get_type(
        &self,
        lua: &mlua::Lua,
        type_index: TypeIndex,
    ) -> mlua::Result<Option<LuaValue>> {
        match self.types.borrow_mut().entry(type_index) {
            Entry::Occupied(entry) => Ok(Some(entry.get().clone())),
            Entry::Vacant(entry) => {
                if self.type_registry.get(type_index).is_none() {
                    return Ok(None);
                }

                let value = Type::new(TypeHandle::new(
                    self.type_registry.clone(),
                    type_index,
                ));
                let value = LuaValue::UserData(lua.create_userdata(value)?);

                entry.insert(value.clone());
                Ok(Some(value))
            }
        }
    }

}

pub struct ResourceInfo {
    pub resource_id: ResourceId,

    original_value: OnceCell<Option<MappedValue>>,
    value: RefCell<Option<LuaValue>>,
    is_dirty: Cell<bool>,
}

impl ResourceInfo {

    pub fn get_lua_value(
        &self,
        lua: &mlua::Lua,
    ) -> mlua::Result<LuaValue> {
        if let Some(value) = self.value.borrow().as_ref() {
            return Ok(value.clone());
        }

        let value = match self.get_mapped_value(lua)? {
            Some(value) => convert_value_to_lua(value, lua)?,
            None => LuaValue::Nil,
        };

        self.value.replace(Some(value.clone()));

        Ok(value)
    }

    pub fn set_lua_value(
        &self,
        lua_value: LuaValue,
        lua: &mlua::Lua,
    ) -> mlua::Result<()> {
        let app_state = lua.app_data_ref::<AppState>().unwrap();
        let type_registry = app_state.type_registry();

        let r#type = type_registry
            .get_by_hash(LookupKey::Qualified(self.resource_id.type_hash()))
            .ok_or_else(|| LuaError::type_not_found(self.resource_id.type_hash()))?;

        let lua_value = validate_and_clone_lua_value(
            &lua_value,
            &TypeHandle::new(
                type_registry.clone(),
                r#type.index,
            ),
            lua,
        ).map_err(LuaError::external)?;

        self.value.replace(Some(lua_value));
        self.is_dirty.replace(true);
        writes::Record::replaced(&app_state.writes, self.resource_id); // kiln-eml

        Ok(())
    }

    pub fn apply(
        &self,
        lua: &mlua::Lua,
    ) -> mlua::Result<Option<Value>> {
        let app_state = lua.app_data_ref::<AppState>().unwrap();
        let type_registry = app_state.type_registry();

        let lua_value = self.value.borrow();
        let lua_value = match lua_value.as_ref() {
            Some(value) => value,
            None => return Ok(None),
        };

        if !self.is_dirty.get() && !is_dirty_lua_value(lua_value)? {
            return Ok(None);
        }

        let r#type = type_registry
            .get_by_hash(LookupKey::Qualified(self.resource_id.type_hash()))
            .ok_or_else(|| LuaError::type_not_found(self.resource_id.type_hash()))?;

        let value = convert_lua_to_value(
            lua_value,
            &TypeHandle::new(
                type_registry.clone(),
                r#type.index,
            ),
        ).map_err(LuaError::external)?;

        Ok(Some(value))
    }

    fn get_mapped_value(
        &self,
        lua: &mlua::Lua,
    ) -> mlua::Result<Option<&MappedValue>> {
        let app_state = lua.app_data_ref::<AppState>().unwrap();

        self.original_value.get_or_try_init(|| {
            match app_state.reader().read_resource(&self.resource_id)? {
                Some(value) => {
                    let type_registry = app_state.type_registry();
                    let r#type = type_registry
                        .get_by_hash(LookupKey::Qualified(self.resource_id.type_hash()))
                        .ok_or_else(|| LuaError::type_not_found(self.resource_id.type_hash()))?;

                    let value = MappedValue::from_bytes(
                        type_registry,
                        r#type,
                        &Rc::from(value.into_boxed_slice()),
                    ).map_err(LuaError::external)?;

                    Ok(Some(value))
                },
                None => Ok(None),
            }
        }).map(|v| v.as_ref())
    }

}
