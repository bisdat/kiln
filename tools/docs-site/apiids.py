#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Matches a line of a PublicAPI.*.txt file (and so a row of managed/api-history.tsv) to the member DocFX shows for it.

  apiids.key_of_line(line)  -> the line's match key, or None (a forwarded type, the extension(...) block, a line it cannot read)
  apiids.key_of_doc_id(id)  -> the match key of an XML documentation id (DocFX's commentId: "M:Kiln.Build.BlockAt(Kiln.WorldPosition)")
  apiids.extension_member(line) -> the extension member the line declares, or None

A key is a tuple (T|M|P|F, type, member, "<generic arity>|<parameter types>"): the shape of a documentation id with the
differences between the two spellings taken out. PublicAPI writes C# (`int`, `Func<A, B>`, `Foo!`, `Foo?`, `ref int x = 1`), a
documentation id the runtime's names (`System.Int32`, `System.Func{A,B}`, `Foo@`). A generic parameter has a different name in
each place (`T` against `` ``0 ``), so every parameter that is not a dotted name reads as `T#`. Two overloads that differ only in
what a type parameter is called would share a key; the callers keep the earliest of what they find for one key.
Standard library only. The audit's coverage.py (docs audit, Nov 22) was the starting point.
"""
import re

PRIMITIVES = {
    "bool": "System.Boolean", "byte": "System.Byte", "sbyte": "System.SByte", "char": "System.Char",
    "decimal": "System.Decimal", "double": "System.Double", "float": "System.Single", "int": "System.Int32",
    "uint": "System.UInt32", "long": "System.Int64", "ulong": "System.UInt64", "short": "System.Int16",
    "ushort": "System.UInt16", "object": "System.Object", "string": "System.String", "void": "System.Void",
    "nint": "System.IntPtr", "nuint": "System.UIntPtr", "dynamic": "System.Object",
}
OPERATORS = {
    "+": "op_Addition", "-": "op_Subtraction", "*": "op_Multiply", "/": "op_Division", "%": "op_Modulus",
    "==": "op_Equality", "!=": "op_Inequality", "<": "op_LessThan", ">": "op_GreaterThan",
    "<=": "op_LessThanOrEqual", ">=": "op_GreaterThanOrEqual", "&": "op_BitwiseAnd", "|": "op_BitwiseOr",
    "^": "op_ExclusiveOr", "!": "op_LogicalNot", "~": "op_OnesComplement", "++": "op_Increment", "--": "op_Decrement",
    "<<": "op_LeftShift", ">>": "op_RightShift", ">>>": "op_UnsignedRightShift", "true": "op_True", "false": "op_False",
}
MODIFIERS = ("static", "abstract", "virtual", "override", "sealed", "const", "readonly", "new", "extern", "unsafe",
             "required", "async")
PARAM_MODIFIERS = re.compile(r"^(?:(?:params|this|ref readonly|ref|out|in|scoped)\s+)+")
NAME_AT_END = re.compile(r"^(.*\S)\s+@?[A-Za-z_]\w*$")


def split_top(text, sep=","):
    """Splits at the separators that are not inside <>, (), [], {} or a quoted string."""
    out, depth, cur, quote, i = [], 0, [], None, 0
    while i < len(text):
        c = text[i]
        if quote:
            cur.append(c)
            if c == "\\" and i + 1 < len(text):
                cur.append(text[i + 1])
                i += 1
            elif c == quote:
                quote = None
        elif c in "\"'":
            quote = c
            cur.append(c)
        elif c in "<([{":
            depth += 1
            cur.append(c)
        elif c in ">)]}":
            depth -= 1
            cur.append(c)
        elif c == sep and depth == 0:
            out.append("".join(cur))
            cur = []
        else:
            cur.append(c)
        i += 1
    if cur or out:
        out.append("".join(cur))
    return [x.strip() for x in out]


def generics_to_arity(name):
    """Kiln.Foo<T>.Bar<A,B> -> Kiln.Foo`1.Bar`2 (the type names of a documentation id)."""
    out, i = [], 0
    while i < len(name):
        if name[i] == "<":
            depth, j = 1, i + 1
            while depth and j < len(name):
                depth += (name[j] == "<") - (name[j] == ">")
                j += 1
            out.append("`%d" % len(split_top(name[i + 1:j - 1])))
            i = j
        else:
            out.append(name[i])
            i += 1
    return "".join(out)


def split_dotted(name):
    """Splits at the dots that are not inside <>, [] or ()."""
    parts, depth, cur = [], 0, []
    for c in name:
        if c in "<[(":
            depth += 1
        elif c in ">])":
            depth -= 1
        if c == "." and depth == 0:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(c)
    parts.append("".join(cur))
    return parts


def api_type(text):
    """A type as PublicAPI writes it -> the shared spelling (System names, <> generics, no nullability marks)."""
    t = PARAM_MODIFIERS.sub("", text.strip()).replace("!", "")
    if t.startswith("("):                                    # a tuple: (int a, string b)
        close = t.rindex(")")
        parts = []
        for p in split_top(t[1:close]):
            m = NAME_AT_END.match(p)
            parts.append(api_type(m.group(1) if m and not p.endswith((">", "]", "*")) else p))
        return "System.ValueTuple<" + ",".join(parts) + ">" + t[close + 1:].replace("?", "")
    suffix = ""
    while True:                                             # trailing ?, [], [,], *
        if t.endswith("?"):
            t = t[:-1]
        elif t.endswith("*"):
            suffix = "*" + suffix
            t = t[:-1]
        else:
            m = re.search(r"\[,*\]$", t)
            if not m:
                break
            suffix = m.group(0) + suffix
            t = t[:m.start()]
    if "<" in t:
        head, args, tail = t[:t.index("<")], t[t.index("<") + 1:t.rindex(">")], t[t.rindex(">") + 1:]
        base = head + "<" + ",".join(api_type(a) for a in split_top(args)) + ">" + tail
    else:
        base = PRIMITIVES.get(t, t)
        if "." not in base:
            base = "T#"                                     # a type parameter
    return base + suffix


def doc_type(text):
    """A type as a documentation id writes it -> the same shared spelling."""
    t = re.sub(r"\[[0-9:,]*\]", lambda m: "[" + "," * m.group(0).count(",") + "]", text.strip().replace("@", ""))
    if t.startswith("System.Nullable{"):
        return doc_type(t[len("System.Nullable{"):t.rindex("}")] + t[t.rindex("}") + 1:])
    head = t.split("[")[0].rstrip("*")
    if re.fullmatch(r"`+\d+", head):                       # `0 / ``0: a type parameter
        return "T#" + t[len(head):]
    if "{" in t:
        h, args, tail = t[:t.index("{")], t[t.index("{") + 1:t.rindex("}")], t[t.rindex("}") + 1:]
        return h + "<" + ",".join(doc_type(a) for a in split_top(args)) + ">" + tail
    return t


def strip_modifiers(s):
    mods = []
    while True:
        m = re.match(r"^(%s)\s+" % "|".join(MODIFIERS), s)
        if not m:
            return s, mods
        mods.append(m.group(1))
        s = s[m.end():]


def parse_line(line):
    """-> a dict (kind type|method|ctor|prop|field|op|delegate, type, name, params, arity, extension, accessor, flags) or None.
    type and name are in documentation-id spelling (`Kiln.Foo`1`)."""
    s = line.strip()
    if not s or s.startswith(("#", "*REMOVED*")):
        return None
    flags = []
    while True:                                              # `~[KILNX002]override ...`: the marks come in either order
        m = re.match(r"^(~|\[(KILNX\d+)\])", s)
        if not m:
            break
        if m.group(2):
            flags.append(m.group(2))
        s = s[m.end():]
    if "(forwarded" in s:                                    # `Kiln.Foo (forwarded, contained in Kiln.Api)`: a stub, not the member
        return None
    extension = None
    if ".extension(" in s:
        m = re.search(r"\.extension\(([^()]*)\)(\.|$)", s)
        if not m or not m.group(2):
            return None                                      # the extension block itself: no member
        extension = m.group(1).strip()
        s = s[:m.start()] + "." + s[m.end():]
    s, mods = strip_modifiers(s)
    if "->" not in s:
        if re.search(r"[(\s]", s.split("<")[0]):
            return None
        return dict(kind="type", type=generics_to_arity(s), name="", params=None, arity=0, extension=None,
                    accessor=None, flags=flags, mods=mods)
    lhs = s.rsplit(" -> ", 1)[0] if " -> " in s else s.split("->")[0]
    return parse_member(lhs.strip(), flags, extension, mods)


def parse_member(lhs, flags, extension, mods):
    value = None
    head = lhs.split("(", 1)[0]
    if " = " in head:                                        # Kiln.Foo.Bar = 3: an enum member or a constant
        lhs, value = lhs.split(" = ", 1)
    plist, name_part = None, lhs
    depth = 0
    for i, c in enumerate(lhs):                              # the parameter list: the first ( outside <> (not an `operator (`)
        if c == "<":
            depth += 1
        elif c == ">":
            depth -= 1
        elif c == "(" and depth == 0 and not re.search(r"operator\s*$", lhs[:i]):
            name_part, plist = lhs[:i], lhs[i + 1:lhs.rindex(")")]
            break
    idx = re.match(r"^(.*)\.this\[(.*)\](?:\.(get|set|init))?$", lhs)
    if idx:                                                  # an indexer
        params = [api_type(strip_default(p)) for p in split_top(idx.group(2)) if p]
        return dict(kind="prop", type=generics_to_arity(idx.group(1)), name="Item", params=params, arity=0,
                    extension=extension, accessor=idx.group(3), flags=flags, mods=mods)
    op = re.match(r"^(.*?)\.(implicit operator|explicit operator|checked operator|operator)\s*(.*)$", name_part)
    if op:
        params = [api_type(strip_default(p)) for p in split_top(plist or "") if p]
        symbol = op.group(3).strip()
        if op.group(2).startswith("implicit") or op.group(2).startswith("explicit"):
            name = ("op_Implicit" if op.group(2).startswith("implicit") else "op_Explicit") + "~" + api_type(symbol)
        else:
            name = OPERATORS.get(symbol)
            if name is None:
                return None
        return dict(kind="op", type=generics_to_arity(op.group(1)), name=name, params=params, arity=0,
                    extension=extension, accessor=None, flags=flags, mods=mods)
    accessor = None
    if plist is None:
        m = re.match(r"^(.*)\.(get|set|init|add|remove)$", name_part)
        if m:
            name_part, accessor = m.group(1), m.group(2)
    parts = split_dotted(name_part)
    member, owner = parts[-1], parts[:-1]
    if not owner:
        return None
    arity = 0
    m = re.match(r"^(.*?)<(.*)>$", member)
    if m and plist is not None:
        arity, member = len(split_top(m.group(2))), m.group(1)
    kind = "method" if plist is not None else "prop" if accessor in ("get", "set", "init") else "field"
    simple_owner = re.sub(r"<.*>$", "", owner[-1])
    if kind == "method" and member == simple_owner:
        kind = "ctor"
    params = None
    if plist is not None:
        params = [api_type(strip_default(p)) for p in split_top(plist) if p]
    return dict(kind=kind, type=generics_to_arity(".".join(owner)), name=member, params=params, arity=arity,
                extension=extension, accessor=accessor, value=value, flags=flags, mods=mods, raw_params=plist)


def strip_default(p):
    """`Kiln.X x = Kiln.X.Default` -> `Kiln.X`: the parameter's type, without its name and default value."""
    p = split_top(p.strip(), "=")[0]
    m = NAME_AT_END.match(p)
    return m.group(1) if m and m.group(1) not in ("ref", "out", "in", "params", "this", "scoped") else p


def key_of_line(line):
    """The match key of one PublicAPI line (its type, or its member), or None."""
    e = parse_line(line)
    return key_of_entry(e) if e else None


def key_of_entry(e):
    if e["kind"] == "type":
        return ("T", e["type"], "", "")
    params = ",".join(e["params"]) if e["params"] is not None else ""
    letter = {"method": "M", "ctor": "M", "op": "M", "prop": "P", "field": "F"}[e["kind"]]
    name = "#ctor" if e["kind"] == "ctor" else e["name"]
    return (letter, e["type"], name, "%d|%s" % (e["arity"], params))


def key_of_doc_id(doc_id):
    """The match key of an XML documentation id: `M:Kiln.Build.BlockAt(Kiln.WorldPosition)`."""
    kind, rest = doc_id.split(":", 1)
    if kind == "N":
        return None
    params = None
    ret = None
    if "(" in rest:
        rest, p = rest[:rest.index("(")], rest[rest.index("(") + 1:]
        if "~" in p.rsplit(")", 1)[-1]:
            ret = p.rsplit(")", 1)[-1][1:]
        p = p[:p.rindex(")")]
        params = [doc_type(x) for x in split_top(p)] if p else []
    elif "~" in rest:
        rest, ret = rest.split("~", 1)
    if kind == "T":
        return ("T", rest, "", "")
    owner, _, member = rest.rpartition(".")
    arity = 0
    m = re.match(r"^(.*)``(\d+)$", member)
    if m:
        member, arity = m.group(1), int(m.group(2))
    if member in ("op_Implicit", "op_Explicit"):
        member += "~" + doc_type(ret or "")
    letter = {"M": "M", "P": "P", "F": "F", "E": "F"}.get(kind)
    if letter is None:
        return None
    return (letter, owner, member, "%d|%s" % (arity, ",".join(params or [])))


def extension_member(line):
    """The extension member a PublicAPI line declares (`static Kiln.AiRulesApi.extension(Kiln.Is!).InCombat.get -> ...`), or None:
    a dict with receiver (the type it extends, C# spelling without marks), owner (the static class, documentation-id
    spelling), name, kind (method|property), instance (the receiver is a named parameter, so the member is an instance
    member of it), and key (the match key of the static method the compiler writes for it: `get_InCombat`, the receiver
    first among the parameters of an instance member)."""
    e = parse_line(line)
    if e is None or e["extension"] is None or e["kind"] not in ("method", "prop"):
        return None
    receiver = e["extension"].replace("!", "").strip()
    instance = bool(re.search(r"\s@?[A-Za-z_]\w*$", receiver)) and not receiver.endswith(("]", ">", ")"))
    if instance:
        receiver = re.sub(r"\s+@?[A-Za-z_]\w*$", "", receiver)
    receiver = receiver.rstrip("?")
    params = list(e["params"] or [])
    if e["kind"] == "prop":
        if e["accessor"] not in ("get", "set"):
            return None
        compiled = ("get_" if e["accessor"] == "get" else "set_") + e["name"]
        params = []
    else:
        compiled = e["name"]
    if instance:
        params = [api_type(receiver)] + params
    key = ("M", e["type"], compiled, "%d|%s" % (e["arity"], ",".join(params)))
    shown = None if e["kind"] == "prop" else re.sub(r"\b(?:Kiln|System(?:\.[A-Za-z]+)*)\.(?=[A-Z])", "", (e.get("raw_params") or "").replace("!", ""))
    return dict(receiver=receiver, owner=e["type"], name=e["name"], kind="property" if e["kind"] == "prop" else "method",
                accessor=e["accessor"], instance=instance, key=key, parameters=shown)
