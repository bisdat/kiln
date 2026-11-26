// SPDX-License-Identifier: MIT
// kiln.ui's window_state slot (GitHub issue 149): a window as Dear ImGui has it now, for Kiln.Ui's window docking
// (managed/services/Kiln.Ui/UiDock.cs). Included by draw_slots.h inside its anonymous namespace (the service table and
// the preview shim share it). Frame callbacks only, like every drawing slot. Read only: it never makes a window.

// One of the window's resize grips or borders is the active id: the player holds it (ImGui keeps the id active while
// the button is held, also on frames where the pointer does not move).
bool window_resizing(ImGuiWindow* w) {
    const ImGuiID active = ImGui::GetActiveID();
    if (active == 0) return false;
    for (int n = 0; n < 4; ++n)
        if (active == ImGui::GetWindowResizeCornerID(w, n)) return true;
    for (int d = 0; d < 4; ++d)
        if (active == ImGui::GetWindowResizeBorderID(w, static_cast<ImGuiDir>(d))) return true;
    return false;
}

bool KILN_CALL t_window_state(KilnStr name, KilnUiWindowState* out) {
    if (!out || out->size < sizeof(KilnUiWindowState)) return false;
    KilnUiWindowState s{};
    s.size = out->size;
    bool found = false;
    if (kui::in_frame()) {
        ImGuiContext& g = *ImGui::GetCurrentContext();
        ImGuiWindow* w = nullptr;
        if (name.len == 0) w = g.CurrentWindow ? g.CurrentWindow->RootWindow : nullptr;
        else if (name.ptr && !std::memchr(name.ptr, 0, name.len))
            w = ImGui::FindWindowByID(ImHashStr(name.ptr, name.len));  // FindWindowByName, with the length ("###" resets the hash)
        if (w) {
            found = true;
            s.flags = KILN_UI_WINDOW_STATE_EXISTS;
            if (g.MovingWindow && g.MovingWindow->RootWindow == w) s.flags |= KILN_UI_WINDOW_STATE_MOVING;
            if (window_resizing(w)) s.flags |= KILN_UI_WINDOW_STATE_RESIZING;
            s.x = w->Pos.x;
            s.y = w->Pos.y;
            s.width = w->Size.x;
            s.height = w->Size.y;
        }
    }
    std::memcpy(out, &s, sizeof s);  // a larger record (a newer caller) keeps its tail
    return found;
}
