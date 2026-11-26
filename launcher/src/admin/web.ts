// @kiln/admin-ui: the launcher's admin pages for hosts outside the Tauri app (Kilnwarden). The same pages, hooks
// and hub as the launcher's server pages; KAP/1 reaches Kiln.Admin through the transport the host configures, here the
// WebSocket one (KAP/1 frames as JSON over a signed-in socket). Docs: docs/kiln/launcher/admin-pages.md, "The
// @kiln/admin-ui package"; the socket's protocol: admin-protocol.md section 12.
//
//   import { configureAdmin, createWebSocketTransport, AdminRoutes, ConfirmHost, Toaster } from "@kiln/admin-ui";
//   import "@kiln/admin-ui/style.css";
//   configureAdmin({ transport: createWebSocketTransport({ url: (id) => `wss://${location.host}/kap/${id}` }) });
//   <QueryClientProvider client={qc}><HashRouter><AdminRoutes serverId="this" /><ConfirmHost /><Toaster /></HashRouter></QueryClientProvider>
//
// Nothing reachable from here imports Tauri, the launcher's command layer or its mock
// (unit-tests/admin-ui-package.test.ts). Built by `bun run build:admin-ui` (launcher/) into packages/admin-ui/dist, where
// the package's package.json is (none here: the launcher's `import("./admin")` must stay this folder's index.tsx).
import "../theme/base.css";

// the pages: all of them as routes, or one by one
export { AdminRoutes, ADMIN_PAGES, ConfirmHost, adminBase } from "./index";
export { default as AdminPlayersPage } from "./pages/AdminPlayersPage";
export { default as AdminMapPage } from "./pages/AdminMapPage";
export { default as AdminZonesPage } from "./pages/AdminZonesPage";
export { default as AdminItemsPage } from "./pages/AdminItemsPage";
export { default as AdminSpawnPage } from "./pages/AdminSpawnPage";
export { default as AdminWorldPage } from "./pages/AdminWorldPage";
export { default as AdminCombatPage } from "./pages/AdminCombatPage";
export { default as AdminCreaturesPage } from "./pages/AdminCreaturesPage";
export { default as AdminGameDataPage } from "./pages/AdminGameDataPage";
export { default as AdminPermissionsPage } from "./pages/AdminPermissionsPage";
export { default as AdminContentPage } from "./pages/AdminContentPage";
export { default as AdminSavesPage } from "./pages/AdminSavesPage";
export { default as AdminCrashesPage } from "./pages/AdminCrashesPage";
export { default as AdminConfigsPage } from "./pages/AdminConfigsPage";
export { OfflineNote } from "./ConnectionBar";
export { assetsRoot } from "./catalog";

// the hub, its hooks, the KAP types, the transport seam and the typed newer ops
export * from "../api/admin";
// KAP/1 over a WebSocket
export { createWebSocketTransport, opBudgetS } from "../api/adminWs";
export type { WebSocketAdminTransport, WebSocketClass, WebSocketLike, WebSocketTransportOptions } from "../api/adminWs";
// op result shapes
export type * from "./types";
export { isKilnError, toKilnError } from "../api/shared";
export type { ErrorCode, Json, KilnError, ServerId } from "../api/shared";

// what the host renders once (toasts; confirm dialogs: ConfirmHost above) and the texts' languages
export { Toaster, toast } from "../ui/Toast";
export { currentLanguage, languages, registerLocale, setLanguage, t, useT } from "../i18n";
