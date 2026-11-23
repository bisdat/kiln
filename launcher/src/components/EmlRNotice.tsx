import type { FoundReport } from "../api/commands";
import { useT } from "../i18n";
import { Banner } from "../ui";

export function EmlRNotice({ found }: { found?: FoundReport }) {
  const t = useT();
  const loader = found?.loaders.find((l) => l.engine === "eml-r");
  if (!loader) return null;
  return (
    <div style={{ flexShrink: 0, overflowWrap: "anywhere" }}>
      <Banner title={t("emlRNotice.title", { file: loader.file })}>
        {t("emlRNotice.text")} {t("emlRNotice.interpreter")}
      </Banner>
    </div>
  );
}
