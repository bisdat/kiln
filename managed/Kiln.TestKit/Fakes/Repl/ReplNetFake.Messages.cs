// SPDX-License-Identifier: MIT
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using static Kiln.ReplNetNative;
using static Kiln.TestKit.FakeTables;

namespace Kiln.TestKit;

internal sealed unsafe partial class ReplNetFake
{
    private const int MaxMessage = 0x4000;

    private static bool Hookable(byte channel) => channel <= 24 && channel is not (4 or 5 or 12 or 14 or 17);
    private static bool Injectable(byte channel) => channel is 1 or 15 or 19 or 20;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int AddHook(ulong owner, byte channel, uint directions, delegate* unmanaged[Cdecl]<Message*, void*, int> callback,
        void* user, ulong* handle)
    {
        const uint both = ReplAbi.Send | ReplAbi.Receive;
        if (!Hookable(channel) || directions == 0 || (directions & ~both) != 0 || callback == null || handle == null)
            return InvalidArgument;
        if (ActiveFor(CapMessagesSend | CapMessagesReceive) is not { } harness) return Unavailable;
        if (!OnTickThread) return WrongThread;
        var repl = ReplFake.StateOf(harness);
        lock (repl.Lock)
        {
            if (repl.Net.Hooks.Count >= 64) return InvalidArgument;
            *handle = (ulong)Interlocked.Increment(ref _serial);
            repl.Net.Hooks.Add(new ReplNetState.Hook(*handle, owner, channel, directions, (nint)callback, (nint)user));
        }
        return Ok;
    }

    internal static byte[]? DriveMessage(KilnHarness harness, uint peer, GameChannel channel, MessageDirection direction, byte[] bytes)
    {
        if (!Hookable((byte)channel)) throw new ArgumentException("this channel cannot be hooked", nameof(channel));
        if (direction is not (MessageDirection.Receive or MessageDirection.Send))
            throw new ArgumentException("choose one message direction", nameof(direction));
        if (bytes.Length is 0 or > MaxMessage) throw new ArgumentException("1..16384 bytes", nameof(bytes));
        if (ActiveFor(direction == MessageDirection.Send ? CapMessagesSend : CapMessagesReceive) != harness)
            throw ReplicationCaps.Off(ReplicationCaps.Messages, "scripted game message hooks are off");
        var repl = ReplFake.StateOf(harness);
        lock (repl.Lock)
        {
            var isClient = harness.Options.Side == HarnessSide.JoinedClient;
            if (isClient ? peer != 0 : repl.MachineOf(peer) < 0)
                throw new ArgumentException(isClient ? "use peer 0 for the server" : "the peer is not connected", nameof(peer));
        }
        return RunHooks(harness, repl, peer, channel, direction, bytes);
    }

    private static byte[]? RunHooks(KilnHarness harness, ReplFakeState repl, uint peer, GameChannel channel, MessageDirection direction,
        byte[] original)
    {
        ReplNetState.Hook[] hooks;
        Func<GameChannel, ReadOnlyMemory<byte>, bool>? validate;
        lock (repl.Lock)
        {
            hooks = repl.Net.Hooks.Where(h => h.Channel == (byte)channel && (h.Directions & (uint)direction) != 0).ToArray();
            validate = repl.Net.ValidateSend;
        }
        var bytes = original;
        foreach (var hook in hooks)
        {
            lock (repl.Lock)
                if (!repl.Net.Hooks.Contains(hook) || harness.ReplOwnerRevoked(hook.Owner)) continue;

            var scratch = (byte[])bytes.Clone();
            var length = (uint)bytes.Length;
            int action;
            fixed (byte* data = bytes, rewrite = scratch)
            {
                var message = new Message
                {
                    Size = (uint)sizeof(Message), Peer = peer, Channel = (byte)channel, Direction = (byte)direction,
                    Length = length, Data = data, Rewrite = rewrite, RewriteCap = length, RewriteLength = &length,
                };
                var callback = (delegate* unmanaged[Cdecl]<Message*, void*, int>)hook.Callback;
                action = callback(&message, (void*)hook.User);
            }
            if (action == Drop) return null;
            if (action == Rewrite && length <= scratch.Length)
                bytes = length == scratch.Length ? scratch : scratch[..(int)length];
        }
        if (direction == MessageDirection.Send && bytes != original && validate?.Invoke(channel, bytes) != true)
            return original;
        return bytes;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Inject(uint peer, byte channel, void* data, uint length)
    {
        if (ActiveFor(CapMessagesSend) is not { } harness || harness.Options.Side == HarnessSide.JoinedClient) return Unavailable;
        if (!OnTickThread) return WrongThread;
        if (!Injectable(channel) || data == null || length is 0 or > MaxMessage) return InvalidArgument;

        var repl = ReplFake.StateOf(harness);
        var bytes = new ReadOnlySpan<byte>(data, (int)length).ToArray();
        Func<GameChannel, ReadOnlyMemory<byte>, bool>? validate;
        lock (repl.Lock)
        {
            repl.Net.Injections.Add((peer, (GameChannel)channel, bytes));
            var machine = repl.MachineOf(peer);
            if (machine < 0 || (repl.Machines[machine]!.Flags & ReplAbi.PeerFake) != 0) return NotFound;
            validate = repl.Net.ValidateSend;
        }
        if (validate is null) return Unavailable;
        try
        {
            return validate((GameChannel)channel, bytes) ? Ok : InvalidArgument;
        }
        catch (Exception exception)
        {
            ReplNetLog.Warn($"test kit: the send validator threw {exception.GetType().Name}: {exception.Message}");
            return Internal;
        }
    }
}
