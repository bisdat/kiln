#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""apiids.py: a line of a PublicAPI file and the XML documentation id of the same member read as one key.

  python3 -I tools/docs-site/test_apiids.py
"""
import importlib.util
from pathlib import Path
import unittest

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("apiids", HERE / "apiids.py")
ids = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ids)

# (a PublicAPI line, the documentation id DocFX gives the member)
PAIRS = [
    ("Kiln.Chat", "T:Kiln.Chat"),
    ("Kiln.Foo<T>", "T:Kiln.Foo`1"),
    ("Kiln.Abilities.Attributes", "T:Kiln.Abilities.Attributes"),
    ("static Kiln.Build.BlockAt(Kiln.WorldPosition position) -> Kiln.BlockQueryResult!",
     "M:Kiln.Build.BlockAt(Kiln.WorldPosition)"),
    ("Kiln.Foo.Foo(int x, string! y) -> void", "M:Kiln.Foo.#ctor(System.Int32,System.String)"),
    ("Kiln.Foo.Bar.get -> int", "P:Kiln.Foo.Bar"),
    ("Kiln.Foo.Bar.set -> void", "P:Kiln.Foo.Bar"),
    ("Kiln.Foo.Bar.init -> void", "P:Kiln.Foo.Bar"),
    ("Kiln.ReflectedValue.this[string! path].get -> object?", "P:Kiln.ReflectedValue.Item(System.String)"),
    ("Kiln.Mode.Fast = 2 -> Kiln.Mode", "F:Kiln.Mode.Fast"),
    ("const Kiln.Limits.Max = 64 -> int", "F:Kiln.Limits.Max"),
    ("static readonly Kiln.Foo.Empty -> Kiln.Foo!", "F:Kiln.Foo.Empty"),
    ("static Kiln.AbilityCost.operator +(Kiln.AbilityCost a, Kiln.AbilityCost b) -> Kiln.AbilityCost",
     "M:Kiln.AbilityCost.op_Addition(Kiln.AbilityCost,Kiln.AbilityCost)"),
    ("static Kiln.Foo.operator ==(Kiln.Foo? left, Kiln.Foo? right) -> bool", "M:Kiln.Foo.op_Equality(Kiln.Foo,Kiln.Foo)"),
    ("static Kiln.Foo.implicit operator string!(Kiln.Foo f) -> string!", "M:Kiln.Foo.op_Implicit(Kiln.Foo)~System.String"),
    # a default value, a parameter that is a modifier word's neighbour (Origin ends in "in"), params, ref and out
    ("static Kiln.Abilities.Cast(Kiln.EntityId caster, string! id, Kiln.CastOrigin origin = Kiln.CastOrigin.Server) -> Kiln.CastResult!",
     "M:Kiln.Abilities.Cast(Kiln.EntityId,System.String,Kiln.CastOrigin)"),
    ("Kiln.Foo.Run(ref int a, out string! b, in Kiln.Bar c, params int[]! rest) -> void",
     "M:Kiln.Foo.Run(System.Int32@,System.String@,Kiln.Bar@,System.Int32[])"),
    # generics, nullable value types, arrays, tuples, type parameters
    ("static Kiln.Foo.Map<T, U>(System.Func<T, U>! f, T[]! items) -> System.Collections.Generic.List<U>!",
     "M:Kiln.Foo.Map``2(System.Func{``0,``1},``0[])"),
    ("Kiln.Box<T>.Put(T value, int? n) -> void", "M:Kiln.Box`1.Put(`0,System.Nullable{System.Int32})"),
    ("Kiln.Foo.Grid(int[,]! cells) -> void", "M:Kiln.Foo.Grid(System.Int32[0:,0:])"),
    ("Kiln.Foo.Pair((int a, string b) pair) -> void", "M:Kiln.Foo.Pair(System.ValueTuple{System.Int32,System.String})"),
    ("Kiln.Foo.Take(System.Collections.Generic.IEnumerable<ulong>! ids) -> void",
     "M:Kiln.Foo.Take(System.Collections.Generic.IEnumerable{System.UInt64})"),
    # a lab member and the experimental mark in front of an override
    ("[KILNX001]static Kiln.Foo.Lab.get -> ulong?", "P:Kiln.Foo.Lab"),
    ("~[KILNX002]override Kiln.Foo.ToString() -> string", "M:Kiln.Foo.ToString"),
    # the compiled static method of an extension member
    ("static Kiln.AiRulesApi.get_InCombat() -> Kiln.RuleCondition!", "M:Kiln.AiRulesApi.get_InCombat"),
]


class Keys(unittest.TestCase):
    def test_a_line_and_its_documentation_id_have_one_key(self):
        for line, doc_id in PAIRS:
            with self.subTest(line=line):
                self.assertIsNotNone(ids.key_of_line(line))
                self.assertEqual(ids.key_of_line(line), ids.key_of_doc_id(doc_id))

    def test_different_members_have_different_keys(self):
        keys = {ids.key_of_line(l) for l, _ in PAIRS}
        self.assertEqual(len(keys), len({d for _, d in PAIRS}))           # the get, set and init lines of one property share one

    def test_lines_that_are_no_member_have_no_key(self):
        for line in ("Kiln.IGameComponent<TSelf> (forwarded, contained in Kiln.Api)", "Kiln.AiRulesApi.extension(Kiln.Is!)",
                     "*REMOVED*static Kiln.Foo.Gone() -> void", "#nullable enable", ""):
            with self.subTest(line=line):
                self.assertIsNone(ids.key_of_line(line))

    def test_a_namespace_id_has_no_key(self):
        self.assertIsNone(ids.key_of_doc_id("N:Kiln"))


class Extensions(unittest.TestCase):
    def test_a_static_extension_property(self):
        e = ids.extension_member("static Kiln.AiRulesApi.extension(Kiln.Is!).InCombat.get -> Kiln.RuleCondition!")
        self.assertEqual((e["receiver"], e["owner"], e["name"], e["kind"], e["instance"]),
                         ("Kiln.Is", "Kiln.AiRulesApi", "InCombat", "property", False))
        # the key of the static method the compiler writes for it: the one of the plain line
        self.assertEqual(e["key"], ids.key_of_line("static Kiln.AiRulesApi.get_InCombat() -> Kiln.RuleCondition!"))

    def test_a_static_extension_method_keeps_its_parameters(self):
        e = ids.extension_member("static Kiln.AiRulesApi.extension(Kiln.Is!).AlliesNear(int count, double metres) -> Kiln.RuleCondition!")
        self.assertEqual(e["parameters"], "int count, double metres")
        self.assertEqual(e["key"], ids.key_of_line("static Kiln.AiRulesApi.AlliesNear(int count, double metres) -> Kiln.RuleCondition!"))
        e = ids.extension_member("static Kiln.NpcBlocksApi.extension(Kiln.Blocks!).Condition(string! name, "
                                 "System.Func<Kiln.NpcView!, bool>! check, double every = 1) -> void")
        self.assertEqual(e["parameters"], "string name, Func<NpcView, bool> check, double every = 1")

    def test_an_instance_extension_member_has_its_receiver_first(self):
        e = ids.extension_member("static Kiln.Ext.extension(string! text).Shout(int times) -> string!")
        self.assertTrue(e["instance"])
        self.assertEqual(e["receiver"], "string")
        self.assertEqual(e["key"], ids.key_of_line("static Kiln.Ext.Shout(string! text, int times) -> string!"))

    def test_the_block_itself_and_plain_lines_are_no_extension_member(self):
        self.assertIsNone(ids.extension_member("Kiln.AiRulesApi.extension(Kiln.Is!)"))
        self.assertIsNone(ids.extension_member("static Kiln.AiRulesApi.AlliesNear(int count, double metres) -> Kiln.RuleCondition!"))


if __name__ == "__main__":
    unittest.main()
