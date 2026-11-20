/* SPDX-License-Identifier: MIT
 * kiln.ui.blocks v1 (native/services/ui, client only, 0.2.0-dev.31): the drawing calls the richer UI blocks need on top
 * of kiln.ui's (dossier 09 §5.1). A table of its own because kiln.ui does not grow while dev.30 is open: the chapter's
 * "kiln.ui v6" slots live here. Every call is made from inside an overlay frame callback (the render thread), like
 * kiln.ui's drawing slots, and does nothing (returns false / 0) outside one.
 *
 *   begin_tab_item_ex   kiln.ui's begin_tab_item with KUI_TAB_SELECT: make this tab the selected one this frame
 *                       (ImGuiTabItemFlags_SetSelected, mapped by name). Pair a true return with kiln.ui's end_tab_item.
 *   set_next_item_open  the next tree node's open state; condition KUI_COND_ALWAYS (0) only (ImGuiCond_Always).
 *   item_state          KUI_ITEM_* of the item drawn last: active, deactivated this frame, deactivated after an edit,
 *                       edited this frame. Lets a text field keep a draft and commit it on Enter or when it loses focus.
 *   input_text_ex       a text field over a NUL-terminated UTF-8 buffer of capacity bytes (NUL included) with Kiln's own
 *                       flags (KUI_INPUT_*), width/height in pixels (0: the default). Returns true when the text changed,
 *                       or with KUI_INPUT_ENTER_RETURNS only when Enter was pressed. Multiline ignores ENTER_RETURNS
 *                       (Enter is a new line there).
 *
 * Readers check header.size against the end of each slot they call; the table is append-only. */
#ifndef KILN_UI_BLOCKS_H
#define KILN_UI_BLOCKS_H
#include "../kiln_api.h"
#ifdef __cplusplus
extern "C" {
#endif

#define KILN_UI_BLOCKS_SERVICE "kiln.ui.blocks"
#define KILN_UI_BLOCKS_VERSION 1u

/* begin_tab_item_ex flags */
enum { KUI_TAB_SELECT = 1u };
/* set_next_item_open conditions */
enum { KUI_COND_ALWAYS = 0u };
/* item_state bits */
enum { KUI_ITEM_ACTIVE = 1u, KUI_ITEM_DEACTIVATED = 2u, KUI_ITEM_DEACTIVATED_AFTER_EDIT = 4u, KUI_ITEM_EDITED = 8u };
/* input_text_ex flags */
enum { KUI_INPUT_ENTER_RETURNS = 1u, KUI_INPUT_READ_ONLY = 2u, KUI_INPUT_MULTILINE = 4u };

typedef struct KilnUiBlocksApi {
    KilnServiceHeader header; /* KILN_UI_BLOCKS_VERSION; append-only */
    bool(KILN_CALL* begin_tab_item_ex)(KilnStr label, uint32_t flags);
    void(KILN_CALL* set_next_item_open)(bool open, uint32_t condition);
    uint32_t(KILN_CALL* item_state)(void);
    bool(KILN_CALL* input_text_ex)(KilnStr label, char* buffer, size_t capacity, uint32_t flags, float width, float height);
} KilnUiBlocksApi;

#ifdef __cplusplus
}
#endif
#endif
