// SPDX-License-Identifier: MIT
// Issue 44, phase 1: shell-like history without a game client or an installed native chat hook.
using System.Text;
using Xunit;

namespace Kiln.ChatHistoryTests;

public sealed class ChatHistoryTests : IDisposable
{
    private readonly string _directory = Path.Combine(Path.GetTempPath(), "kiln-chat-history-" + Guid.NewGuid().ToString("N"));
    private string Pathname => Path.Combine(_directory, "kiln", "user", "chat-history.json");

    public void Dispose()
    {
        try { if (Directory.Exists(_directory)) Directory.Delete(_directory, true); }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }

    [Fact]
    public void Up_and_down_navigate_and_restore_the_unsent_draft()
    {
        var h = new ChatHistory(Pathname);
        h.Submit("/help");
        h.Submit("/tp home");
        h.Submit("/time");

        Assert.Equal("/time", h.Older("/give me something"));
        Assert.Equal("/tp home", h.Older("/time"));
        Assert.Equal("/help", h.Older("/tp home"));
        Assert.Equal("/help", h.Older("/help")); // bounded at the oldest
        Assert.Equal("/tp home", h.Newer());
        Assert.Equal("/time", h.Newer());
        Assert.Equal("/give me something", h.Newer());
        Assert.Equal("/give me something", h.Newer()); // no more history browsing
    }

    [Fact]
    public void Manual_edit_starts_a_fresh_recall_with_the_new_draft()
    {
        var h = new ChatHistory(Pathname);
        h.Submit("/a");
        h.Submit("/b");
        Assert.Equal("/b", h.Older("/draft"));
        h.Edited();
        Assert.Equal("/b", h.Older("/different"));
        Assert.Equal("/different", h.Newer());
    }

    [Fact]
    public void Constrains_capacity_and_collapses_consecutive_duplicates()
    {
        var h = new ChatHistory(Pathname, limit: 2);
        h.Submit("/one");
        h.Submit("/one");
        Assert.Equal(1, h.Count);
        h.Submit("/two");
        h.Submit("/three");
        Assert.Equal<string>(["/two", "/three"], h.Lines);
        Assert.Equal<string>(["/two", "/three"], new ChatHistory(Pathname, limit: 2).Lines);
    }

    [Fact]
    public void Commands_are_persisted_but_chat_is_session_only_by_default()
    {
        var h = new ChatHistory(Pathname);
        h.Submit("hello friends");
        h.Submit("/help", serverIdentity: "example.org:15637");
        h.Submit("/time", serverIdentity: "example.org:15637");
        Assert.Equal(3, h.Count);

        var saved = File.ReadAllText(Pathname);
        Assert.DoesNotContain("hello friends", saved);
        Assert.DoesNotContain("example.org", saved); // never store raw address or server name
        Assert.Contains("/help", saved);

        var nextSession = new ChatHistory(Pathname);
        Assert.Equal<string>(["/help", "/time"], nextSession.Lines);
        Assert.Equal("/time", nextSession.Older(""));
    }

    [Fact]
    public void Chat_persistence_must_be_explicitly_enabled()
    {
        var h = new ChatHistory(Pathname, persistChat: true);
        h.Submit("ordinary conversation");
        Assert.Contains("ordinary conversation", File.ReadAllText(Pathname));
        Assert.Equal<string>(["ordinary conversation"], new ChatHistory(Pathname, persistChat: true).Lines);
        Assert.Empty(new ChatHistory(Pathname).Lines); // preference off
    }

    [Theory]
    [InlineData("/login password123")]
    [InlineData("/register hunter2")]
    [InlineData("/auth token abcd")]
    [InlineData("/key show")]
    [InlineData("/set secret value")]
    [InlineData("/PASS word")]
    public void Sensitive_command_names_and_arguments_are_never_recorded(string line)
    {
        var h = new ChatHistory(Pathname);
        h.Submit(line);
        Assert.Empty(h.Lines);
        Assert.False(File.Exists(Pathname));
    }

    [Fact]
    public void Commands_flagged_by_the_server_are_not_recorded()
    {
        var h = new ChatHistory(Pathname);
        h.Submit("/customcredential swordfish", serverSensitive: true);
        h.Submit("/help");
        Assert.Equal<string>(["/help"], h.Lines);
        Assert.DoesNotContain("swordfish", File.ReadAllText(Pathname));
    }

    [Fact]
    public void Invalid_or_oversize_input_never_enters_history()
    {
        var h = new ChatHistory(Pathname);
        h.Submit("");
        h.Submit(" ");
        h.Submit("line\nother");
        h.Submit("line\0other");
        h.Submit("/" + new string('x', 1024));
        h.Submit("/" + string.Concat(Enumerable.Repeat("雪", 342))); // UTF-8 > 1024 bytes
        Assert.Empty(h.Lines);
        Assert.False(File.Exists(Pathname));
    }

    [Fact]
    public void Search_is_recent_first_case_insensitive_and_can_cycle()
    {
        var h = new ChatHistory(Pathname);
        h.Submit("/give wood");
        h.Submit("/help");
        h.Submit("/give stone");
        Assert.Equal("/give stone", h.SearchBack("GIVE"));
        Assert.Equal("/give wood", h.SearchBack("give", skip: 1));
        Assert.Null(h.SearchBack("give", skip: 2));
        Assert.Null(h.SearchBack(""));
    }

    [Fact]
    public void Bad_or_untrusted_history_json_cannot_break_loading_or_reintroduce_secrets()
    {
        Directory.CreateDirectory(System.IO.Path.GetDirectoryName(Pathname)!);
        File.WriteAllText(Pathname, "not-json");
        Assert.Empty(new ChatHistory(Pathname).Lines);

        File.WriteAllText(Pathname, """
            {"version":1,"entries":[
              {"text":"/help","server":"plaintext-machine"},
              {"text":"/login root hunter2"},
              {"text":"hello"},
              {"text":"/time","server":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"}
            ]}
            """);
        var h = new ChatHistory(Pathname);
        Assert.Equal<string>(["/help", "/time"], h.Lines);
        h.Clear();
        Assert.Empty(new ChatHistory(Pathname).Lines);
        Assert.DoesNotContain("hunter2", File.ReadAllText(Pathname));
    }

    [Fact]
    public void Unwritable_storage_must_leave_in_memory_recall_working()
    {
        Directory.CreateDirectory(Pathname); // a directory exists where a JSON file should be
        var h = new ChatHistory(Pathname);
        h.Submit("/help"); // Save cannot replace a directory, and may not throw into the game
        Assert.Equal("/help", h.Older("draft"));
        Assert.Equal("draft", h.Newer());
    }
}
