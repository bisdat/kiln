// SPDX-License-Identifier: MIT
using System.Text.Json.Nodes;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Runtime.CompilerServices;
using Xunit;
namespace Kiln.WorldTests;
[Collection("kiln.world backend")]
public sealed class StrictScaleReplayTests : IDisposable
{
    private static readonly Guid Chest = Guid.Parse("00000002-0000-0000-0000-000000000002");
    private readonly EntityTable table = new();
    private readonly ServiceEntityData.IStore? previous = ServiceEntityData.Store;
    private readonly IKilnBackend backend = Backend.Current;
    private sealed class Capture : IKilnBackend {
        public List<string> Lines = [];
        public string Version => "test";
        public bool IsTickThread => true;
        public bool HasFeature(string name) => false;
        public void Log(ulong owner, LogLevel level, string text) => Lines.Add(text);
        public nint WorldTable;
        public nint Service(string name, uint version) => name == "kiln.world" ? WorldTable : 0;
    }
    private readonly Capture capture = new();
    public StrictScaleReplayTests() {
        TestWorld.Reset(); TestWorld.Templates[Chest]=new(3,9,"Chest");
        Backend.Current=capture; ServiceEntityData.Store=new ServiceEntities(table);
        TransformScaleKeeper.OnWorldChanged();
        TransformScaleKeeper.Record(Place(2000), Scale3.One, Scale3.Uniform(2), "m");
        table.Clear(); TestWorld.World.Clear();
    }
    private static EntityId Place(uint id, long? generation=null, double x=42) {
        var e=new EntityId(id); TestWorld.World[id]=new(e,Chest,null,new(x,0,0),0,generation); return e;
    }
    [Theory]
    [InlineData(null, 2L)]
    [InlineData(1L, 2L)]
    [InlineData(1L, null)]
    public void Same_template_id_reuse_drops_record_before_periodic_check(long? before, long? after) {
        var e=Place(2001, before); Assert.True(TransformScaleKeeper.Record(e,Scale3.One,Scale3.Uniform(2),"m"));
        TransformScaleKeeper.Queue(e,new("m",Scale3.Uniform(2),Scale3.One));
        Place(2001,after); TransformScaleKeeper.ApplyWaiting();
        Assert.Equal(0,TransformScaleKeeper.Counts.Waiting);
        Assert.Empty(ServiceEntityData.Store!.Replays("kiln:world", "transform-scale"));
    }
    [Fact] public void Restart_neighbours_wait_even_after_one_disappears() {
        Place(2001); Place(2002,x:42.2);
        table.Absorb("kiln:world",JsonNode.Parse("""{"entities":[{"template":"CHEST","pos":[42,0,0],"keys":{"transform-scale":{"mod":"m","scale":{"x":2,"y":2,"z":2},"base":{"x":1,"y":1,"z":1}}}}]}""".Replace("CHEST",Chest.ToString()))!.AsObject());
        table.StartMatching(); table.Tick(); table.HandOverServiceValues(); TransformScaleKeeper.ApplyWaiting();
        Assert.Equal(0,table.Strict("kiln:world").LiveCount);
        Assert.Contains(capture.Lines,l=>l.Contains("two entities of this kind") && l.Contains("m") && l.Contains(Chest.ToString()));
        TestWorld.World.Remove(2002); table.Tick(); TransformScaleKeeper.ApplyWaiting();
        Assert.Equal(0,table.Strict("kiln:world").LiveCount);
        var waiting = Assert.Single(World.GetWaitingTransformScales());
        Assert.Equal("m", waiting.Mod);
        Assert.Equal(Chest, waiting.Template);
        Assert.Equal(new WorldPosition(42, 0, 0), waiting.Position);
        Assert.Equal(Scale3.Uniform(2), waiting.Scale);
        Assert.Contains("two entities of this kind", waiting.Reason);
        Assert.Single(capture.Lines, l => l.Contains("two entities of this kind"));
        Assert.True(TransformScaleKeeper.Record(new EntityId(2001), Scale3.One, Scale3.Uniform(3), "m"));
        Assert.Empty(World.GetWaitingTransformScales());
        Assert.True(ServiceEntityData.Store!.Holds(new EntityId(2001), "kiln:world", "transform-scale"));
    }
    [Fact]
    public void Waiting_scale_has_typed_actionable_fields()
    {
        var method = typeof(World).GetMethod("GetWaitingTransformScales");
        Assert.NotNull(method);
        var record = typeof(World).Assembly.GetType("Kiln.WaitingTransformScale");
        Assert.NotNull(record);
        Assert.Equal(new[] { "Mod", "Template", "Position", "Scale", "Reason" }, record.GetProperties().Select(p => p.Name).ToArray());
        Assert.Equal(typeof(IReadOnlyList<>).MakeGenericType(record), method.ReturnType);
    }

