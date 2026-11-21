using System.Text;
using PSVulkanPlay.Core;

namespace PSVulkanPlay.ViewModels;

/// <summary>The diagnostics page: what this computer offers the stream, and how the last session went.</summary>
public sealed class DiagnosticsViewModel : Observable
{
    private readonly IProgramRunner _runner;
    private readonly Action<Action> _post;
    private DiagnosticsFacts? _facts;
    private bool _looking;
    private string _problem = "";

    public DiagnosticsViewModel(IProgramRunner runner, Action<Action> post)
    {
        _runner = runner;
        _post = post;
        LookAgain = new Command(() => _ = LookAsync(), () => !Looking);
    }

    public Command LookAgain { get; }
    /// <summary>The app's window gives it: puts the text on the clipboard.</summary>
    public Action<string>? CopyText { get; set; }
    /// <summary>The app's window gives it: opens a folder in the desktop's file manager.</summary>
    public Action<string>? OpenFolder { get; set; }

    public bool Looking { get => _looking; private set { if (Set(ref _looking, value)) LookAgain.Refresh(); } }
    public bool Known => _facts is not null;
    public string Problem { get => _problem; private set => Set(ref _problem, value); }
    public bool HasProblem => Problem.Length > 0;

    public string Versions => _facts is null ? "" : $"PS VulkanPlay {_facts.Version}  ·  FFmpeg {_facts.Ffmpeg}  ·  SDL {_facts.Sdl}  ·  Vulkan loader {_facts.VulkanLoader}";
    public string Devices => _facts is null ? "" : _facts.VulkanError.Length > 0 ? _facts.VulkanError : string.Join("\n", _facts.Devices);
    public bool DecodesOnCard => _facts is { GpuDecoding.Length: > 0 };
    public string Decoding => _facts is null ? "" : DecodesOnCard
        ? $"On {_facts.GpuDecoding}. Each session checks the card's first two seconds against software decoding."
        : $"In software: {_facts.GpuDecodingWhyNot}.";
    public string LastSessionTitle => _facts is null || _facts.LastSessionStarted.Length == 0 ? "No session was played yet" : $"The last session, started {Started(_facts.LastSessionStarted)}";
    public string LastSession => _facts is null ? "" : string.Join("\n", _facts.LastSession);
    public bool HasLastSession => _facts is { LastSession.Count: > 0 };
    public bool HasLogs => _facts is { LogsFolder.Length: > 0 };

    public Command Copy => new(() => CopyText?.Invoke(Everything()));
    public Command OpenLogs => new(() => { if (_facts is { LogsFolder.Length: > 0 }) OpenFolder?.Invoke(_facts.LogsFolder); });

    /// <summary>Asks the program again; the page shows the answer when it comes.</summary>
    public async Task LookAsync()
    {
        if (Looking) return;
        Looking = true;
        RunResult result = await _runner.RunAsync(["diagnose", "--json"]).ConfigureAwait(false);
        DiagnosticsFacts? facts = DiagnosticsFacts.Read(result.Output.Trim());
        _post(() =>
        {
            _facts = facts ?? _facts;
            Problem = facts is null ? $"The stream program did not answer: {result.Reason}" : "";
            Looking = false;
            foreach (string name in new[] { nameof(Known), nameof(HasProblem), nameof(Versions), nameof(Devices), nameof(DecodesOnCard), nameof(Decoding),
                                            nameof(LastSessionTitle), nameof(LastSession), nameof(HasLastSession), nameof(HasLogs) })
            {
                Changed(name);
            }
        });
    }

    /// <summary>All of it as text, for a bug report.</summary>
    public string Everything()
    {
        var text = new StringBuilder();
        text.AppendLine(Versions).AppendLine(Devices).AppendLine("Decoding: " + Decoding).AppendLine().AppendLine(LastSessionTitle);
        if (HasLastSession) text.AppendLine(LastSession);
        return text.ToString();
    }

    private static string Started(string utc) =>
        DateTime.TryParse(utc + "Z", null, System.Globalization.DateTimeStyles.AdjustToUniversal, out DateTime when)
            ? when.ToLocalTime().ToString("MMMM d, h:mm tt", System.Globalization.CultureInfo.InvariantCulture)
            : utc;
}
