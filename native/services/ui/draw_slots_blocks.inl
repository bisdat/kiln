// SPDX-License-Identifier: MIT
// kiln.ui.blocks v1 drawing slots (kiln_ui_blocks.h): controlled tabs and trees, the last item's edit state and a text
// field with Kiln's own flags, for the richer UI blocks (dossier 09). Included by draw_slots.h inside its namespace;
// plain Dear ImGui calls. Kiln's flag values are mapped to ImGui's by name, never passed through.

bool KILN_CALL t_begin_tab_item_ex(KilnStr l, uint32_t flags) {
    IN_FRAME(false);
    return ImGui::BeginTabItem(z(l), nullptr, (flags & KUI_TAB_SELECT) ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None);
}
void KILN_CALL t_set_next_item_open(bool open, uint32_t condition) {
    IN_FRAME();
    if (condition != KUI_COND_ALWAYS) return;  // v1: Always only
    ImGui::SetNextItemOpen(open, ImGuiCond_Always);
}
uint32_t KILN_CALL t_item_state() {
    IN_FRAME(0u);
    uint32_t s = 0;
    if (ImGui::IsItemActive()) s |= KUI_ITEM_ACTIVE;
    if (ImGui::IsItemDeactivated()) s |= KUI_ITEM_DEACTIVATED;
    if (ImGui::IsItemDeactivatedAfterEdit()) s |= KUI_ITEM_DEACTIVATED_AFTER_EDIT;
    if (ImGui::IsItemEdited()) s |= KUI_ITEM_EDITED;
    return s;
}
bool KILN_CALL t_input_text_ex(KilnStr l, char* buf, size_t cap, uint32_t flags, float w, float h) {
    IN_FRAME(false);
    if (!buf || !cap) return false;
    ImGuiInputTextFlags f = ImGuiInputTextFlags_None;
    if (flags & KUI_INPUT_READ_ONLY) f |= ImGuiInputTextFlags_ReadOnly;
    if (flags & KUI_INPUT_MULTILINE) return ImGui::InputTextMultiline(z(l), buf, cap, ImVec2(w, h), f);
    if (flags & KUI_INPUT_ENTER_RETURNS) f |= ImGuiInputTextFlags_EnterReturnsTrue;
    if (w > 0.0f) ImGui::SetNextItemWidth(w);
    return ImGui::InputText(z(l), buf, cap, f);
}