    private sealed class LoadedMod(ModInfo info) : IModsBackend
    {
        private readonly ModEntry entry = new(info.Id, info.Name, info.Version, info.Directory, ModSide.Server, ModState.Loaded, null, [], 0, info);
        public IReadOnlyList<ModEntry> All => [entry];
        public ModSide Side => ModSide.Server;
        public ModEntry? Get(string id) => id == info.Id ? entry : null;
        public ModStats Stats(string id) => default;
        public bool Send(ModContext? sender, string targetId, string method, object? payload) => false;
        public Task<ModOperationResult> Request(ModOperation op, string arg) => throw new NotSupportedException();
        public void Request(ModOperation op, string arg, Action<ModOperationResult> done, ModContext? caller) => throw new NotSupportedException();
    }

    private static Scale3 replayed;
    private static int writes;
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static unsafe uint Caps() => WorldWritesAbi.CapTrackedWrites;
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static unsafe int Status(WriteStatusNative* status) { status->Program = WorldWritesAbi.ProgramActive; status->Declared = 1; return 0; }
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static unsafe int Info(NativeStr mod, uint component, WriteInfoNative* info) { info->State = 0; info->ComponentSize = 56; return 0; }
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static unsafe int Submit(ulong owner, WriteRequestNative* request, ulong* ticket)
    {
        writes++; replayed = *(Scale3*)request->Data; *ticket = 1;
        var before = Scale3.One;
        var result = new WriteResultNative { Size = (uint)sizeof(WriteResultNative), Entity = request->Entity,
            Length = 12, ComponentSize = 56, Before = &before, After = request->Data, Status = 0 };
        request->Callback(&result, request->User);
        return 0;
    }

    [Fact]
    public unsafe void Restart_without_SpawnTime_replays_the_kept_scale_through_the_tracked_write()
    {
        var priorMods = ModsBackend.Current;
        var layoutField = typeof(TransformScale).GetField("_layout", BindingFlags.Static | BindingFlags.NonPublic)!;
        var oldLayout = layoutField.GetValue(null);
        var native = new WorldWritesTable();
        native.V1.Header = new ServiceHeader { Size = (uint)sizeof(WorldWritesTable), Version = 3 };
        native.Capabilities = &Caps; native.WriteStatus = &Status; native.WriteInfo = &Info; native.WriteSubmit = &Submit;
        var info = new ModInfo("m", "Scale mod", "1.0.0", AppContext.BaseDirectory);
        var mod = HostAccess.CreateContext(info, 777, AppContext.BaseDirectory);
        try
        {
            _ = World.GetWaitingTransformScales(); // initialise the facade before supplying its reflected layout
            capture.WorldTable = (nint)(&native);
            WorldNative.Reset(); WorldWrites.Reset(); WorldCapabilities.Reset(); Capabilities.Reset();
            TransformScale.Rows.Ensure();
            ModsBackend.Current = new LoadedMod(info);
            layoutField.SetValue(null, new TransformScale.Layout(40, "test float3"));
            Capabilities.Report(TransformScale.Capability, CapabilityState.Available);
            WorldCapabilities.Refresh();
            writes = 0; replayed = default;
            Place(2001); // loaded entities have no SpawnTime
            table.Absorb("kiln:world", JsonNode.Parse("""{"entities":[{"template":"CHEST","pos":[42,0,0],"keys":{"transform-scale":{"mod":"m","scale":{"x":2,"y":2,"z":2},"base":{"x":1,"y":1,"z":1}}}}]}""".Replace("CHEST", Chest.ToString()))!.AsObject());
            table.StartMatching(); table.Tick(); table.HandOverServiceValues();
            Assert.True(writes == 1, string.Join("\n", capture.Lines));
            Assert.Equal(Scale3.Uniform(2), replayed);
        }
        finally
        {
            mod.Retire(); ModsBackend.Current = priorMods; capture.WorldTable = 0;
            layoutField.SetValue(null, oldLayout);
            WorldNative.Reset(); WorldWrites.Reset(); WorldCapabilities.Reset(); Capabilities.Reset();
        }
    }

    public void Dispose(){TransformScaleKeeper.OnWorldChanged();ServiceEntityData.Store=previous;Backend.Current=backend;TestWorld.Reset();}
}
