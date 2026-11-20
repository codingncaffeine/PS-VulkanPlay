using System.Text.Json;

namespace PSVulkanPlay.Core;

/// <summary>What is pressed on a controller, as what it means to an app that is moved through with it.</summary>
public enum ControllerPress { Up, Down, Left, Right, Accept, Back, PreviousPage, NextPage }

/// <summary>
/// What a controller's maker calls the buttons the app is moved through with. A PlayStation
/// controller's two are shapes (a cross and a circle), which are drawn, not written.
/// </summary>
public sealed record ButtonNames(bool Shapes, string Accept, string Back, string Previous, string Next)
{
    public static ButtonNames For(string kind) => kind switch
    {
        "dualsense" or "dualsense-edge" or "dualshock4" => new ButtonNames(true, "Cross", "Circle", "L1", "R1"),
        "nintendo" => new ButtonNames(false, "A", "B", "L", "R"),
        _ => new ButtonNames(false, "A", "B", "LB", "RB"),
    };
}

/// <summary>A controller that is connected, as the stream program sees it.</summary>
public sealed record ControllerInfo(string Name, string Kind, string Link, int Battery, string Power, string Haptics)
{
    public bool IsDualSense => Kind is "dualsense" or "dualsense-edge";
    public ButtonNames Buttons => ButtonNames.For(Kind);

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

    /// <summary>
    /// One line of 'controllers --watch --presses' that says a press: what, and which controller
    /// of the list last said. What is not such a line is nothing.
    /// </summary>
    public static (ControllerPress Press, int Controller)? ReadPress(string line)
    {
        try
        {
            using JsonDocument document = JsonDocument.Parse(line);
            if (document.RootElement.ValueKind != JsonValueKind.Object || !document.RootElement.TryGetProperty("press", out JsonElement name) ||
                name.ValueKind != JsonValueKind.String) return null;
            ControllerPress? press = name.GetString() switch
            {
                "up" => ControllerPress.Up,
                "down" => ControllerPress.Down,
                "left" => ControllerPress.Left,
                "right" => ControllerPress.Right,
                "accept" => ControllerPress.Accept,
                "back" => ControllerPress.Back,
                "previous" => ControllerPress.PreviousPage,
                "next" => ControllerPress.NextPage,
                _ => null,
            };
            if (press is null) return null;
            int controller = document.RootElement.TryGetProperty("controller", out JsonElement place) && place.TryGetInt32(out int at) ? at : 0;
            return (press.Value, controller);
        }
        catch (JsonException) { return null; }
    }

    private static string Text(JsonElement item, string name) =>
        item.TryGetProperty(name, out JsonElement value) && value.ValueKind == JsonValueKind.String ? value.GetString() ?? "" : "";
}
