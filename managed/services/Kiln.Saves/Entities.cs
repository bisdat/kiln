// SPDX-License-Identifier: MIT
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Kiln;

/// <summary>
/// Data attached to non-player entities (<see cref="EntityData"/>). In a session an entity is its id. Across a
/// restart the game renumbers entities and keeps no id of its own (it saves template + transform and a few
/// components, docs/re/saves.md), so Kiln keys each attachment by the entity's <b>template and position at the
/// save</b> and matches loaded entities against that: props and other static entities come back at exactly their saved
/// position (matched within 0.5 m), creatures and NPCs within 16 m (nearest unclaimed one of the template).
/// </summary>
/// <remarks>Entities without a known, saved world template carrying CurrentTransform keep attachments for the session
/// only. In particular, inventory item entities have no world position and cannot use this positional identity.
/// A destroyed entity loses its attachments within a second. Attachments not matched within 10 minutes of a
/// load are kept for 3 sessions, then dropped. Already-saved attachments survive temporarily unknown metadata
/// within that retention window, but cannot match until their template's persistence flags are known.
/// Opted-in strict scopes are separate: exactly one candidate in the radius, then equal optional SpawnTime values.
/// Loaded entities normally have no SpawnTime; when both values are absent, use also checks template and positional
/// uniqueness at the recorded place. This is positional identity, not proof of an incarnation. Ambiguity stays waiting
/// until explicit attachment or ordinary missed-session expiry. Destroyed/replaced live records are dropped.</remarks>
internal sealed class EntityTable(bool strict = false)
{
    private readonly Dictionary<string, EntityTable> _strict = new(StringComparer.Ordinal);

    /// <summary>Opt in one scope without changing other attachments. Late claims are checked at their original saved place.</summary>
    internal EntityTable Strict(string scope)
    {
        if (_strict.TryGetValue(scope, out var table)) return table;
        table = new EntityTable(true);
        _strict.Add(scope, table);
        foreach (var r in _live.Values.Concat(_unresolved).ToList())
            if (r.ByMod.Remove(scope, out var keys))
            {
                var copy = new Rec { Template = r.Template, Name = r.Name, X = r.X, Y = r.Y, Z = r.Z,
                    Yaw = r.Yaw, Generation = r.Generation, MatchPosition = r.MatchPosition,
                    WaitingReason = r.WaitingReason, Missed = r.Missed, Transient = r.Transient };
                copy.ByMod[scope] = keys;
                if (_live.TryGetValue(r.Entity, out var live) && ReferenceEquals(live, r))
                {
                    copy.Entity = r.Entity;
                    table._live[copy.Entity] = copy; // transfer ownership before inspection/search can throw
                    try
                    {
                        if (!table.SameGeneration(copy, new EntityId(copy.Entity))) continue;
                        if (table.Unique(copy, new EntityId(copy.Entity)))
                        {
                            // Register immediately hands existing live values to the new strict consumer.
                            continue;
                        }
                    }
                    catch (Exception e) when (e is InvalidOperationException or KilnUnavailableException)
                    {
                        // Keep ownership and continue migrating the remaining keys. Register hands this live value to
                        // the consumer, whose use-time check will retry; failure must not lose or permanently block it.
                        SavesRuntime.Log(LogLevel.Debug, $"Kiln.Saves: strict identity of entity {copy.Entity} waits for lookup: {e.Message}");
                        continue;
                    }
                    table._live.Remove(copy.Entity);
                    copy.Entity = 0;
                }
                table._unresolved.Add(copy);
            }
        table.StartMatching();
        return table;
    }

    internal IEnumerable<Rec> Records => _live.Values.Concat(_unresolved);

    private const double StaticRadius = 0.5, MobileRadius = 16;
    private static readonly TimeSpan MatchWindow = TimeSpan.FromMinutes(10);
    private const int MaxMissedSessions = 3;

