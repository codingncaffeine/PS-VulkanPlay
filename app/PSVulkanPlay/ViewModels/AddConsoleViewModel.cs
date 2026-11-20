using System.Collections.ObjectModel;
using PSVulkanPlay.Core;

namespace PSVulkanPlay.ViewModels;

/// <summary>
/// Registering with a console, in three steps: which console, which account, the PIN it shows.
/// </summary>
public sealed class AddConsoleViewModel : Observable
{
    private readonly IProgramRunner _runner;
    private readonly ConsoleDirectory _directory;
    private readonly Action<Action> _post;
    private readonly Action<string> _registered;
    private readonly PsnSignIn _signIn;
    private int _step = 1;
    private bool _searching, _working, _signingIn, _showPaste, _signedIn;
    private string _address = "", _accountId = "", _pin = "", _error = "", _chosenName = "", _pasted = "";
    private FoundConsole? _chosen;

    public AddConsoleViewModel(IProgramRunner runner, ConsoleDirectory directory, Action<Action> post, Action<string> registered, PsnSignIn? signIn = null)
    {
        _signIn = signIn ?? new PsnSignIn();
        _runner = runner;
        _directory = directory;
        _post = post;
        _registered = registered;
        Search = new Command(() => _ = SearchAsync(), () => !Searching);
        Choose = new Command(found => { if (found is FoundConsole console) Pick(console); });
        UseAddress = new Command(() => _ = AskAddressAsync(), () => Address.Trim().Length > 0 && !Searching);
        Back = new Command(() => { Error = ""; Step = Math.Max(1, Step - 1); });
        Next = new Command(() => { Error = ""; Step = 3; }, () => AccountIdLooksRight);
        Register = new Command(() => _ = RegisterAsync(), () => PinLooksRight && !Working);
        SignIn = new Command(() => _ = SignInAsync(), () => !SigningIn);
        UsePasted = new Command(() => _ = TakeAddressAsync(Pasted), () => !SigningIn && PsnSignIn.CodeFrom(Pasted) is not null);
        OpenInBrowser = new Command(() => { ShowPaste = true; OpenBrowser?.Invoke(_signIn.LoginPage); });
    }

    /// <summary>The window that shows the login page; the app's window gives it. Without one, the browser is the road.</summary>
    public ISignInWindow? SignInWindow { get; set; }

    /// <summary>Opens an address in the user's own browser; the app's window gives it.</summary>
    public Action<Uri>? OpenBrowser { get; set; }

    public Command SignIn { get; }
    public Command UsePasted { get; }
    public Command OpenInBrowser { get; }

    public bool SigningIn
    {
        get => _signingIn;
        private set
        {
            if (!Set(ref _signingIn, value)) return;
            SignIn.Refresh();
            UsePasted.Refresh();
        }
    }

    /// <summary>The account ID in the field came from a sign-in just now.</summary>
    public bool SignedIn { get => _signedIn; private set => Set(ref _signedIn, value); }

    /// <summary>The other road is shown: the login page in the user's browser, and its last address pasted here.</summary>
    public bool ShowPaste { get => _showPaste; set => Set(ref _showPaste, value); }

    public string Pasted { get => _pasted; set { if (Set(ref _pasted, value)) UsePasted.Refresh(); } }

    /// <summary>
    /// What the clipboard holds when the app's window is looked at again: taken when it is the
    /// address the login page leads to, so that nobody has to paste it.
    /// </summary>
    public void LookAt(string clipboard)
    {
        if (!IsStep2 || !ShowPaste || SigningIn || PsnSignIn.CodeFrom(clipboard) is null || clipboard.Trim() == Pasted.Trim()) return;
        Pasted = clipboard.Trim();
        _ = TakeAddressAsync(Pasted);
    }

    private async Task SignInAsync()
    {
        if (SigningIn) return;
        Error = "";
        if (SignInWindow is null)
        {
            OpenInBrowser.Execute(null);
            return;
        }
        SigningIn = true;
        SignInAnswer answer = await SignInWindow.ShowAsync(_signIn.LoginPage, PsnSignIn.RedirectAddress).ConfigureAwait(false);
        if (answer.Unavailable.Length > 0)
        {
            // No window for the page here: the browser, and the address pasted.
            _post(() =>
            {
                SigningIn = false;
                OpenInBrowser.Execute(null);
            });
            return;
        }
        if (answer.Address.Length == 0)
        {
            _post(() => SigningIn = false);   // closed without signing in: nothing to say
            return;
        }
        await ExchangeAsync(answer.Address).ConfigureAwait(false);
    }

    private Task TakeAddressAsync(string address)
    {
        if (SigningIn) return Task.CompletedTask;
        Error = "";
        SigningIn = true;
        return ExchangeAsync(address);
    }

    // SigningIn is set. The address is where the login page led; what comes of it is put on the page.
    private async Task ExchangeAsync(string address)
    {
        string? code = PsnSignIn.CodeFrom(address);
        (string id, string error) = code is null
            ? ("", "The sign-in did not end on the page it should end on. Sign in again, or type the account ID.")
            : await _signIn.AccountIdForAsync(code).ConfigureAwait(false);
        // Kept by the program too, so that the next console does not ask again.
        if (id.Length > 0) await _runner.RunAsync(["account", id]).ConfigureAwait(false);
        _post(() =>
        {
            SigningIn = false;
            if (id.Length == 0)
            {
                Error = error;
                return;
            }
            AccountId = id;
            SignedIn = true;
            ShowPaste = false;
            Pasted = "";
            Step = 3;
        });
    }

