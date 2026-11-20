// (29.18, inbox 92) The launcher's own files gone from its folder at this start: a Windows install's exe or its
// uninstall.exe, which an antivirus took (launcher/src-tauri/src/own_files.rs, app_info's ownFilesMissing). Said once a
// start, in a notice that stays until closed, with the docs' Antivirus section. launcher.log and the support report
// (launcher-files-missing) say it too.
import { useEffect } from "react";
import * as api from "../api/commands";
import { useAppInfo } from "../api/hooks";
import { t } from "../i18n";
import { toast } from "../ui";
import { ANTIVIRUS_DOCS } from "./runtimeFiles";

let said = false;

export function OwnFilesNotice() {
  const gone = useAppInfo().data?.ownFilesMissing ?? [];
  const files = gone.join(", ");
  useEffect(() => {
    if (said || !files) return;
    said = true;
    const count = gone.length;
    toast.info(t("shell.ownFiles.title", { count }), t("shell.ownFiles.text", { count, files }), {
      ms: 0, actions: [{ label: t("runtime.antivirusHelp"), onClick: () => void api.appOpenUrl(ANTIVIRUS_DOCS).catch((e) => toast.error(e)) }],
    });
  }, [files]); // eslint-disable-line react-hooks/exhaustive-deps -- `gone` is `files` as a list
  return null;
}
