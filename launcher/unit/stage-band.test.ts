// The fire's canvas behind a sheet (shell/Stage.tsx stageCanvas): only the header band is drawn, and it shows exactly the
// pixels the whole-window canvas has there (same scale, offset by the rows below it), so the look does not change.
import { describe, expect, test } from "bun:test";
import { STAGE_BAND, stageCanvas } from "../src/shell/Stage";

describe("stageCanvas", () => {
  for (const [W, H] of [[1280, 800], [1360, 860], [1920, 1080], [2560, 1440], [3840, 2160], [1024, 640], [1281, 801]] as const) {
    test(`${W}x${H}`, () => {
      const full = stageCanvas(W, H, false);
      const band = stageCanvas(W, H, true);
      // the whole window: as before (at most 1000 px wide, 0.72 of the window at most)
      const S = Math.min(0.72, 1000 / W);
      expect(full).toEqual({ cw: Math.max(2, Math.round(W * S)), ch: Math.max(2, Math.round(H * S)), fullCh: Math.max(2, Math.round(H * S)), cssH: H, offset: 0, S });
      // the band: same width and scale, the top rows of the whole canvas
      expect(band.cw).toBe(full.cw);
      expect(band.fullCh).toBe(full.ch);
      expect(band.offset + band.ch).toBe(full.ch);
      expect(band.cssH * (full.ch / H)).toBeCloseTo(band.ch, 6);
      // it covers the header and the sheet's lit edge, and not much more
      expect(band.cssH).toBeGreaterThanOrEqual(STAGE_BAND);
      expect(band.cssH - STAGE_BAND).toBeLessThan(H / full.ch); // less than one canvas row more
      expect(band.ch).toBeLessThan(full.ch / 5);
    });
  }
  test("a window no taller than the band draws it whole", () => {
    expect(stageCanvas(1280, 90, true)).toEqual(stageCanvas(1280, 90, false));
  });
});
