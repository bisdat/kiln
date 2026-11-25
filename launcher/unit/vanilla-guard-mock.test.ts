// The browser mock follows the backend's Vanilla start (0.2.0-dev.31.13, src-tauri commands/launch.rs): launch_begin
// with mode `vanilla` is a start the backend tracks (start_tracked), so the update's restart waits for it as it waits for
// a Kiln start, until launch_end or its game.
import { expect, test } from "bun:test";
import { mockInvoke, mockStartTracked } from "../src/api/mock";

test("a Vanilla start launch_begin began holds the update's restart, as a Kiln start does", async () => {
  for (const mode of ["vanilla", "profile"]) {
    expect(mockStartTracked()).toBe(false);
    await mockInvoke("launch_begin", { gameId: "client-steam", mode });
    expect(mockStartTracked()).toBe(true);
    await mockInvoke("launch_end", { gameId: "client-steam" });
    expect(mockStartTracked()).toBe(false);
  }
});
