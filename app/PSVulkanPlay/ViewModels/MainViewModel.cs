using System.Collections.ObjectModel;
using PSVulkanPlay.Core;

namespace PSVulkanPlay.ViewModels;

public enum Page { Home, Settings, AddConsole }

/// <summary>
/// The app's one model: the consoles, what is running, and which page shows. Everything that
/// takes time is the native program's work in its own process; what comes back is put on the UI
/// thread through <see cref="_post"/>.
/// </summary>
public sealed class MainViewModel : Observable
{
    private readonly IProgramRunner _runner;
    private readonly ConsoleDirectory _directory;
    private readonly SettingsStore _store;
    private readonly Action<Action> _post;
    private Page _page = Page.Home;
    private bool _loaded, _refreshing;
    private string _notice = "", _sessionTitle = "", _sessionLine = "", _report = "";
    private bool _noticeIsError, _streamShown, _showReport;
    private IRunningProgram? _session, _watch;
    private string _busyWith = "";
    private IReadOnlyList<ControllerInfo> _controllers = [];
    private bool _controllersKnown;

    /// <param name="post">Runs an action on the UI thread.</param>
    /// <param name="signIn">The PSN sign-in; tests give one that asks a stand-in service.</param>
    public MainViewModel(IProgramRunner runner, SettingsStore store, Action<Action> post, PsnSignIn? signIn = null)
    {
        _runner = runner;
        _directory = new ConsoleDirectory(runner);
        _store = store;
        _post = post;
        Settings = new SettingsViewModel(store, text => Notify(text, true));
        AddConsole = new AddConsoleViewModel(runner, _directory, post, Registered, signIn);
        GoHome = new Command(() => Show(Page.Home));
        GoSettings = new Command(() => Show(Page.Settings));
        GoAddConsole = new Command(() => { AddConsole.Begin(); Show(Page.AddConsole); }, () => !Busy);
        RefreshNow = new Command(() => _ = RefreshAsync());
        Stop = new Command(() => _session?.Stop(), () => _session is not null);
        DismissNotice = new Command(() => { Notice = ""; ShowReport = false; });
        ToggleReport = new Command(() => ShowReport = !ShowReport);
    }

    public ObservableCollection<ConsoleCard> Consoles { get; } = [];
    public SettingsViewModel Settings { get; }
    public AddConsoleViewModel AddConsole { get; }

    public Command GoHome { get; }
    public Command GoSettings { get; }
    public Command GoAddConsole { get; }
    public Command RefreshNow { get; }
    public Command Stop { get; }
    public Command DismissNotice { get; }
    public Command ToggleReport { get; }

    public string ProgramLocation => _runner.Location;
    public string Version => typeof(MainViewModel).Assembly.GetName().Version?.ToString(3) ?? "";

    public Page Page
    {
        get => _page;
        private set
        {
            if (!Set(ref _page, value)) return;
            Changed(nameof(IsHome));
            Changed(nameof(IsSettings));
            Changed(nameof(IsAddConsole));
        }
    }

    public bool IsHome => Page == Page.Home;
    public bool IsSettings => Page == Page.Settings;
    public bool IsAddConsole => Page == Page.AddConsole;

    /// <summary>The consoles have been asked for once: an empty list means none is registered.</summary>
    public bool Loaded { get => _loaded; private set { if (Set(ref _loaded, value)) Changed(nameof(ShowEmpty)); } }
    public bool ShowEmpty => Loaded && Consoles.Count == 0;

    /// <summary>A sentence for the user at the bottom of the window: what just happened, or what went wrong.</summary>
    public string Notice { get => _notice; private set { if (Set(ref _notice, value)) Changed(nameof(HasNotice)); } }
    public bool HasNotice => Notice.Length > 0;
    public bool NoticeIsError { get => _noticeIsError; private set => Set(ref _noticeIsError, value); }

    /// <summary>Everything the program printed for the last session.</summary>
    public string Report { get => _report; private set { if (Set(ref _report, value)) Changed(nameof(HasReport)); } }
    public bool HasReport => Report.Length > 0;
    public bool ShowReport { get => _showReport; set => Set(ref _showReport, value); }

    /// <summary>A session or a wake-up runs: one thing at a time is asked of a console.</summary>
    public bool Busy => _busyWith.Length > 0;
    public bool Playing => _session is not null;
    public string SessionTitle { get => _sessionTitle; private set => Set(ref _sessionTitle, value); }
    public string SessionLine { get => _sessionLine; private set => Set(ref _sessionLine, value); }

