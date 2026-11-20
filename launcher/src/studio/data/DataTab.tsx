// The pack layer's data editors (DESIGN 3.3): Items, Recipes, Loot, Drops, New items, Raw JSON, with the game's art
// icons and the live lint. Placeholder until the data editors land (dev30/pack-studio-data).
import type { GameId, Studio } from "../../api/commands";
import { useT } from "../../i18n";

export default function DataTab({ studio }: { studio: Studio; gameId: GameId | null }) {
  const t = useT();
  return <p className="kst-p" data-testid="studio-data">{t("studio.data.soon", { count: studio.layerFiles })}</p>;
}
