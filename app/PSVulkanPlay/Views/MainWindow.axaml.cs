using System.ComponentModel;
using Avalonia.Controls;
using Avalonia.Threading;
using PSVulkanPlay.Platform;
using PSVulkanPlay.ViewModels;

namespace PSVulkanPlay.Views;

public partial class MainWindow : Window
{
    private readonly DispatcherTimer _look;
    private MainViewModel? _model;
    private bool _started, _minimizedForStream;

    public MainWindow()
    {
        InitializeComponent();
        // Made stopped: a timer made with its handler starts by itself.
        _look = new DispatcherTimer(DispatcherPriority.Background) { Interval = TimeSpan.FromSeconds(6) };
        _look.Tick += (_, _) =>
        {
            // How the consoles are, while somebody can see it and nothing else is being asked of them.
            if (_model is { IsHome: true, Busy: false } && IsActive) _ = _model.RefreshAsync();
        };
        Opened += OnOpened;
        Closing += (_, _) => _model?.Shutdown();
        DataContextChanged += (_, _) => Attach(DataContext as MainViewModel);
    }

    private void Attach(MainViewModel? model)
    {
        if (_model is not null) _model.PropertyChanged -= ModelChanged;
        _model = model;
        if (model is null) return;
        model.PropertyChanged += ModelChanged;
        AboutLine.Text = $"PS VulkanPlay {model.Version}   ·   stream program: {(model.ProgramLocation.Length > 0 ? model.ProgramLocation : "not found")}   ·   settings: {model.Settings.File}";
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

    // While the stream's own window is up this one gets out of the way, and comes back when it ends.
    private void ModelChanged(object? sender, PropertyChangedEventArgs e)
    {
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