    private readonly Dictionary<uint, Rec> _live = new();
    private readonly List<Rec> _unresolved = [];
    private readonly List<Rec> _serviceClaims = [];  // matched records with a Kiln service's values, for the next pump
    private DateTime _matchUntil = DateTime.MinValue, _nextMatch = DateTime.MinValue, _nextCheck = DateTime.MinValue;
    private bool _missCounted;
    private int _matched, _dropped;

    internal sealed class Rec
    {
        public uint Entity;
        public long? Generation;
        public WorldPosition? MatchPosition; // original attachment/save place, retained across a lenient claim
        public string? WaitingReason;
        public Guid? Template;
        public string? Name;
        public double X, Y, Z;
        public float Yaw;
        public int Missed;
        public bool Transient;  // no safe positional identity: session only
        public readonly Dictionary<string, SortedDictionary<string, WorldStore.Slot>> ByMod = new(StringComparer.Ordinal);
        public string Where => $"({X:F1}, {Y:F1}, {Z:F1})";
    }

    public string Summary => $"entity attachments: {LiveCount} live, {UnresolvedCount} waiting for their entity";
    public int LiveCount => _live.Count + _strict.Values.Sum(t => t.LiveCount);
    public int UnresolvedCount => _unresolved.Count + _strict.Values.Sum(t => t.UnresolvedCount);
    public int Matched => _matched + _strict.Values.Sum(t => t.Matched);

    /// <summary>Loaded attachments of <paramref name="mod"/> (it has keys on them) still waiting for their entity.</summary>
    public int UnresolvedFor(string mod) =>
        _unresolved.Count(r => r.ByMod.TryGetValue(mod, out var k) && k.Count > 0) + _strict.Values.Sum(t => t.UnresolvedFor(mod));

    /// <summary>Saved attachments of <paramref name="mod"/> matched to a loaded entity since the world's data was loaded.</summary>
    public int MatchedFor(string mod) => _matchedByMod.GetValueOrDefault(mod) + _strict.Values.Sum(t => t.MatchedFor(mod));

    private readonly Dictionary<string, int> _matchedByMod = new(StringComparer.Ordinal);

    public void Clear()
    {
        foreach (var t in _strict.Values) t.Clear();
        _live.Clear();
        _unresolved.Clear();
        _serviceClaims.Clear();
        _matchedByMod.Clear();
        _missCounted = false;
    }

    // ---- load ----
    public void Absorb(string mod, JsonObject doc)
    {
        if (!strict && (ServiceEntityData.IsStrict(mod) || doc["strict"]?.GetValue<bool>() == true))
        { Strict(mod).Absorb(mod, doc); return; }
        if (doc["entities"] is not JsonArray list) return;
        foreach (var n in list)
        {
            if (n is not JsonObject o || o["pos"] is not JsonArray p || p.Count != 3) continue;
            var rec = new Rec
            {
                Template = Guid.TryParse(o["template"]?.GetValue<string>(), out var g) ? g : null,
                Name = o["name"]?.GetValue<string>(),
                MatchPosition = new WorldPosition(p[0]!.GetValue<double>(), p[1]!.GetValue<double>(), p[2]!.GetValue<double>()),
                X = p[0]!.GetValue<double>(),
                Y = p[1]!.GetValue<double>(),
                Z = p[2]!.GetValue<double>(),
                Yaw = o["yaw"]?.GetValue<float>() ?? 0,
                WaitingReason = o["waiting"]?.GetValue<string>(),
                Missed = o["missed"]?.GetValue<int>() ?? 0,
            };
            // Several mods may have attached data to the same entity: one record per (template, position).
            var same = _unresolved.FirstOrDefault(r => r.Template == rec.Template && Math.Abs(r.X - rec.X) < 0.01 &&
                                                       Math.Abs(r.Y - rec.Y) < 0.01 && Math.Abs(r.Z - rec.Z) < 0.01);
            if (same != null) rec = same;
            else _unresolved.Add(rec);
            var keys = new SortedDictionary<string, WorldStore.Slot>(StringComparer.Ordinal);
            if (o["keys"] is JsonObject ks)
                foreach (var (k, v) in ks)
                    keys[k] = new WorldStore.Slot { Json = v?.ToJsonString() ?? "null" };
            rec.ByMod[mod] = keys;
        }
    }

