// Settings, Library: where the mods' files are kept, what they take on disk, and Clean up (every version no profile and
// no game's profile file uses; library_unused, then one library_remove_many). The main list is where mods are seen,
// switched and removed; a mod's other versions are in its row's menu. Its last line is the data folder (the library
// and everything else the launcher keeps, moved as one): ./DataFolderSettings.tsx. Below the numbers: the earlier versions
// an update kept for a roll back (shell/rollback.ts), each with its size and a Remove; Clean up leaves those.
import { useQueryClient } from "@tanstack/react-query";
import { useState } from "react";
import * as api from "../api/commands";
import type { LibraryRef } from "../api/commands";
import { useLibrary } from "../api/hooks";
import { keptSize, keptVersions, liveVersions, type KeptRow } from "../shell/rollback";
import { Button, Card, ConfirmDanger, EmptyState, KeyValue, Modal, Skeleton, errorText, fmtAgo, fmtBytes, toast } from "../ui";
import { DataFolderValue } from "./DataFolderSettings";

export function LibraryCard() {
  const qc = useQueryClient();
  const lib = useLibrary(null);
  const [gc, setGc] = useState<LibraryRef[] | null>(null);
  const [drop, setDrop] = useState<KeptRow | null>(null);
  const all = lib.data ?? [];
  // a kept version is no second copy of a mod: counted on its own line
  const versions = all.reduce((n, e) => n + liveVersions(e), 0);
  const bytes = all.reduce((n, e) => n + e.versions.reduce((m, v) => m + (v.size ?? 0), 0), 0);
  const several = all.filter((e) => liveVersions(e) > 1).length;
  const kept = keptVersions(all);
  const keptBytes = kept.reduce((n, k) => n + keptSize(k.version), 0);
  const refresh = () => qc.invalidateQueries({ queryKey: ["library_list"] });
  const cleanUp = async () => {
    try { setGc(await api.libraryUnused()); }
    catch (e) { toast.error(e); }
  };
  return (
    <Card id="library" aria-label="Library" title="Library" sub="Where Kiln keeps every mod's files, by version."
      actions={<Button size="s" disabled={!all.length} onClick={() => void cleanUp()}>Clean up</Button>}>
      {lib.isLoading ? <Skeleton lines={2} /> : lib.error && <EmptyState title="The library is not readable" hint={errorText(lib.error)} />}
      {/* the data folder shows whatever the library does: it is where to look when the library cannot be read */}
      <KeyValue rows={[
        lib.data && ["Mods", `${all.length}${versions > all.length ? ` (${versions} versions${several ? `; ${several} with more than one: their menu on Play picks one` : ""})` : ""}`],
        lib.data && ["On disk", fmtBytes(bytes)],
        kept.length > 0 && ["Kept for a roll back", `${kept.length} earlier version${kept.length === 1 ? "" : "s"}, ${fmtBytes(keptBytes)}`],
        ["Data folder", <DataFolderValue />],
      ]} />
      {kept.length > 0 && (
        <ul className="k-kept-list" aria-label="Kept versions">
          {kept.map((k) => (
            <li key={`${k.kind}/${k.id}/${k.version.hash}`} className="k-kept-row">
              <span className="k-kept-name">{k.name} <span className="k-mono">{k.version.version}</span></span>
              <span className="k-muted">{fmtBytes(keptSize(k.version))}
                {k.version.kept!.settings.length ? `, ${k.version.kept!.settings.length} settings file${k.version.kept!.settings.length === 1 ? "" : "s"}` : ""}
                {k.version.kept!.replacedBy ? `, replaced by ${k.version.kept!.replacedBy}` : ""} {fmtAgo(k.version.kept!.kept)}</span>
              <Button size="s" variant="ghost" aria-label={`Remove ${k.name} ${k.version.version}`} onClick={() => setDrop(k)}>Remove</Button>
            </li>
          ))}
        </ul>
      )}
      <span className="k-hint">Clean up removes the versions no profile and no game uses, not the ones kept for a roll back. Removing one mod: its menu on Play. Rolling back: the mod's menu on Play.</span>
      <ConfirmDanger open={!!drop} onClose={() => setDrop(null)} relaxable word={drop?.id ?? ""} action="Remove"
        title={drop ? `Remove ${drop.name} ${drop.version.version}?` : ""}
        message="This earlier version and the settings kept with it leave the Library: you can no longer roll back to it."
        onConfirm={async () => {
          const k = drop!;
          try {
            await api.libraryRemove({ kind: k.kind, id: k.id, version: k.version.version, hash: k.version.hash });
            toast.ok(`Removed ${k.name} ${k.version.version}`);
          } catch (e) { toast.error(e); throw e; } finally { refresh(); }
        }} />
      <ConfirmDanger open={!!gc && gc.length > 0} onClose={() => setGc(null)} title={`Remove ${gc?.length ?? 0} unused?`} word="clean" action="Remove"
        message="No profile and no game uses them. Profiles and saves stay as they are."
        onConfirm={async () => {
          // one call: the backend looks at the running processes and the profiles once for the whole batch
          try {
            const r = await api.libraryRemoveMany(gc ?? []);
            for (const f of r.failed) toast.error(f.error, `${f.entry.id} ${f.entry.version}`);
            const n = r.removed.length;
            if (n) toast.ok(`Removed ${n}`, `${n} version${n === 1 ? "" : "s"} left the library.`);
          } catch (e) {
            // a running game stops the whole clean up before anything goes ("Close Enshrouded first: ...")
            toast.error(e);
          }
          refresh();
        }} />
      {gc && gc.length === 0 && <Modal open onClose={() => setGc(null)} title="Nothing to clean" width={480} footer={<Button onClick={() => setGc(null)}>OK</Button>}>Every version is in a profile, or in the profile file of a game.</Modal>}
    </Card>
  );
}
