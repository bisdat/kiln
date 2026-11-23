// Result shapes of Kiln.Admin's ops (mods/Kiln.Admin/Ops*.cs is the source of truth).
import type { AdminPos, KapRestart } from "../api/admin";

export interface AdminInfo {
  admin: string;
  kiln: string;
  api: string;
  protocol: string;
  transports: string[];
  endpoint: { host: string; port: number } | null;
  server: { id?: string | null; name?: string | null };
  uptimeS: number;
  connections: number;
  bridge?: string;
  ops: string[];
  topics: string[];
  /** since 0.2.0-dev.30.9 */
  restart?: KapRestart & { request: RestartRequest | null };
}

// restart (server_restart, server_restart_cancel; admin-protocol.md section 11)
export interface RestartRequest {
  state: "counting" | "saving" | "stopping" | "failed" | "canceled";
  by: string;
  requested: string;
  secondsLeft: number;
  message: string | null;
  error: string | null;
  /** how the stop was raised (e.g. "kernelbase.dll!CtrlRoutine") */
  via: string | null;
  restart: KapRestart;
}

// one-time login (login_mint, login_redeem; KAP only)
export interface LoginMintResult {
  secret: string;
  consumer: string;
  expiresAt: string;
  ttlS: number;
}
export interface LoginRedeemResult {
  role: "owner";
  consumer: string;
  label: string | null;
  mintedBy: string;
  mintedAt: string;
}

/** The "metrics" topic's push data (every 2 s to subscribers). */
export interface AdminMetrics {
  /** Unix ms at the window's end */
  at: number;
  windowS: number;
  steps: number;
  stepsPerS: number;
  /** from one step's start to the next */
  frameMs: { avg: number; p95: number; max: number };
  /** one step itself (Pre to Post); null when none finished in the window */
  tickMs: { avg: number; p95: number; max: number } | null;
  memory: { workingSet: number; managedHeap: number };
  /** percent: 100 = one core; null on the first push */
  cpu: { percent: number | null; cores: number };
  gcPauseMs: number;
  players: number | null;
}

export interface StatusResult {
  kiln: string;
  features: Record<string, boolean>;
  ops: string[];
}

// world
export interface EnemyNear {
  entity: number;
  pos: AdminPos;
  guid?: string;
  name?: string;
}
export interface EnemiesResult {
  near: EnemyNear[];
  catalog: { guid: string; name: string; seen: number }[];
  total: number;
}
export interface TeleportResult {
  entity: number;
  to: AdminPos;
  method: string;
  position: AdminPos;
  distance: number;
  steps: number;
  flags: string;
}

// items
export interface InventorySlot {
  item: number;
  count: number;
  pide?: number;
  level?: number;
  rarity?: string | null;
  equipped?: boolean;
  name?: string;
}
export interface InventoryResult {
  inventories: { link: number; entity: number; category: string; slots: InventorySlot[] }[];
  generic: number;
  available: number;
}
export interface GiveResult {
  given: number;
  requested: number;
  stacks: number;
  status: string;
}
export interface ItemHit {
  id: number;
  /** What players see (a mod item's display name on a current Kiln.Admin). */
  name: string;
  /** The game's debug name (a current Kiln.Admin sends it). */
  debug?: string;
  gear: boolean;
}

// spawn
export interface SpawnResult {
  spawned: number;
  failed: number;
  reason: string | null;
}

// settings / time
export interface SettingsGet {
  values: Record<string, number | boolean | string | null>;
  write_supported: boolean;
}
export interface SettingsSet {
  path: string;
  before: number | boolean | string | null;
  after: number | boolean | string | null;
}
export interface TimeState {
  hours: number;
  clock: string;
  day: boolean;
  dayMinutes: number;
  nightMinutes: number;
  weather: boolean;
}

// combat
export interface HealthResult {
  entity: number;
  health: number;
  healthMax: number;
  dead: boolean;
  god: boolean;
}

// saves
export interface SaveStatus {
  state: string | null;
  world: string | null;
  revision: number | null;
  lastSaved: string | null;
  autosaveSeconds: number | null;
  lastReason: string | null;
  written: number | null;
  failed: number | null;
  dataGeneration: number | null;
  dataMatch: string | null;
  pendingRestore: string | null;
}
export interface BackupInfo {
  name: string;
  folder: string;
  created: string;
  world: string | null;
  revision: number | null;
  bytes: number;
  files: number;
  dataBundles: number;
  reason: string;
}

// gameplay
export interface Zone {
  name: string;
  id: number;
  active: boolean;
  pos: AdminPos;
  radius: number;
  deny: string[];
  owners: string[];
  allowAdmins: boolean;
}
export interface GameplayHookRow { state: "idle" | "waiting" | "installed" | "refused" | "unknown"; reason: string }
export interface GameplayStatus {
  hookGroups?: Partial<Record<"build" | "terraform" | "spawn", GameplayHookRow>>;
  available: boolean;
  intercept?: boolean;
  craftHook?: boolean;
  buildHooks?: boolean;
  terraformHook?: boolean;
  spawnHook?: boolean;
  zones?: number;
  denied?: number;
  canceled?: number;
}