    public void StartMatching()
    {
        foreach (var t in _strict.Values) t.StartMatching();
        _matchUntil = DateTime.UtcNow + MatchWindow;
        _nextMatch = DateTime.MinValue;
    }

    // ---- access ----
    public Rec? Find(EntityId e) => _live.GetValueOrDefault(e.Value);

    public IEnumerable<Rec> LiveRecs => _live.Values;

    /// <summary>The record of a live entity, or of a loaded attachment saved for it (matched now); null if neither.</summary>
    public Rec? FindOrMatch(EntityId e)
    {
        if (_live.TryGetValue(e.Value, out var r)) return !strict || SameGeneration(r, e) ? r : null;
        if (_unresolved.Count == 0 || !SpawnLink.Available || SpawnLink.Inspect(e) is not { } info) return null;
        if (!CanPersist(info.Template)) return null;
        foreach (var u in _unresolved)
        {
            if (u.Template != info.Template || !Near(u, info.Position, RadiusFor(u.Template))) continue;
            if (strict && !Unique(u, e)) continue;
            Claim(u, e);
            return u;
        }
        return null;
    }

    /// <summary>Explicitly attaches to a chosen live entity, creating or rebinding a record. For a strict scope this
    /// clears a waiting refusal; automatic replay must use FindOrMatch instead.</summary>
    public Rec Attach(EntityId e)
    {
        if (_live.TryGetValue(e.Value, out var r))
        {
            if (!strict || SameGeneration(r, e)) return r;
            // A replaced live entity has no attachment to rebind; SameGeneration dropped it.
        }
        if (!SpawnLink.Available) throw SavesCaps.Off("kiln.spawn", "EntityData needs kiln.spawn (entity lookups), which is unavailable");
        var info = SpawnLink.Inspect(e) ?? throw new ArgumentException($"entity {e} does not exist", nameof(e));
        // A loaded attachment waiting for exactly this entity?
        if (CanPersist(info.Template))
            foreach (var u in _unresolved)
            {
                if (u.Template != info.Template || !Near(u, info.Position, RadiusFor(u.Template))) continue;
                if (strict) { u.WaitingReason = null; u.MatchPosition = info.Position; } // explicit choice
                Claim(u, e);
                return u;
            }
        r = new Rec { Entity = e.Value };
        Update(r, info);
        r.MatchPosition = info.Position;
        _live[e.Value] = r;
        return r;
    }

    private static void Update(Rec r, SpawnLink.Ent info)
    {
        r.Generation = info.Generation;
        r.Template = info.Template;
        r.Name = info.Name;
        r.X = info.Position.X;
        r.Y = info.Position.Y;
        r.Z = info.Position.Z;
        r.Yaw = info.Yaw;
        r.Transient = !CanPersist(info.Template);
    }

    private static bool CanPersist(Guid? template) => template is { } g && SpawnLink.Template(g).canPersist;

    private static bool CanRetainSaved(Guid? template)
    {
        if (template is not { } g) return false;
        var metadata = SpawnLink.Template(g);
        // An unresolved saved record already has an identity; unavailable metadata cannot disprove it.
        // Keep it under the usual session TTL, without granting unknown live entities a positional identity.
        return !metadata.known || metadata.canPersist;
    }

    public void Drop(uint entity) => _live.Remove(entity);

