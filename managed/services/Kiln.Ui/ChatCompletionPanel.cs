// SPDX-License-Identifier: MIT
// Issue #44 phase 1b: a passive suggestion list over Enshrouded's original
// chat edit. Only value snapshots cross from the native UI thread to this
// overlay render thread; EditSetText always happens on the native UI thread.
using System.Text;

namespace Kiln;

internal static unsafe class ChatCompletionPanel
{
    private const int MaxVisible = 10;
    private const long DebounceMs = 90;
    private static string _line = "";
    private static string _requestText = "", _prefix = "";
    private static string? _expectedEdit;
    private static string[] _suggestions = [];
    private static uint _requestId;
    private static int _selected;
    private static long _changedAt, _lastAttempt;
    private static bool _active, _dismissed;
    private static uint _field;

    private static void Reset()
    {
        _line = _requestText = _prefix = "";
        _expectedEdit = null;
        _suggestions = [];
        _requestId = 0;
        _selected = 0;
        _active = _dismissed = false;
        _field = 0;
        UiNative.ChatCandidates(false, false);
        UiNative.ChatHover(false);
    }

    private static void Changed(string text, uint field, long now)
    {
        _line = text;
        _field = field;
        _requestText = _prefix = "";
        _expectedEdit = null;
        _requestId = 0;
        _suggestions = [];
        _selected = 0;
        _active = _dismissed = false;
        _changedAt = now;
        UiNative.ChatCandidates(false, false);
    }

    private static bool ValidCursor(string text, uint cursor)
    {
        uint count = 0;
        foreach (var rune in text.EnumerateRunes()) count++;
        return cursor == count;
    }

    private static bool TryPrefix(ChatCompletionReply reply, out string prefix)
    {
        prefix = "";
        if (!ChatCompletionRules.Matches(reply, reply.Id, reply.Text)) return false;
        var bytes = Encoding.UTF8.GetBytes(reply.Text);
        var n = (int)reply.Start;
        if (n > bytes.Length || (n < bytes.Length && n > 0 && (bytes[n] & 0xC0) == 0x80))
            return false;
        prefix = Encoding.UTF8.GetString(bytes, 0, n);
        return true;
    }

    private static bool Accept(in UiNative.ChatCompletion input, string text, int index)
    {
        if (index < 0 || index >= _suggestions.Length) return false;
        var replacement = _prefix + _suggestions[index];
        if (Encoding.UTF8.GetByteCount(replacement) > 256 || replacement == text) return false;
        if (!UiNative.ChatReplace(input, text, replacement)) return false;
        _expectedEdit = replacement;   // preserve candidate list through our own EditSetText
        _selected = index;
        return true;
    }