    /// <summary>The controllers that are connected, as the stream program sees them; the first is the one a session uses.</summary>
    public IReadOnlyList<ControllerInfo> Controllers
    {
        get => _controllers;
        private set
        {
            _controllers = value;
            Changed();
            Changed(nameof(HasController));
            Changed(nameof(ControllerLine));
            Changed(nameof(ControllerNote));
            Changed(nameof(HasControllerNote));
            Changed(nameof(ControllerBatteryLow));
        }
    }

    public bool HasController => _controllers.Count > 0;

    /// <summary>The bottom line of the window: which controller a session would use.</summary>
    public string ControllerLine
    {
        get
        {
            if (!_controllersKnown) return "Looking for a controller…";
            if (_controllers.Count == 0) return "No controller connected";
            return _controllers.Count == 1 ? _controllers[0].Line : $"{_controllers[0].Line}  ·  and {_controllers.Count - 1} more";
        }
    }

    public string ControllerNote => _controllers.Count == 0 ? (_controllersKnown ? "Plug one in or pair it: any controller works, a DualSense gets its haptics and trigger effects." : "")
                                    : _controllers[0].BatteryLow ? "Its battery is low" : _controllers[0].Abilities;
    public bool HasControllerNote => ControllerNote.Length > 0;
    public bool ControllerBatteryLow => _controllers.Count > 0 && _controllers[0].BatteryLow;

    /// <summary>
    /// Starts asking the program which controllers are connected; it says so again whenever
    /// that changes. Not while a session runs: the stream has the controller then.
    /// </summary>
    public void WatchControllers()
    {
        if (_watch is not null || Playing || Busy && _busyWith == "play") return;
        _watch = _runner.Start(["controllers", "--json", "--watch"], line => _post(() =>
        {
            if (ControllerJson.Read(line) is not { } list) return;
            _controllersKnown = true;
            Controllers = list;
        }));
    }

    // The watcher has the controllers open; it lets go of them before a session takes them.
    private Task StopWatchingAsync()
    {
        IRunningProgram? watch = _watch;
        _watch = null;
        if (watch is null) return Task.CompletedTask;
        watch.Stop();
        return Task.WhenAny(watch.Ended, Task.Delay(TimeSpan.FromSeconds(3)));
    }

    /// <summary>The window closes: whatever this app started ends with it.</summary>
    public void Shutdown()
    {
        _session?.Stop();
        _watch?.Stop();
        _watch = null;
        Settings.Flush();
    }

    /// <summary>The stream's own window is up: this one can get out of the way.</summary>
    public bool StreamShown { get => _streamShown; private set => Set(ref _streamShown, value); }

    private void Show(Page page)
    {
        Page = page;
        if (page == Page.Home) _ = RefreshAsync();
    }

    private void Notify(string text, bool error)
    {
        NoticeIsError = error;
        Notice = text;
    }

    private void SetBusy(string with)
    {
        _busyWith = with;
        Changed(nameof(Busy));
        Changed(nameof(Playing));
        foreach (ConsoleCard card in Consoles) card.Refresh();
        GoAddConsole.Refresh();
        Stop.Refresh();
    }

    /// <summary>Reads the registered consoles and asks how each one is. Called on the UI thread; returns at once.</summary>
    public async Task RefreshAsync()
    {
        if (_refreshing) return;
        _refreshing = true;
        try
        {
            (IReadOnlyList<RegisteredConsole> registered, string error) = await _directory.RegisteredAsync().ConfigureAwait(false);
            _post(() =>
            {
                if (error.Length > 0) Notify(error, true);
                Merge(registered);
                Loaded = true;
            });
            if (registered.Count == 0) return;
            IReadOnlyList<ConsoleStatus> status = await _directory.StatusAsync(registered).ConfigureAwait(false);
            _post(() =>
            {
                foreach (ConsoleStatus one in status)
                    Consoles.FirstOrDefault(card => card.Console.Mac == one.Console.Mac)?.Take(one);
            });
        }
        finally
        {
            _refreshing = false;
        }
    }