    public ObservableCollection<FoundConsole> Found { get; } = [];
    public Command Search { get; }
    public Command Choose { get; }
    public Command UseAddress { get; }
    public Command Back { get; }
    public Command Next { get; }
    public Command Register { get; }

    public int Step
    {
        get => _step;
        private set
        {
            if (!Set(ref _step, value)) return;
            Changed(nameof(IsStep1));
            Changed(nameof(IsStep2));
            Changed(nameof(IsStep3));
        }
    }

    public bool IsStep1 => Step == 1;
    public bool IsStep2 => Step == 2;
    public bool IsStep3 => Step == 3;

    public bool Searching
    {
        get => _searching;
        private set
        {
            if (!Set(ref _searching, value)) return;
            Changed(nameof(NothingFound));
            Search.Refresh();
            UseAddress.Refresh();
        }
    }

    public bool NothingFound => !Searching && Found.Count == 0;
    public bool Working { get => _working; private set { if (Set(ref _working, value)) Register.Refresh(); } }
    public string Error { get => _error; private set { if (Set(ref _error, value)) Changed(nameof(HasError)); } }
    public bool HasError => Error.Length > 0;
    public string ChosenName { get => _chosenName; private set => Set(ref _chosenName, value); }

    public string Address { get => _address; set { if (Set(ref _address, value)) UseAddress.Refresh(); } }

    public string AccountId
    {
        get => _accountId;
        set
        {
            if (!Set(ref _accountId, value)) return;
            Changed(nameof(AccountIdLooksRight));
            Next.Refresh();
        }
    }

    /// <summary>A number, or the twelve base64 characters the same number is written as.</summary>
    public bool AccountIdLooksRight
    {
        get
        {
            string text = AccountId.Trim();
            if (text.Length is >= 1 and <= 20 && text.All(char.IsAsciiDigit)) return true;
            return text.Length == 12 && text.All(c => char.IsAsciiLetterOrDigit(c) || c is '+' or '/' or '=');
        }
    }

    public string Pin
    {
        get => _pin;
        set
        {
            if (!Set(ref _pin, value)) return;
            Changed(nameof(PinLooksRight));
            Register.Refresh();
        }
    }

    private string PinDigits => new(Pin.Where(char.IsAsciiDigit).ToArray());
    public bool PinLooksRight => PinDigits.Length == 8 && Pin.All(c => char.IsAsciiDigit(c) || c == ' ');

    /// <summary>A fresh start: called each time the page is opened.</summary>
    public void Begin()
    {
        Step = 1;
        Error = "";
        Pin = "";
        Pasted = "";
        ShowPaste = false;
        SignedIn = false;
        _chosen = null;
        _ = SearchAsync();
        _ = LoadAccountAsync();
    }

    private async Task LoadAccountAsync()
    {
        RunResult result = await _runner.RunAsync(["account", "--json"]).ConfigureAwait(false);
        string id = result.Ok ? ConsoleJson.AccountId(result.Output) : "";
        if (id.Length > 0) _post(() => { if (AccountId.Length == 0) AccountId = id; });
    }

    private async Task SearchAsync()
    {
        if (Searching) return;
        Searching = true;
        IReadOnlyList<FoundConsole> found = await _directory.DiscoverAsync(null, 2).ConfigureAwait(false);
        _post(() =>
        {
            Found.Clear();
            foreach (FoundConsole console in found) Found.Add(console);
            Searching = false;
            Changed(nameof(NothingFound));
        });
    }

    private async Task AskAddressAsync()
    {
        string address = Address.Trim();
        Searching = true;
        Error = "";
        IReadOnlyList<FoundConsole> found = await _directory.DiscoverAsync(address, 2).ConfigureAwait(false);
        _post(() =>
        {
            Searching = false;
            if (found.Count == 0) Error = $"No console answered at {address}. Check the address, and that the console is on.";
            else Pick(found[0]);
        });
    }

    private void Pick(FoundConsole console)
    {
        Error = "";
        if (console.Registered)
        {
            Error = $"{console.Name} is already registered here.";
            return;
        }
        if (!console.Supported)
        {
            Error = $"{console.Name} runs system software that is too old for this program. Update the console.";
            return;
        }
        _chosen = console;
        ChosenName = console.Name.Length > 0 ? console.Name : console.Address;
        Step = 2;
    }

    private async Task RegisterAsync()
    {
        if (_chosen is null || Working) return;
        Working = true;
        Error = "";
        FoundConsole console = _chosen;
        RunResult result = await _runner.RunAsync(["register", "--host", console.Address, "--pin", PinDigits, "--account-id", AccountId.Trim()]).ConfigureAwait(false);
        _post(() =>
        {
            Working = false;
            if (result.Ok)
            {
                Pin = "";
                _registered(ChosenName);
            }
            else
            {
                Error = string.Join(" ", result.Reason.Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries));
            }
        });
    }
}
