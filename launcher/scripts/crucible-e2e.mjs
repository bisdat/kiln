#!/usr/bin/env node
// End to end: the launcher's Crucible client (kiln_manage::crucible, driven the way commands/crucible.rs drives it)
// against a Crucible service running on this machine: MinIO in Docker for R2 and the CDN, the service's fake relay as
// the Discord sign-in, the real service started from an env file. Then the ignored Rust test
// commands::crucible::tests::end_to_end_against_a_local_crucible runs with KILN_CRUCIBLE_DEV (the local API, CDN and
// signing key) and KILN_CRUCIBLE_E2E_SESSION (the stub's session): share -> code -> signed index -> install -> version 2
// with only the new file uploaded -> update -> undo. No GUI, no game.
//
//   CRUCIBLE_DIR=<the crucible service repo> node launcher/scripts/crucible-e2e.mjs     (from the repo root; needs Docker)
import { spawn, execFileSync } from 'node:child_process';
import crypto from 'node:crypto';
import { mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { pathToFileURL } from 'node:url';

const svc = process.env.CRUCIBLE_DIR;
if (!svc) { console.error('set CRUCIBLE_DIR to the crucible service repo'); process.exit(2); }
const repo = path.resolve(new URL('../..', import.meta.url).pathname);
const load = (p) => import(pathToFileURL(path.join(svc, p)).href);
const { s3Client } = await load('src/s3.mjs');
const { fakeRelay } = await load('testkit/fake-relay.mjs');
const { minio } = await load('testkit/minio.mjs');

const dir = mkdtempSync(path.join(tmpdir(), 'kiln-crucible-e2e-'));
let child = null;
const cleanup = () => { if (child && child.exitCode === null) child.kill('SIGTERM'); rmSync(dir, { recursive: true, force: true }); };
process.on('exit', cleanup);

const m = await minio();
const bucket = 'crucible-e2e';
const relaySecret = crypto.randomBytes(24).toString('hex');
const relay = fakeRelay({ secret: relaySecret });
const relayUrl = await relay.listen();
for (let i = 0; ; i++) {
  try { await s3Client({ endpoint: m.endpoint, region: m.region, bucket, accessKeyId: m.accessKeyId, secretAccessKey: m.secretAccessKey }).ensureBucket({ publicRead: true }); break; }
  catch (e) { if (e.status !== 503 || i > 150) throw e; await new Promise((r) => setTimeout(r, 200)); }
}
const keyFile = path.join(dir, 'signing-key.pem');
const pub = JSON.parse(execFileSync(process.execPath, [path.join(svc, 'src/main.mjs'), '--gen-key', keyFile], { encoding: 'utf8' }));
const port = 20000 + crypto.randomInt(20000);
const envFile = path.join(dir, 'crucible.env');
writeFileSync(envFile, [
  'HOST=127.0.0.1', `PORT=${port}`, `CRUCIBLE_DB=${path.join(dir, 'crucible.db')}`, `CRUCIBLE_PUBLIC_ORIGIN=http://127.0.0.1:${port}`,
  `CRUCIBLE_CDN_ORIGIN=${m.endpoint}/${bucket}`, `S3_ENDPOINT=${m.endpoint}`, `S3_REGION=${m.region}`, `S3_BUCKET=${bucket}`,
  `S3_ACCESS_KEY_ID=${m.accessKeyId}`, `S3_SECRET_ACCESS_KEY=${m.secretAccessKey}`, `RELAY_INTERNAL_URL=${relayUrl}`,
  `CRUCIBLE_RELAY_SECRET=${relaySecret}`, 'CRUCIBLE_MULTIPART_OVER=1000000', 'CRUCIBLE_PART_SIZE=5242880', `CRUCIBLE_ADMIN_SECRET=${crypto.randomBytes(24).toString('hex')}`, `CRUCIBLE_SIGNING_KEY_FILE=${keyFile}`,
].join('\n'), { mode: 0o600 });
child = spawn(process.execPath, [path.join(svc, 'src/main.mjs')], { env: { PATH: process.env.PATH, CRUCIBLE_ENV_FILE: envFile }, stdio: ['ignore', 'pipe', 'inherit'] });
const api = `http://127.0.0.1:${port}/kiln/crucible/api/v1`;
let health;
for (let i = 0; i < 100; i++) { try { health = await (await fetch(`${api}/health`)).json(); if (health.ok) break; } catch { /* starting */ } await new Promise((r) => setTimeout(r, 100)); }
if (!health?.ok) { console.error('the service did not start'); process.exit(1); }
console.log(`service up on ${port}, signing key ${pub.keyId}, MinIO ${m.endpoint}`);
const session = relay.signIn({ name: 'Emberfox' });
const env = {
  ...process.env,
  RUSTUP_TOOLCHAIN: process.env.RUSTUP_TOOLCHAIN || '1.99.1',
  KILN_CRUCIBLE_DEV: `api=${api} cdn=${m.endpoint}/${bucket} key=${pub.publicKey}`,
  KILN_CRUCIBLE_E2E_SESSION: session,
  KILN_NO_KEYRING: '1',
};
// spawn, not spawnSync: the fake relay answers the service from this process's event loop
const status = await new Promise((resolve) => {
  const c = spawn('cargo', ['test', '-p', 'kiln-launcher', '--lib', '--offline', 'commands::crucible::tests::end_to_end_against_a_local_crucible', '--', '--ignored', '--nocapture'],
    { cwd: repo, env, stdio: 'inherit' });
  c.on('exit', (code) => resolve(code ?? 1));
});
const r = { status };
await relay.close();
console.log(r.status === 0 ? '\nend to end against the local Crucible: passed' : '\nend to end against the local Crucible: FAILED');
cleanup();
process.exit(r.status ?? 1);
