// api/channel.ts: the channels' names (dev is the release line while there is no stable release) and the select's
// choices, with the stored values a 0.2.0-dev.28 launcher keeps reading.
import { describe, expect, test } from "bun:test";
import { RELEASE_CHANNEL, STABLE_RELEASED, channelName, channelOptions, channelWord } from "../src/api/channel";

describe("channel names", () => {
  test("no stable release yet: dev is the Release, beta the test builds", () => {
    expect(STABLE_RELEASED).toBe(false);
    expect(channelName("dev")).toBe("Release");
    expect(channelName("beta")).toBe("Beta (test builds)");
    expect(channelWord("beta")).toBe("Beta");
    expect(channelWord("dev")).toBe("Release");
    expect(RELEASE_CHANNEL).toBe("dev");
  });
  test("the select lists Release first, then Beta; Stable only where it is already chosen", () => {
    expect(channelOptions("dev")).toEqual([{ value: "dev", label: "Release" }, { value: "beta", label: "Beta (test builds)" }]);
    expect(channelOptions("beta").map((o) => o.value)).toEqual(["dev", "beta"]);
    expect(channelOptions("stable").map((o) => o.value)).toEqual(["dev", "beta", "stable"]);
    expect(channelOptions("stable").at(-1)?.label).toBe("Stable (none yet)");
  });
  test("plain words: no jargon in a name", () => {
    for (const ch of ["stable", "dev", "beta"] as const) {
      expect(channelName(ch)).not.toMatch(/pre-?release|channel|nightly/i);
    }
  });
});
