#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Small helpers for the XML docs the API reference is built from (annotate.py writes to them, services.py reads them).

  para(*parts)             a <para> from strings and elements (an element's tail is the string after it)
  code(text), bold(text), see(cid, label), link(href, label)    <c>, <b>, <see cref>, <see href>
  add_to_summary(tree, members, cid, paras)         the paragraphs at the end of a member's <summary>, or in its <remarks> when it has none
  add_to_remarks(tree, members, cid, elements)      elements at the end of a member's <remarks>
  plain(element), first_sentence(element)           an element's text without markup, and its first sentence
Standard library only.
"""
import re
import xml.etree.ElementTree as ET


def para(*parts):
    p = ET.Element("para")
    p.text = ""
    last = None
    for part in parts:
        if isinstance(part, str):
            if last is None:
                p.text += part
            else:
                last.tail = (last.tail or "") + part
        else:
            p.append(part)
            last = part
    return p


def code(text):
    c = ET.Element("c")
    c.text = text
    return c


def bold(text):
    b = ET.Element("b")
    b.text = text
    return b


def see(cid, label=None):
    s = ET.Element("see", cref=cid)
    s.text = label
    return s


def link(href, label):
    s = ET.Element("see", href=href)
    s.text = label
    return s


def member_of(tree, members, cid):
    """The <member> of a documentation id, made when the assembly's XML has none (an undocumented member)."""
    m = members.get(cid)
    if m is None:
        m = ET.SubElement(tree.find("members"), "member", name=cid)
        members[cid] = m
    return m


def has_text(summary):
    return summary is not None and bool("".join(summary.itertext()).strip() or len(summary))


def add_to_summary(tree, members, cid, paras):
    """paras go at the end of the member's <summary> (a space before the first, in text-only views); a member with no
    summary gets them first in <remarks> instead, so undocumented.py still lists it."""
    if not paras:
        return
    member = member_of(tree, members, cid)
    summary = member.find("summary")
    if has_text(summary):
        for p in paras:
            last = summary[-1] if len(summary) else None
            if last is None:
                summary.text = (summary.text or "").rstrip() + " "
            else:
                last.tail = (last.tail or "").rstrip() + " "
            summary.append(p)
    else:
        remarks = member.find("remarks")
        if remarks is None:
            remarks = ET.SubElement(member, "remarks")
        lead, remarks.text = remarks.text, None
        for i, p in enumerate(paras):
            remarks.insert(i, p)
        paras[-1].tail = lead


def add_to_remarks(tree, members, cid, elements):
    if not elements:
        return
    member = member_of(tree, members, cid)
    remarks = member.find("remarks")
    if remarks is None:
        remarks = ET.SubElement(member, "remarks")
    for e in elements:
        remarks.append(e)


def plain(element):
    """The text of an element as a sentence reads: <see cref="T:Kiln.Foo"/> gives Foo, <paramref name="x"/> gives x."""
    if element is None:
        return ""
    out = [element.text or ""]
    for child in element:
        if child.tag in ("see", "seealso") and not (child.text or "").strip():
            name = child.get("cref") or child.get("langword") or child.get("href") or ""
            out.append(re.sub(r"\(.*$", "", name.split(":", 1)[-1]).rsplit(".", 1)[-1])
        elif child.tag in ("paramref", "typeparamref"):
            out.append(child.get("name") or "")
        else:
            out.append(plain(child))
        out.append(child.tail or "")
    return " ".join("".join(out).split())


def first_sentence(element, limit=200):
    """The first sentence of a <summary> on one line (cut at limit characters)."""
    text = plain(element)
    m = re.match(r"^(.*?[.!?])(?:\s|$)", text)
    s = m.group(1) if m else text
    if len(s) > limit:
        s = s[:limit].rsplit(" ", 1)[0] + "..."
    return s
