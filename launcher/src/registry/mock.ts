// A normal browser session has no native registry report. Playwright supplies synthetic report fixtures.
export async function readMockRegistryReport(): Promise<string | null> { return null; }
