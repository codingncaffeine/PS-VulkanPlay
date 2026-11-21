using System.Text.Json;

namespace PSVulkanPlay.Core;

/// <summary>What the program says of this computer (<c>diagnose --json</c>).</summary>
public sealed record DiagnosticsFacts(
    string Version, string VulkanLoader, string VulkanError, IReadOnlyList<string> Devices, string GpuDecoding, string GpuDecodingWhyNot,
    string Ffmpeg, string Sdl, string LastSessionStarted, IReadOnlyList<string> LastSession, string LogsFolder)
{
    /// <summary>The one line of JSON the program writes, or nothing when it is not that.</summary>
    public static DiagnosticsFacts? Read(string json)
    {
        try
        {
            using JsonDocument document = JsonDocument.Parse(json);
            JsonElement root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object || !root.TryGetProperty("version", out _)) return null;
            string Text(string name) => root.TryGetProperty(name, out JsonElement value) && value.ValueKind == JsonValueKind.String ? value.GetString() ?? "" : "";
            var devices = new List<string>();
            if (root.TryGetProperty("devices", out JsonElement list) && list.ValueKind == JsonValueKind.Array)
            {
                foreach (JsonElement device in list.EnumerateArray())
                {
                    string Of(string name) => device.TryGetProperty(name, out JsonElement value) ? value.GetString() ?? "" : "";
                    devices.Add($"{Of("name")} ({Of("kind")}, Vulkan {Of("vulkan")})");
                }
            }
            var session = new List<string>();
            if (root.TryGetProperty("last_session", out JsonElement lines) && lines.ValueKind == JsonValueKind.Array)
            {
                foreach (JsonElement line in lines.EnumerateArray()) session.Add(line.GetString() ?? "");
            }
            return new DiagnosticsFacts(Text("version"), Text("vulkan_loader"), Text("vulkan_error"), devices, Text("gpu_decoding"), Text("gpu_decoding_why_not"),
                                        Text("ffmpeg"), Text("sdl"), Text("last_session_started"), session, Text("logs_folder"));
        }
        catch (JsonException)
        {
            return null;
        }
    }
}
