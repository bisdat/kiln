import { useEffect, useState, type ReactNode } from "react";
import * as api from "../api/commands";
import { Button, Modal, Splash, errorText } from "../ui";

export function SnapshotGate({ children }: { children: ReactNode }) {
  const [status, setStatus] = useState<api.SnapshotStartup | null>(null);
  const [error, setError] = useState<string | null>(null);
  useEffect(() => {
    let live = true;
    const read = () => api.snapshotStatus().then((s) => { if (live) setStatus(s); }, (e) => { if (live) setError(errorText(e)); });
    void read();
    const timer = setInterval(() => void read(), 500);
    return () => { live = false; clearInterval(timer); };
  }, []);
  if (status?.phase === "ready") return <>{children}</>;
  if (status?.phase !== "blocked" && !error) return <Splash done={false} />;
  const go = async () => {
    try { await api.snapshotContinue(); setError(null); setStatus(await api.snapshotStatus()); }
    catch (e) { setError(errorText(e)); }
  };
  return <Modal open locked onClose={() => {}} title="The copy from before the beta could not be completed" footer={<>
    <Button onClick={() => void api.deckQuit()}>Quit</Button>
    {status?.canContinue && <Button variant="primary" onClick={() => void go()}>Go on without a way back</Button>}
  </>}>
    <p>Settings and profiles are held until you decide. Going on records that this beta has no complete copy to restore.</p>
    {(status?.reasons ?? []).map((reason) => <p key={reason}>{reason}</p>)}
    {error && <p>{error}</p>}
  </Modal>;
}
