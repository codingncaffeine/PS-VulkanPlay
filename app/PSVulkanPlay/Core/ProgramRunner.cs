using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

namespace PSVulkanPlay.Core;

/// <summary>What one run of the native program gave back.</summary>
public sealed record RunResult(int ExitCode, string Output, string Error)
{
    public bool Ok => ExitCode == 0;

    /// <summary>The program's own sentence about what went wrong: the last thing it wrote for a person.</summary>
    public string Reason
    {
        get
        {
            string text = Error.Trim();
            if (text.Length == 0) text = Output.Trim();
            return text.Length == 0 ? $"The program ended with code {ExitCode} and said nothing." : text;
        }
    }
}

/// <summary>A run that goes on: the stream.</summary>
public interface IRunningProgram
{
    /// <summary>Completes when the program has ended, with everything it wrote.</summary>
    Task<RunResult> Ended { get; }

    /// <summary>Asks it to end the way closing its window does; kills it if it has not ended a few seconds later.</summary>
    void Stop();
}

/// <summary>Runs the native program. Everything a console is asked goes through here.</summary>
public interface IProgramRunner
{
    /// <summary>Runs it to its end. Never on the caller's thread: the work is the process's.</summary>
    Task<RunResult> RunAsync(IReadOnlyList<string> arguments, CancellationToken cancel = default);

    /// <summary>Starts it and hands every line it writes to <paramref name="line"/> (on a worker thread).</summary>
    IRunningProgram Start(IReadOnlyList<string> arguments, Action<string> line);

    /// <summary>Where the program is, for the diagnostics page; empty when it was not found.</summary>
    string Location { get; }
}

public sealed class ProgramRunner : IProgramRunner
{
    public const string BinaryName = "ps-vulkanplay";

    public ProgramRunner() => Location = Find();

    public string Location { get; }

    /// <summary>
    /// The program this app belongs to: named by the program itself when it started the app, else
    /// beside the app where a build and a package put it, else whatever the search path has.
    /// </summary>
    internal static string Find()
    {
        string? named = Environment.GetEnvironmentVariable("PSVP_NATIVE");
        if (!string.IsNullOrEmpty(named) && File.Exists(named)) return named;
        string here = AppContext.BaseDirectory;
        foreach (string candidate in new[]
                 {
                     Path.Combine(here, "..", BinaryName),                      // bin/app -> bin
                     Path.Combine(here, BinaryName),
                     Path.Combine(here, "..", "..", "..", "bin", BinaryName),   // lib/ps-vulkanplay/app -> bin
                 })
        {
            if (File.Exists(candidate)) return Path.GetFullPath(candidate);
        }
        foreach (string folder in (Environment.GetEnvironmentVariable("PATH") ?? "").Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries))
        {
            string candidate = Path.Combine(folder, BinaryName);
            if (File.Exists(candidate)) return candidate;
        }
        return "";
    }

    private ProcessStartInfo StartInfo(IReadOnlyList<string> arguments)
    {
        var info = new ProcessStartInfo(Location)
        {
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            RedirectStandardInput = true,
            UseShellExecute = false,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8,
        };
        foreach (string argument in arguments) info.ArgumentList.Add(argument);
        return info;
    }

    public async Task<RunResult> RunAsync(IReadOnlyList<string> arguments, CancellationToken cancel = default)
    {
        if (Location.Length == 0) return new RunResult(127, "", MissingText);
        try
        {
            using var process = new Process { StartInfo = StartInfo(arguments) };
            // Starting a process blocks for as long as the system takes: not on the caller's thread.
            await Task.Run(process.Start, cancel).ConfigureAwait(false);
            process.StandardInput.Close();
            Task<string> output = process.StandardOutput.ReadToEndAsync(cancel);
            Task<string> error = process.StandardError.ReadToEndAsync(cancel);
            try
            {
                await process.WaitForExitAsync(cancel).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                try { process.Kill(true); } catch (InvalidOperationException) { }
                throw;
            }
            return new RunResult(process.ExitCode, await output.ConfigureAwait(false), await error.ConfigureAwait(false));
        }
        catch (Exception ex) when (ex is System.ComponentModel.Win32Exception or IOException or InvalidOperationException)
        {
            return new RunResult(126, "", $"{Location} could not be run: {ex.Message}");
        }
    }

    public IRunningProgram Start(IReadOnlyList<string> arguments, Action<string> line)
    {
        if (Location.Length == 0) return new Finished(new RunResult(127, "", MissingText));
        try
        {
            return new Running(StartInfo(arguments), line);
        }
        catch (Exception ex) when (ex is System.ComponentModel.Win32Exception or IOException or InvalidOperationException)
        {
            return new Finished(new RunResult(126, "", $"{Location} could not be run: {ex.Message}"));
        }
    }

    private const string MissingText = "The stream program (ps-vulkanplay) was not found beside this app. Install them together.";

    private sealed class Finished(RunResult result) : IRunningProgram
    {
        public Task<RunResult> Ended { get; } = Task.FromResult(result);
        public void Stop() { }
    }

    private sealed class Running : IRunningProgram
    {
        private readonly Process _process;
        private readonly StringBuilder _output = new(), _error = new();
        private readonly TaskCompletionSource<RunResult> _ended = new(TaskCreationOptions.RunContinuationsAsynchronously);
        private int _streamsOpen = 2;

        public Running(ProcessStartInfo info, Action<string> line)
        {
            _process = new Process { StartInfo = info, EnableRaisingEvents = true };
            _process.OutputDataReceived += (_, e) => Take(e.Data, _output, line);
            _process.ErrorDataReceived += (_, e) => Take(e.Data, _error, line);
            _process.Start();
            _process.StandardInput.Close();
            _process.BeginOutputReadLine();
            _process.BeginErrorReadLine();
        }

        public Task<RunResult> Ended => _ended.Task;

        // A null line is a stream's end; the process has ended when both have and it has exited.
        private void Take(string? text, StringBuilder into, Action<string> line)
        {
            if (text is null)
            {
                if (Interlocked.Decrement(ref _streamsOpen) == 0) _ = Task.Run(Finish);
                return;
            }
            lock (into) into.AppendLine(text);
            line(text);
        }

        private void Finish()
        {
            _process.WaitForExit();
            string output, error;
            lock (_output) output = _output.ToString();
            lock (_error) error = _error.ToString();
            _ended.TrySetResult(new RunResult(_process.ExitCode, output, error));
            _process.Dispose();
        }

        [DllImport("libc", EntryPoint = "kill", SetLastError = true)]
        private static extern int SendSignal(int pid, int signal);

        public void Stop()
        {
            if (_ended.Task.IsCompleted) return;
            int pid;
            try { pid = _process.Id; }
            catch (InvalidOperationException) { return; }
            // The signal a window manager's "close" stands for: the program says goodbye to the console.
            try { SendSignal(pid, 15); }
            catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException) { }
            _ = Task.Run(async () =>
            {
                if (await Task.WhenAny(_ended.Task, Task.Delay(TimeSpan.FromSeconds(6))).ConfigureAwait(false) == _ended.Task) return;
                try { _process.Kill(true); }
                catch (Exception ex) when (ex is InvalidOperationException or ObjectDisposedException) { }
            });
        }
    }
}
