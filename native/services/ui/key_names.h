// SPDX-License-Identifier: MIT
// kiln.ui: the names of keys and which key a binding means (GitHub issue 33). Plain code without windows.h: the host test
// (tests/key_names_test.cpp) runs it, and Kiln.Ui's KeyNames (managed/services/Kiln.Ui/KeyBindings.cs) is the same table
// in C#; managed/tests/Kiln.Ui.Tests/KeyNameTests.cs reads this file and fails when the two differ. Keep the entries one
// per brace pair, `{0xNN, "Name"}` and `{0xSS, 0xVV}`: that test reads them as text.
//
// Names. Every key code a binding can hold has a name that Kiln.Ui's KeyChord.TryParse reads back: the table below, then
// F1..F24, 0..9, A..Z, Numpad0..Numpad9, and "Key<code>" (decimal) for a code with no name. keybinds.json stores names.
//
// Physical keys. The punctuation names (Backquote, Minus, ..., IntlBackslash) name the key at that place of a US
// keyboard, not a virtual-key code: other layouts give those keys other codes (the key left of 1 sends VK_OEM_3 on a US
// layout, VK_OEM_8 on a UK one, VK_OEM_5 on a German one, VK_OEM_7 on a French one; its scan code is 0x29 on all of them).
// A key press that carries a scan code of one of those places and a punctuation (OEM) virtual key is that place's key;
// any other press is its virtual key. A press without a scan code (0: an injected key) is its virtual key, so the US
// codes still work there. Letters, digits and every other key stay virtual keys: "K" is the key that types K.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace kui::keys {

struct Name {
    uint32_t vk;
    const char* name;
};

// Names of codes the ranges in name() do not cover. Codes 0x100..0x103 are the wheel (kiln_ui.h KILN_UI_VK_WHEEL_*).
inline constexpr Name kNames[] = {
    {0x03, "Break"}, {0x04, "MouseMiddle"}, {0x05, "Mouse4"}, {0x06, "Mouse5"}, {0x08, "Backspace"}, {0x09, "Tab"},
    {0x0C, "Clear"}, {0x0D, "Enter"}, {0x10, "Shift"}, {0x11, "Ctrl"}, {0x12, "Alt"}, {0x13, "Pause"}, {0x14, "CapsLock"},
    {0x15, "Kana"}, {0x16, "ImeOn"}, {0x17, "Junja"}, {0x18, "Final"}, {0x19, "Kanji"}, {0x1A, "ImeOff"},
    {0x1B, "Escape"}, {0x1C, "Convert"}, {0x1D, "NonConvert"}, {0x1E, "Accept"}, {0x1F, "ModeChange"}, {0x20, "Space"},
    {0x21, "PageUp"}, {0x22, "PageDown"}, {0x23, "End"}, {0x24, "Home"}, {0x25, "Left"}, {0x26, "Up"}, {0x27, "Right"},
    {0x28, "Down"}, {0x2C, "PrintScreen"}, {0x2D, "Insert"}, {0x2E, "Delete"}, {0x5B, "LeftWin"}, {0x5C, "RightWin"},
    {0x5D, "ContextMenu"}, {0x6A, "NumpadMultiply"}, {0x6B, "NumpadAdd"}, {0x6C, "NumpadSeparator"},
    {0x6D, "NumpadSubtract"}, {0x6E, "NumpadDecimal"}, {0x6F, "NumpadDivide"}, {0x90, "NumLock"}, {0x91, "ScrollLock"},
    {0x92, "OemNecEqual"}, {0x93, "OemFjMasshou"}, {0x94, "OemFjTouroku"}, {0x95, "OemFjLoya"}, {0x96, "OemFjRoya"},
    {0xA0, "LeftShift"}, {0xA1, "RightShift"}, {0xA2, "LeftCtrl"}, {0xA3, "RightCtrl"}, {0xA4, "LeftAlt"},
    {0xA5, "RightAlt"}, {0xA6, "BrowserBack"}, {0xA7, "BrowserForward"}, {0xA8, "BrowserRefresh"}, {0xA9, "BrowserStop"},
    {0xAA, "BrowserSearch"}, {0xAB, "BrowserFavorites"}, {0xAC, "BrowserHome"}, {0xAD, "VolumeMute"},
    {0xAE, "VolumeDown"}, {0xAF, "VolumeUp"}, {0xB0, "MediaNext"}, {0xB1, "MediaPrevious"}, {0xB2, "MediaStop"},
    {0xB3, "MediaPlayPause"}, {0xB4, "LaunchMail"}, {0xB5, "LaunchMedia"}, {0xB6, "LaunchApp1"}, {0xB7, "LaunchApp2"},
    {0xBA, "Semicolon"}, {0xBB, "Equals"}, {0xBC, "Comma"}, {0xBD, "Minus"}, {0xBE, "Period"}, {0xBF, "Slash"},
    {0xC0, "Backquote"}, {0xC1, "IntlRo"}, {0xC2, "NumpadComma"}, {0xDB, "LeftBracket"}, {0xDC, "Backslash"},
    {0xDD, "RightBracket"}, {0xDE, "Quote"}, {0xDF, "Oem8"}, {0xE1, "OemAx"}, {0xE2, "IntlBackslash"},
    {0xE3, "IcoHelp"}, {0xE4, "Ico00"}, {0xE6, "IcoClear"}, {0xE9, "OemReset"}, {0xEA, "OemJump"}, {0xEB, "OemPa1"},
    {0xEC, "OemPa2"}, {0xED, "OemPa3"}, {0xEE, "OemWsCtrl"}, {0xEF, "OemCuSel"}, {0xF0, "OemAttn"},
    {0xF1, "OemFinish"}, {0xF2, "OemCopy"}, {0xF3, "OemAuto"}, {0xF4, "OemEnlw"}, {0xF5, "OemBackTab"},
    {0xFE, "OemClear"}, {0x100, "WheelUp"}, {0x101, "WheelDown"}, {0x102, "WheelLeft"}, {0x103, "WheelRight"},
};

