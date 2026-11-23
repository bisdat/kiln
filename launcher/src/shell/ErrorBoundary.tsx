// Route error boundary: a page that throws (or a lazy page that fails to load) shows this
// instead of blanking the whole launcher. The shell keys it by page and resets it when the path changes, so navigating
// away (or to another Settings section) recovers. `inline`: inside a page (a Settings section), without a Page of its own,
// so the page around it (Settings' list of sections) stays.
import { TriangleAlert } from "lucide-react";
import { Component, type ReactNode } from "react";
import { Button, EmptyState, Page } from "../ui";
import { t } from "../i18n";

interface Props {
  children: ReactNode;
  /** Inside a page: the notice alone, no Page around it. */
  inline?: boolean;
  /** The notice's title (default: "This page failed"). */
  title?: string;
  /** A new value starts over (the shell: the path, so /settings/<section> recovers though Settings is one page). */
  resetKey?: unknown;
}
interface State { error: Error | null }

export class RouteErrorBoundary extends Component<Props, State> {
  state: State = { error: null };
  static getDerivedStateFromError(error: Error): State {
    return { error };
  }
  componentDidCatch(error: Error) {
    console.warn("page failed:", error);
  }
  componentDidUpdate(prev: Props) {
    if (this.state.error && prev.resetKey !== this.props.resetKey) this.setState({ error: null });
  }
  render() {
    if (!this.state.error) return this.props.children;
    const notice = (
      <EmptyState icon={<TriangleAlert />} title={this.props.title ?? t("shell.errorBoundary.title")} hint={<span className="k-mono" style={{ fontSize: 12 }}>{this.state.error.message}</span>}
        action={<Button onClick={() => this.setState({ error: null })}>{t("shell.errorBoundary.tryAgain")}</Button>} />
    );
    return this.props.inline ? <div role="alert" data-testid="section-failed">{notice}</div> : <Page>{notice}</Page>;
  }
}
