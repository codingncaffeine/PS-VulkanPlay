using System.Text.Json;

namespace PSVulkanPlay.Core;

/// <summary>A console this computer is registered with, as the program's store has it.</summary>
public sealed record RegisteredConsole(string Name, string Kind, string Mac, string LastAddress)
{
    /// <summary>The hardware address as twelve lowercase hex digits: what a console's answer is compared with.</summary>
    public string HostId => ConsoleJson.HexOnly(Mac);
}

/// <summary>A console that answered on the network.</summary>
public sealed record FoundConsole(string Name, string Kind, string State, string Address, string HostId, string RunningApp, bool Registered, bool Supported)
{
    public bool Ready => State == "ready";
    public bool Resting => State == "standby";
}

/// <summary>Reads what the program's --json commands print.</summary>
public static class ConsoleJson
{
    public static string HexOnly(string text) => new string(text.Where(Uri.IsHexDigit).ToArray()).ToLowerInvariant();

    public static IReadOnlyList<RegisteredConsole> Registered(string json)
    {
        var list = new List<RegisteredConsole>();
        foreach (JsonElement item in Items(json))
            list.Add(new RegisteredConsole(Text(item, "name"), Text(item, "kind"), Text(item, "mac"), Text(item, "last_address")));
        return list;
    }

    public static IReadOnlyList<FoundConsole> Found(string json)
    {
        var list = new List<FoundConsole>();
        foreach (JsonElement item in Items(json))
            list.Add(new FoundConsole(Text(item, "name"), Text(item, "kind"), Text(item, "state"), Text(item, "address"), HexOnly(Text(item, "host_id")),
                                      Text(item, "running_app"), Flag(item, "registered"), Flag(item, "supported", true)));
        return list;
    }

    /// <summary>The account ID the program has, in its base64 form; empty when none is set.</summary>
    public static string AccountId(string json)
    {
        try
        {
            using JsonDocument document = JsonDocument.Parse(json);
            return document.RootElement.ValueKind == JsonValueKind.Object ? Text(document.RootElement, "account_id") : "";
        }
        catch (JsonException) { return ""; }
    }

    // What is not a list of objects is an empty list: a program that printed something else has said why elsewhere.
    private static List<JsonElement> Items(string json)
    {
        var items = new List<JsonElement>();
        if (string.IsNullOrWhiteSpace(json)) return items;
        try
        {
            using JsonDocument document = JsonDocument.Parse(json);
            if (document.RootElement.ValueKind != JsonValueKind.Array) return items;
            foreach (JsonElement item in document.RootElement.EnumerateArray())
                if (item.ValueKind == JsonValueKind.Object) items.Add(item.Clone());
        }
        catch (JsonException) { }
        return items;
    }

    private static string Text(JsonElement item, string name) =>
        item.TryGetProperty(name, out JsonElement value) && value.ValueKind == JsonValueKind.String ? value.GetString() ?? "" : "";

    private static bool Flag(JsonElement item, string name, bool otherwise = false) =>
        item.TryGetProperty(name, out JsonElement value) && value.ValueKind is JsonValueKind.True or JsonValueKind.False ? value.GetBoolean() : otherwise;
}

/// <summary>How a registered console is right now.</summary>
public enum ConsoleState { Looking, Ready, Resting, NotFound }

public sealed record ConsoleStatus(RegisteredConsole Console, ConsoleState State, string Address, string RunningApp);

/// <summary>Asks the program which consoles there are and how each one is.</summary>
public sealed class ConsoleDirectory(IProgramRunner runner)
{
    /// <summary>The registered consoles; the error is the program's sentence.</summary>
    public async Task<(IReadOnlyList<RegisteredConsole> Consoles, string Error)> RegisteredAsync(CancellationToken cancel = default)
    {
        RunResult result = await runner.RunAsync(["consoles", "--json"], cancel).ConfigureAwait(false);
        return result.Ok ? (ConsoleJson.Registered(result.Output), "") : ([], result.Reason);
    }

    /// <summary>
    /// How each registered console is. One whose address is known is asked there (an answer to a
    /// question sent to everyone does not get through every firewall); the others are looked for
    /// with a question to the whole network.
    /// </summary>
    public async Task<IReadOnlyList<ConsoleStatus>> StatusAsync(IReadOnlyList<RegisteredConsole> consoles, CancellationToken cancel = default)
    {
        var asked = new List<Task<IReadOnlyList<FoundConsole>>>();
        foreach (string address in consoles.Select(c => c.LastAddress).Where(a => a.Length > 0).Distinct())
            asked.Add(DiscoverAsync(address, 1, cancel));
        if (consoles.Any(c => c.LastAddress.Length == 0)) asked.Add(DiscoverAsync(null, 2, cancel));
        var found = (await Task.WhenAll(asked).ConfigureAwait(false)).SelectMany(list => list).ToList();
        return consoles.Select(console =>
        {
            FoundConsole? answer = found.FirstOrDefault(f => f.HostId == console.HostId);
            if (answer is null) return new ConsoleStatus(console, ConsoleState.NotFound, console.LastAddress, "");
            return new ConsoleStatus(console, answer.Ready ? ConsoleState.Ready : ConsoleState.Resting, answer.Address, answer.RunningApp);
        }).ToList();
    }

    /// <summary>The consoles that answer: at one address, or anywhere on the network.</summary>
    public async Task<IReadOnlyList<FoundConsole>> DiscoverAsync(string? address, int waitSeconds, CancellationToken cancel = default)
    {
        var arguments = new List<string> { "discover", "--json", "--wait", waitSeconds.ToString() };
        if (!string.IsNullOrEmpty(address)) arguments.AddRange(["--host", address]);
        RunResult result = await runner.RunAsync(arguments, cancel).ConfigureAwait(false);
        // It prints the list also when nobody answered (and then ends with an error and a sentence).
        return ConsoleJson.Found(result.Output);
    }
}