struct Place {
    uint32_t scan;  // set-1 make code as kiln.ui passes it (KilnUiKey.scan: 0x100 = E0 prefix; none of these has it)
    uint32_t vk;    // the code a US layout gives the key there, which is the binding's code
};

// The punctuation keys, by place. 0x56 is the ISO key between left Shift and Z; 0x73 the Brazilian /? and Japanese Ro
// key; 0x7E the Brazilian keypad's . key.
inline constexpr Place kPlaces[] = {
    {0x29, 0xC0}, {0x0C, 0xBD}, {0x0D, 0xBB}, {0x1A, 0xDB}, {0x1B, 0xDD}, {0x2B, 0xDC}, {0x27, 0xBA},
    {0x28, 0xDE}, {0x33, 0xBC}, {0x34, 0xBE}, {0x35, 0xBF}, {0x56, 0xE2}, {0x73, 0xC1}, {0x7E, 0xC2},
};

// A punctuation (OEM) virtual key: the codes layouts give the keys above, whatever their place.
inline constexpr bool oem(uint32_t vk) {
    return (vk >= 0xBA && vk <= 0xC2) || (vk >= 0xDB && vk <= 0xE4) || vk == 0xE6 || (vk >= 0xE9 && vk <= 0xF5) ||
           (vk >= 0x92 && vk <= 0x96) || vk == 0xFE;
}

// A binding on this code means a place (a punctuation name), not a virtual key.
inline constexpr bool is_place(uint32_t vk) {
    for (const auto& p : kPlaces)
        if (p.vk == vk) return true;
    return false;
}

// The binding code of a press: a punctuation key at a known place is that place's code, anything else its virtual key.
// The Keys page stores this for a pressed key.
inline constexpr uint32_t press_code(uint32_t vk, uint32_t scan) {
    if (scan == 0 || !oem(vk)) return vk;
    for (const auto& p : kPlaces)
        if (p.scan == scan) return p.vk;
    return vk;
}

// Does a binding on `bound` take a press of `vk` with `scan`? Its place's key, or (for a code that names no place, such
// as Key223 a dev.27 Keys page saved) the virtual key itself.
inline constexpr bool matches(uint32_t bound, uint32_t vk, uint32_t scan) {
    return bound == press_code(vk, scan) || (!is_place(bound) && bound == vk);
}

// The name of `vk` into out (always terminated).
inline void name(uint32_t vk, char* out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = 0;
    for (const auto& n : kNames)
        if (n.vk == vk) {
            std::snprintf(out, cap, "%s", n.name);
            return;
        }
    if (vk >= 0x70 && vk <= 0x87) std::snprintf(out, cap, "F%u", static_cast<unsigned>(vk - 0x70 + 1));
    else if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) std::snprintf(out, cap, "%c", static_cast<char>(vk));
    else if (vk >= 0x60 && vk <= 0x69) std::snprintf(out, cap, "Numpad%u", static_cast<unsigned>(vk - 0x60));
    else std::snprintf(out, cap, "Key%u", static_cast<unsigned>(vk));
}

}  // namespace kui::keys
