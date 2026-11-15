// SPDX-License-Identifier: MIT
// Systems.Define (chapter 06a 9.5): a typed program of the mod's own. Its access lists are a [Query] struct's; the build
// (Kiln.Sdk's KilnPrograms task) writes them into the mod's kiln.programs.json, kiln.world reserves that program before
// the world starts (the boot reservations EcsPrograms.Bind binds, docs/kiln/world.md "Programs v2"), and Define binds the
// mod's per-entity handler to it.
using System.Reflection;

namespace Kiln;

/// <summary>Declares a program of the mod's own over the struct it is on: which components each entity it runs for has
/// (the access lists) and how the game schedules it. The build writes it into the mod's <c>kiln.programs.json</c> (Kiln.Sdk),
/// kiln.world reserves it before the world starts, and <see cref="Systems.Define{T}"/> binds a handler to it. The program is
/// an entity program: it runs once per entity whose archetype matches, on the game's worker threads.</summary>
/// <remarks>Names are the game's component names (<c>"Fuel"</c>, <c>"CurrentTransform"</c>), singletons with their namespace
/// (<c>"impact::AttributeRegistry"</c>). The engine's limits apply (docs/kiln/world.md): at most 8 template, 42 component,
/// 64 singleton, 9 other-template, 92 other-entity and 24 event names; a component at most once per access group.</remarks>
[AttributeUsage(AttributeTargets.Struct, Inherited = false, AllowMultiple = false)]
public sealed class QueryAttribute : Attribute
{
    /// <summary>The reservation's key in kiln.programs.json. Default: the struct's full name with <c>.</c> and <c>+</c> as
    /// <c>_</c> (<c>MyMod_WateringCanQuery</c>); letters, digits and <c>_</c>, at most 63.</summary>
    public string? Key { get; set; }
    /// <summary>Template data every entity's archetype has (tpl).</summary>
    public string[] Template { get; set; } = [];
    /// <summary>Template data an archetype may have (tplOptional).</summary>
    public string[] TemplateOptional { get; set; } = [];
    /// <summary>Components the program reads; every entity has them (read).</summary>
    public string[] Read { get; set; } = [];
    /// <summary>Components the program reads when the entity has them (readOptional).</summary>
    public string[] ReadOptional { get; set; } = [];
    /// <summary>Components the program writes; every entity has them (write).</summary>
    public string[] Write { get; set; } = [];
    /// <summary>Components the program writes when the entity has them (writeOptional).</summary>
    public string[] WriteOptional { get; set; } = [];
    /// <summary>Singletons it reads (singletonsRead).</summary>
    public string[] SingletonsRead { get; set; } = [];
    /// <summary>Singletons it writes (singletonsWrite).</summary>
    public string[] SingletonsWrite { get; set; } = [];
    /// <summary>Other entities' template data it reads (otherTpl).</summary>
    public string[] OtherTemplate { get; set; } = [];
    /// <summary>Other entities' components it reads (otherRead).</summary>
    public string[] OtherRead { get; set; } = [];
    /// <summary>Other entities' components it writes (otherWrite).</summary>
    public string[] OtherWrite { get; set; } = [];
    /// <summary>Components an entity must have, not read (with).</summary>
    public string[] With { get; set; } = [];
    /// <summary>Components an entity must not have (without).</summary>
    public string[] Without { get; set; } = [];
    /// <summary>Components of which an entity must have at least one (anyOf).</summary>
    public string[] AnyOf { get; set; } = [];
    /// <summary>Event streams it reads (eventsIn).</summary>
    public string[] EventsIn { get; set; } = [];
    /// <summary>Event streams it sends (eventsOut).</summary>
    public string[] EventsOut { get; set; } = [];
    /// <summary>An INIT program: runs for archetype groups that gained entities (flag init).</summary>
    public bool Init { get; set; }
    /// <summary>Runs only where the world is hosted (flag hostOnly).</summary>
    public bool HostOnly { get; set; }
    /// <summary>Runs alone (flag exclusive).</summary>
    public bool Exclusive { get; set; }
    /// <summary>Runs per chunk (flag chunked).</summary>
    public bool Chunked { get; set; }
    /// <summary>Runs on the world's own thread (flag pinned).</summary>
    public bool Pinned { get; set; }
    /// <summary>A scheduling barrier (flag barrier).</summary>
    public bool Barrier { get; set; }
}

/// <summary>A query struct of <see cref="Systems.Define{T}"/>: a <c>ref struct</c> with <see cref="QueryAttribute"/> that
/// makes itself from the program's context for one entity. Its members read and write through that context (what the
/// [Query] lists): <c>context.TryGet&lt;Ecs.Fuel&gt;(out var fuel)</c>, <c>context.TrySet(fuel)</c>.</summary>
/// <typeparam name="TSelf">The query struct itself.</typeparam>
public interface IQuery<TSelf> where TSelf : IQuery<TSelf>, allows ref struct
{
    /// <summary>The query of the entity <paramref name="context"/> is at; valid during its call only.</summary>
    static abstract TSelf Bind(EcsProgramContext context);
}

