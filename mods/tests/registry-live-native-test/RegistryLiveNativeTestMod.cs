// SPDX-License-Identifier: MIT
// Lab-only access to the C ABI through the same internal bridge as registry-mixed-test. Never a mod-author example.
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json.Nodes;
using Kiln;

namespace RegistryLiveNativeTest;

public sealed unsafe class RegistryLiveNativeTestMod : Mod
{
    [StructLayout(LayoutKind.Sequential)] private struct Str { public byte* Ptr; public nuint Len; }
    [StructLayout(LayoutKind.Sequential)] private struct Reply
    {
        public uint Size, Outcome, Accepted, Refused, Skipped, Reserved;
        public ulong World, Generation;
        public fixed byte Reason[192];
    }
    [StructLayout(LayoutKind.Sequential)] private struct Status
    {
        public uint Size, Kind;
        public ulong World, Generation;
        public uint Drafts, Active;
    }
    private byte* _table;
    private ulong _owner;
    private int _pass, _fail;
    private bool _ran;
    private const string Wood = "Material_T1_Wood_Logs";
    private void Check(bool ok, string text)
    {
        if (ok) ++_pass; else ++_fail;
        Log.Info($"registry-live-native-test: {(ok ? "PASS" : "FAIL")} {text}");
    }
    public override void OnLoad() => Content.OnReady(_ =>
    {
        if (_ran) return;
        _ran = true;
        try { Run(); }
        catch (Exception e) { Check(false, e.ToString()); }
        finally { Log.Info($"registry-live-native-test: DONE pass={_pass} fail={_fail}"); }
    });
    private ulong Begin(uint point)
    {
        ulong handle = 0;
        var fn = *(delegate* unmanaged[Cdecl]<ulong, uint, ulong*, int>*)(_table + 8);
        var rc = fn(_owner, point, &handle);
        Check(rc == 0 && handle != 0, $"begin point={point}, rc={rc}");
        return handle;
    }
    private Reply Submit(ulong handle, string registry, string id, string field, int value)
    {
        var ir = new JsonObject { ["ir"] = 1, ["mod"] = "registry-live-native-test", ["stage"] = "alter",
            ["source"] = "native-table-lab", ["edits"] = new JsonArray(new JsonObject {
                ["registry"] = registry, ["select"] = id, ["field"] = field, ["op"] = value }) };
        var bytes = Encoding.UTF8.GetBytes(ir.ToJsonString());
        var reply = new Reply { Size = (uint)sizeof(Reply) };
        var fn = *(delegate* unmanaged[Cdecl]<ulong, ulong, Str, Reply*, byte*, nuint, nuint*, int>*)(_table + 16);
        fixed (byte* p = bytes) Check(fn(_owner, handle, new Str { Ptr = p, Len = (nuint)bytes.Length }, &reply, null, 0, null) == 0, "submit rc=OK");
        return reply;
    }
    private Reply Finish(ulong handle, bool revert = false)
    {
        var reply = new Reply { Size = (uint)sizeof(Reply) };
        var fn = *(delegate* unmanaged[Cdecl]<ulong, ulong, Reply*, int>*)(_table + (revert ? 32 : 24));
        Check(fn(_owner, handle, &reply) == 0, revert ? "revert rc=OK" : "commit rc=OK");
        return reply;
    }
    private string Read(string registry, string id, string field)
    {
        var r = Encoding.UTF8.GetBytes(registry); var i = Encoding.UTF8.GetBytes(id); var f = Encoding.UTF8.GetBytes(field);
        var buffer = new byte[256]; nuint len = 0;
        var fn = *(delegate* unmanaged[Cdecl]<ulong, Str, Str, Str, byte*, nuint, nuint*, int>*)(_table + 40);
        fixed (byte* rp = r, ip = i, fp = f, bp = buffer)
            Check(fn(_owner, new Str { Ptr = rp, Len = (nuint)r.Length }, new Str { Ptr = ip, Len = (nuint)i.Length },
                new Str { Ptr = fp, Len = (nuint)f.Length }, bp, (nuint)buffer.Length, &len) == 0, "mapped read rc=OK");
        return Encoding.UTF8.GetString(buffer, 0, (int)Math.Min(len, (nuint)(buffer.Length - 1)));
    }
    private void Run()
    {
        var bridge = typeof(Content).Assembly.GetType("Kiln.BlocksBridge");
        _table = (byte*)(nint)(bridge?.GetMethod("ServiceTable", BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static)?
            .Invoke(null, new object[] { "kiln.registry.live", 1u }) ?? (nint)0);
        _owner = (ulong)(typeof(ModContext).GetProperty("Owner", BindingFlags.NonPublic | BindingFlags.Instance)?.GetValue(Context) ?? 0UL);
        Check(_table != null && *(uint*)_table >= 56 && _owner != 0 && sizeof(Reply) == 232, "v1 native layout and mod owner");
        if (_table == null || *(uint*)_table < 56 || _owner == 0) return;
        var status = new Status { Size = (uint)sizeof(Status) };
        var statusFn = *(delegate* unmanaged[Cdecl]<Status*, int>*)(_table + 48);
        Check(statusFn(&status) == 0 && status.World != 0 && status.Kind == 0, "current server-world identity is a nonzero Kiln token");
        var item = Content.Items.Get(Wood) ?? throw new InvalidOperationException("fixture wood is absent");
        var before = (int)item.GetNumber("maxStackSize");
        Check(!item.Modified, "fixture starts unmodified (primes the entry-flag cache)");
        var h = Begin(4);
        var submitted = Submit(h, "items", "game:material_t1_wood_logs", "maxStack", before + 123);
        Check(submitted.Accepted == 1 && submitted.Refused == 0, "mapped edit accepted");
        var applied = Finish(h);
        Check(applied.Outcome == 0 && item.GetNumber("maxStackSize") == before + 123, "live bytes changed");
        Check(Read("items", "game:material_t1_wood_logs", "maxStack") == (before + 123).ToString(System.Globalization.CultureInfo.InvariantCulture), "stable read returns those live bytes");
        Check(Content.Items.Get(Wood)!.Modified, "live commit refreshes the entry's modified flag");
        item.Set("maxStackSize", before + 333); // an existing Content.* code owner must stay above the registry
        var reverted = Finish(h, true);
        Check(reverted.Outcome == 5 && item.GetNumber("maxStackSize") == before + 333, "revert leaves the code layer in place");
        Content.Revert();
        Check(item.GetNumber("maxStackSize") == before, "taking code off restores exactly the value before the transaction");
        Check(!Content.Items.Get(Wood)!.Modified, "reverting all layers clears the entry's modified flag");
        h = Begin(3);
        submitted = Submit(h, "balance", "game:balance", "levelCap", 60);
        Check(submitted.Accepted == 0 && submitted.Refused == 1, "hot reload refuses the restart-only field");
        Check(Finish(h).Outcome == 3, "restart-required outcome, never a silent write");
    }
}
