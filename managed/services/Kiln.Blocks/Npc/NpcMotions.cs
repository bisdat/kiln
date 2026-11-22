// SPDX-License-Identifier: MIT
using System.Text.Json;
using System.Text.Json.Serialization;

namespace Kiln;

/// <summary>A roster villager's motion. Sit and Sleep variants 0/1/2 are enter/loop/exit; Door variants 0/1/2 are guarded alternatives. Other roles use variant 0.</summary>
[JsonConverter(typeof(NpcMotionRoleConverter))]
public enum NpcMotionRole
{
    Sit, Sleep, Inspect, Greet, Talk, Reaction0, Reaction1, Refuse, IdleGesture0, IdleGesture1,
    Interaction, GreetingExtra, Farewell, DialogOption, AttentionGreeting, TurnLeftSmall, TurnLeftBig,
    TurnRightSmall, TurnRightBig, Jump, Door,
}

/// <summary>Binds one chassis motion to an authored graph state. Interaction needs its nonzero interaction key; other roles leave it null.</summary>
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
