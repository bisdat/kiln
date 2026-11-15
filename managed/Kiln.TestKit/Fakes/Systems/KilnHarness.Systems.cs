// SPDX-License-Identifier: MIT
namespace Kiln.TestKit;

public sealed partial class KilnHarness
{
    private HarnessPrograms? _programs;

    /// <summary>The game's ECS programs in the harness's fake server world (kiln.world v4): the ones a test declares, which
    /// Kiln.Systems' wraps (<c>Systems.Wrap</c>, <c>Systems.Wrap&lt;T&gt;</c>) find and run on offline. Each step the fake runs
    /// every declared program once per archetype of the fake world's entities that match its lists, through its wraps'
    /// stages, as kiln.world's trampoline runs a slice.</summary>
    public HarnessPrograms Programs => _programs ??= new HarnessPrograms(this);
}

/// <summary>A fake program's descriptor lists: the game's component names (<c>"Fuel"</c>), singletons with their namespace.
/// An entity is in the program's slices when it has every <see cref="Template"/>, <see cref="Read"/>, <see cref="Write"/>
/// and <see cref="With"/> component, none of <see cref="Without"/>, and one of <see cref="AnyOf"/> when it is not empty
/// (template data is a component of the entity in the fake world).</summary>
public sealed record FakeProgramLists
{
    /// <summary>tpl.</summary>
    public IReadOnlyList<string> Template { get; init; } = [];
    /// <summary>tpl_opt.</summary>
    public IReadOnlyList<string> TemplateOptional { get; init; } = [];
    /// <summary>read.</summary>
    public IReadOnlyList<string> Read { get; init; } = [];
    /// <summary>read_opt.</summary>
    public IReadOnlyList<string> ReadOptional { get; init; } = [];
    /// <summary>write.</summary>
    public IReadOnlyList<string> Write { get; init; } = [];
    /// <summary>write_opt.</summary>
    public IReadOnlyList<string> WriteOptional { get; init; } = [];
    /// <summary>sys_read (singletons; the fake has none: their slots are empty).</summary>
    public IReadOnlyList<string> SingletonsRead { get; init; } = [];
    /// <summary>sys_write.</summary>
    public IReadOnlyList<string> SingletonsWrite { get; init; } = [];
    /// <summary>other_tpl.</summary>
    public IReadOnlyList<string> OtherTemplate { get; init; } = [];
    /// <summary>other_read.</summary>
    public IReadOnlyList<string> OtherRead { get; init; } = [];
    /// <summary>other_write.</summary>
    public IReadOnlyList<string> OtherWrite { get; init; } = [];
    /// <summary>with.</summary>
    public IReadOnlyList<string> With { get; init; } = [];
    /// <summary>without.</summary>
    public IReadOnlyList<string> Without { get; init; } = [];
    /// <summary>any_of.</summary>
    public IReadOnlyList<string> AnyOf { get; init; } = [];
    /// <summary>events_in.</summary>
    public IReadOnlyList<string> EventsIn { get; init; } = [];
    /// <summary>events_out.</summary>
    public IReadOnlyList<string> EventsOut { get; init; } = [];
    /// <summary>The descriptor's flags (0x1: a client program, which has no update in a server world).</summary>
    public uint Flags { get; init; }
    /// <summary>The descriptor's interval.</summary>
    public uint Interval { get; init; } = 1;
}

/// <summary>What a fake program's update sees: one slice of its program (one archetype's matching entities).</summary>
/// <param name="Program">The program.</param>
/// <param name="Entities">The slice's entities, in row order.</param>
/// <param name="Step">The world's step.</param>
public sealed record FakeProgramSlice(string Program, IReadOnlyList<EntityId> Entities, ulong Step);

/// <summary>A write a wrap's stage marked for the save and replication (the game's MarkWritten).</summary>
/// <param name="Program">The wrapped program.</param>
/// <param name="Entity">The entity.</param>
/// <param name="Component">The component's game name.</param>
/// <param name="Step">The world's step.</param>
public readonly record struct FakeProgramMark(string Program, EntityId Entity, string Component, ulong Step);

/// <summary>The fake world's programs (<see cref="KilnHarness.Programs"/>). What is modelled and what is not:
/// docs/kiln/modding/unit-tests.md, "Program wraps".</summary>
public sealed class HarnessPrograms
{
    private readonly KilnHarness _harness;

    internal HarnessPrograms(KilnHarness harness) => _harness = harness;

    private SystemsState State => KitFakes.State<SystemsState>(_harness);

    /// <summary>Declares one of the game's programs in the fake server world, after the ones declared before (its index).
    /// <paramref name="update"/> stands for the game's own update: it runs once per slice unless a Replace stage runs
    /// instead (a Replace's <c>Slice.RunOriginal()</c> runs it). Its contract (chapter 06a 9.7) is made from the lists.</summary>
    /// <exception cref="ArgumentException">An empty or repeated name, a list with an empty name.</exception>
    public void Add(string name, FakeProgramLists lists, Action<FakeProgramSlice>? update = null)
    {
        ArgumentException.ThrowIfNullOrEmpty(name);
        ArgumentNullException.ThrowIfNull(lists);
        State.Add(name, lists, SystemsState.ContractText(lists, 0, 0, 0), update);
    }

    /// <summary>Declares the program of Kiln.Game's typed view <typeparamref name="T"/> (<c>Ecs.Programs.FuelSetup</c>): its
    /// name and lists are the view's, and so its contract, so <c>Systems.Wrap&lt;T&gt;()</c> is made and its view binds.</summary>
    /// <exception cref="ArgumentException">The view's lists do not hash to its contract, or the name is declared.</exception>
    public void Add<T>(Action<FakeProgramSlice>? update = null) where T : IGameProgram<T>, allows ref struct
    {
        var (lists, text) = SystemsState.FromView(T.Lists);
        if (SystemsState.Fnv(text) != T.Contract)
            throw new ArgumentException($"{typeof(T).Name}: its lists do not hash to its contract 0x{T.Contract:X8}", nameof(T));
        State.Add(T.Name, lists, text, update);
    }

    /// <summary>The names declared, in table order.</summary>
    public IReadOnlyList<string> Names => State.Names();

    /// <summary>How many slices of <paramref name="program"/> the game's update (the declared one) ran on.</summary>
    public int Updates(string program) => State.Updates(program);

    /// <summary>The writes wrap stages marked, in order.</summary>
    public IReadOnlyList<FakeProgramMark> Marks => State.MarksNow();
}
