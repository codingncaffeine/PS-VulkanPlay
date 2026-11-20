using PSVulkanPlay.Core;

namespace PSVulkanPlay.Tests;

/// <summary>Stands in for the native program: answers by its first argument, and keeps what it was asked.</summary>
public sealed class FakeRunner : IProgramRunner
{
    public const string TwoConsoles =
        """[{"name":"Living Room","kind":"PS5","mac":"02:00:5e:10:20:30","last_address":"192.0.2.10"},{"name":"Den","kind":"PS4","mac":"02:00:5e:10:20:31","last_address":""}]""";

    private readonly object _lock = new();
    private readonly List<IReadOnlyList<string>> _calls = [];

    public string Location => "/opt/fake/ps-vulkanplay";
    public Dictionary<string, Func<IReadOnlyList<string>, RunResult>> Answers { get; } = [];
    public FakeSession? Session { get; private set; }

    /// <summary>The run that watches the controllers, when the app has started one.</summary>
    public FakeSession? Watch { get; private set; }

    public IReadOnlyList<IReadOnlyList<string>> Calls { get { lock (_lock) return _calls.ToList(); } }
    public IReadOnlyList<string>? Last(string command) => Calls.LastOrDefault(call => call[0] == command);

    /// <summary>A living room PS5 that is on at its address, and a PS4 that is found resting by asking everyone.</summary>
    public static FakeRunner WithTwoConsoles()
    {
        var fake = new FakeRunner();
        fake.Answers["consoles"] = _ => new RunResult(0, TwoConsoles, "");
        fake.Answers["discover"] = arguments => arguments.Contains("--host")
            ? new RunResult(0, """[{"name":"Living Room","kind":"PS5","state":"ready","address":"192.0.2.10","host_id":"02005E102030","system_version":"1","running_app":"","registered":true,"supported":true}]""", "")
            : new RunResult(0, """[{"name":"Den","kind":"PS4","state":"standby","address":"192.0.2.11","host_id":"02005E102031","system_version":"1","running_app":"","registered":true,"supported":true}]""", "");
        return fake;
    }

    public Task<RunResult> RunAsync(IReadOnlyList<string> arguments, CancellationToken cancel = default)
    {
        lock (_lock) _calls.Add(arguments);
        RunResult result = Answers.TryGetValue(arguments[0], out var answer) ? answer(arguments) : new RunResult(0, "", "");
        // Off the caller's thread, as a process is.
        return Task.Run(() => result, cancel);
    }

    public IRunningProgram Start(IReadOnlyList<string> arguments, Action<string> line)
    {
        lock (_lock) _calls.Add(arguments);
        var session = new FakeSession(line);
        if (arguments[0] == "controllers") Watch = session;
        else Session = session;
        return session;
    }
}

public sealed class FakeSession(Action<string> line) : IRunningProgram
{
    private readonly TaskCompletionSource<RunResult> _ended = new(TaskCreationOptions.RunContinuationsAsynchronously);
    public Task<RunResult> Ended => _ended.Task;
    public bool Stopped { get; private set; }
    public void Say(string text) => line(text);
    public void End(int code, string output = "", string error = "") => _ended.TrySetResult(new RunResult(code, output, error));
    public void Stop() { Stopped = true; End(0, "Goodbye sent and acknowledged.\n"); }
}
