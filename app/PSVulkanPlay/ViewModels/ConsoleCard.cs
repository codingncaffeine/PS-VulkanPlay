using PSVulkanPlay.Core;

namespace PSVulkanPlay.ViewModels;

/// <summary>One registered console on the home page.</summary>
public sealed class ConsoleCard : Observable
{
    private readonly MainViewModel _main;
    private ConsoleState _state = ConsoleState.Looking;
    private string _address, _runningApp = "";
    private bool _confirmForget;

    public ConsoleCard(MainViewModel main, RegisteredConsole console)
    {
        _main = main;
        Console = console;
        _address = console.LastAddress;
        Play = new Command(() => _main.Play(this), () => CanPlay);
        Wake = new Command(() => _main.Wake(this), () => CanWake);
        AskForget = new Command(() => ConfirmForget = true);
        CancelForget = new Command(() => ConfirmForget = false);
        Forget = new Command(() => _main.Forget(this));
    }

    public RegisteredConsole Console { get; }
    public string Name => Console.Name.Length > 0 ? Console.Name : "(no name)";
    public string Kind => Console.Kind;
    public bool IsPs4 => Console.Kind.Equals("PS4", StringComparison.OrdinalIgnoreCase);

    public Command Play { get; }
    public Command Wake { get; }
    public Command AskForget { get; }
    public Command CancelForget { get; }
    public Command Forget { get; }

    public ConsoleState State
    {
        get => _state;
        private set
        {
            if (!Set(ref _state, value)) return;
            Changed(nameof(StateText));
            Changed(nameof(IsReady));
            Changed(nameof(IsResting));
            Changed(nameof(IsMissing));
            Changed(nameof(IsLooking));
            Refresh();
        }
    }

    public string Address { get => _address; private set { if (Set(ref _address, value)) Changed(nameof(Detail)); } }
    public string RunningApp { get => _runningApp; private set { if (Set(ref _runningApp, value)) Changed(nameof(Detail)); } }

    public bool IsReady => State == ConsoleState.Ready;
    public bool IsResting => State == ConsoleState.Resting;
    public bool IsMissing => State == ConsoleState.NotFound;
    public bool IsLooking => State == ConsoleState.Looking;

    public string StateText => State switch
    {
        ConsoleState.Ready => "On",
        ConsoleState.Resting => "In rest mode",
        ConsoleState.NotFound => "Not found on the network",
        _ => "Looking for it…",
    };

    /// <summary>The second line under the state: what it runs, or where it is.</summary>
    public string Detail => RunningApp.Length > 0 ? RunningApp : Address.Length > 0 ? Address : "Address not known yet";

    public bool ConfirmForget { get => _confirmForget; set => Set(ref _confirmForget, value); }

    // A console that was not found may still take a session at its last address (a firewall can
    // swallow its answers), so Play stays open; only a session that runs closes it.
    public bool CanPlay => !_main.Busy;
    public bool CanWake => !_main.Busy && State != ConsoleState.Ready;

    public void Take(ConsoleStatus status)
    {
        State = status.State;
        if (status.Address.Length > 0) Address = status.Address;
        RunningApp = status.RunningApp;
    }

    public void Refresh()
    {
        Changed(nameof(CanPlay));
        Changed(nameof(CanWake));
        Play.Refresh();
        Wake.Refresh();
    }
}
