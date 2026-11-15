// SPDX-License-Identifier: MIT
namespace Kiln;

// The same receiving-graph query serves block planning and server-sync admission. Never infer existence from a GUID.
internal sealed record AnimationReferenceAnswer(PlayerAnimStateKind Kind, bool Live, string Reason);
internal static class AnimationGraphReferences
{
    internal static IReadOnlyList<AnimationReferenceAnswer> Find(Guid guid)
    {
        if (!PlayerAnimStates.Available) return [];
        var id = PlayerAnimStates.IdOf(guid);
        var result = PlayerAnimStates.FindGameStates($"0x{id:x8}")
            .Select(s => new AnimationReferenceAnswer(s.Kind, true, "")).ToList();
        result.AddRange(PlayerAnimStates.All.Where(s => s.Id == id)
            .Select(s => new AnimationReferenceAnswer(s.Kind, s.Status == PlayerAnimStateStatus.Live, s.Reason)));
        return result;
    }
}
