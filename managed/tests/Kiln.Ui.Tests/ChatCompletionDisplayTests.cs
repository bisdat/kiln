// SPDX-License-Identifier: MIT
// Phase 1b: avoid a self-completion popup for full commands and recalled history.
using Xunit;

namespace Kiln.UiTests;

public sealed class ChatCompletionDisplayTests
{
    [Fact]
    public void PartialCommandStillOffersCompletions()
    {
        Assert.False(ChatCompletionDisplayRules.AlreadyComplete("/he", "/", ["help"]));
    }

    [Fact]
    public void CompleteCommandDoesNotSuggestItself()
    {
        Assert.True(ChatCompletionDisplayRules.AlreadyComplete("/help", "/", ["help"]));
        Assert.True(ChatCompletionDisplayRules.AlreadyComplete("/HELP", "/", ["HELP"]));
    }

    [Fact]
    public void ExactCompletionTakesPrecedenceOverOtherCandidates()
    {
        Assert.True(ChatCompletionDisplayRules.AlreadyComplete("/help", "/", ["helper", "help"]));
        Assert.False(ChatCompletionDisplayRules.AlreadyComplete("/hel", "/", ["help", "helper"]));
    }

    [Fact]
    public void FullArgumentsAreNotMistakenForExactSuggestions()
    {
        Assert.False(ChatCompletionDisplayRules.AlreadyComplete("/time set n", "/time set ", ["night"]));
        Assert.True(ChatCompletionDisplayRules.AlreadyComplete("/time set night", "/time set ", ["night"]));
    }

    [Theory]
    [InlineData(0, 54f)]
    [InlineData(1, 94f)]
    [InlineData(10, 414f)]
    [InlineData(20, 414f)]
    public void PopupHeightAllowsHintAndLargeRows(int rows, float height)
    {
        Assert.Equal(height, ChatCompletionDisplayRules.PopupHeight(1f, rows));
        Assert.Equal(height * 1.25f, ChatCompletionDisplayRules.PopupHeight(1.25f, rows));
    }
}
