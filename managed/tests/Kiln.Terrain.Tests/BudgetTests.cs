using Kiln;
using Xunit;
using System.Runtime.InteropServices;
namespace TerrainTests;
public sealed class BudgetTests
{
    internal sealed class Driver
    {
        public ulong Step;
        public double Milliseconds;
        public double ReadCost;
        public int NativeRecovery;
        public readonly Dictionary<ulong,long> NativeReads = [], NativeWrites = [];
        public bool Reserve(long read, long write)
        {
            var r=NativeReads.GetValueOrDefault(Step);
            var w=NativeWrites.GetValueOrDefault(Step,NativeRecovery);
            if(r+read>32768 || w+write>4096) return false;
            NativeReads[Step]=r+read; NativeWrites[Step]=w+write; return true;
        }
        private readonly List<TaskCompletionSource> waiting = [];
        public readonly Dictionary<ulong,long> Reads = [], Writes = [];
        public Task Next() { var t = new TaskCompletionSource(); waiting.Add(t); return t.Task; }
        public void Tick() { Step++; var q = waiting.ToArray(); waiting.Clear(); foreach(var t in q) t.SetResult(); }
        public void Finish(params Task[] work) { for(var i=0; i<2000 && work.Any(t=>!t.IsCompleted); i++) Tick(); Assert.All(work,t=>Assert.True(t.IsCompleted)); }
    }
    internal sealed class Scheduled(ITerrainBackend inner, Driver d, bool newWorld = true) : ITerrainBackend
    {
        public string Mod => inner.Mod;
        public bool WorldRunning => true;
        public bool NewWorldSession => newWorld;
        public string? WorldId => inner.WorldId;
        public (int X,int Z)? GridCells => inner.GridCells;
        public string KilnVersion => "budget-test";
        public ulong? BudgetStep => d.Step;
        public double BudgetMilliseconds => d.Milliseconds;
        public bool TryReserveBudget(long read,long write) => d.Reserve(read,write);
        public Task NextTick() => d.Next();
        public Task<bool> Load(CellBox b,TimeSpan t) => inner.Load(b,t);
        public bool TryRead(CellBox b,out Voxel[] c) { d.Milliseconds += d.ReadCost; d.Reads[d.Step]=d.Reads.GetValueOrDefault(d.Step)+b.Count; return inner.TryRead(b,out c); }
        public Task<BuildResult> Write(CellBox b,Voxel[] c,bool[] mask,bool bases,bool transient,string label,TimeSpan timeout,bool dryRun=false,Voxel[]? expected=null)
        {
            if(!dryRun) d.Writes[d.Step]=d.Writes.GetValueOrDefault(d.Step)+b.Count;
            return inner.Write(b,c,mask,bases,transient,label,timeout,dryRun,expected);
        }
        public TerrainBook Book()=>inner.Book();
        public void Save(TerrainBook b)=>inner.Save(b);
    }
    private static TerrainPlan Plan(string name,int x) => TerrainPlan.Lower(new TerrainPatch(name,1,new(x,104,64,32,16,32),new TerrainVoxelVolume(Enumerable.Repeat((byte)1,16384).ToArray()),[new(12)]));
    [Fact]
    public void Native_budget_extension_preserves_the_existing_table_prefix()
    {
        var t=typeof(Terrain).Assembly.GetType("Kiln.TerrainNativeBudget+Table")!;
        Assert.Equal(152,Marshal.SizeOf(t));
        Assert.Equal(136,Marshal.OffsetOf(t,"Reserve").ToInt32());
        Assert.Equal(144,Marshal.OffsetOf(t,"Charge").ToInt32());
    }