    // ---- periodic (tick thread, ~1 s) ----
    public void Tick()
    {
        foreach (var t in _strict.Values) t.Tick();
        if (_live.Count == 0 && _unresolved.Count == 0) return;
        var available = SpawnLink.Available;
        var now = DateTime.UtcNow;
        if (available && now >= _nextCheck && _live.Count > 0)
        {
            _nextCheck = now + TimeSpan.FromSeconds(1);
            CheckDestroyed();
        }
        if (available && _unresolved.Count > 0 && now < _matchUntil && now >= _nextMatch && SpawnLink.WorldRunning)
        {
            _nextMatch = now + TimeSpan.FromSeconds(5);
            MatchLoaded();
        }
        if (now >= _matchUntil && _matchUntil != DateTime.MinValue && !_missCounted)
        {
            _missCounted = true;
            foreach (var u in _unresolved) u.Missed++;
            var gone = _unresolved.RemoveAll(u => u.Missed >= MaxMissedSessions);
            if (_unresolved.Count + gone > 0)
                SavesRuntime.Log(LogLevel.Info, $"Kiln.Saves: entity attachments: {_unresolved.Count} still without their entity after " +
                                                $"{MatchWindow.TotalMinutes:F0} min (kept for later sessions), {gone} dropped after {MaxMissedSessions} sessions");
        }
    }

    private void CheckDestroyed()
    {
        List<uint>? gone = null;
        foreach (var (id, r) in _live)
        {
            SpawnLink.Ent? info;
            try
            {
                info = SpawnLink.Inspect(new EntityId(id));
            }
            catch (InvalidOperationException)
            {
                return;  // kiln.spawn cannot tell right now: drop nothing
            }
            if (strict && (info is null || info.Template != r.Template || info.Generation != r.Generation))
            {
                (gone ??= []).Add(id);
                continue;
            }
            if (info != null && info.Template == r.Template)
            {
                r.X = info.Position.X;
                r.Y = info.Position.Y;
                r.Z = info.Position.Z;
                r.Yaw = info.Yaw;
                r.Transient = !CanPersist(info.Template);
                continue;
            }
            (gone ??= []).Add(id);
        }
        if (gone == null) return;
        foreach (var id in gone)
        {
            DropRecord(_live[id]);
        }
    }

    private void MatchLoaded()
    {
        var before = _unresolved.Count;
        foreach (var u in _unresolved.ToList())
        {
            if (u.Template is not { } g || !CanPersist(g)) continue;
            var radius = RadiusFor(g);
            foreach (var cand in SpawnLink.Find(g, new WorldPosition(u.X, u.Y, u.Z), radius, 16))
            {
                if (strict && !Unique(u, cand)) break;
                if (_live.ContainsKey(cand.Value)) continue;
                Claim(u, cand);
                break;
            }
        }
        var n = before - _unresolved.Count;
        if (n > 0)
            SavesRuntime.Log(LogLevel.Info, $"Kiln.Saves: entity attachments: matched {n} loaded entities ({_matched} this session), {_unresolved.Count} waiting");
    }

    /// <summary>Equal optional SpawnTime values, including absent == absent. Missing values require positional checking
    /// at use (HoldsStrict); equality alone is not proof. A known missing/replaced live entity loses its record.</summary>
    internal bool SameGeneration(Rec r, EntityId e)
    {
        var info = SpawnLink.Inspect(e);
        if (info is not null && info.Template == r.Template && info.Generation == r.Generation) return true;
        DropRecord(r);
        return false;
    }

    private void DropRecord(Rec r)
    {
        if (_live.TryGetValue(r.Entity, out var live) && ReferenceEquals(live, r)) _live.Remove(r.Entity);
        _dropped++;
        SavesRuntime.Log(LogLevel.Debug, $"Kiln.Saves: entity {r.Entity} ({r.Name ?? "?"}) is gone: attachments of {string.Join(", ", r.ByMod.Keys)} dropped");
    }

