// SPDX-License-Identifier: MIT
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json.Nodes;

namespace Kiln.Admin.Tests;

/// <summary>A KAP/1 client for tests (docs/kiln/launcher/admin-protocol.md): handshake, req, sub, reading frames.</summary>
internal sealed class KapClient : IDisposable
{
    private readonly TcpClient _tcp;
    private readonly NetworkStream _s;
    private readonly List<JsonObject> _held = [];
    private ulong _id;
    private volatile bool _closed;

    public JsonObject Hello { get; }
    public JsonObject Ready { get; }

    public KapClient(int port, string tokenFile, string client = "kiln-admin-tests")
    {
        _tcp = new TcpClient("127.0.0.1", port) { ReceiveTimeout = 10_000 };
        _s = _tcp.GetStream();
        Hello = Read();
        var token = Convert.FromHexString(File.ReadAllText(tokenFile).Trim());
        var nonce = Hello["nonce"]!.GetValue<string>();
        var cnonce = Convert.ToHexStringLower(RandomNumberGenerator.GetBytes(32));
        var mac = HMACSHA256.HashData(token, Encoding.ASCII.GetBytes("kap1-auth\n" + nonce + "\n" + cnonce));
        Send(new JsonObject { ["t"] = "auth", ["proto"] = "kap/1", ["cnonce"] = cnonce, ["mac"] = Convert.ToHexStringLower(mac), ["client"] = client });
        Ready = Read();
        if (Ready["t"]?.GetValue<string>() != "ready") throw new InvalidOperationException("no ready: " + Ready.ToJsonString());
        _tcp.ReceiveTimeout = 0;
        new Thread(ReadLoop) { IsBackground = true, Name = "kap test reader" }.Start();
    }

    public void Send(JsonObject frame)
    {
        var payload = Encoding.UTF8.GetBytes(frame.ToJsonString());
        var head = BitConverter.GetBytes((uint)payload.Length);
        _s.Write(head);
        _s.Write(payload);
    }

    public JsonObject Read()
    {
        var head = new byte[4];
        _s.ReadExactly(head);
        var payload = new byte[BitConverter.ToUInt32(head)];
        _s.ReadExactly(payload);
        return (JsonObject)JsonNode.Parse(payload)!;
    }

    /// <summary>The next frame that matches (frames read on the way are kept for later), from the reader thread.</summary>
    public JsonObject Next(Func<JsonObject, bool> match, int timeoutMs = 10_000)
    {
        var until = Environment.TickCount64 + timeoutMs;
        lock (_held)
        {
            while (true)
            {
                for (var i = 0; i < _held.Count; i++)
                    if (match(_held[i]))
                    {
                        var f = _held[i];
                        _held.RemoveAt(i);
                        return f;
                    }
                var left = until - Environment.TickCount64;
                if (left <= 0 || _closed) break;
                Monitor.Wait(_held, (int)left);
            }
            throw new TimeoutException("no matching frame" + (_closed ? " (closed)" : "") + "; held: " + string.Join(" ", _held.Select(h => h.ToJsonString())));
        }
    }

    /// <summary>Frames seen so far that match (left in place).</summary>
    public List<JsonObject> Seen(Func<JsonObject, bool> match)
    {
        lock (_held) return _held.Where(match).ToList();
    }

    public bool Closed => _closed;

    private void ReadLoop()
    {
        try
        {
            while (true)
            {
                var f = Read();
                lock (_held)
                {
                    _held.Add(f);
                    Monitor.PulseAll(_held);
                }
            }
        }
        catch (Exception)
        {
            lock (_held)
            {
                _closed = true;
                Monitor.PulseAll(_held);
            }
        }
    }

    /// <summary>Sends a req and returns its res (the caller drives the tick thread meanwhile through <paramref name="pump"/>).</summary>
    public JsonObject Call(string op, JsonObject? args, Action pump)
    {
        var id = ++_id;
        Send(new JsonObject { ["t"] = "req", ["id"] = id, ["op"] = op, ["args"] = args ?? new JsonObject() });
        var until = Environment.TickCount64 + 10_000;
        while (Environment.TickCount64 < until)
        {
            pump();
            try
            {
                return Next(f => f["t"]?.GetValue<string>() == "res" && f["id"]?.GetValue<ulong>() == id, 50);
            }
            catch (TimeoutException)
            {
            }
        }
        throw new TimeoutException($"no answer to {op}");
    }

    public JsonObject Sub(string topic, bool on = true)
    {
        var id = ++_id;
        Send(new JsonObject { ["t"] = on ? "sub" : "unsub", ["id"] = id, ["topic"] = topic });
        return Next(f => f["t"]?.GetValue<string>() == "res" && f["id"]?.GetValue<ulong>() == id);
    }

    public void Dispose() => _tcp.Dispose();
}
