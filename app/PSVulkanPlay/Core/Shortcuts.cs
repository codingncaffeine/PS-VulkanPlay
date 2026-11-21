using System.Text;

namespace PSVulkanPlay.Core;

/// <summary>
/// A console's own entry in the desktop's application menu (a desktop entry in the user's
/// applications folder): it starts a session with that console straight away, with the app's
/// settings (<see cref="QuickPlay"/>). Desktops, launchers and Steam's "add a non-Steam game" list
/// read the same folder.
/// </summary>
public sealed class Shortcuts(string folder, string program)
{
    public const string AppId = "io.github.codingncaffeine.PSVulkanPlay";

    /// <summary>The folder the entries go in.</summary>
    public string Folder { get; } = folder;

    /// <summary>What the entries run: the stream program, by name when the search path finds this one, else by its full path.</summary>
    public string Program { get; } = program;

    /// <summary>$XDG_DATA_HOME/applications, or ~/.local/share/applications.</summary>
    public static string DefaultFolder()
    {
        string? data = Environment.GetEnvironmentVariable("XDG_DATA_HOME");
        if (string.IsNullOrEmpty(data) || !Path.IsPathRooted(data))
            data = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile, Environment.SpecialFolderOption.DoNotVerify), ".local", "share");
        return Path.Combine(data, "applications");
    }

    /// <summary>The program as an entry should name it: its plain name when that is what the search path finds (a package's), else its full path.</summary>
    public static string ProgramForEntries(string location)
    {
        if (location.Length == 0) return ProgramRunner.BinaryName;
        foreach (string dir in (Environment.GetEnvironmentVariable("PATH") ?? "").Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries))
        {
            string candidate = Path.Combine(dir, ProgramRunner.BinaryName);
            if (File.Exists(candidate) && SameFile(candidate, location)) return ProgramRunner.BinaryName;
        }
        return Path.GetFullPath(location);
    }

    private static bool SameFile(string a, string b)
    {
        try
        {
            string real(string path) => new FileInfo(path).ResolveLinkTarget(true)?.FullName ?? Path.GetFullPath(path);
            return real(a) == real(b);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { return false; }
    }

    /// <summary>The entry's file for a console; named by its hardware address, which a new name does not change.</summary>
    public string FileFor(RegisteredConsole console) => Path.Combine(Folder, $"{AppId}.Console_{console.HostId}.desktop");

    public bool Has(RegisteredConsole console) => File.Exists(FileFor(console));

    /// <summary>Writes the entry (beside, then moved over). Says what went wrong, or nothing.</summary>
    public string Add(RegisteredConsole console)
    {
        try
        {
            Directory.CreateDirectory(Folder);
            string file = FileFor(console);
            File.WriteAllText(file + ".new", Entry(console), new UTF8Encoding(false));
            File.Move(file + ".new", file, true);
            return "";
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            return $"The shortcut could not be written to {Folder}: {ex.Message}";
        }
    }

    public string Remove(RegisteredConsole console)
    {
        try
        {
            File.Delete(FileFor(console));
            return "";
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            return $"The shortcut could not be removed from {Folder}: {ex.Message}";
        }
    }

    /// <summary>The desktop entry's text (Desktop Entry Specification 1.5).</summary>
    public string Entry(RegisteredConsole console)
    {
        string name = Value(console.Name.Length > 0 ? console.Name : console.Mac);
        // The hardware address, not the name: it has nothing to quote and stays when the console is renamed.
        string exec = $"{ExecArgument(Program)} --play {ExecArgument(console.Mac)}";
        return $"""
                [Desktop Entry]
                Type=Application
                Name={name} (PS VulkanPlay)
                GenericName=Remote Play
                Comment=Play on {name} with PS VulkanPlay
                Exec={Value(exec)}
                Icon={AppId}
                StartupWMClass={AppId}
                Terminal=false
                Categories=Game;
                X-PSVulkanPlay-Console={console.HostId}

                """.Replace("\r\n", "\n");
    }

    /// <summary>
    /// One argument of an Exec key: quoted when it holds anything but plain characters, with the
    /// four characters the specification reserves inside quotes escaped, and every % doubled
    /// (a single one would be a field code).
    /// </summary>
    public static string ExecArgument(string argument)
    {
        string text = argument.Replace("%", "%%");
        if (text.Length > 0 && text.All(c => char.IsAsciiLetterOrDigit(c) || "-_./:%+,@=".Contains(c))) return text;
        var quoted = new StringBuilder("\"");
        foreach (char c in text)
        {
            if (c is '"' or '`' or '$' or '\\') quoted.Append('\\');
            quoted.Append(c);
        }
        return quoted.Append('"').ToString();
    }

    /// <summary>A string value of a desktop entry: its backslash and line breaks escaped, control characters dropped.</summary>
    public static string Value(string text)
    {
        var built = new StringBuilder();
        foreach (char c in text)
        {
            switch (c)
            {
                case '\\': built.Append(@"\\"); break;
                case '\n': built.Append(@"\n"); break;
                case '\t': built.Append(@"\t"); break;
                case '\r': built.Append(@"\r"); break;
                default:
                    if (!char.IsControl(c)) built.Append(c);
                    break;
            }
        }
        return built.ToString();
    }
}
