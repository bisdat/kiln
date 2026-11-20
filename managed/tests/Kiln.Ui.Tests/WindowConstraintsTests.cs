using System.Runtime.InteropServices;
using Xunit;

namespace Kiln.WindowConstraintsTests;

// It installs the fake kiln.ui table and its backend, which every Kiln.Ui test class shares: it runs alone, as they do
// (FakeUiTable.Install frees a table another class's test may be drawing with).
[Collection(Kiln.FakeUi.UiSerial.Name)]
public unsafe class WindowConstraintsTests
{
    [Fact]
    public void Tail_appends_after_v7_and_checks_member_end()
    {
        Assert.Equal(888, sizeof(UiNative.Table7));
        Assert.Equal(896, sizeof(UiNative.TableConstraints));
        Assert.Equal(sizeof(UiNative.Table7), (int)Marshal.OffsetOf<UiNative.TableConstraints>(nameof(UiNative.TableConstraints.SetNextWindowSizeConstraints)));
        Assert.Equal(sizeof(UiNative.Table7) + 8, sizeof(UiNative.TableConstraints));
        var previous = Backend.Current;
        Kiln.FakeUi.FakeUiTable.Install();
        Backend.Current = new Kiln.FakeUi.FakeUiTable.Backend();
        var old = Kiln.FakeUi.FakeUiTable.Table;
        var t = (UiNative.TableConstraints*)NativeMemory.AllocZeroed((nuint)sizeof(UiNative.TableConstraints));
        try
        {
            ((UiNative.Table*)t)->Header.Size = (uint)sizeof(UiNative.Table7);
            ((UiNative.Table*)t)->Header.Version = 7;
            Kiln.FakeUi.FakeUiTable.Table = (UiNative.Table4*)t;
            UiNative.ResetForTests();
            t->SetNextWindowSizeConstraints = &Constraints;
            UiNative.T->Header.Size = (uint)sizeof(UiNative.Table7);
            Assert.True(UiNative.WindowConstraintsSlot == null);
            UiNative.T->Header.Size = (uint)sizeof(UiNative.TableConstraints) - 1;
            Assert.True(UiNative.WindowConstraintsSlot == null);
            UiNative.T->Header.Size++;
            Assert.True(UiNative.WindowConstraintsSlot == t->SetNextWindowSizeConstraints);
            t->SetNextWindowSizeConstraints = null;
            Assert.True(UiNative.WindowConstraintsSlot == null);
        }
        finally
        {
            Kiln.FakeUi.FakeUiTable.Table = old;
            Backend.Current = previous;
            UiNative.ResetForTests();
            Kiln.FakeUi.FakeUiTable.Free();
            NativeMemory.Free(t);
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    private static void Constraints(float a, float b, float c, float d) { }
}
