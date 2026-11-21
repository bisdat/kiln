// SPDX-License-Identifier: MIT
// Lab test mod of Kiln.ModValues. A lab peer has no player, so mvnet.py's peers join as fixed characters through
// kiln:character's lab join, and ask this mod over "mod-values-test:ctl" to act for them. An award runs the ordinary
// server command mvt-award through Commands.Execute with a player source (a fixed lab subject, never the console), so
// it proves the command path a UI button would take; the character comes from the request's fixture, as in the
// research run (production derives it from the player).
using System.Text.Json;
using Kiln;

namespace ModValuesTest;

public sealed record Req(int Id, string Op, string Character, long Amount = 0);

public sealed record Reply(int Id, bool Ok, string Reason, long Xp, long Coins, ulong Rev, long Carried, long Tokens, bool Tick, string Source);

public sealed class ModValuesTestMod : Mod
{
    private const ulong Account = 0x1111222233336666;
    private static readonly ModValueOptions XpOptions = new(ModValueKind.Experience, UiText.Of(("en", "Fishing XP"), ("de", "Angel-EP")),
        Thresholds: [0, 100, 400, 900], ReadPolicy: ModValueReadPolicy.Public, Sync: ModValueSyncPolicy.Readers);
    private static readonly ModValueOptions CoinOptions = new(ModValueKind.Currency, "Coins", Max: 1_000_000);
    private static readonly ModValueOptions CarriedOptions = new(ModValueKind.Experience, "Carried XP", Thresholds: [0, 1000],
        Persistence: ModValuePersistence.Carried, TrustClientImports: true);

    private NetChannel? _ctl;
    private CharacterKey _active;
    private string _source = "";
    private readonly Dictionary<(uint, int), Reply> _seen = [];

    public override void OnLoad()
    {
        foreach (var (id, o) in new[] { ("xp", XpOptions), ("coins", CoinOptions), ("carried", CarriedOptions) })
        {
            var r = ModValues.Define(id, o);
            Log.Info($"mod-values-test: define {id} ok={r.Ok}{(r.Ok ? "" : " " + r.Reason)}");
        }
        _ctl = Network.Channel("mod-values-test:ctl", "1", required: false, maxMessageBytes: 4096)
            .RegisterJson<Req>(Receive, NetDirection.ToServer)
            .RegisterJson<Reply>(null, NetDirection.ToClient);
        Commands.Register("mvt-award", c =>
        {
            _source = c.Source.Kind.ToString();
            var xp = ModValues.Add(_active, ModValues.Key("xp"), 7);
            var coins = ModValues.Add(_active, ModValues.Key("coins"), 11);
            Log.Info($"mod-values-test: mod-values: command tick={Game.IsTickThread} source={_source} xp={xp.After?.Value} coins={coins.After?.Value} rev={coins.After?.ValueRevision}");
            c.Reply(xp.Ok && coins.Ok ? "awarded" : $"refused: {xp.Reason ?? coins.Reason}");
        }, CommandPermission.Everyone);
        Commands.Register("mvt-admin", c => c.Reply("must not run"), CommandPermission.Admin);
        Events.On<ModValueChangedEvent>(e =>
        {
            if (e.Key.Mod == "mod-values-test")
                Log.Info($"mod-values-test: event {e.Cause} {e.Key} {e.Character}: {e.Before?.Value.ToString() ?? "-"} -> {e.After?.Value.ToString() ?? "-"} rev {e.After?.ValueRevision}");
        });
        Log.Info("mod-values-test: loaded (tokens come from mod-values/tokens.json, read at Kiln.ModValues' first service tick)");
    }

    private void Receive(Req r, NetContext c)
    {
        if (!c.Sender.IsLab) return;  // lab transport only
        _ = Run(r, c);
    }

    private async Task Run(Req r, NetContext c)
    {
        try
        {
            if (_seen.TryGetValue((c.Sender.Id, r.Id), out var old))
            {
                c.Reply(old);
                return;
            }
            var who = new CharacterKey(Account, Guid.Parse(r.Character));
            bool ok = true;
            var why = "";
            _source = "";
            switch (r.Op)
            {
                case "award":
                    _active = who;
                    var x = Commands.Execute(CommandSource.ForSteamId(Account, "mvt-lab"), "mvt-award");
                    ok = x.Ok;
                    why = x.Error ?? "";
                    break;
                case "admin":
                    var a = Commands.Execute(CommandSource.ForSteamId(Account, "mvt-lab"), "mvt-admin");
                    ok = a.Ok;
                    why = a.Error ?? "";
                    break;
                case "spend":
                    var s = ModValues.Spend(who, ModValues.Key("coins"), r.Amount);
                    ok = s.Ok;
                    why = s.Reason ?? "";
                    break;
                case "carry-seed":
                    var cs = ModValues.Add(who, ModValues.Key("carried"), r.Amount);
                    ok = cs.Ok;
                    why = cs.Reason ?? "";
                    break;
                case "save":
                    var saved = await Saves.Request();
                    ok = saved.Success;
                    why = $"revision {saved.Revision}";
                    break;
                case "read":
                    break;
                default:
                    ok = false;
                    why = "unknown operation";
                    break;
            }
            long V(string id) => ModValues.Read(who, ModValues.Key(id)) is { Ok: true, Value: { } v } ? v.Value : -1;
            var coinsNow = ModValues.Read(who, ModValues.Key("coins"));
            var reply = new Reply(r.Id, ok, why, V("xp"), V("coins"), coinsNow.Value?.ValueRevision ?? 0, V("carried"), V("tokens"), Game.IsTickThread, _source);
            if (_seen.Count > 256) _seen.Clear();
            _seen[(c.Sender.Id, r.Id)] = reply;
            c.Reply(reply);
            Log.Info($"mod-values-test: reply id={r.Id} op={r.Op} character={who.Id} ok={ok} xp={reply.Xp} coins={reply.Coins} rev={reply.Rev} carried={reply.Carried} tokens={reply.Tokens} tick={reply.Tick} {why} {(coinsNow.Ok ? "" : coinsNow.Reason)}");
        }
        catch (Exception e)
        {
            Log.Error($"mod-values-test: FAIL {e}");
            c.Reply(new Reply(r.Id, false, e.Message, -1, -1, 0, -1, -1, Game.IsTickThread, ""));
        }
    }
}
