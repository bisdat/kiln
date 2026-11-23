#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Which of the samples (mods/samples/**) use which API types, for the "Used in samples" line of the API reference.

  tools/docs-site/samples.py <repository> [<type name>...]      the sample files that name each type (a maintainer's check)

A sample file uses a type when its code (comments and string literals taken out) names it: `Chat`, `Kiln.Chat`, or for a
type nested in another one `Abilities.Attributes`. A name after a dot (`x.Position`), or after `class`, `struct`, `enum`,
`interface`, `record` or `namespace` (the sample's own type of that name), is not a use. An attribute type
(`FooAttribute`) is also named `Foo`. Only git-tracked files count (stage-guide.py's list: the release's export has no .git,
KILN_DOCS_TRACKED names its files), so an untracked experiment never appears on the public site.
Standard library only.
"""
import importlib.util
from pathlib import Path
import re
import sys

HERE = Path(__file__).resolve().parent
SAMPLES = "mods/samples"
REPOSITORY = "https://github.com/frostdev-ops/kiln"
CHAIN = re.compile(r"(?<![\w.@])@?[A-Za-z_]\w*(?:\s*\.\s*[A-Za-z_]\w*)*")
DECLARES = re.compile(r"\b(?:class|struct|enum|interface|record|namespace|delegate)\s+(?:struct\s+|class\s+)?$")
STRIP = re.compile(
    r"//[^\n]*|/\*.*?\*/"                                  # comments
    r"|\$@\"(?:[^\"]|\"\")*\"|@\$\"(?:[^\"]|\"\")*\"|@\"(?:[^\"]|\"\")*\""   # verbatim strings
    r"|\"\"\".*?\"\"\""                                    # raw strings
    r"|\$?\"(?:\\.|[^\"\\\n])*\""                          # strings
    r"|'(?:\\.|[^'\\\n])'",                                # characters
    re.S)
KILN_SERVICES = re.compile(r"<KilnServices>([^<]*)</KilnServices>")
REFERENCES = re.compile(r'<ProjectReference\s+Include="[^"]*managed/(?:services/)?Kiln\.(\w+)/Kiln\.\1\.csproj"')


def stage_guide():
    spec = importlib.util.spec_from_file_location("stage_guide", HERE / "stage-guide.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def tracked_samples(repo):
    """The git-tracked files of mods/samples as paths relative to the repository (sorted), or [] when the repository has none."""
    repo = Path(repo).resolve()
    root = repo / SAMPLES
    return sorted(str(p.relative_to(repo)) for p in stage_guide().tracked_files(repo) if p.is_relative_to(root))


def clean(code):
    """The code without comments and string literals (a string keeps its place as an empty one)."""
    return STRIP.sub(lambda m: '""' if m.group(0)[0] in "\"$@" else " ", code)


def chains(code):
    """The dotted names a file's code writes, each with its leading parts too (`A.B.C` gives A, A.B and A.B.C), without
    spaces, a leading `Kiln.` and the names a declaration introduces."""
    found = set()
    code = clean(code)
    for m in CHAIN.finditer(code):
        if DECLARES.search(code[max(0, m.start() - 24):m.start()]):
            continue
        parts = [p.strip().lstrip("@") for p in m.group(0).split(".")]
        for i in range(1, len(parts) + 1):
            found.add(".".join(parts[:i]))
        if parts[0] == "Kiln":
            for i in range(2, len(parts) + 1):
                found.add(".".join(parts[1:i]))
    return found


class Samples:
    """The names each tracked sample code file uses: use(type) is the list of files that use a type."""

    def __init__(self, repo):
        self.repo = Path(repo).resolve()
        self.files = {}
        for rel in tracked_samples(self.repo):
            if rel.endswith(".cs"):
                p = self.repo / rel
                if p.is_file():
                    self.files[rel] = chains(p.read_text(encoding="utf-8", errors="replace"))

    def use(self, qualified):
        """The sample files (relative to the repository, sorted) that use a type; qualified is its name inside the namespace:
        `Chat`, `Abilities.Attributes` (generic arity marks and a trailing `Attribute` are taken care of)."""
        names = {qualified}
        if qualified.endswith("Attribute") and len(qualified) > 9:
            names.add(qualified[:-9])
        return sorted(rel for rel, used in self.files.items() if names & used)


def url(rel):
    return f"{REPOSITORY}/blob/main/{rel}"


def label(rel):
    return rel[len(SAMPLES) + 1:] if rel.startswith(SAMPLES + "/") else rel


def sample_services(repo):
    """{sample folder: [the services it uses, by KilnServices name]}, from the samples' project files (tracked ones only): the
    <KilnServices> list of a mod on Kiln.Sdk, or the <ProjectReference>s of one built from the repository. A sample that
    names none maps to []."""
    repo = Path(repo).resolve()
    out = {}
    for rel in tracked_samples(repo):
        parts = rel.split("/")
        if len(parts) < 4:
            continue
        out.setdefault(parts[2], set())
        if rel.endswith(".csproj"):
            text = (repo / rel).read_text(encoding="utf-8", errors="replace")
            for m in KILN_SERVICES.finditer(text):
                out[parts[2]] |= {s for s in re.split(r"[\s,;]+", m.group(1).strip()) if s}
            out[parts[2]] |= {m.group(1) for m in REFERENCES.finditer(text) if m.group(1) != "Api"}
    return {k: sorted(v) for k, v in sorted(out.items())}


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__.split("\n\n")[1])
    s = Samples(sys.argv[1])
    print(f"{len(s.files)} sample code files")
    for name in sys.argv[2:]:
        print(name, "->", ", ".join(label(r) for r in s.use(name)) or "none")


if __name__ == "__main__":
    main()