    // Cards stay the objects they are while their consoles stay registered: a refresh does not blink.
    private void Merge(IReadOnlyList<RegisteredConsole> registered)
    {
        for (int i = Consoles.Count - 1; i >= 0; --i)
            if (registered.All(console => console.Mac != Consoles[i].Console.Mac)) Consoles.RemoveAt(i);
        foreach (RegisteredConsole console in registered)
            if (Consoles.All(card => card.Console.Mac != console.Mac)) Consoles.Add(new ConsoleCard(this, console));
        Changed(nameof(ShowEmpty));
    }

    private void Registered(string name)
    {
        // What there is to say first, the page last: whoever sees the page change finds the sentence there.
        Notify($"{name} is registered. Press Play to start.", false);
        Show(Page.Home);
    }

    public void Play(ConsoleCard card)
    {
        if (Busy) return;
        Notice = "";
        Report = "";
        ShowReport = false;
        StreamShown = false;
        SessionTitle = $"Connecting to {card.Name}";
        SessionLine = "";
        IReadOnlyList<string> arguments = Settings.Current.PlayArguments(card.Console.Name, card.Address, card.IsPs4);
        SetBusy("play");
        void Start()
        {
            IRunningProgram session = _runner.Start(arguments, line => _post(() => SessionSaid(card, line)));
            _session = session;
            Changed(nameof(Playing));
            Stop.Refresh();
            _ = session.Ended.ContinueWith(ended => _post(() => SessionEnded(card, ended.Result)), TaskScheduler.Default);
        }
        // With no watcher there is nothing to wait for; with one, the session starts when it has let go.
        Task stopping = StopWatchingAsync();
        if (stopping.IsCompleted) Start();
        else _ = stopping.ContinueWith(_ => _post(Start), TaskScheduler.Default);
    }

    // The program's own lines are sentences for a person; the last one is shown as they come.
    private void SessionSaid(ConsoleCard card, string line)
    {
        if (_session is null) return;
        if (line.Contains("render: first picture shown", StringComparison.Ordinal))
        {
            StreamShown = true;
            SessionTitle = $"Playing on {card.Name}";
            SessionLine = "Close the stream's window to end the session.";
            return;
        }
        // Log lines (they begin with a time) are for the log; what the program says to a person is not.
        if (line.Length == 0 || char.IsDigit(line[0]) || StreamShown) return;
        SessionLine = line;
    }

    private void SessionEnded(ConsoleCard card, RunResult result)
    {
        _session = null;
        StreamShown = false;
        Report = (result.Output + result.Error).Trim();
        if (result.Ok) Notify($"The session with {card.Name} has ended.", false);
        else Notify(LastSentence(result), true);
        SetBusy("");   // last: whoever waits for the session's end finds what it left
        WatchControllers();
        _ = RefreshAsync();
    }

    // Of what went to the error stream, the lines that are not log lines: the reason, in the program's words.
    private static string LastSentence(RunResult result)
    {
        var sentences = result.Error.Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
                              .Where(line => !char.IsDigit(line[0])).ToList();
        if (sentences.Count == 0) return result.Reason.Split('\n', StringSplitOptions.RemoveEmptyEntries).LastOrDefault() ?? "The session did not start.";
        // A reason can run over several lines (the following ones begin in lower case); what was
        // said before it, about an earlier try, is not part of it.
        int from = sentences.Count - 1;
        while (from > 0 && char.IsLower(sentences[from][0])) --from;
        return string.Join(" ", sentences.Skip(from));
    }

    public void Wake(ConsoleCard card)
    {
        if (Busy) return;
        SetBusy("wake");
        Notify($"Waking {card.Name}…", false);
        var arguments = new List<string> { "wake", card.Console.Name, "--wait", "60" };
        if (card.Address.Length > 0) arguments.AddRange(["--host", card.Address]);
        _ = _runner.RunAsync(arguments).ContinueWith(done => _post(() =>
        {
            RunResult result = done.Result;
            Notify(result.Ok ? result.Output.Trim() : result.Reason, !result.Ok);
            SetBusy("");
            _ = RefreshAsync();
        }), TaskScheduler.Default);
    }

    public void Forget(ConsoleCard card)
    {
        if (Busy) return;
        _ = _runner.RunAsync(["forget", card.Console.Name]).ContinueWith(done => _post(() =>
        {
            RunResult result = done.Result;
            Notify(result.Ok ? $"{card.Name} is no longer registered here." : result.Reason, !result.Ok);
            _ = RefreshAsync();
        }), TaskScheduler.Default);
    }
}