// content
export type ContentKind = "items" | "recipes" | "loot" | "drops";
export interface ContentStatus {
  ready: boolean;
  items: number;
  recipes: number;
  loot: number;
  drops: number;
  packs: number;
  errors: number;
  conflicts: number;
  changed: number;
  stackGuard: boolean;
  generation: number;
  detail: string;
}
export interface ContentField {
  path: string;
  type: string;
  valueType: string;
  writable: boolean;
  enumKind: "plain" | "enum" | "flags" | string;
  options?: string[];
  value?: string;
  original?: string;
  changed?: boolean;
  error?: string;
}
export interface ContentHit {
  id: string;
  name: string;
  modified: boolean;
}
export interface RecipeStack {
  item: string;
  name?: string | null;
  count: number;
  category?: string;
}
export interface ContentEntry {
  kind: ContentKind;
  id: string;
  name: string;
  guid: string;
  modified: boolean;
  fields: ContentField[];
  inputs?: RecipeStack[];
  outputs?: RecipeStack[];
  stacks?: { index: number; item: string; name?: string | null; countMin: number; countMax: number; chance: number; weight: number; group: number }[];
}
export interface ContentValue {
  value: string;
  original: string;
  reset?: boolean;
}
export interface PackReport {
  packs?: { file: string; applied: number; errors?: string[]; warnings?: string[] }[];
  conflicts?: { field: string; winner: string; winnerValue: string; loser: string; loserValue: string }[];
  skipped?: string[];
  [key: string]: unknown;
}

// configs
export interface ConfigEntry {
  path: string;
  type: string;
  comment?: string | null;
  default: unknown;
  value: unknown;
  fileValue?: unknown;
  fromServer?: boolean;
  requiresRestart?: boolean;
  synced?: boolean;
  min?: number;
  max?: number;
  options?: string[];
}
export interface ConfigFile {
  id: string;
  mod: string;
  kind: string;
  file: string;
  loadedAt?: string | null;
  error?: string | null;
  warnings: string[];
  sections: { path: string; comment?: string | null; synced?: boolean }[];
  entries: ConfigEntry[];
}

// permissions
export interface PermGroup {
  admin?: boolean;
  inherits?: string[];
  nodes?: string[];
}
export interface PermissionsModel {
  file?: string;
  defaultGroup: string;
  kickBanIsAdmin?: boolean;
  groups: Record<string, PermGroup>;
  players?: Record<string, { name?: string | null; groups: string[]; nodes: string[] }>;
  userGroups?: Record<string, string[]>;
  serverUserGroups?: { name: string; rights: string }[];
  nodes?: { node: string; level: string; description?: string; owner?: string }[];
  [key: string]: unknown;
}
export interface PermissionCheck {
  allowed: boolean;
  node: string;
  level: string;
  rule: string | null;
  from: string | null;
  groups: string[];
}

// commands
export interface CommandInfo {
  name: string;
  aliases?: string[];
  usage?: string;
  description?: string;
  /** the owning mod's id ("kiln" for Kiln's own) */
  owner?: string;
  node?: string;
  /** the command's level: everyone, admin or console */
  permission?: string;
  typed?: boolean;
  usageLines?: string[];
  tree?: CommandNode;
}
/** A node of a typed command's tree; the form fields are since 0.2.0-dev.30.9 (admin-protocol.md section 11). */
export interface CommandNode {
  kind?: "literal" | "argument" | string;
  name?: string;
  executable?: boolean;
  description?: string;
  aliases?: string[];
  /** a subcommand's own permission node, and its level */
  permission?: string;
  level?: "everyone" | "admin" | "console" | string;
  /** argument: the line may end before it */
  optional?: boolean;
  /** argument: the value used when it is left out */
  default?: number | boolean | string | null;
  type?: string;
  describe?: string;
  options?: string[];
  min?: number;
  max?: number;
  /** duration: "s", time: "h" */
  unit?: string;
  greedy?: boolean;
  quotable?: boolean;
  many?: boolean;
  selectors?: string[];
  relative?: boolean;
  children?: CommandNode[];
}
export interface CompleteResult {
  start: number;
  suggestions: string[];
}
export interface CommandRunResult {
  name: string;
  found: boolean;
  ok: boolean;
  denied: boolean;
  canceled: boolean;
  parseError: boolean | string | null;
  error: string | null;
  output: string[];
  /** the source it ran as ("dashboard", or the request's source); since 0.2.0-dev.30.9 */
  source?: string;
}

// mods
export interface KilnMod {
  id: string;
  name: string;
  version: string;
  side: string;
  state: "Loaded" | "Failed" | "Disabled" | "Unloaded" | "Skipped" | "Pending" | string;
  error?: string | null;
  loadIndex: number;
  advanced?: boolean | null;
  folder?: string | null;
  description?: string | null;
  authors?: string[];
  dependencies?: string[];
  preMsPerStep?: number;
  postMsPerStep?: number;
  maxMs?: number;
  faults?: number;
  lastFault?: string | null;
}
export interface ModOpResult {
  ok: boolean;
  message: string;
  affected: string[];
}
export interface TestReport {
  mod: string;
  version: string;
  ms: number;
  passed: number;
  failed: number;
  skipped: number;
  file?: string;
  results: { name: string; outcome: "Pass" | "Fail" | "Timeout" | "Skip" | string; message?: string | null }[];
}
export interface CrashReportInfo {
  name: string;
  bytes: number;
  time: string;
}
export interface LogTail {
  file: string;
  lines: string[];
  size: number;
}
