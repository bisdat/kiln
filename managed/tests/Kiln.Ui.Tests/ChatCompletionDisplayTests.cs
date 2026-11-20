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

    [Fact]
    public void TabAndArrowsCycleMultipleCandidatesWithoutForgettingOriginalPrefix()
    {
        var candidates = new[] { "help", "heal", "hello" };
        const string prefix = "/";
        Assert.False(ChatCompletionDisplayRules.AlreadyComplete("/he", prefix, candidates));
        var first = ChatCompletionDisplayRules.Next(candidates.Length, 0, false, false);
        Assert.Equal("/help", prefix + candidates[first]);
        var second = ChatCompletionDisplayRules.Next(candidates.Length, first, true, false);
        Assert.Equal("/heal", prefix + candidates[second]);
        var third = ChatCompletionDisplayRules.Next(candidates.Length, second, true, false);
        Assert.Equal("/hello", prefix + candidates[third]);
        Assert.Equal(first, ChatCompletionDisplayRules.Next(candidates.Length, third, true, false));
        Assert.Equal(third, ChatCompletionDisplayRules.Next(candidates.Length, first, true, true));
        // A history-recalled /help, in contrast, is a fresh reply and must
        // never open its own popup even when other candidates are present.
        Assert.True(ChatCompletionDisplayRules.AlreadyComplete("/help", prefix, candidates));
        Assert.True(ChatCompletionDisplayRules.AlreadyComplete("/HELP", prefix, candidates));
    }

    [Fact]
    public void SingleOrEmptyCandidateDoesNotCycle()
    {
        Assert.Equal(0, ChatCompletionDisplayRules.Next(1, 0, false, false));
        Assert.Equal(0, ChatCompletionDisplayRules.Next(1, 0, true, false));
        Assert.Equal(-1, ChatCompletionDisplayRules.Next(0, 0, false, false));
    }

    [Theory]
    [InlineData(0, 66f)]
    [InlineData(1, 106f)]
    [InlineData(10, 466f)]
    [InlineData(20, 466f)]
    public void PopupHeightAllowsHintAndLargeRows(int rows, float height)
    {
        Assert.Equal(height, ChatCompletionDisplayRules.PopupHeight(1f, rows));
        Assert.Equal(height * 1.25f, ChatCompletionDisplayRules.PopupHeight(1.25f, rows));
    }
}
