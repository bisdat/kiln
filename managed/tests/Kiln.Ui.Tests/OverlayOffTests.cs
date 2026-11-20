using Kiln.FakeUi;
using Xunit;
namespace Kiln;
[Collection(UiSerial.Name)]
public sealed class OverlayOffTests : IDisposable
{
    private readonly IKilnBackend _old = Backend.Current;
    public OverlayOffTests() {
        FakeUiTable.Install();
        Backend.Current = new FakeUiTable.Backend();
        UiNative.ResetForTests();
        FakeUiTable.StatusError = "the overlay's drawing is off: NVIDIA Streamline is installed";
    }
    public void Dispose() {
        Backend.Current = _old;
        UiNative.ResetForTests();
        FakeUiTable.Free();
    }
    [Fact]
    public void WindowsStayClosedWhileTableAndKeysRemainAvailable() {
        Assert.True(UiNative.Available);
        using var w = new UiWindow("off", _ => {}, new UiWindowOptions(), null);
        Assert.Equal(FakeUiTable.StatusError, UiNative.DrawingOffReason);
        Assert.False(w.Open);
        w.Show(); Assert.False(w.Open);
        w.Toggle(); Assert.False(w.Open);
    }
    [Fact]
    public void ScreensStayClosedWithoutTakingInput() {
        using var screen = new UiScreen("off", _ => {}, new UiScreenOptions(), null);
        screen.Open(); Assert.False(screen.IsOpen);
        screen.Toggle(); Assert.False(screen.IsOpen);
    }
    [Fact]
    public void OrdinaryStatusErrorsDoNotDisableWindows() {
        FakeUiTable.StatusError = "a different status error";
        using var w = new UiWindow("on", _ => {}, new UiWindowOptions(), null);
        Assert.True(w.Open);
    }
}
