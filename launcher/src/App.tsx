// Routes. Server pages and admin pages live in their own folders; route lines change here.
// Optional server slots (until a page exists, the shell's defaults render):
//   src/pages/servers/ServersPage.tsx   default export: the /servers index (else: redirect to the first server)
//   src/pages/servers/ServerLayout.tsx  default export: a layout with <Outlet/> around every /servers/:id/* page
//   src/pages/servers/ModpackOffer.tsx  default export: the modpack consent modal Play opens (see PlayPage)
import { lazy, Suspense, type ComponentType } from "react";
import { Navigate, Outlet, Route, Routes, useParams } from "react-router-dom";
import { ADMIN_MERGED } from "./shell/serverNav";
import { AppShell } from "./shell/AppShell";
import { NotFound } from "./shell/NotFound";
import { ServersIndex } from "./shell/ServersIndex";
import PlayPage from "./pages/PlayPage";
import ModsLayout from "./pages/profiles/ProfileLayout";
import LoadOrder from "./pages/mods/LoadOrder";
import ProfileNativePage from "./pages/profiles/ProfileNativePage";
import ProfileConfigsPage from "./pages/profiles/ProfileConfigsPage";
import EnginesView from "./pages/mods/EnginesView";
import { OldProfileRoute } from "./pages/mods/OldRoutes";
import CatalogPage from "./pages/CatalogPage";
import { NexusRoute } from "./components/NexusGate";
import RuntimePage from "./pages/RuntimePage";
import LogsPage from "./pages/LogsPage";
import RegistryConflictsPage from "./registry/RegistryConflictsPage";
import SavesPage from "./pages/SavesPage";
import SettingsPage from "./pages/SettingsPage";
import AboutPage from "./pages/AboutPage";
import { CommunityRoute } from "./community/CommunityPage";

// Server and admin pages load lazily: a page that is mid-edit (or broken) fails alone, inside the shell's error
// boundary, instead of blanking the launcher.
const ServerOverviewPage = lazy(() => import("./pages/servers/ServerOverviewPage"));
const ServerConsolePage = lazy(() => import("./pages/servers/ServerConsolePage"));
const ServerSettingsPage = lazy(() => import("./pages/servers/ServerSettingsPage"));
const ServerModsPage = lazy(() => import("./pages/servers/ServerModsPage"));
const ServerConfigsPage = lazy(() => import("./pages/servers/ServerConfigsPage"));
const ServerBackupsPage = lazy(() => import("./pages/servers/ServerBackupsPage"));
const ServerLogsPage = lazy(() => import("./pages/servers/ServerLogsPage"));
const ServerModpackPage = lazy(() => import("./pages/servers/ServerModpackPage"));
const ServerTicksPage = lazy(() => import("./pages/servers/ServerTicksPage"));
const AdminRoutes = lazy(() => import("./admin").then((m) => ({ default: m.AdminRoutes })));

// Pack Studio (src/studio/, DESIGN 3): desktop only, behind Crucible's switch; loaded on first visit.
const StudioPage = lazy(() => import("./studio/StudioPage"));

// The save editor is big (its game data): loaded on first visit.
const EditorPage = lazy(() => import("./pages/EditorPage"));

// The Steam Deck UI (src/deck/, docs/kiln/launcher/deck.md): its own frame, outside the desktop shell; loaded only when used.
const DeckApp = lazy(() => import("./deck/DeckApp"));

const slots = import.meta.glob<{ default: ComponentType }>(["./pages/servers/ServersPage.tsx", "./pages/servers/ServerLayout.tsx"]);
const slot = (name: string, fallback: ComponentType): ComponentType => {
  const load = slots[`./pages/servers/${name}.tsx`];
  return load ? lazy(load) : fallback;
};
/** An old admin route of a server merged into one of the server's own pages (ADMIN_MERGED). */
function ServerRedirect({ to }: { to: string }) {
  const { id = "" } = useParams();
  return <Navigate to={`/servers/${encodeURIComponent(id)}/${to}`} replace />;
}

const ServersPage = slot("ServersPage", ServersIndex);
const ServerLayout = slot("ServerLayout", Outlet);

export default function App() {
  return (
    <Routes>
      <Route path="deck/*" element={<Suspense fallback={null}><DeckApp /></Suspense>} />
      <Route element={<AppShell />}>
        <Route index element={<PlayPage />} />
        {/* the advanced views of the loaded profile's mods; the main list (Play) is where mods are seen and switched */}
        <Route path="mods" element={<ModsLayout />}>
          <Route index element={<Navigate to="order" replace />} />
          <Route path="order" element={<LoadOrder />} />
          <Route path="native" element={<ProfileNativePage />} />
          <Route path="engines" element={<EnginesView />} />
          <Route path="configs" element={<ProfileConfigsPage />} />
        </Route>
        {/* the old places (before dev.24): Profiles moved into Settings, the Library into the main list */}
        <Route path="profiles/*" element={<OldProfileRoute />} />
        <Route path="library" element={<Navigate to="/" replace />} />
        <Route path="catalog" element={<NexusRoute home="/"><CatalogPage /></NexusRoute>} />
        <Route path="runtime" element={<RuntimePage />} />
        <Route path="logs" element={<LogsPage />} />
        <Route path="conflicts" element={<RegistryConflictsPage />} />
        <Route path="saves" element={<SavesPage />} />
        <Route path="editor" element={<EditorPage />} />
        <Route path="studio" element={<StudioPage />} />
        <Route path="studio/:packId" element={<StudioPage />} />
        <Route path="studio/:packId/:tab" element={<StudioPage />} />
        {/* /settings/<section>: one section of Settings (pages/settings/model.ts); /settings#<group> opens a group's section */}
        <Route path="settings/:section?" element={<SettingsPage />} />
        <Route path="about" element={<AboutPage />} />
        <Route path="community" element={<CommunityRoute />} />
        <Route path="servers" element={<ServersPage />} />
        {/* outside the server's layout and the lazy admin pages: the redirect is the router's alone */}
        {ADMIN_MERGED.map(([from, to]) => <Route key={from} path={`servers/:id/admin/${from}`} element={<ServerRedirect to={to} />} />)}
        <Route path="servers/:id" element={<ServerLayout />}>
          <Route index element={<Navigate to="overview" replace />} />
          <Route path="overview" element={<ServerOverviewPage />} />
          <Route path="console" element={<ServerConsolePage />} />
          <Route path="settings" element={<ServerSettingsPage />} />
          <Route path="mods" element={<ServerModsPage />} />
          <Route path="configs" element={<ServerConfigsPage />} />
          <Route path="backups" element={<ServerBackupsPage />} />
          <Route path="logs" element={<ServerLogsPage />} />
          <Route path="ticks" element={<ServerTicksPage />} />
          <Route path="modpack" element={<ServerModpackPage />} />
          <Route path="admin/*" element={<AdminRoutes />} />
        </Route>
        <Route path="*" element={<NotFound />} />
      </Route>
    </Routes>
  );
}
