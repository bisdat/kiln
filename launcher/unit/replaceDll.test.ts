import { expect, test } from 'bun:test';
import { canReplaceDll, replacementLabel } from '../src/shell/replaceDll';
test('Replace DLL is offered only for a native Library row in a profile', () => {
  expect(canReplaceDll('native', true, true)).toBe(true);
  expect(canReplaceDll('native', false, true)).toBe(false);
  expect(canReplaceDll('native', true, false)).toBe(false);
  expect(canReplaceDll('greeneye', true, true)).toBe(false);
  expect(canReplaceDll(null, true, true)).toBe(false);
});
test('local replacement date is visible without changing the release version', () => {
  expect(replacementLabel({ source: { type: 'replaced-dll', from: '{}' }, imported: '2026-11-15T12:00:00Z' })).toBe('replaced DLL, 15 Nov');
  expect(replacementLabel({ source: null, imported: '' })).toBe('');
});
