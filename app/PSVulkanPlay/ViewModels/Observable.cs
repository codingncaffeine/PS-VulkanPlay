using System.ComponentModel;
using System.Runtime.CompilerServices;
using System.Windows.Input;

namespace PSVulkanPlay.ViewModels;

public abstract class Observable : INotifyPropertyChanged
{
    public event PropertyChangedEventHandler? PropertyChanged;

    protected bool Set<T>(ref T field, T value, [CallerMemberName] string name = "")
    {
        if (EqualityComparer<T>.Default.Equals(field, value)) return false;
        field = value;
        Changed(name);
        return true;
    }

    protected void Changed([CallerMemberName] string name = "") => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
}

/// <summary>A command that runs on the UI thread and starts work that does not stay there.</summary>
public sealed class Command(Action<object?> run, Func<bool>? allowed = null) : ICommand
{
    public Command(Action run, Func<bool>? allowed = null) : this(_ => run(), allowed) { }

    public event EventHandler? CanExecuteChanged;
    public bool CanExecute(object? parameter) => allowed?.Invoke() ?? true;
    public void Execute(object? parameter) { if (CanExecute(parameter)) run(parameter); }
    public void Refresh() => CanExecuteChanged?.Invoke(this, EventArgs.Empty);
}