    /// <summary>Right before replay: template and optional generation equality; absent generations additionally require
    /// one candidate at the attachment's place now. This is positional identity, not proof against every id reuse.</summary>
    internal bool HoldsStrict(Rec r, EntityId e)
    {
        if (!SameGeneration(r, e) || r.WaitingReason is not null) return false;
        if (r.Generation is not null || Unique(r, e)) return true;
        if (r.WaitingReason is not null && _live.Remove(e.Value)) _unresolved.Add(r);
        return false;
    }

    /// <summary>Finds an existing record for explicit removal, including an ambiguous waiting record, without claiming
    /// or creating one. A nonexistent entity or mismatched live generation has no record to remove.</summary>
    internal Rec? FindForRemoval(EntityId e)
    {
        var info = SpawnLink.Inspect(e);
        if (_live.TryGetValue(e.Value, out var live))
            return SameGeneration(live, e) ? live : null;
        if (info is null) return null;
        return _unresolved.FirstOrDefault(r => r.Template == info.Template && Near(r, info.Position, RadiusFor(r.Template)));
    }

    private bool Unique(Rec r, EntityId e)
    {
        if (r.WaitingReason is not null || r.Template is not { } g) return false;
        var candidates = SpawnLink.Find(g, r.MatchPosition ?? new WorldPosition(r.X, r.Y, r.Z), RadiusFor(g), 2, required: true);
        if (candidates.Count > 1)
            r.WaitingReason = "two entities of this kind are within reach of the recorded place: Kiln cannot tell which one was scaled; mod must re-apply";
        return candidates.Count == 1 && candidates[0] == e;
    }

    private void Claim(Rec u, EntityId e)
    {
        _unresolved.Remove(u);
        u.MatchPosition ??= new WorldPosition(u.X, u.Y, u.Z);
        u.Entity = e.Value;
        u.Missed = 0;
        if (SpawnLink.Inspect(e) is { } info)
        {
            SavesRuntime.Log(LogLevel.Debug, $"Kiln.Saves: entity {e} ({info.Name}) at {info.Position} gets the attachments saved at {u.Where} " +
                                             $"({info.Position.DistanceTo(new WorldPosition(u.X, u.Y, u.Z)):F2} m)");
            Update(u, info);
        }
        _live[e.Value] = u;
        _matched++;
        foreach (var (mod, keys) in u.ByMod)
            if (keys.Count > 0) _matchedByMod[mod] = _matchedByMod.GetValueOrDefault(mod) + 1;
        if (u.ByMod.Keys.Any(ServiceEntityData.IsScope)) _serviceClaims.Add(u);
    }

    /// <summary>Records matched since the last pump that carry a Kiln service's values (scope <c>kiln:&lt;service&gt;</c>)
    /// are handed to that service (Kiln.World sets a saved transform scale again). Tick thread, service context.</summary>
    public void HandOverServiceValues()
    {
        foreach (var t in _strict.Values) t.HandOverServiceValues();
        if (_serviceClaims.Count == 0) return;
        var claims = _serviceClaims.ToList();
        _serviceClaims.Clear();
        foreach (var r in claims)
        {
            if (!_live.TryGetValue(r.Entity, out var live) || !ReferenceEquals(live, r)) continue;
            foreach (var (scope, keys) in r.ByMod.Where(p => ServiceEntityData.IsScope(p.Key)).ToList())
            {
                var values = keys.Where(p => p.Value.Json is not null).ToDictionary(p => p.Key, p => p.Value.Json!, StringComparer.Ordinal);
                if (values.Count > 0) ServiceEntityData.RaiseLoaded(scope, new EntityId(r.Entity), values);
            }
        }
    }

    private static double RadiusFor(Guid? template)
    {
        if (template is not { } g) return StaticRadius;
        return SpawnLink.IsMobile(g) ? MobileRadius : StaticRadius;
    }

    private static bool Near(Rec r, WorldPosition p, double radius) => p.DistanceTo(new WorldPosition(r.X, r.Y, r.Z)) <= radius;

