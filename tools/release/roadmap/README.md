# Kiln's roadmap source

`roadmap.json` here is the one source of Kiln's public roadmap, <https://frostdev.io/kiln/roadmap>. It is the site's
`src/data/kilnRoadmap.json` (frostdev.io repository) byte for byte: `tools/release/roadmap.py site CHECKOUT` writes it
there, and the release flow does that for you (below). Edit it here, never in the site: the next release overwrites the
site's copy. Seeded 2026-11-20 from frostdev.io `af9b71e` (the page live that day), replacing the one-off wave script
that built it before (`/mnt/Dongus/kiln-wave/dev29/out/roadmap-dev30/build.py`).

## In the release flow

| When | What runs | What it does |
|---|---|---|
| `release.sh VERSION` (a final build), before anything is built | `roadmap.py check VERSION` | VERSION must be the first `upcoming` entry and the file well-formed, or the build stops. A test build (`--beta N`, `--worktree`) only warns. Items of `docs/kiln/release-notes/VERSION.md` the entry does not seem to mention are a warning. |
| `publish.sh` (a release, not a test build), after the GitHub release and the frostdev.io downloads | `roadmap.py ship VERSION` | Moves the entry to `released` with its date (the GitHub publish time as a New York date), drops its `status` and `note`, and makes the following upcoming entry `next`. |
| right after | (printed) `git -C <repo> commit -m "Kiln roadmap: VERSION released" -- tools/release/roadmap/roadmap.json` | publish.sh does not commit, as with the API record: commit it on the release branch and push it with the release's follow-ups. |
| right after | `roadmap.py deploy $KILN_SITE_DIR --version VERSION` | The frostdev.io checkout (default `~/Work/frostdev.io`): pull, check the live revision, write, build, commit, push, `ops/deploy.sh`, check the page. |

A roadmap failure never fails or undoes a release that is out: publish.sh prints `WARNING: ROADMAP NOT UPDATED` with
the exact commands to run again, and its exit status is the release's. By hand, from the Kiln repository:

    python3 -I tools/release/roadmap.py check 0.2.0-dev.29.19
    python3 -I tools/release/roadmap.py ship 0.2.0-dev.29.19            # --date YYYY-MM-DD if GitHub has no date for it
    git commit -m "Kiln roadmap: 0.2.0-dev.29.19 released" -- tools/release/roadmap/roadmap.json
    python3 -I tools/release/roadmap.py deploy ~/Work/frostdev.io --version 0.2.0-dev.29.19 --dry-run
    python3 -I tools/release/roadmap.py deploy ~/Work/frostdev.io --version 0.2.0-dev.29.19

`ship` is idempotent (a released version is left alone). `deploy` stops, changing nothing, when the checkout is dirty
or not on main, has unpushed commits, or the live site (`readlink /opt/frostdev-public/current` on loothing-vps) is not
origin/main: a live site ahead of or apart from origin/main would lose someone's deploy, and undeployed commits on
origin/main would go live unreviewed. The one exception is undeployed commits that change only the roadmap file (a
deploy that failed after its push): a retry deploys them. `--dry-run` runs everything up to the commit (the pull, the
live check, `npm ci`, `npm run check`, `npm run build`) and puts the data file back. Without `--yes` it asks before
the commit. Tests: `python3 -I tools/release/test_roadmap.py`.

## The format

```
{ "released": [ newest first ], "upcoming": [ next first ], "later": [ ... ], "milestones": [ ... ] }
```

- **A release** (`released` or `upcoming`): `version` (`0.2.0-dev.N`, a minor patch `0.2.0-dev.N.M`, or `N.N.N`),
  `title` (a few words), `summary` (a sentence or two, what players get), `"patch": true` on a minor patch and only
  there (the page shows it as a smaller "Minor patch · dev.N" entry), `features` (at least one). Released ones have
  `date` (YYYY-MM-DD, the GitHub publish time in New York; `ship` writes it). Upcoming ones have `status`: the first
  is `"next"`, the rest `"planned"`, in version order, and may have a `note` that replaces the page's default
  sentence about their state (no dates, no internal plan sizes); `ship` drops it.
  Key order as in the file: `version`, `date` or `status`, `title`, `summary`, `patch`, `note`, `features`.
- **A feature**: `title`, `text`, `area`, and only when not the default: `kind` (`"improved"` or `"fix"`; `"new"` is the
  default and left out), `"gamePending": true` (built and checked, not yet seen in a real game), `held` (`"all"`:
  switched off until that game test, `"hosted"`: only on dedicated servers until then; needs `gamePending`),
  `"groundwork": true` (ships, nothing promised with it yet).
- **Areas**: `launcher` (installing, profiles, mod lists, updates, reports), `api` (the C# modding API and the SDK),
  `content` (items, data packs, world, creatures, quests, models, sound, UI), `servers` (dedicated servers, multiplayer,
  admin tools), `compat` (other loaders' mods with Kiln: EMF, ShroudForge, EML, ...), `tools` (the kiln command,
  cooking, VS Code, testing, publishing), `core` (the runtime: hooks, events, limits, safety after game updates).
- **later**: `title`, `text`, `area`, optional `note` ("Not scheduled yet", "Waiting for Nexus Mods", ...).
- **milestones**: `title`, `text`, `after` (a version on the roadmap, or `null`).

`roadmap.py check` validates all of it (what the site's `src/lib/kilnRoadmap.ts` rejects, plus one `next`, first, and
upcoming in version order). Keep the file as `json.dumps(data, indent=2, ensure_ascii=False)` plus a newline writes it
(`ship` rewrites it that way): two spaces, real UTF-8 characters, no trailing spaces.

## Adding and editing items

The page is for players and mod authors, so is every word here:

- Player-facing wording: what they get or what is fixed, in plain sentences ("Mods can make an axe wear faster, slower
  or not at all."), not code names, branches, inbox numbers or internal plans. Credit outside contributors by name and
  pull request, as the release notes do.
- Never claim in public text that something "doesn't work" or is broken. Say what it does now, what it waits for
  ("not yet seen in a real game", `gamePending`), or that it is switched off until then (`held`).
- Future items name no dates and nothing that is not public. An upcoming release with no date says so in its `note`.
- One item per thing a player would notice. A release's items should cover the bullets of its release notes
  (`check` warns about bullets it can't find in the entry; housekeeping lines such as "The mod API is ..." or
  "Nothing changes until a mod asks" need no item).
- `kind`: `fix` for something that went wrong before, `improved` for something that existed and got better, else new.

**The next minor patch** (`0.2.0-dev.29.19`): the first `upcoming` entry, `"status": "next"`, `"patch": true`. Add
items as they land on main. If the patch gets another number or is folded into the next major, rename or remove the
entry (its items move to the major).

**After a minor patch ships**, the next entry becomes `next` (the next major, say). If another patch is planned, add its
entry first in `upcoming` with `"status": "next"` (and the major after it `"planned"`) before its release.

**After a major ships** (`0.2.0-dev.30`), the following major becomes `next`. A patch slot for it
(`0.2.0-dev.30.1`) appears only when you add one: `ship` never invents entries, and the page needs at least one item per
release. Add it as the first upcoming entry once its first fix is known.

**A release that is not on the roadmap at all** (a quick hotfix): add its entry as the first upcoming one before
`release.sh`, or `release.sh` stops with a message pointing here.

**Betas** change nothing here: a test build is not a release. The page has no "in beta" field; the next minor patch's
`note` may say in words that test builds are on the beta channel (0.2.0-dev.29.19's does). Edit that note by hand when
it should change; `ship` drops it with the release.
