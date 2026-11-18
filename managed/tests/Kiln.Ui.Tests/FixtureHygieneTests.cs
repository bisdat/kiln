// SPDX-License-Identifier: MIT
// Every test class puts Kiln.Ui's process-wide state back: the backend, and the fake kiln.ui table with the services it
// hands out. A class that leaves its backend behind hands it to every class xunit runs after it, and the order changes
// from run to run: ImageTrimTests kept its fake backend, GameMenuEntriesTests then freed a v6 table that backend still
// handed out as kiln.ui, and WheelTests and StatusLinesTests read the freed table (a failed assertion, then now and then
// a crash of the test host once the memory was reused).
using System.Runtime.InteropServices;
using Xunit;

namespace Kiln.FakeUi;

[Collection(UiSerial.Name)]
public sealed class FixtureHygieneTests
{
    /// <summary>Every test class xunit can construct without arguments (this one aside), by full name.</summary>
    public static TheoryData<string> TestClasses()
    {
        var data = new TheoryData<string>();
        foreach (var t in typeof(FixtureHygieneTests).Assembly.GetTypes()
                     .Where(t => t is { IsClass: true, IsAbstract: false, IsVisible: true } && t != typeof(FixtureHygieneTests)
                                 && t.GetConstructor(Type.EmptyTypes) != null
                                 && t.GetMethods().Any(m => m.IsDefined(typeof(FactAttribute), true)))
                     .OrderBy(t => t.FullName, StringComparer.Ordinal))
            data.Add(t.FullName!);
        return data;
    }

    // What xunit does around each of the class's tests: the constructor (and InitializeAsync), then Dispose
    [Theory]
    [MemberData(nameof(TestClasses))]
    public async Task A_test_class_puts_the_backend_and_the_fake_kiln_ui_back(string name)
    {
        var before = Backend.Current;
        FakeUiTable.Free();
        try
        {
            var instance = Activator.CreateInstance(typeof(FixtureHygieneTests).Assembly.GetType(name, true)!)!;
            if (instance is IAsyncLifetime lifetime) await lifetime.InitializeAsync();
            if (instance is IAsyncDisposable async) await async.DisposeAsync();
            else (instance as IDisposable)?.Dispose();
            Assert.Same(before, Backend.Current);
            Assert.False(FakeTableAllocated, "the fake kiln.ui table is still allocated");
            Assert.Empty(FakeUiTable.Other);
        }
        finally
        {
            Backend.Current = before;
            UiNative.ResetForTests();
            FakeUiTable.Free();
        }
    }

    private static unsafe bool FakeTableAllocated => FakeUiTable.Table != null;

    [Fact]
    public unsafe void A_freed_fake_kiln_ui_hands_out_none_of_the_tables_its_tests_free()
    {
        // GameMenuEntriesTests' v6 table and SetToolsDrawTests' kiln.set.ui table: their tests free them after FakeUiTable.Free
        var own = NativeMemory.AllocZeroed(64);
        try
        {
            FakeUiTable.Install();
            FakeUiTable.Other["kiln.ui"] = (nint)own;
            FakeUiTable.Other["kiln.set.ui"] = (nint)own;
            FakeUiTable.Free();
            var backend = new FakeUiTable.Backend();
            Assert.Equal(0, backend.Service("kiln.ui", 1));
            Assert.Equal(0, backend.Service("kiln.ui", 6));
            Assert.Equal(0, backend.Service("kiln.set.ui", 1));
        }
        finally
        {
            FakeUiTable.Free();
            NativeMemory.Free(own);
        }
    }
}
