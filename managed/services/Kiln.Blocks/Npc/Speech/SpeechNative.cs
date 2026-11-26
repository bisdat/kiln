// SPDX-License-Identifier: MIT
// The native kiln.npcspeech table (native/core/abi/services/kiln_npcspeech.h). Field order and types must match the C header.
using System.Runtime.InteropServices;

namespace Kiln;

internal static unsafe class SpeechNative
{
    public const string Name = "kiln.npcspeech";
    public const uint Version = 1;
    public const int Ok = 0, InvalidArgument = 1, Unavailable = 3;
    public const string BubbleCapability = "npcspeech.bubble";
    public const string VoiceCapability = "npcspeech.voice";

    [StructLayout(LayoutKind.Sequential)]
    public struct Status
    {
        public uint Size, Resolved, Installed, Overrides, Says, Texts;
        public ulong Bubbles, Changed, Muted;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Table
    {
        public uint HeaderSize, HeaderVersion;
        public delegate* unmanaged<uint, uint, uint*, uint, int> Set;
        public delegate* unmanaged<uint, uint, uint, int> Say;
        public delegate* unmanaged<uint, byte*, nuint, int> Text;
        public delegate* unmanaged<int> ClearAll;
        public delegate* unmanaged<Status*, int> GetStatus;
    }

    private static Table* _t;
    private static bool _looked;

    /// <summary>Test seam: a table to use instead of the process's (null: look it up).</summary>
    internal static Func<nint>? TableHook;

    internal static void LookAgain()
    {
        if (_t == null) _looked = false;
    }

    /// <summary>The table, or null where kiln.npcspeech is not loaded (a dedicated server, an unknown client build).</summary>
    public static Table* T
    {
        get
        {
            if (TableHook is { } hook) return (Table*)hook();
            if (_t != null || _looked) return _t;
            try { _t = (Table*)BlocksBridge.ServiceTable(Name, Version); }
            catch (Exception) { _t = null; }
            _looked = _t != null || Lifecycle.IsServerRunning;
            return _t;
        }
    }

    public static bool Loaded => T != null;

    /// <summary>Without kiln.npcspeech, its sigdb features are lowered, and where sigdb never declared them (a dedicated
    /// server: the bubble is client data) they get a row that says why (Kiln.Npcs' members name them).</summary>
    internal static void ReportRows()
    {
        if (ServiceCaps.Probe(Name)) return;  // the service is here: sigdb's rows say what works
        ServiceCaps.LowerFeatures(Name, BubbleCapability, VoiceCapability);
        var server = !Client.IsClient;
        foreach (var row in new[] { BubbleCapability, VoiceCapability })
            if (Capabilities.Get(row).Reason == CapabilityReason.Unknown)
                ServiceCaps.Report(row, CapabilityState.Unavailable, server ? CapabilityReason.WrongSide : CapabilityReason.ServiceMissing,
                    server ? "the NPC speech bubble is the game client's: a server sends Kiln.Npcs' decisions to its Kiln clients, which show them"
                           : "kiln.npcspeech is not here: this game shows no Kiln speech");
    }

    public static Status? Read()
    {
        var t = T;
        if (t == null || t->GetStatus == null) return null;
        var s = new Status { Size = (uint)sizeof(Status) };
        return t->GetStatus(&s) == Ok ? s : null;
    }

    /// <summary>Registers the text of a tag once per process (kiln.content keeps it).</summary>
    private static readonly HashSet<uint> Registered = new();

    public static int Text(uint tag, string json)
    {
        var t = T;
        if (t == null) return Unavailable;
        lock (Registered)
        {
            if (Registered.Contains(tag)) return Ok;
            var bytes = System.Text.Encoding.UTF8.GetBytes(json);
            int rc;
            fixed (byte* p = bytes) rc = t->Text(tag, p, (nuint)bytes.Length);
            if (rc == Ok) Registered.Add(tag);
            return rc;
        }
    }

    public static int Set(uint npc, SpeechMode mode, uint[] tags)
    {
        var t = T;
        if (t == null) return Unavailable;
        fixed (uint* p = tags) return t->Set(npc, (uint)mode, tags.Length == 0 ? null : p, (uint)tags.Length);
    }

    public static int Say(uint npc, uint tag, uint ms)
    {
        var t = T;
        return t == null ? Unavailable : t->Say(npc, tag, ms);
    }

    public static void ClearAll()
    {
        var t = T;
        if (t != null) t->ClearAll();
    }

    internal static void ForgetTexts()
    {
        lock (Registered) Registered.Clear();
    }
}