    [Theory]
    [InlineData(2)]
    [InlineData(5)]
    public async Task Concurrent_plans_share_one_step_budget_and_all_finish(int count)
    {
        var w=new MemoryWorld(); var d=new Driver(); var session=new TerrainSession();
        var tasks=Enumerable.Range(0,count).Select(i=>new TerrainEngine(new Scheduled(w.For("m"+i),d),session,null).Apply(Plan("p"+i,64+i*64),new(){CellsPerTick=262144},64)).ToArray();
        d.Finish(tasks);
        Assert.All(await Task.WhenAll(tasks),r=>Assert.Equal(TerrainState.Completed,r.State));
        Assert.All(d.Writes.Values,n=>Assert.InRange(n,0,4096));
        Assert.All(d.Reads.Values,n=>Assert.InRange(n,0,32768));
        Assert.All(Enumerable.Range(0,count),i=>Assert.Contains(w.Writes,x=>x.Mod=="m"+i));
    }
    // Lab (dev29-terrain2, boot 1): one 32,768-cell patch took 64 ticks, a tick after every 512-cell unit, where the
    // step's allowance is 4,096 cells. A plan alone on the host uses the step's allowance.
    [Fact]
    public async Task One_plan_alone_uses_the_steps_allowance_and_not_one_unit_a_tick()
    {
        var w=new MemoryWorld(); var d=new Driver(); var session=new TerrainSession();
        var t=new TerrainEngine(new Scheduled(w.For("m"),d),session,null).Apply(Plan("p",64),new(),32);  // 16,384 cells, default CellsPerTick
        d.Finish(t);
        Assert.Equal(TerrainState.Completed,(await t).State);
        Assert.All(d.Writes.Values,n=>Assert.InRange(n,0,4096));
        Assert.Contains(d.Writes.Values,n=>n==4096);
        Assert.InRange(d.Writes.Count(p=>p.Value>0),4,6);  // 16,384 / 4,096 = 4 steps (32 with a tick after every unit)
    }

    // The mod's own CellsPerTick still lowers its share: one unit a tick when it asks for 512.
    [Fact]
    public async Task A_mods_own_cells_per_tick_is_a_ceiling_below_the_hosts()
    {
        var w=new MemoryWorld(); var d=new Driver(); var session=new TerrainSession();
        var t=new TerrainEngine(new Scheduled(w.For("m"),d),session,null).Apply(Plan("p",64),new(){CellsPerTick=1024},32);
        d.Finish(t);
        Assert.Equal(TerrainState.Completed,(await t).State);
        Assert.All(d.Writes.Values,n=>Assert.InRange(n,0,1024));
        Assert.Contains(d.Writes.Values,n=>n==1024);
    }

    [Fact]
    public async Task Native_recovery_and_managed_plans_cannot_spend_the_same_allowance()
    {
        var w=new MemoryWorld(); var d=new Driver {NativeRecovery=512}; var session=new TerrainSession();
        var tasks=Enumerable.Range(0,9).Select(i=>new TerrainEngine(new Scheduled(w.For("m"+i),d),session,null).Apply(Plan("p"+i,64+i*64),new(),32)).ToArray();
        d.Finish(tasks);
        Assert.All(await Task.WhenAll(tasks),r=>Assert.True(r.Ok));
        Assert.All(d.Writes.Values,n=>Assert.InRange(n+512,0,4096));
    }

    [Fact]
    public async Task Expensive_units_stop_admission_for_the_rest_of_the_step()
    {
        var w=new MemoryWorld(); var d=new Driver { ReadCost=3 };
        var t=new TerrainEngine(new Scheduled(w.For("m"),d),new TerrainSession(),null).Inspect(Plan("p",64),new(){InBases=true},32);
        Assert.False(t.IsCompleted);
        Assert.Equal(512,d.Reads[0]);
        d.Finish(t); Assert.True((await t).Ok);
        Assert.All(d.Reads.Values,n=>Assert.InRange(n,0,512));
    }

    [Fact]
    public async Task Synchronous_inspect_yields_instead_of_scanning_the_world_in_one_step()
    {
        var w=new MemoryWorld(); var d=new Driver();
        var t=new TerrainEngine(new Scheduled(w.For("m"),d),new TerrainSession(),null).Inspect(Plan("p",64),new(),32);
        Assert.False(t.IsCompleted);
        d.Finish(t); Assert.True((await t).Ok);
    }
}
