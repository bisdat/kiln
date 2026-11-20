// The Deck's QR codes (src/community/qr.ts): the smallest version that holds the link, the fixed patterns where a
// reader looks for them, and one whole symbol checked against a reader. The symbol below was decoded with zbarimg
// (ZBar 0.23) to "https://discord.gg/7pjr8SMTfU" when it was recorded, as were versions 1 to 10 of other links
// (dev29/out/discord/scripts/qr-check.ts).
import { expect, test } from "bun:test";
import { qrEncode, qrPath } from "../src/community/qr";

const INVITE = [
  "#######...#.#..##..##.#######", "#.....#....##....##...#.....#", "#.###.#.###.#.#.#.##..#.###.#", "#.###.#.##.#....####..#.###.#",
  "#.###.#.#.##.###.#.##.#.###.#", "#.....#.#..######.#...#.....#", "#######.#.#.#.#.#.#.#.#######", "........#....#.#..#..........",
  "#.#####..#....##....#.#####..", "....#.......#..##..######...#", ".#.#..##.#.##....##.#.##.....", "#.#.##.####...#.#..##.#.##.#.",
  ".....##..##.....#...#....##..", ".##....###.#.###.###..###...#", "########..##.####...##.#.##..", "##.###..#....#.#...####.#..#.",
  "#..#..###.##..##...#...#.##..", "##.#...#.#.....##..#.####.#.#", "#.##..#.#.##........#..##.#..", "#...#..#.#..#.#.#.####.....#.",
  "#....###..###...##..#####.###", "........########..#.#...#####", "#######..#.#.#####.##.#.###..", "#.....#.###..#.#..###...#....",
  "#.###.#.#...#.##....#####.#.#", "#.###.#.#.##...##..###...##..", "#.###.#.#..####....#.#######.", "#.....#..###.#..#...#.#.##.#.",
  "#######.#####.....##.####.#..",
];

const rows = (text: string) => qrEncode(text).modules.map((r) => r.map((b) => (b ? "#" : ".")).join(""));

test("the Kiln Discord's invite encodes to the symbol a reader decoded", () => {
  expect(rows("https://discord.gg/7pjr8SMTfU")).toEqual(INVITE);
});

test("each link takes the smallest version that holds it, up to version 10", () => {
  expect(qrEncode("a").version).toBe(1);
  expect(qrEncode("https://frostdev.io/link/HV3R9TCW").version).toBe(3);
  expect(qrEncode("https://discord.com/channels/1568454645683195976/1573130884210000003").version).toBe(5);
  expect(qrEncode("https://example.com/" + "z".repeat(190)).version).toBe(10);
  expect(() => qrEncode("x".repeat(214))).toThrow();
  const q = qrEncode("https://frostdev.io/kiln/privacy");
  expect(q.size).toBe(q.version * 4 + 17);
});

test("the finder patterns, the timing lines and the dark module are where readers look", () => {
  for (const text of ["a", "https://discord.gg/7pjr8SMTfU", "https://example.com/" + "y".repeat(120)]) {
    const { modules: m, size } = qrEncode(text);
    const finder = (x0: number, y0: number) => {
      for (let y = 0; y < 7; y++) for (let x = 0; x < 7; x++) {
        const ring = Math.max(Math.abs(x - 3), Math.abs(y - 3));
        expect(m[y0 + y][x0 + x]).toBe(ring !== 2);
      }
    };
    finder(0, 0); finder(size - 7, 0); finder(0, size - 7);
    for (let i = 8; i < size - 8; i++) { expect(m[6][i]).toBe(i % 2 === 0); expect(m[i][6]).toBe(i % 2 === 0); }
    expect(m[size - 8][8]).toBe(true);
  }
});

test("the SVG path has one square per dark module, inside the quiet zone", () => {
  const q = qrEncode("a");
  const dark = q.modules.flat().filter(Boolean).length;
  const d = qrPath(q);
  expect(d.match(/M/g)?.length).toBe(dark);
  expect(d.startsWith("M4 4h1v1h-1z")).toBe(true);
});
