// SPDX-License-Identifier: MIT
// The persistent framework module of chapter 03 §4.6: started by Kiln's service autostart (ServiceTick.AutoStart, after
// Kiln.NetworkStartup) at the first service tick, under no mod. It opens the channel kiln.camera v1 for Kiln itself
// (never for the first mod that happens to call Camera), creates the bridge's own native owner
// (kiln.camera.transport), and binds KilnCameraTransport in native kiln.camera; camera.remote is on from then on. The
// table's functions are static (UnmanagedCallersOnly), so they stay valid for the life of the process: the bridge is never
// unbound while the game runs.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using Kiln.CameraTransport;

namespace Kiln
{
    /// <summary>Started by Kiln (ServiceTick.AutoStart) at its first service tick: the rows are there before a mod asks,
    /// and the transport bridge is bound (camera.remote).</summary>
    internal static class CameraStartup
    {
        static CameraStartup()
        {
            CameraCaps.Rows.Ensure();
            CameraBridgeStartup.Start();
        }
    }
}

namespace Kiln.CameraTransport
{
    /// <summary>Starts and holds the bridge; the native transport table's functions.</summary>
    internal static unsafe class CameraBridgeStartup
    {
        internal const string ChannelName = "kiln.camera";
        internal const string ChannelVersion = "1";
        internal const string OwnerId = "kiln.camera.transport";

        private static int _started;
        private static ulong _owner;

        /// <summary>The running bridge (null before the start or when it could not start).</summary>
        internal static CameraBridgeHost? Host { get; private set; }

        /// <summary>What the start did (logs, tests).</summary>
        internal static string State { get; private set; } = "not started";

        /// <summary>Lab servers only: the player a lab transport peer (tools/nettest) stands for. A lab peer has no game
        /// player; a lab test mod names one. Null: lab peers carry no player.</summary>
        internal static Func<NetPeer, uint>? LabPlayerOf { get; set; }

        /// <summary>Starts the bridge once (CameraStartup; tests pass fakes). Never throws. Whatever mod's code runs, the
        /// channel and the bridge are Kiln's.</summary>
        internal static void Start(ICameraNet? net = null, ICameraLocalNative? local = null, Func<ulong, int>? bind = null)
        {
            if (Interlocked.CompareExchange(ref _started, 1, 0) != 0) return;
            var outer = ModContext.CurrentMod;
            ModContext.CurrentMod = null;  // Kiln's own channel, owner and handlers, never a mod's
            try
            {
                StartCore(net, local, bind);
            }
            catch (Exception e)
            {
                State = "failed: " + e.Message;
                Log(LogLevel.Warn, $"the camera transport did not start: {e.Message}; camera.remote stays off");
            }
            finally
            {
                ModContext.CurrentMod = outer;
            }
        }

        /// <summary>Tests: forget the bridge (the fakes are gone).</summary>
        internal static void Reset()
        {
            Host = null;
            _owner = 0;
            _started = 0;
            State = "not started";
            LabPlayerOf = null;
        }

