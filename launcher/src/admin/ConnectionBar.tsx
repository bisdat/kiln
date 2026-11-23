// The admin session when it is not there: why, how to connect, and Reconnect with the transport choice. While connected
// the session shows in the server's live sentence (pages/servers/ServerLayout.tsx), not here.
import { adminTransport, reconnectAdmin, useAdminState, type AdminTransportKind } from "../api/admin";
import { t as tr, useT } from "../i18n";
import { Banner, Button, Select } from "./ui";

const LABELS: Record<AdminTransportKind, string> = {
  auto: "admin.connection.transportAuto", tcp: "admin.connection.transportTcp", file: "admin.connection.transportFile", ssh: "admin.connection.transportSsh",
};
/** The kinds the transport offers (the launcher: all four; a web page: none, so no choice is shown). */
const transports = (): { value: AdminTransportKind; label: string }[] =>
  (adminTransport()?.kinds ?? []).map((value) => ({ value, label: tr(LABELS[value]) }));

/** Shown by pages while there is no session: why, and what the live tools need. */
export function OfflineNote() {
  const t = useT();
  const st = useAdminState();
  if (st.session) return null;
  if (st.status === "connecting" || st.status === "idle") return <span className="k-empty-title">{t("admin.connection.connecting")}</span>;
  const kinds = transports();
  return (
    <Banner tone="warn" title={t("admin.connection.notConnected")}
      actions={<>
        {kinds.length ? <Select aria-label={t("admin.connection.transport")} value={st.want} onChange={(v) => reconnectAdmin(st.serverId, v as AdminTransportKind)} options={kinds} style={{ width: "auto", height: 32 }} /> : null}
        <Button size="s" onClick={() => reconnectAdmin(st.serverId)}>{t("admin.connection.reconnect")}</Button>
      </>}>
      {t("admin.connection.offline", { reason: st.error?.message ?? t("admin.connection.notAnswering") })}
    </Banner>
  );
}