/// <summary>How the game schedules a defined program: the flags of its <see cref="QueryAttribute"/>, said again where it is
/// bound (the build reserved it with the attribute's; a difference is a mod built from other sources).</summary>
public sealed record ProgramOptions
{
    /// <summary>See <see cref="QueryAttribute.Init"/>.</summary>
    public bool Init { get; init; }
    /// <summary>See <see cref="QueryAttribute.HostOnly"/>.</summary>
    public bool HostOnly { get; init; }
    /// <summary>See <see cref="QueryAttribute.Exclusive"/>.</summary>
    public bool Exclusive { get; init; }
    /// <summary>See <see cref="QueryAttribute.Chunked"/>.</summary>
    public bool Chunked { get; init; }
    /// <summary>See <see cref="QueryAttribute.Pinned"/>.</summary>
    public bool Pinned { get; init; }
    /// <summary>See <see cref="QueryAttribute.Barrier"/>.</summary>
    public bool Barrier { get; init; }
}

/// <summary>The outcome of <see cref="Systems.Define{T}"/>: a bound program, or why not. Dispose unbinds it (the reservation
/// stays, inert, until the server restarts); the mod's unload does too.</summary>
public sealed class ProgramRegistration : IKilnResult, IDisposable
{
    private IDisposable? _binding;

    internal ProgramRegistration(bool ok, string? reason, string key, IDisposable? binding)
    {
        Ok = ok;
        Reason = reason;
        Key = key;
        _binding = binding;
    }

    /// <summary>True when the handler is bound to the program reserved at boot.</summary>
    public bool Ok { get; }

    /// <summary>Why not: <c>"not reserved at boot: rebuild the mod and restart the server"</c>, or kiln.world's refusal.
    /// Null when <see cref="Ok"/>.</summary>
    public string? Reason { get; }

    /// <summary>The reservation's key.</summary>
    public string Key { get; }

    /// <summary>Unbinds the handler: kiln.world waits until no call of it runs. Tick thread.</summary>
    [TickThreadOnly]
    public void Dispose()
    {
        if (Volatile.Read(ref _binding) is null) return;
        Services.RequireTickThread("ProgramRegistration.Dispose");
        var b = Interlocked.Exchange(ref _binding, null);
        b?.Dispose();
    }
}

public static unsafe partial class Systems
{
    /// <summary>The reason a Define whose key kiln.world did not reserve at boot gives.</summary>
    internal const string NotReserved = "not reserved at boot: rebuild the mod and restart the server";

