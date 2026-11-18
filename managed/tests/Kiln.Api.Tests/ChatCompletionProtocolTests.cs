// SPDX-License-Identifier: MIT
// Phase 1b's pure request/reply bounds, character safety and stale-response rules.
using Xunit;

namespace Kiln.Api.Tests;

public sealed class ChatCompletionProtocolTests
{
    [Theory]
    [InlineData("/he")]
    [InlineData("/zone create ")]
    [InlineData("/say café")]
    public void Valid_command_prefixes(string line) =>
        Assert.True(ChatCompletionRules.RequestText(line));

    [Theory]
    [InlineData("")]
    [InlineData("hello")]
    [InlineData("/")]
    [InlineData("/echo\nmalformed")]
    [InlineData("/echo\ttab")]
    public void Invalid_or_plain_text_is_rejected(string line) =>
        Assert.False(ChatCompletionRules.RequestText(line));

    [Fact]
    public void Utf8_limit_is_in_bytes_not_characters()
    {
        Assert.True(ChatCompletionRules.RequestText("/" + new string('a', 255)));
        Assert.False(ChatCompletionRules.RequestText("/" + new string('a', 256)));
        Assert.True(ChatCompletionRules.RequestText("/" + new string('é', 127)));
        Assert.False(ChatCompletionRules.RequestText("/" + new string('é', 128)));
    }

    [Theory]
    [InlineData("help", true)]
    [InlineData("@a", true)]
    [InlineData("", false)]
    [InlineData("abc\ndef", false)]
    public void Candidate_bounds_and_control_characters(string value, bool ok) =>
        Assert.Equal(ok, ChatCompletionRules.Candidate(value));

    [Fact]
    public void A_response_must_match_the_exact_active_request_and_text()
    {
        var reply = new ChatCompletionReply(12, "/hea", 1, ["heal", "health"]);
        Assert.True(ChatCompletionRules.Matches(reply, 12, "/hea"));
        Assert.False(ChatCompletionRules.Matches(reply, 11, "/hea"));
        Assert.False(ChatCompletionRules.Matches(reply, 12, "/heal"));
        Assert.False(ChatCompletionRules.Matches(reply with { Start = 100 }, 12, "/hea"));
        Assert.False(ChatCompletionRules.Matches(reply with { Suggestions = ["\\ud800"] }, 12, "/hea"));
        Assert.False(ChatCompletionRules.Matches(reply with { Suggestions = ["bad\tword"] }, 12, "/hea"));
        Assert.False(ChatCompletionRules.Matches(reply with { Suggestions = Enumerable.Repeat("x", 21).ToArray() }, 12, "/hea"));
    }

    [Fact]
    public void Unicode_offsets_are_measured_in_utf8_bytes()
    {
        var text = "/say café";
        // A position in the middle of the final Unicode character cannot be
        // used as an unvalidated replacement boundary.
        var utf8Length = System.Text.Encoding.UTF8.GetByteCount(text);
        Assert.Equal(10, utf8Length);
        Assert.False(ChatCompletionRules.Matches(
            new ChatCompletionReply(1, text, (ushort)(utf8Length - 1), ["ok"]), 1, text));
        Assert.False(ChatCompletionRules.Matches(
            new ChatCompletionReply(1, text, (ushort)(utf8Length + 1), ["ok"]), 1, text));
    }
    [Fact]
    public void Unpaired_surrogates_are_never_accepted_as_wire_or_native_text()
    {
        Assert.False(ChatCompletionRules.RequestText("/bad" + (char)0xD800));
        Assert.False(ChatCompletionRules.RequestText("/bad" + (char)0xDC00));
        Assert.False(ChatCompletionRules.Candidate("bad" + (char)0xD800));
        Assert.False(ChatCompletionRules.Candidate("bad" + (char)0xDC00));
        Assert.True(ChatCompletionRules.RequestText("/say 😀"));
    }


}