    internal static void Draw(UiDraw ui)
    {
        UiNative.ChatHover(false); // clear last frame's mouse capture
        // A completely unsupported ABI or a closed/nonfocused game chat
        // leaves the original native chat and PR #63 history untouched.
        var result = UiNative.ChatInput();
        if (result is not { } input)
        {
            if (_line.Length != 0 || _suggestions.Length != 0) Reset();
            return;
        }

        var text = UiNative.ChatText(input);
        if (text.Length < 2 || text[0] != '/' || !ValidCursor(text, input.Cursor) ||
            input.Width <= 0 || input.Height <= 0)
        {
            Reset();
            return;
        }

        var now = Environment.TickCount64;
        if (_field != input.Field || text != _line)
        {
            if (_field == input.Field && _expectedEdit == text)
            {
                _line = text;
                _expectedEdit = null;
                // Preserve the original candidates while Tab cycles through
                // completions, instead of asking for a new prefix every frame.
            }
            else Changed(text, input.Field, now);
        }

        ChatCompletionNetwork.Start();
        if (!_active && _suggestions.Length == 0 && _requestId == 0 &&
            now - _changedAt >= DebounceMs && now - _lastAttempt >= DebounceMs &&
            ChatCompletionRules.RequestText(text))
        {
            _lastAttempt = now;
            _requestId = ChatCompletionNetwork.Ask(text);
            if (_requestId != 0) _requestText = text;
        }

        if (ChatCompletionNetwork.TakeReply(out var reply) &&
            _requestId != 0 &&
            ChatCompletionRules.Matches(reply, _requestId, _requestText) &&
            reply.Text == _line &&
            TryPrefix(reply, out var prefix))
        {
            _prefix = prefix;
            _suggestions = reply.Suggestions;
            _selected = 0;
            _requestId = 0;
            _active = false;
        }

        // These actions were captured from the game's input before reaching
        // this render frame. No key is acted on without live suggestions.
        if (input.Action == 7)
        {
            _active = false;
            _dismissed = true;
        }
        else if (_suggestions.Length > 0 && !_dismissed)
        {
            switch (input.Action)
            {
                case 3: // Tab: first suggestion, then cycle forwards
                    _selected = _active ? (_selected + 1) % _suggestions.Length : 0;
                    _active = true;
                    Accept(input, text, _selected);
                    break;
                case 4: // Shift+Tab: same, but backwards
                    _selected = _active ? (_selected + _suggestions.Length - 1) % _suggestions.Length
                        : _suggestions.Length - 1;
                    _active = true;
                    Accept(input, text, _selected);
                    break;
                case 5: // Up: previous suggestion, not command history
                    if (_active)
                    {
                        _selected = (_selected + _suggestions.Length - 1) % _suggestions.Length;
                        Accept(input, text, _selected);
                    }
                    break;
                case 6: // Down: next suggestion
                    if (_active)
                    {
                        _selected = (_selected + 1) % _suggestions.Length;
                        Accept(input, text, _selected);
                    }
                    break;
            }
        }

        UiNative.ChatCandidates(_suggestions.Length > 0 && !_dismissed, _active && !_dismissed);
        if (_suggestions.Length == 0 || _dismissed) return;

        // Use the player's current Kiln look, including game-style themes.
        // NoFocusOnAppearing | NoNavInputs | NoNavFocus leaves the game's chat
        // widget in control of keyboard typing and Enter/Escape.
        const uint flags = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) |
                           (1u << 6) | (1u << 8) | (1u << 12) |
                           (1u << 18) | (1u << 19);
        var width = MathF.Min(MathF.Max(input.Width, 260 * ui.Scale), MathF.Min(520 * ui.Scale, ui.Width - 8));
        var count = Math.Min(MaxVisible, _suggestions.Length);
        var height = (count + 1) * 24f * ui.Scale + 16f * ui.Scale;
        var x = Math.Clamp(input.X, 4, Math.Max(4, ui.Width - width - 4));
        var y = input.Y >= height + 12
            ? input.Y - height - 5
            : Math.Min(ui.Height - height - 4, input.Y + input.Height + 5);
        y = Math.Max(4, y);

        var hovered = false;
        ui.PushStyleRaw(UiStyle.Player);
        try
        {
            ui.NextWindowPos(x, y, 1);
            ui.NextWindowSize(width, height, 1);
            if (ui.BeginWindowNoClose(KilnText.T("chat.completion.title") + "###kiln.chat.complete", flags))
            {
                ui.TextDisabled(KilnText.T("chat.completion.hint"));
                // Show at most ten rows at once, but keep the other permitted
                // suggestions reachable by mouse-wheel scrolling.
                if (ui.BeginChild("##kiln.chat.completion.rows", 0, count * 24f * ui.Scale))
                {
                    for (var i = 0; i < _suggestions.Length; ++i)
                    {
                        if (ui.Selectable(_suggestions[i] + "##chat-candidate-" + i, _active && i == _selected))
                        {
                            _active = true;
                            Accept(input, text, i);
                        }
                        if (ui.ItemHovered) hovered = true;
                    }
                }
                ui.EndChild();
            }
            ui.EndWindow();
        }
        finally { UiNative.ChatHover(hovered); ui.PopStyleRaw(); }
    }
}
