# kiln.ui window size constraints

See [the overlay and managed UI guide](../ui.md) for the other window calls.

`KilnUiApi.set_next_window_size_constraints(min_w, min_h, max_w, max_h)` is appended
at the end of the native table, after `subscribe_key_ex`; the service version stays 7.
It forwards to Dear ImGui `SetNextWindowSizeConstraints` for the next `begin`, in
screen pixels. Call it inside a frame callback before `begin`, every frame that
needs constraints. Like the neighbouring window setters it does nothing outside
a drawing frame, including `KILN_UI_OVERLAY=0`.

Readers must check `header.size >= offsetof(KilnUiApi, set_next_window_size_constraints) +
sizeof(ui->set_next_window_size_constraints)` and that the pointer is non-null.
Do not require the size of today's entire table. Existing offsets stay unchanged: on x64 the previous table was 888 bytes, the new member is at byte offset 888, and the extended table is 896 bytes (`subscribe_key_ex` remains at 880).
The managed internal mirror appends `TableConstraints` after the frozen `Table7`
and checks the member end before reading the pointer. Window setters are internal
to `Kiln.Ui`, so this adds no public C# API or tick-thread analyzer entry.

With `KILN_UI_WINDOW_ALWAYS_AUTO_RESIZE`, equal minimum and maximum width fixes
the width while height follows content between `min_h` and `max_h`. EMF windows
use `(width, 0, width, frame.height * 0.85f)`: later rows grow the window and content
past the cap scrolls. Older overlays retain the previous appearing-only size call.

## Kiln's line in the main menu's footer (0.2.0-dev.29.11)

`KilnUiApi.menu_footer(int32_t mods, uint32_t flags)` is appended after `set_next_window_size_constraints`; the service
version stays 7. On x64 it is at byte offset 896 and the table is 904 bytes. Readers check `header.size >=
offsetof(KilnUiApi, menu_footer) + sizeof(ui->menu_footer)` and that the pointer is non-null; the managed mirror is
`UiNative.TableFooter`.

Kiln.Ui calls it with the count of mods loaded this start (< 0: not known yet, no line) and `KILN_UI_FOOTER_SHOWN`
unless the player hid the line (setting `mainMenuFooter`). kiln.ui then draws "Kiln &lt;version&gt; - &lt;N&gt; Mods
Loaded" above the game's build number, in place of XHL-Core's footer line ([the guide](../ui.md#kilns-line-in-the-main-menus-footer-since-020-dev2911)).
Any thread; the next UI frame follows it. A native mod has no reason to call it.
