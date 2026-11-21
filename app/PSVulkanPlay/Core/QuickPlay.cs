namespace PSVulkanPlay.Core;

/// <summary>
/// Straight into a session, with no window of the app's: for a shortcut in the desktop's menu, a
/// launcher such as Steam, or a link. <c>ps-vulkanplay --play CONSOLE</c> (a name or a hardware
/// address) and <c>ps-vulkanplay://play/CONSOLE</c> both come here, and the session gets the
/// same arguments the app's Play button would give it, from the app's settings.
/// </summary>
public static class QuickPlay
{
    public const string Scheme = "ps-vulkanplay";

    /// <summary>
    /// The console the app's arguments ask to play on, or null when they ask for the app itself.
    /// <paramref name="error"/> is set when they ask for a session but say it wrongly.
    /// </summary>
    public static string? ConsoleAskedFor(IReadOnlyList<string> args, out string error)
    {
        error = "";
        if (args.Count == 0) return null;
        if (args[0] == "--play")
        {
            if (args.Count == 2 && args[1].Trim().Length > 0) return args[1].Trim();
            error = "--play takes one console: its name or its hardware address.";
            return null;
        }
        if (args.Count == 1 && args[0].StartsWith(Scheme + ":", StringComparison.OrdinalIgnoreCase))
        {
            string? console = FromLink(args[0]);
            if (console is null) error = $"The link {args[0]} does not name a console; it reads {Scheme}://play/CONSOLE.";
            return console;
        }
        return null;
    }

    /// <summary>The console of a link <c>ps-vulkanplay://play/NAME</c> (NAME percent-encoded), or null.</summary>
    public static string? FromLink(string link)
    {
        if (!Uri.TryCreate(link, UriKind.Absolute, out Uri? uri) || !uri.Scheme.Equals(Scheme, StringComparison.OrdinalIgnoreCase)) return null;
        if (!uri.Host.Equals("play", StringComparison.OrdinalIgnoreCase)) return null;
        string path = uri.AbsolutePath.Trim('/');
        if (path.Length == 0 || path.Contains('/')) return null;
        string console = Uri.UnescapeDataString(path).Trim();
        return console.Length == 0 || console.Any(char.IsControl) ? null : console;
    }

    /// <summary>The registered console a name or hardware address stands for.</summary>
    public static RegisteredConsole? Find(IReadOnlyList<RegisteredConsole> registered, string asked)
    {
        string hex = ConsoleJson.HexOnly(asked);
        return registered.FirstOrDefault(console => console.Name == asked)
            ?? registered.FirstOrDefault(console => console.Name.Equals(asked, StringComparison.OrdinalIgnoreCase))
            ?? (hex.Length == 12 && asked.Count(Uri.IsHexDigit) == 12 ? registered.FirstOrDefault(console => console.HostId == hex) : null);
    }

    /// <summary>Runs the session to its end and returns the program's exit code; what it says goes to <paramref name="said"/>.</summary>
    public static async Task<int> RunAsync(IProgramRunner runner, AppSettings settings, string asked, Action<string> said, CancellationToken cancel = default)
    {
        RunResult listed = await runner.RunAsync(["consoles", "--json"], cancel).ConfigureAwait(false);
        if (!listed.Ok)
        {
            said(listed.Reason);
            return listed.ExitCode == 0 ? 1 : listed.ExitCode;
        }
        RegisteredConsole? console = Find(ConsoleJson.Registered(listed.Output), asked);
        if (console is null)
        {
            said($"No console called {asked} is registered here. Open PS VulkanPlay to add it.");
            return 1;
        }
        // The address it was last seen at is the program's own default; the app's settings do the rest.
        IReadOnlyList<string> arguments = settings.PlayArguments(console.Name, "", console.Kind.Equals("PS4", StringComparison.OrdinalIgnoreCase));
        IRunningProgram session = runner.Start(arguments, said);
        using CancellationTokenRegistration stopping = cancel.Register(session.Stop);
        RunResult ended = await session.Ended.ConfigureAwait(false);
        return ended.ExitCode;
    }
}