        private static void StartCore(ICameraNet? net, ICameraLocalNative? local, Func<ulong, int>? bind)
        {
            var t = CameraNative.T;
            if (t == null && bind is null)
            {
                State = "kiln.camera is not here";
                return;  // the native rows say why (Kiln.Camera reports them all off)
            }
            net ??= new NetworkCameraNet();
            if (!net.Available)
            {
                State = "no transport";
                Log(LogLevel.Info, "kiln.net is not here: no server camera reaches a player's client (camera.remote stays off)");
                return;
            }
            var limit = net.Open(static (peer, toServer, message, body) => Host?.OnRaw(peer, toServer, message, body));
            var owner = Backend.Current.OwnerCreate(OwnerId);
            if (owner == 0)
            {
                State = "no owner";
                return;
            }
            _owner = owner;
            local ??= new NativeCameraLocal(owner);
            var host = new CameraBridgeHost(net, local, static () => Environment.TickCount64, Random64, limit, Ended);
            Host = host;
            int rc;
            if (bind is not null)
            {
                rc = bind(owner);  // tests: they pump the host themselves
            }
            else
            {
                ulong handle = 0;
                t->Subscribe(owner, &OnLocalChanged, null, &handle);  // the client's own ends of the bridge's LOCAL leases
                ServiceTick.Add("kiln camera bridge", static (in TickInfo _) => Host?.ServerPump());
                if (net is NetworkCameraNet real) real.HookClientTick(static () => Host?.ClientPump());
                rc = Bind(t, owner);
            }
            if (rc != CameraNative.Ok)
            {
                State = $"bind_transport answered {rc}";
                Log(LogLevel.Warn, $"kiln.camera refused the transport bridge ({rc}): camera.remote stays off");
                return;
            }
            State = "bound";
            Log(LogLevel.Info, $"the transport bridge is bound: {ChannelName} v{ChannelVersion} (at most {Math.Min(limit, CameraWireAbi.MaxBody)} bytes), " +
                               "server visuals reach players whose client runs Kiln, the host's own player through a local queue");
        }

        private static int Bind(CameraNative.Table* t, ulong owner)
        {
            var table = new CameraNative.Transport
            {
                Header = new ServiceHeader { Size = (uint)sizeof(CameraNative.Transport), Version = 1 },
                Submit = &OnSubmit,
                Caps = &OnCaps,
                Retire = &OnRetire,
            };
            return t->BindTransport(owner, &table);  // copied by kiln.camera
        }

        private static ulong Random64()
        {
            Span<byte> b = stackalloc byte[8];
            RandomNumberGenerator.Fill(b);
            return BitConverter.ToUInt64(b);
        }

        private static void Log(LogLevel level, string text) => Backend.Current.Log(0, level, "camera: " + text);

        // ---- transport_ended ----

        private static void Ended(RemoteEnded e)
        {
            var t = CameraNative.T;
            if (t == null || _owner == 0) return;
            var text = Encoding.UTF8.GetBytes(e.EndReason switch
            {
                CameraEndReasons.Expired => "the player's client let the lease expire",
                CameraEndReasons.PeerDisconnected => "the player's connection ended",
                CameraEndReasons.AuthorityTimeout => "the player's client stopped hearing the server",
                CameraEndReasons.WorldStopped => "the server world stopped",
                _ => "the player's client ended the lease",
            });
            fixed (byte* p = text)
                t->TransportEnded(_owner, e.Lease, e.Revision, e.EndReason, e.Reason, new NativeStr { Ptr = p, Len = (nuint)text.Length });
        }

