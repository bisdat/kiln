import { expect, test } from "bun:test";
import { renderToStaticMarkup } from "react-dom/server";
import type { FoundReport, Profile } from "../src/api/commands";
import { EmlRNotice } from "../src/components/EmlRNotice";
import { SfSelfHostedNotice } from "../src/components/SfSelfHostedNotice";
import { loaderLine } from "../src/pages/profiles/GameFolder";

test("the EML-R loader gets its own notice alongside a profile with ShroudForge mods", () => {
  const profile = { mods: { shroudforge: [{ enabled: true }] } } as Profile;
  const found: FoundReport = {
    gameId: "game", profileId: "profile", kilnInstalled: true, mods: [], unreadable: [], truncated: false,
    loaders: [{ engine: "eml-r", name: "Enshrouded Mod Loader - Revamped", kinds: [], file: "dbghelp.dll" }],
  };
  const markup = renderToStaticMarkup(
    <>
      <EmlRNotice found={found} />
      <SfSelfHostedNotice profile={profile} found={found} />
    </>,
  );
  expect(markup).toContain("Kiln found EML-R&#x27;s loader (dbghelp.dll)");
  expect(markup).toContain("loads the mods on its own list when it starts, independently of Kiln and its profile switches");
  expect(markup).toContain("Kiln&#x27;s ShroudForge interpreter stands aside");
  expect(markup).toContain("EML-R may run those mods itself");
  expect(markup).not.toContain("ShroudForge&#x27;s own loader is installed");
  expect(loaderLine(found)).toEqual([]);
});
