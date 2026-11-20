using System.ComponentModel;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Threading;
using PSVulkanPlay.Core;
using PSVulkanPlay.Platform;
using PSVulkanPlay.ViewModels;

namespace PSVulkanPlay.Views;

public partial class MainWindow : Window
{
    private readonly DispatcherTimer _look;
    private readonly ControllerNavigator _navigator;
    private MainViewModel? _model;
    private bool _started, _minimizedForStream;

    public MainWindow()
    {
        InitializeComponent();
        _navigator = new ControllerNavigator(this, () => HomeScroll.IsVisible ? HomeScroll : SettingsScroll.IsVisible ? SettingsScroll : AddScroll);
        // A key of the keyboard (not the one a controller's press is passed on as): whoever types has a keyboard.
        AddHandler(KeyDownEvent, (_, e) => { if (e.KeyDeviceType == KeyDeviceType.Keyboard) _model?.KeyboardUsed(); }, RoutingStrategies.Tunnel);
        // Made stopped: a timer made with its handler starts by itself.
        _look = new DispatcherTimer(DispatcherPriority.Background) { Interval = TimeSpan.FromSeconds(6) };
        _look.Tick += (_, _) =>
        {
            // How the consoles are, while somebody can see it and nothing else is being asked of them.
            if (_model is { IsHome: true, Busy: false } && IsActive) _ = _model.RefreshAsync();
        };
        Opened += OnOpened;
        Activated += (_, _) => _ = LookAtClipboardAsync();
        // In front again (after a stream, say): the ring is where a controller goes on from.
        Activated += (_, _) => FollowWithFocus();
        Closing += (_, _) => _model?.Shutdown();
        DataContextChanged += (_, _) => Attach(DataContext as MainViewModel);
    }

    private void Attach(MainViewModel? model)
    {
        if (_model is not null)
        {
            _model.PropertyChanged -= ModelChanged;
            _model.AddConsole.PropertyChanged -= StepChanged;
            _model.Consoles.CollectionChanged -= ContentArrived;
            _model.Pressed -= Pressed;
        }
        _model = model;
        if (model is null) return;
        model.PropertyChanged += ModelChanged;
        model.AddConsole.PropertyChanged += StepChanged;
        model.Consoles.CollectionChanged += ContentArrived;
        model.Pressed += Pressed;
        // The sign-in's two roads: the login page in a window of its own, or in the user's browser.
        model.AddConsole.SignInWindow ??= new SignInWindow(this);
        model.AddConsole.OpenBrowser ??= address => _ = Launcher.LaunchUriAsync(address);
        AboutLine.Text = $"PS VulkanPlay {model.Version}   ·   stream program: {(model.ProgramLocation.Length > 0 ? model.ProgramLocation : "not found")}   ·   settings: {model.Settings.File}";
    }

    // Back from the browser with the sign-in's last address copied: it is taken without a paste.
    // Asked for only while the page that wants it is showing; the clipboard is nobody else's business.
    private async Task LookAtClipboardAsync()
    {
        if (_model is not { IsAddConsole: true } model || !model.AddConsole.IsStep2 || !model.AddConsole.ShowPaste || Clipboard is null) return;
        try
        {
            string? text = await Avalonia.Input.Platform.ClipboardExtensions.TryGetTextAsync(Clipboard);
            if (!string.IsNullOrEmpty(text)) model.AddConsole.LookAt(text);
        }
        catch (Exception ex) when (ex is InvalidOperationException or TimeoutException or NotSupportedException) { }
    }

    // Opened is raised every time the window is shown; the first refresh and the timer start once.
    private void OnOpened(object? sender, EventArgs e)
    {
        DesktopIdentity.Apply(this);
        if (_started) return;
        _started = true;
        _ = _model?.RefreshAsync();
        _model?.WatchControllers();
        _look.Start();
    }

    /// <summary>What moves the focus for a controller; the tests ask it where a direction leads.</summary>
    internal ControllerNavigator Navigator => _navigator;

    // A controller is read whether or not this window is the one in front: its presses count
    // only while it is, or they would move through the app from inside another program.
    private void Pressed(ControllerPress press, int controller)
    {
        if (_model is null || !IsActive || WindowState == WindowState.Minimized) return;
        _model.ControllerUsed(controller);
        switch (press)
        {
            case ControllerPress.Back: _model.Back(); break;
            case ControllerPress.PreviousPage: _model.PreviousPage(); break;
            case ControllerPress.NextPage: _model.NextPage(); break;
            default: _navigator.Press(press); break;
        }
        FollowWithFocus();
    }

    private enum FocusGoes { OnFromWhereItIs, ToWhereThePageBegins, ToWhatArrived }

    // What just happened may have taken the focused control away (a button that cannot be pressed
    // now), or put another page or step there, or brought what the page is for: once the page is
    // laid out again, the focus is put where a controller goes on from.
    private void FollowWithFocus(FocusGoes where = FocusGoes.OnFromWhereItIs)
    {
        if (_model is not { ControllerInUse: true }) return;
        Dispatcher.UIThread.Post(() =>
        {
            if (!IsActive) return;
            if (where == FocusGoes.ToWhereThePageBegins) _navigator.FocusFirst();
            else if (where == FocusGoes.ToWhatArrived) _navigator.ContentChanged();
            else _navigator.EnsureFocus();
        }, DispatcherPriority.Loaded);
    }

    private void StepChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(AddConsoleViewModel.Step)) FollowWithFocus(FocusGoes.ToWhereThePageBegins);
        else if (e.PropertyName == nameof(AddConsoleViewModel.Searching)) FollowWithFocus(FocusGoes.ToWhatArrived);
    }

    private void ContentArrived(object? sender, System.Collections.Specialized.NotifyCollectionChangedEventArgs e) => FollowWithFocus(FocusGoes.ToWhatArrived);

    // While the stream's own window is up this one gets out of the way, and comes back when it ends.
    private void ModelChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(MainViewModel.Page)) FollowWithFocus(FocusGoes.ToWhereThePageBegins);
        else if (e.PropertyName == nameof(MainViewModel.Busy)) FollowWithFocus(FocusGoes.ToWhatArrived);
        if (e.PropertyName != nameof(MainViewModel.StreamShown) || _model is null) return;
        if (_model.StreamShown && WindowState != WindowState.Minimized)
        {
            _minimizedForStream = true;
            WindowState = WindowState.Minimized;
        }
        else if (!_model.StreamShown && _minimizedForStream)
        {
            _minimizedForStream = false;
            WindowState = WindowState.Normal;
            Activate();
        }
    }
}
