# SPDX-License-Identifier: MIT
"""Cook features: what modelcook may write into a cooked model, one row each, and whether a cook of a mod has a row on.

Since 0.2.0-dev.31 every row is on (the owner's rule: nothing ships held). A row still names the guided client checks
that have not confirmed it in a real game yet (`checks`; the release notes list it under "on, not yet confirmed").

The switch stays for a row the owner decides to hold at a release: such a row is off by default, and a recipe asks for
it by name, for that mod only:

    { "format": 1, "experimental": ["modelcook.cloth", "modelcook.chains2"], "steps": [ ... ] }

and the cook prints one warning line for it. A model that asks for a row that is off is refused, the row and the
reason named: a cook never leaves authored physics out without a word. The key naming a row that is on is accepted and
changes nothing (recipes written for 0.2.0-dev.30's held rows keep cooking).

A held row is also an entry of the client gate's list (native/core/gates/*.json, with the same checks), so every game's
capability table says "game test pending" for it too. The cook never reads that list (it does not ship with the cook):
tests/test_physics_input.py keeps the two the same.
"""
from dataclasses import dataclass

KEY = "experimental"  # the recipe key that turns a row on for one mod
PENDING = "game test pending: "  # the reason of a row that waits for its guided client check, as kiln.caps words it


class FeatureError(Exception):
    """A recipe's "experimental" list is not a list of known row names."""


class FeatureOff(Exception):
    """A model asks for a row that is off for this cook."""


@dataclass(frozen=True)
class Feature:
    name: str
    default: bool  # on without the recipe key
    checks: tuple  # guided client checks that confirm it in a real game (that open it, while it is held), lower-case ids
    what: str

    @property
    def reason(self):
        """Why the row is off by default, None when it is on."""
        return None if self.default else PENDING + ", ".join(self.checks)


ROWS = (
    Feature("modelcook.cloth", True, ("16e-cc16e",), "cloth simulation data (ClothData, fabric, colliders) in a cooked RenderModel"),
    Feature("modelcook.chains2", True, ("16e-cc16e",), "bone chains (chains2) in a cooked RenderModel"),
)
BY_NAME = {f.name: f for f in ROWS}


def experimental(doc, where="assets/cook.json"):
    """The rows a recipe turns on with its "experimental" key: a frozenset of known names. A name the cook does not
    know is refused (a typo would otherwise drop the physics the author asked for)."""
    names = doc.get(KEY, []) if isinstance(doc, dict) else []
    if not isinstance(names, list) or not all(isinstance(n, str) for n in names):
        raise FeatureError(f"{where}: \"{KEY}\" is a list of cook feature names, as [\"modelcook.cloth\"]")
    for i, n in enumerate(names):
        if n not in BY_NAME:
            raise FeatureError(f"{where}: {KEY}[{i}]: {n!r} is not a cook feature (known: {', '.join(sorted(BY_NAME))})")
    return frozenset(names)


def is_on(name, turned_on=()):
    """THE decision: is row `name` on for a cook whose recipe lists `turned_on` under "experimental"? Every caller asks
    here, nothing else reads a row's default."""
    f = BY_NAME.get(name)
    if f is None:
        raise KeyError(f"cook feature {name!r} is not a row of cook/features.py")
    return f.default or name in turned_on


def require(name, turned_on, what):
    """Refuse `what` (a field path) when row `name` is off for this cook."""
    if not is_on(name, turned_on):
        f = BY_NAME[name]
        raise FeatureOff(f"{what}: needs cook feature {name}, which is off ({f.reason}). It writes {f.what}; to cook it anyway "
                         f"for this mod, add \"{KEY}\": [\"{name}\"] to assets/cook.json")


def warning(turned_on):
    """The one warning line a cook prints for a recipe that turns rows on, None when it turns on none that are off by default."""
    off = [BY_NAME[n] for n in sorted(turned_on) if not BY_NAME[n].default]
    if not off:
        return None
    return ("experimental cook features on for this mod: " + "; ".join(f"{f.name} ({f.reason})" for f in off)
            + ". The game has not been seen to show them right yet: players may see them wrong or not at all")


def table():
    """The rows as a report shows them."""
    return [{"name": f.name, "default": f.default, "reason": f.reason, "checks": list(f.checks)} for f in ROWS]
