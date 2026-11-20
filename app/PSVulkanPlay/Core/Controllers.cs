using System.Text.Json;

namespace PSVulkanPlay.Core;

/// <summary>A controller that is connected, as the stream program sees it.</summary>
public sealed record ControllerInfo(string Name, string Kind, string Link, int Battery, string Power, string Haptics)
{
    public bool IsDualSense => Kind is "dualsense" or "dualsense-edge";

    /// <summary>"DualSense Edge Wireless Controller · on a cable · 55%, charging".</summary>
    public string Line
    {
        get
        {
            var parts = new List<string> { Name.Length > 0 ? Name : "A controller" };
            if (Link == "cable") parts.Add("on a cable");
            else if (Link == "wireless") parts.Add("wireless");
            string power = Power switch { "charging" => "charging", "full" => "full", _ => "" };
            if (Battery >= 0) parts.Add(power.Length > 0 ? $"{Battery}%, {power}" : $"{Battery}%");
            else if (power.Length > 0) parts.Add(power);
            return string.Join("  ·  ", parts);
        }
    }

    /// <summary>What a session can do with it that a plain controller cannot; empty for one that has nothing to say.</summary>
    public string Abilities => Haptics switch
    {
        "sound-device" => "Haptics and trigger effects",
        "rumble" => "Trigger effects; haptics as rumble (the full haptics need the cable)",
        _ => "",
    };

    /// <summary>The battery is low enough to say so: on its battery and at 15% or under.</summary>
    public bool BatteryLow => Battery is >= 0 and <= 15 && Power == "battery";
}

public static class ControllerJson
{
    /// <summary>One line of 'controllers --json'. What is not such a line is nothing (null), not an empty list.</summary>
    public static IReadOnlyList<ControllerInfo>? Read(string line)
    {
        try
        {
            using JsonDocument document = JsonDocument.Parse(line);
            if (document.RootElement.ValueKind != JsonValueKind.Object || !document.RootElement.TryGetProperty("controllers", out JsonElement list) ||
                list.ValueKind != JsonValueKind.Array) return null;
            var controllers = new List<ControllerInfo>();
            foreach (JsonElement item in list.EnumerateArray())
            {
                if (item.ValueKind != JsonValueKind.Object) continue;
                controllers.Add(new ControllerInfo(Text(item, "name"), Text(item, "kind"), Text(item, "link"),
                                                   item.TryGetProperty("battery", out JsonElement battery) && battery.TryGetInt32(out int percent) ? percent : -1,
                                                   Text(item, "power"), Text(item, "haptics")));
            }
            return controllers;
        }
        catch (JsonException) { return null; }
    }

    private static string Text(JsonElement item, string name) =>
        item.TryGetProperty(name, out JsonElement value) && value.ValueKind == JsonValueKind.String ? value.GetString() ?? "" : "";
}
