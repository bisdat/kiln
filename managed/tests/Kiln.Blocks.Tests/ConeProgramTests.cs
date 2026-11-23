// SPDX-License-Identifier: MIT
using System.Numerics;
using Xunit;

namespace Kiln.BlockTests;

public sealed class ConeProgramTests
{
    [Theory]
    [InlineData(179, 2)]
    [InlineData(180, 2)]
    [InlineData(359, 5)]
    [InlineData(360, 7)]
    public void Generated_cone_program_hits_the_real_aperture_including_behind_at_360(float angle, int hits)
    {
        var ctx = new BlockContext(new ModBlocks("cone-test", 1), "attack sweep");
        var (_, blob, _, _) = EffectEmitter.DeliveryProgram(ctx, "sweep", new ConeBlock
        {
            Range = 5, Angle = angle, Effects = [new EffectBlock { Heal = 1 }],
        }, "delivery", null, "onCast", false);
        var directions = new[] { 0f, 89f, 91f, 150f, 179f, 179.75f, 180f };
        var positions = directions.Select(d => new Vector3(MathF.Sin(d * MathF.PI / 180), 0, MathF.Cos(d * MathF.PI / 180)) * 3).ToArray();
        var vm = new ConeVm(blob, positions);
        vm.Run();
        Assert.Equal(Enumerable.Range(1, hits).Select(n => (uint)n), vm.Hits);
    }

    // Execute the emitted Impact instructions, not a second copy of the cone predicate. The game natives' vector
    // operations and range enumeration are stubbed; real-game resource/animation integration is deliberately not claimed.
    private sealed class ConeVm
    {
        private readonly uint[] _code;
        private readonly byte[][] _values;
        private readonly Stack<uint> _stack = new();
        private readonly Vector3[] _positions;
        private readonly Dictionary<uint, int> _argc;
        private int _pc;
        public readonly List<uint> Hits = [];

        public ConeVm(byte[] blob, Vector3[] positions)
        {
            uint Word(int at) => BitConverter.ToUInt32(blob, at);
            var start = 0x18 + (int)Word(0x18);
            _code = Enumerable.Range(0, (int)Word(0x1c)).Select(i => Word(start + 4 * i)).ToArray();
            var layout = 0x28 + (int)Word(0x28);
            var data = 0x30 + (int)Word(0x30);
            _values = Enumerable.Range(0, (int)Word(0x2c)).Select(i =>
            {
                var entry = layout + 24 * i;
                var offset = BitConverter.ToUInt16(blob, entry + 14);
                var size = BitConverter.ToUInt16(blob, entry + 12);
                return blob.AsSpan(data + offset, size).ToArray();
            }).ToArray();
            _positions = positions;
            // Arity comes from the emitter's actual native contracts, so an added call cannot silently shift the stack.
            var natives = (Dictionary<string, (uint Hash, int Argc)>)typeof(ProgramBuilder)
                .GetField("Natives", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static)!.GetValue(null)!;
            _argc = natives.Values.ToDictionary(n => n.Hash, n => n.Argc);
        }

        private byte[] Value(uint h) => _values[(int)(h & 0xffff)];
        private uint Get(uint h) => h == 0 ? 0 : Value(h).Length == 1 ? Value(h)[0] : BitConverter.ToUInt32(Value(h));
        private float Float(uint h) => BitConverter.ToSingle(Value(h));
        private Vector3 Vec(uint h) => h == 0 ? Vector3.Zero : new(BitConverter.ToSingle(Value(h), 0), BitConverter.ToSingle(Value(h), 4), BitConverter.ToSingle(Value(h), 8));
        private void Put(uint h, uint value) { if (h != 0) BitConverter.GetBytes(value).CopyTo(Value(h), 0); }
        private void PutFloat(uint h, float value) => BitConverter.GetBytes(value).CopyTo(Value(h), 0);
        private void PutVec(uint h, Vector3 v)
        {
            BitConverter.GetBytes(v.X).CopyTo(Value(h), 0);
            BitConverter.GetBytes(v.Y).CopyTo(Value(h), 4);
            BitConverter.GetBytes(v.Z).CopyTo(Value(h), 8);
        }
        private Vector3 Position(uint entity) => entity is >= 1 && entity <= _positions.Length ? _positions[entity - 1] : Vector3.Zero;

        public void Run()
        {
            for (var steps = 0; steps < 10000; steps++)
            {
                switch (_code[_pc++])
                {
                    case 8: _pc = (int)_code[_pc]; break;
                    case 9:
                    case 10:
                        var op = _code[_pc - 1];
                        var at = (int)_code[_pc++];
                        if ((_stack.Pop() != 0) == (op == 9)) _pc = at;
                        break;
                    case 11: _stack.Push(_code[_pc++]); break;
                    case 12: _stack.Push(0); break;
                    case 19: _pc++; _stack.Push(Call(_code[_pc++])); break;
                    case 22: _stack.Push(Get(_code[_pc++])); break;
                    case 24: Put(_code[_pc++], _stack.Pop()); break;
                    case 27: _stack.Pop(); break;
                    case 29: break;
                    case 30: return;
                    default: throw new InvalidOperationException($"unexpected cone opcode {_code[_pc - 1]}");
                }
            }
            throw new InvalidOperationException("cone did not stop");
        }

        private uint Call(uint hash)
        {
            var a = Enumerable.Range(0, _argc[hash]).Select(_ => _stack.Pop()).ToArray();
            switch (hash)
            {
                case 0x01d09759: Put(a[0], 100); break; // self
                case 0xbf5ad13b: Put(a[0], 101); break; // caster
                case 0xa18f5959: // transform, position, entity
                    if (a[0] != 0) PutVec(a[0], Position(Get(a[2])));
                    if (a[1] != 0) PutVec(a[1], Position(Get(a[2])));
                    break;
                case 0x3e1c0b5d: PutVec(a[1], Position(Get(a[3]))); break; // chest
                case 0x90cf9818: PutVec(a[0], Vec(a[1]) + Vec(a[2])); break; // identity aim transform
                case 0x866dfbeb: PutVec(a[3], Vec(a[5]) - Vec(a[4])); break;
                case 0xd367cdeb: PutVec(a[0], Vec(a[2]) - Vec(a[3])); break;
                case 0xd8e52c57: PutFloat(a[0], Vec(a[1]).Length()); break;
                case 0xa5ff238b: PutVec(a[0], Vector3.Normalize(Vec(a[1])) * Float(a[2])); break;
                case 0xc19163a7: PutFloat(a[0], Float(a[2]) * Float(a[3])); break;
                case 0x69190348: return Float(a[1]) < Float(a[2]) ? 1u : 0u;
                case 0x19143416:
                    var index = Get(a[2]);
                    if (index >= _positions.Length) return 0;
                    Put(a[0], index + 1); Put(a[2], index + 1); break;
                case 0xa385dcb9: Put(a[0], 1); break; // every candidate has health
                case 0x044e4187: Hits.Add(Get(a[3])); break;
                case 0x79b5a97b: // enemies
                case 0xcf3810a6: // valid entity
                case 0x93e40aa9: break; // query shutdown
                default: throw new InvalidOperationException($"unexpected cone native {hash:x8}");
            }
            return 1;
        }
    }
}
