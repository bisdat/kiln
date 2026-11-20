// SPDX-License-Identifier: MIT
// Kiln's task pool for these tests, as the host binds it for the whole process (Kiln.Host Entry.StartTasks): the test
// kit's lease table over real worker threads (Kiln.TestKit/Shared/KitTaskPool.cs, linked) and one Kiln-internal owner
// per component (ComponentTask.Owners). Since 0.2.0-dev.30 Kiln.Saves' writer runs on it; without it a save's files are
// written on the calling thread (WriterPoolTests covers that too).
using System.Runtime.CompilerServices;

namespace Kiln;

internal static class TestPool
{
    /// <summary>The owner Kiln.Host would create for kiln.saves.</summary>
    public const ulong SavesOwner = 0x5A7E5;

    public static readonly Kiln.TestKit.KitTaskPool Pool = new(4, () => false, "saves-test-pool-", notifyEnded: true);

    /// <summary>The host's source of component owners: kiln.saves gets <see cref="SavesOwner"/>, others an id of their own.</summary>
    public static ulong Owners(string component) => component == SaveWriter.Component ? SavesOwner : 0xC0000 + (ulong)(component.Length & 0xFF);

    [ModuleInitializer]
    internal static void Install()
    {
        TaskBackend.Install(Pool);
        ComponentTask.Owners = Owners;
    }

    /// <summary>Admissions of the writer's runner so far (owner kiln.saves, task SaveWriter).</summary>
    public static int WriterAdmissions(ulong owner = SavesOwner)
    {
        lock (Pool.Admitted) return Pool.Admitted.Count(a => a.Owner == owner && a.Name == SaveWriter.TaskName);
    }
}
