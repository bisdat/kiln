// /servers/:id/console (Console section, its first page): the server's console at full size, with the Kiln command line.
import { useParams } from "react-router-dom";
import { ServerConsole } from "./ServerConsole";

export default function ServerConsolePage() {
  const { id = "" } = useParams();
  return <div className="k-sconsole-page"><ServerConsole serverId={id} /></div>;
}