    /// <summary>Binds <paramref name="each"/> to the program the build reserved for query <typeparamref name="T"/> (its
    /// <see cref="QueryAttribute"/>, in the mod's kiln.programs.json): it runs once per matching entity of every step, on
    /// the game's worker threads, after every game program it conflicts with. Call in OnLoad. No tick-thread API inside the
    /// handler (they throw); <see cref="Slice.Post"/>-style work goes through the mod's scheduler.</summary>
    /// <param name="key">The reservation's key: the [Query]'s <see cref="QueryAttribute.Key"/>, or the struct's full name
    /// with <c>.</c> and <c>+</c> as <c>_</c>.</param>
    /// <param name="options">The [Query]'s flags, said again.</param>
    /// <param name="each">The per-entity handler.</param>
    /// <returns><c>Ok = false</c> with <see cref="NotReserved"/>'s text when kiln.world reserved no such program for the mod
    /// at boot (a mod built without its [Query] in kiln.programs.json, or loaded after the server started), or with
    /// kiln.world's refusal (another binding holds it).</returns>
    /// <exception cref="ArgumentException">A null or empty key, <typeparamref name="T"/> has no [Query], the key or the
    /// options are not the [Query]'s, or a name of its lists is not in the program kiln.world reserved under the key (a
    /// mod built from other sources than the running one: rebuild and restart).</exception>
    /// <exception cref="KilnUnavailableException"><c>systems.define</c> is off (kiln.world's programs did not resolve on
    /// this build).</exception>
    [TickThreadOnly, ServerOnly, RequiresCapability("systems.define")]
    public static ProgramRegistration Define<T>(string key, ProgramOptions options, EntityHandler<T> each)
        where T : IQuery<T>, allows ref struct
    {
        ArgumentException.ThrowIfNullOrEmpty(key);
        ArgumentNullException.ThrowIfNull(options);
        ArgumentNullException.ThrowIfNull(each);
        Services.RequireTickThread("Systems.Define");
        var query = typeof(T).GetCustomAttribute<QueryAttribute>(false)
            ?? throw new ArgumentException($"{typeof(T).Name} has no [Query]: Define needs the access lists the build reserves", nameof(T));
        var reserved = QueryKey(typeof(T), query);
        if (!string.Equals(key, reserved, StringComparison.Ordinal))
            throw new ArgumentException($"{typeof(T).Name} is reserved at boot as '{reserved}', not '{key}'", nameof(key));
        if (Flags(options) != Flags(query))
            throw new ArgumentException($"{typeof(T).Name}: the options ({string.Join(",", Flags(options))}) are not its [Query]'s " +
                $"({string.Join(",", Flags(query))}), which the build reserved", nameof(options));
        var mod = ModContext.CurrentMod ?? throw new InvalidOperationException("Systems.Define must be called by a loaded mod");
        if (!mod.Alive) throw new InvalidOperationException("the mod is unloading");
        var row = Capabilities.Get("systems.define");
        if (row.State != CapabilityState.Available) throw new KilnUnavailableException("systems.define", row.ReasonText);
        var table = EcsPrograms.Table;
        if (table == null || (table->Capabilities() & 4) == 0)
            throw new KilnUnavailableException("systems.define", "kiln.world's boot programs (v2) are not here");
        EcsProgramCallback callback = context =>
        {
            var view = T.Bind(context);
            each(ref view);
        };
        var rc = EcsPrograms.TryBind(mod, key, callback, table, out var binding);
        if (rc == SystemsNative.NotFound) return new ProgramRegistration(false, NotReserved, key, null);
        if (rc != SystemsNative.Ok)
            return new ProgramRegistration(false, rc switch
            {
                SystemsNative.InvalidArgument => $"{key}: another binding holds the program (bind it once, or Dispose the first)",
                SystemsNative.Unavailable => $"{key}: the program is not running (it faulted or its world is gone)",
                _ => $"{key}: kiln.world refused the binding (result {rc})",
            }, key, null);
        // The reservation is the build's [Query]: every name the iterator gives a slot must be in it.
        foreach (var (kind, list, name) in SlotNames(query))
            if (binding!.Lookup(kind, name).Slot < 0)
            {
                binding.Dispose();
                throw new ArgumentException($"{typeof(T).Name}: '{name}' is in its [Query] {list} list but not in the program " +
                    $"kiln.world reserved at boot as '{key}' (a mod built from other sources): rebuild the mod and restart the server", nameof(T));
            }
        return new ProgramRegistration(true, null, key, binding);
    }

    /// <summary>The reservation key of query type <paramref name="type"/> (the rule Kiln.Sdk's KilnPrograms task uses).</summary>
    internal static string QueryKey(Type type, QueryAttribute query)
    {
        var key = query.Key ?? (type.FullName ?? type.Name).Replace('.', '_').Replace('+', '_');
        if (key.Length is 0 or > 63 || !key.All(c => c is (>= 'a' and <= 'z') or (>= 'A' and <= 'Z') or (>= '0' and <= '9') or '_'))
            throw new ArgumentException($"{type.Name}: its program key '{key}' is not 1 to 63 letters, digits or _ (set [Query(Key = ...)])", nameof(type));
        return key;
    }

    private static string Flags(bool init, bool hostOnly, bool exclusive, bool chunked, bool pinned, bool barrier) =>
        string.Join(",", new[] { init ? "init" : null, hostOnly ? "hostOnly" : null, exclusive ? "exclusive" : null,
            chunked ? "chunked" : null, pinned ? "pinned" : null, barrier ? "barrier" : null }.Where(f => f is not null));

    private static string Flags(ProgramOptions o) => Flags(o.Init, o.HostOnly, o.Exclusive, o.Chunked, o.Pinned, o.Barrier);
    private static string Flags(QueryAttribute q) => Flags(q.Init, q.HostOnly, q.Exclusive, q.Chunked, q.Pinned, q.Barrier);

    // The names of a [Query] that hold an iterator slot, with kiln.world's slot kind (KILN_SLOT_*: 0 tpl, 1 read, 2 write,
    // 3 singleton, 4 other tpl, 5 other read, 6 other write, 7 events in, 8 events out). with/without/anyOf hold none.
    private static IEnumerable<(int Kind, string List, string Name)> SlotNames(QueryAttribute q)
    {
        (int, string, string[])[] lists =
        [
            (0, "Template", q.Template), (0, "TemplateOptional", q.TemplateOptional), (1, "Read", q.Read),
            (1, "ReadOptional", q.ReadOptional), (2, "Write", q.Write), (2, "WriteOptional", q.WriteOptional),
            (3, "SingletonsRead", q.SingletonsRead), (3, "SingletonsWrite", q.SingletonsWrite), (4, "OtherTemplate", q.OtherTemplate),
            (5, "OtherRead", q.OtherRead), (6, "OtherWrite", q.OtherWrite), (7, "EventsIn", q.EventsIn), (8, "EventsOut", q.EventsOut),
        ];
        foreach (var (kind, list, names) in lists)
            foreach (var name in names ?? [])
                yield return (kind, list, name);
    }
}
