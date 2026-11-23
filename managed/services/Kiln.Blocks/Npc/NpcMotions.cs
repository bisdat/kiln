// SPDX-License-Identifier: MIT
using System.Text.Json;
using System.Text.Json.Serialization;

namespace Kiln;

/// <summary>A roster villager's motion. Sit and Sleep variants 0/1/2 are enter/loop/exit; Door variants 0/1/2 are guarded alternatives. Other roles use variant 0.</summary>
[JsonConverter(typeof(NpcMotionRoleConverter))]
public enum NpcMotionRole
{
    /// <summary>Sitting down, sitting and getting up: variants 0, 1 and 2 (only 1 loops). Required.</summary>
    Sit,
    /// <summary>Lying down, sleeping and getting up: variants 0, 1 and 2 (only 1 loops). Required.</summary>
    Sleep,
    /// <summary>Looking something over. Required (variant 0).</summary>
    Inspect,
    /// <summary>Greeting a player.</summary>
    Greet,
    /// <summary>Talking.</summary>
    Talk,
    /// <summary>The first reaction gesture.</summary>
    Reaction0,
    /// <summary>The second reaction gesture.</summary>
    Reaction1,
    /// <summary>Refusing.</summary>
    Refuse,
    /// <summary>The first gesture while idle.</summary>
    IdleGesture0,
    /// <summary>The second gesture while idle.</summary>
    IdleGesture1,
    /// <summary>The chassis interaction; its binding may name the chassis interaction key (<see cref="NpcMotionBinding.InteractionKey"/>).</summary>
    Interaction,
    /// <summary>An extra greeting.</summary>
    GreetingExtra,
    /// <summary>Saying goodbye.</summary>
    Farewell,
    /// <summary>Answering a dialogue option a player picked.</summary>
    DialogOption,
    /// <summary>Greeting to catch a player's attention.</summary>
    AttentionGreeting,
    /// <summary>A small turn to the left on the spot.</summary>
    TurnLeftSmall,
    /// <summary>A big turn to the left on the spot.</summary>
    TurnLeftBig,
    /// <summary>A small turn to the right on the spot.</summary>
    TurnRightSmall,
    /// <summary>A big turn to the right on the spot.</summary>
    TurnRightBig,
    /// <summary>A jump. Required (variant 0).</summary>
    Jump,
    /// <summary>Going through a door: variants 0, 1 and 2 are guarded alternatives, not phases. Required.</summary>
    Door,
}

/// <summary>Binds one chassis motion to an authored graph state. Interaction uses the chassis key when omitted or null; an explicit key must match it. Other roles leave it null.</summary>
/// <param name="Role">The motion this binding plays.</param>
/// <param name="Variant">Which variant of the role: 0, 1 or 2 for Sit, Sleep and Door (see <see cref="NpcMotionRole"/>), 0 for the others.</param>
/// <param name="State">The state of the body's authored animation graph that plays it.</param>
/// <param name="InteractionKey">For <see cref="NpcMotionRole.Interaction"/>: the chassis interaction key, null to use the chassis's own (an explicit key must match it). Null for every other role.</param>
public sealed record NpcMotionBinding(
    [property: JsonRequired, JsonConverter(typeof(NpcMotionRoleConverter))] NpcMotionRole Role,
    [property: JsonRequired] int Variant,
    [property: JsonRequired] AnimState State,
    Guid? InteractionKey = null);

/// <summary>A complete roster body's motion bindings. Required motions never fall back; missing optional motions need an explicit clamped fallback.</summary>
[BlockClients(ClientsLevel.None)]
public sealed record NpcMotionsBlock : INpcBlock
{
    /// <summary>One binding per role and variant, never last-wins.</summary>
    public IReadOnlyList<NpcMotionBinding> Bindings { get; init; } = [];
    /// <summary>A state to clamp/hold for missing optional motions. Sit, Sleep, Inspect, Jump and Door must always be bound explicitly.</summary>
    public AnimState? OptionalFallback { get; init; }
}