    // ---- staging ----
    /// <summary>One bundle per mod: {"entities":[{template, name, pos, yaw, missed, keys}]}.</summary>
    public IEnumerable<(string mod, byte[] data)> Stage(Dictionary<string, JsonSerializerOptions> json)
    {
        if (SpawnLink.Available) CheckDestroyed();
        var perMod = new SortedDictionary<string, List<(Rec r, SortedDictionary<string, WorldStore.Slot> keys)>>(StringComparer.Ordinal);
        foreach (var r in _live.Values.Where(r => !r.Transient).Concat(_unresolved.Where(r => CanRetainSaved(r.Template))))
            foreach (var (mod, keys) in r.ByMod)
                if (keys.Count > 0)
                {
                    if (!perMod.TryGetValue(mod, out var l)) perMod[mod] = l = [];
                    l.Add((r, keys));
                }
        foreach (var (mod, list) in perMod)
        {
            var opts = json.GetValueOrDefault(mod);
            var ms = new MemoryStream();
            using (var w = new Utf8JsonWriter(ms))
            {
                w.WriteStartObject();
                w.WriteNumber("format", 1);
                if (strict) w.WriteBoolean("strict", true);
                w.WriteString("scope", "entity/" + mod);
                w.WriteStartArray("entities");
                foreach (var (r, keys) in list.OrderBy(t => t.r.Template).ThenBy(t => t.r.X).ThenBy(t => t.r.Z).ThenBy(t => t.r.Y))
                {
                    w.WriteStartObject();
                    if (r.Template is { } g) w.WriteString("template", g.ToString());
                    if (r.Name != null) w.WriteString("name", r.Name);
                    var place = strict && r.WaitingReason is not null && r.MatchPosition is { } anchor
                        ? anchor : new WorldPosition(r.X, r.Y, r.Z);
                    w.WriteStartArray("pos");
                    w.WriteNumberValue(Math.Round(place.X, 3));
                    w.WriteNumberValue(Math.Round(place.Y, 3));
                    w.WriteNumberValue(Math.Round(place.Z, 3));
                    w.WriteEndArray();
                    w.WriteNumber("yaw", MathF.Round(r.Yaw, 2));
                    if (r.WaitingReason is not null) w.WriteString("waiting", r.WaitingReason);
                    if (r.Missed > 0) w.WriteNumber("missed", r.Missed);
                    w.WriteStartObject("keys");
                    foreach (var (k, s) in keys)
                    {
                        if (s.Value != null)
                        {
                            try
                            {
                                s.Json = JsonSerializer.Serialize(s.Value, s.Type ?? s.Value.GetType(), opts);
                            }
                            catch (Exception e)
                            {
                                SavesRuntime.Log(LogLevel.Error, $"Kiln.Saves: entity/{mod} key '{k}' could not be serialized ({e.Message})");
                            }
                        }
                        if (s.Json == null) continue;
                        w.WritePropertyName(k);
                        w.WriteRawValue(s.Json, skipInputValidation: true);
                    }
                    w.WriteEndObject();
                    w.WriteEndObject();
                }
                w.WriteEndArray();
                w.WriteEndObject();
            }
            yield return (mod, ms.ToArray());
        }
        foreach (var t in _strict.Values)
            foreach (var bundle in t.Stage(json)) yield return bundle;
    }

    public void Retire(string mod, JsonSerializerOptions? opts, ModContext ctx)
    {
        foreach (var t in _strict.Values) t.Retire(mod, opts, ctx);
        foreach (var r in _live.Values.Concat(_unresolved))
            if (r.ByMod.TryGetValue(mod, out var keys))
                foreach (var (k, s) in keys)
                    WorldStore.Freeze(s, opts, ctx, "entity/" + mod, k);
    }

    internal static string Describe(Rec r) => new StringBuilder().Append(r.Name ?? r.Template?.ToString() ?? "?").Append(' ').Append(r.Where).ToString();
}
