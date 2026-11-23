// SPDX-License-Identifier: MIT
namespace Kiln;

public static partial class Config
{
    // Registry authoring can run before a tick and on a retained author assembly. Do not join the live config registry,
    // install watchers, write defaults, or retain that assembly there. Reuse already registered runtime configs when present.
    private static ModConfig<T> RegistryInput<T>(RegistryContext frame, ConfigKind kind, string? name) where T : class, new()
    {
        frame.Ensure();
        var mod = frame.Owner; var id = ConfigId(mod, kind, name);
        if (frame.InputConfigs.TryGetValue(id, out var prior))
            return prior as ModConfig<T> ?? throw new ArgumentException("the registry input already has another settings type");
        ModConfig<T>? have;
        lock (Lock) have = Files.OfType<ModConfig<T>>().FirstOrDefault(f => ReferenceEquals(f.Owner, mod) && f.Id == id && !f.PerWorld);
        var file = have ?? new ModConfig<T>(mod, id, kind);
        if (have is null) file.LoadRegistryInput(null);
        frame.InputConfigs.Add(id, file);
        return file;
    }
    private static WorldConfig<T> RegistryWorldInput<T>(RegistryContext frame, string? name) where T : class, new()
    {
        frame.EnsureWorldSettings();
        var mod = frame.Owner;
        if (name is not null && !ValidName(name)) throw new ArgumentException("a config name uses [a-z0-9_.-]", nameof(name));
        var id = mod.Info.Id + (name is null ? "" : "-" + name) + "-world";
        var directory = WorldConfigs.Current();
        if (directory is null || frame.World?.Id != System.IO.Path.GetFileName(directory))
            throw new RegistryContextException(RegistryContextError.WorldSettingsUnavailable, "the current world's settings are not bound");
        if (frame.InputConfigs.TryGetValue(id, out var prior))
            return prior as WorldConfig<T> ?? throw new ArgumentException("the registry input already has another settings type");
        WorldConfig<T>? have;
        lock (Lock) have = Files.OfType<WorldConfig<T>>().FirstOrDefault(f => ReferenceEquals(f.Owner, mod) && f.Id == id && f.WorldDirectory == directory);
        var file = have ?? new WorldConfig<T>(mod, id);
        if (have is null) file.LoadRegistryInput(directory);
        frame.InputConfigs.Add(id, file);
        return file;
    }
}