        // ---- the native transport table (KilnCameraTransport) ----

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        private static int OnSubmit(ulong author, uint op, ulong lease, ulong revision, CameraNative.Request* request,
            delegate* unmanaged[Cdecl]<CameraNative.Result*, void*, void> done, void* user)
        {
            try
            {
                var host = Host;
                if (host == null || done == null) return CameraNative.Unavailable;
                if (op is < 1 or > 4 || lease == 0 || revision == 0 || (op == 4) != (request == null)) return CameraNative.InvalidArgument;
                var wire = request == null ? null : CameraNativeWire.FromNative(request);
                var player = request == null ? 0u : request->Player;
                var fn = (nint)done;
                var u = (nint)user;
                host.Submit(author, (TransportOp)op, lease, revision, player, wire, r => CallDone(fn, u, r, player));
                return CameraNative.Ok;
            }
            catch
            {
                return CameraNative.InvalidArgument;  // never unwind into native code
            }
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        private static int OnCaps(uint player, ulong* mask)
        {
            try
            {
                if (mask == null) return CameraNative.InvalidArgument;
                *mask = 0;
                if (Host is not { } host || !host.Caps(player, out var m)) return CameraNative.NotFound;
                *mask = m;
                return CameraNative.Ok;
            }
            catch
            {
                return CameraNative.NotFound;
            }
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        private static void OnRetire(ulong author)
        {
            try
            {
                Host?.Retire(author);
            }
            catch
            {
                // never unwind into native code
            }
        }

        private static void CallDone(nint fn, nint user, TransportResult r, uint player)
        {
            var text = r.Diagnostic is { Length: > 0 } d ? Encoding.UTF8.GetBytes(d) : [];
            var result = new CameraNative.Result
            {
                Size = (uint)sizeof(CameraNative.Result), Ok = r.Ok ? 1u : 0u, ReasonCode = r.Reason, CommitScope = r.Scope, Flags = r.Flags,
                Endpoint = CameraNative.EndpointRemote, Player = player, Lease = r.Lease, Revision = r.Revision, Frame = r.Frame,
            };
            fixed (byte* p = text)
            {
                result.Reason = new NativeStr { Ptr = p, Len = (nuint)text.Length };
                ((delegate* unmanaged[Cdecl]<CameraNative.Result*, void*, void>)fn)(&result, (void*)user);
            }
        }

        // ---- the bridge's LOCAL requests: answers and changes (client Pre or a world-stop thread: queued) ----

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        internal static void OnLocalDone(CameraNative.Result* r, void* user)
        {
            try
            {
                if (r == null || Host is not { } host) return;
                var text = r->Ok != 0 ? null : r->Reason.ToString();
                host.LocalDone((nint)user, new NativeOutcome(r->Ok != 0, (ushort)Math.Min(r->ReasonCode, CameraWireAbi.MaxReason), (byte)r->CommitScope,
                    r->Flags & (CameraWireAbi.FlagMayHaveApplied | CameraWireAbi.FlagAlreadyEnded), r->Frame, text is { Length: > 0 } ? text : null));
            }
            catch
            {
                // never unwind into native code
            }
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        private static void OnLocalChanged(CameraNative.Change* c, void* user)
        {
            try
            {
                if (c == null || c->Endpoint != CameraNative.EndpointLocal || Host is not { } host) return;
                host.LocalChanged(c->Lease, c->Kind, (byte)Math.Min(c->EndReason, CameraWireAbi.MaxEndReason), (ushort)Math.Min(c->ReasonCode, CameraWireAbi.MaxReason), c->Frame);
            }
            catch
            {
                // never unwind into native code
            }
        }
    }

    /// <summary>The canonical wire request from and to native kiln.camera's KilnCameraRequest (the adapter point of
    /// CameraWireRequest): the same fields, no other shape.</summary>
    internal static unsafe class CameraNativeWire
    {
        public static CameraWireRequest FromNative(CameraNative.Request* q)
        {
            var keys = new WireKey[q->Keys == null ? 0 : Math.Min(q->KeyCount, (uint)CameraWireAbi.MaxKeys)];
            for (var i = 0; i < keys.Length; i++)
            {
                var k = q->Keys[i];
                keys[i] = new WireKey(k.AtMs, k.Pose.X, k.Pose.Y, k.Pose.Z, k.Pose.Qx, k.Pose.Qy, k.Pose.Qz, k.Pose.Qw, k.FovDegrees);
            }
            var profile = (q->Parts & CameraWireAbi.PartProfile) != 0;
            return new CameraWireRequest
            {
                Parts = q->Parts, InputPolicy = (byte)q->InputPolicy, DurationMs = q->DurationMs, BlendInMs = q->BlendInMs, BlendOutMs = q->BlendOutMs,
                Priority = q->Priority, LensSource = (byte)q->LensSource, CameraId = q->CameraId, Transition = q->Transition,
                FollowEntity = profile ? q->Profile.Follow : 0, RotationMode = profile ? (byte)q->Profile.RotationMode : (byte)0,
                Collision = profile ? (byte)q->Profile.Collision : (byte)0, Distance = profile ? q->Profile.Distance : 0,
                OffsetX = profile ? q->Profile.OffsetX : 0, OffsetY = profile ? q->Profile.OffsetY : 0, OffsetZ = profile ? q->Profile.OffsetZ : 0,
                OffsetWorldY = profile ? q->Profile.OffsetWorldY : 0, RotX = profile ? q->Profile.Qx : 0, RotY = profile ? q->Profile.Qy : 0,
                RotZ = profile ? q->Profile.Qz : 0, RotW = profile ? q->Profile.Qw : 1, FovDegrees = q->FovDegrees,
                Amplitude = q->Shake.Amplitude, Frequency = q->Shake.Frequency, FalloffMs = q->Shake.FalloffMs, Seed = q->Shake.Seed, Keys = keys,
            };
        }

        /// <summary>A LOCAL_VISUAL request of the local player (keys: the caller pins <paramref name="keys"/>).</summary>
        public static CameraNative.Request ToNative(CameraWireRequest r, CameraNative.Key[] keys)
        {
            for (var i = 0; i < r.Keys.Length; i++)
            {
                var k = r.Keys[i];
                keys[i] = new CameraNative.Key
                {
                    AtMs = k.AtMs, Pose = new CameraNative.Pose { X = k.X, Y = k.Y, Z = k.Z, Qx = k.Qx, Qy = k.Qy, Qz = k.Qz, Qw = k.Qw }, FovDegrees = k.Fov,
                };
            }
            var q = new CameraNative.Request
            {
                Size = (uint)sizeof(CameraNative.Request), Parts = r.Parts, Endpoint = CameraNative.EndpointLocal, Player = 0, InputPolicy = r.InputPolicy,
                DurationMs = r.DurationMs, BlendInMs = r.BlendInMs, BlendOutMs = r.BlendOutMs, Priority = r.Priority, LensSource = r.LensSource,
                FovDegrees = r.FovDegrees, CameraId = r.CameraId, Transition = r.Transition, KeyCount = (uint)r.Keys.Length,
            };
            q.Profile.Size = (uint)sizeof(CameraNative.Profile);
            q.Profile.Qw = 1;
            if (r.Has(CameraWireAbi.PartProfile))
            {
                q.Profile.Follow = r.FollowEntity;
                q.Profile.RotationMode = r.RotationMode;
                q.Profile.Collision = r.Collision;
                q.Profile.Distance = r.Distance;
                q.Profile.OffsetX = r.OffsetX;
                q.Profile.OffsetY = r.OffsetY;
                q.Profile.OffsetZ = r.OffsetZ;
                q.Profile.OffsetWorldY = r.OffsetWorldY;
                q.Profile.Qx = r.RotX;
                q.Profile.Qy = r.RotY;
                q.Profile.Qz = r.RotZ;
                q.Profile.Qw = r.RotW;
            }
            if (r.Has(CameraWireAbi.PartShake))
                q.Shake = new CameraNative.Shake { Amplitude = r.Amplitude, Frequency = r.Frequency, FalloffMs = r.FalloffMs, Seed = r.Seed };
            return q;
        }
    }

    /// <summary>native kiln.camera's LOCAL_VISUAL entries for the bridge (in a game).</summary>
    internal sealed unsafe class NativeCameraLocal(ulong transportOwner) : ICameraLocalNative
    {
        private const ulong LocalParts = CameraWireAbi.RemoteParts;

        public ulong LocalCaps()
        {
            var t = CameraNative.T;
            ulong mask = 0;
            return t != null && t->Capabilities(CameraNative.EndpointLocal, 0, &mask) == CameraNative.Ok ? mask & LocalParts : 0;
        }

        // one native owner per remote author (LocalEndpoint reuses them, at most 32)
        public ulong OwnerFor(ulong ownerToken) => transportOwner == 0 ? 0 : Backend.Current.OwnerCreate(CameraBridgeStartup.OwnerId + ".remote");

        public int Apply(ulong owner, CameraWireRequest request, nint key, out ulong lease)
        {
            lease = 0;
            var t = CameraNative.T;
            if (t == null) return CameraNative.Unavailable;
            var keys = new CameraNative.Key[request.Keys.Length];
            var q = CameraNativeWire.ToNative(request, keys);
            ulong l = 0;
            int rc;
            fixed (CameraNative.Key* k = keys)
            {
                q.Keys = keys.Length == 0 ? null : k;
                rc = t->Apply(owner, &q, &l, &CameraBridgeStartup.OnLocalDone, (void*)key);
            }
            lease = l;
            return rc;
        }

        public int Update(ulong owner, ulong lease, byte timing, CameraWireRequest request, nint key)
        {
            var t = CameraNative.T;
            if (t == null) return CameraNative.Unavailable;
            var keys = new CameraNative.Key[request.Keys.Length];
            var q = CameraNativeWire.ToNative(request, keys);
            fixed (CameraNative.Key* k = keys)
            {
                q.Keys = keys.Length == 0 ? null : k;
                return t->Update(owner, lease, timing, &q, &CameraBridgeStartup.OnLocalDone, (void*)key);
            }
        }

        public int End(ulong owner, ulong lease, nint key)
        {
            var t = CameraNative.T;
            return t == null ? CameraNative.Unavailable : t->End(owner, lease, &CameraBridgeStartup.OnLocalDone, (void*)key);
        }
    }

    /// <summary>The bridge's network in a game: Kiln.Network and Kiln.Client.</summary>
    internal sealed class NetworkCameraNet : ICameraNet
    {
        private NetChannel? _channel;

        public bool Available => Network.Available;

        public int Open(CameraRawHandler onRaw)
        {
            _channel = Network.Channel(CameraBridgeStartup.ChannelName, CameraBridgeStartup.ChannelVersion, required: false, maxMessageBytes: CameraWireAbi.MaxBody);
            _channel.OnRaw((ctx, message, body) => onRaw(ctx.Sender, ctx.Direction == NetDirection.ToServer, message, body));
            return _channel.MaxMessageBytes;
        }

        public void HookClientTick(Action pump)
        {
            if (!Client.Available || !Client.IsClient) return;
            Client.Tick += (in TickInfo _) => pump();
        }

        private static CameraPeerView View(NetPeer p)
        {
            var player = p.Player.Value;
            if (player == 0 && p.IsLab && CameraBridgeStartup.LabPlayerOf is { } lab) player = lab(p);
            return new(p, p.Id, p.State, player, p.HasChannel(CameraBridgeStartup.ChannelName), !p.IsServer && !p.IsLab && (p.Id & 0x7F) == 0);
        }

        public IReadOnlyList<CameraPeerView> ServerPeers()
        {
            var peers = Network.Peers;
            var list = new List<CameraPeerView>(peers.Count);
            foreach (var p in peers) list.Add(View(p));
            return list;
        }

        public CameraPeerView? ServerLink() => Network.Server is { } s ? View(s) : null;

        public bool Send(object peer, ushort message, ReadOnlySpan<byte> body) => _channel is { } ch && peer is NetPeer p && ch.SendRaw(p, message, body);

        public bool IsClient => Client.Available && Client.IsClient;

        public bool IsHosting => Client.Available && Client.IsHosting;

        public uint LocalPlayer => Client.Available && Client.HasLocalPlayer ? Client.LocalPlayer.Value : 0;

        // a dedicated server runs one server world from its start; a client only while it hosts
        public uint ServerWorld => !Client.Available ? 1u : Client.IsClient && !Client.IsHosting ? 0u : Client.ServerWorldSession;

        public uint ClientWorld => Client.Available && Client.IsClient && Client.InGame ? Client.ClientWorldSession : 0u;
    }
}
