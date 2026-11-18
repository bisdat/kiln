// What each shell mounts once for the Discord features: the live feed from the backend, the facts Rich Presence needs
// from the window, and the dialogs any page can open (send a report, sign in, what is sent and kept).
import { PresenceFacts } from "./Presence";
import { PrivacyDialog } from "./Privacy";
import { SignInDialog } from "./SignIn";
import { useCommunityFeed, useCommunityUi, useShown } from "./state";

export function CommunityDesktopHost() {
  useCommunityFeed();
  const shown = useShown();
  const signIn = useCommunityUi((s) => s.signIn);
  const privacy = useCommunityUi((s) => s.privacy);
  if (!shown) return null;
  return (
    <>
      <PresenceFacts />
      {signIn && <SignInDialog resumeSend={signIn.resumeSend} />}
      {privacy && <PrivacyDialog />}
    </>
  );
}

export function CommunityDeckHost() {
  useCommunityFeed();
  const shown = useShown();
  if (!shown) return null;
  return (
    <>
      <PresenceFacts />
    </>
  );
}
