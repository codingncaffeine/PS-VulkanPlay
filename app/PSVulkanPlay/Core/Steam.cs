using System.Text;
using SkiaSharp;

namespace PSVulkanPlay.Core;

/// <summary>
/// A map of Steam's binary key-value files (shortcuts.vdf): keys in the order they came, each a
/// string, a 32-bit number, or a map of its own.
/// </summary>
public sealed class VdfMap
{
    public List<KeyValuePair<string, object>> Items { get; } = [];

    /// <summary>The value of a key, its case not minded (Steam wrote "AppName" once, "appname" now).</summary>
    public object? Get(string key) => Items.FirstOrDefault(item => item.Key.Equals(key, StringComparison.OrdinalIgnoreCase)).Value;
    public string GetString(string key) => Get(key) as string ?? "";
    public void Add(string key, object value) => Items.Add(new(key, value));
}

/// <summary>
/// Steam's binary key-value format: a map is its entries and then 0x08; an entry is a type byte
/// (0x00 a map, 0x01 a string, 0x02 a 32-bit little-endian number), its key and its value, keys and
/// strings ending in a zero byte (UTF-8). The file itself is one map.
/// </summary>
public static class BinaryVdf
{
    private const byte MapType = 0x00, StringType = 0x01, NumberType = 0x02, End = 0x08;

    /// <summary>The file's map. A file not laid out so throws <see cref="FormatException"/>.</summary>
    public static VdfMap Parse(byte[] bytes)
    {
        int at = 0;
        VdfMap map = ReadMap(bytes, ref at, 0);
        return map;
    }

    private static VdfMap ReadMap(byte[] bytes, ref int at, int depth)
    {
        if (depth > 32) throw new FormatException("maps nested too deeply");
        var map = new VdfMap();
        while (true)
        {
            if (at >= bytes.Length)
            {
                // The outermost map may end with the file.
                if (depth == 0) return map;
                throw new FormatException("a map does not end");
            }
            byte type = bytes[at++];
            if (type == End) return map;
            string key = ReadString(bytes, ref at);
            switch (type)
            {
                case MapType: map.Add(key, ReadMap(bytes, ref at, depth + 1)); break;
                case StringType: map.Add(key, ReadString(bytes, ref at)); break;
                case NumberType:
                    if (at + 4 > bytes.Length) throw new FormatException("a number is cut off");
                    map.Add(key, BitConverter.ToUInt32(bytes, at));
                    at += 4;
                    break;
                default: throw new FormatException($"an entry of unknown type {type}");
            }
        }
    }

    private static string ReadString(byte[] bytes, ref int at)
    {
        int end = Array.IndexOf(bytes, (byte)0, at);
        if (end < 0) throw new FormatException("a string does not end");
        string text = Encoding.UTF8.GetString(bytes, at, end - at);
        at = end + 1;
        return text;
    }

    /// <summary>The bytes of a file holding the map (written as Steam writes it: the outermost map ends with 0x08).</summary>
    public static byte[] Write(VdfMap map)
    {
        using var out_ = new MemoryStream();
        WriteMap(out_, map);
        return out_.ToArray();
    }

    private static void WriteMap(Stream out_, VdfMap map)
    {
        foreach ((string key, object value) in map.Items)
        {
            switch (value)
            {
                case VdfMap inner: out_.WriteByte(MapType); WriteString(out_, key); WriteMap(out_, inner); break;
                case string text: out_.WriteByte(StringType); WriteString(out_, key); WriteString(out_, text); break;
                case uint number: out_.WriteByte(NumberType); WriteString(out_, key); out_.Write(BitConverter.GetBytes(number)); break;
                default: throw new ArgumentException($"{key}: a value of type {value.GetType().Name} has no place in the file");
            }
        }
        out_.WriteByte(End);
    }

    private static void WriteString(Stream out_, string text)
    {
        if (text.Contains('\0')) throw new ArgumentException("a string with a zero byte in it");
        out_.Write(Encoding.UTF8.GetBytes(text));
        out_.WriteByte(0);
    }
}

/// <summary>
/// A console in Steam's library as a non-Steam game: an entry in the shortcuts.vdf of the Steam
/// account used last, which starts a session with that console with the app's settings
/// (<c>ps-vulkanplay --play MAC</c>, <see cref="QuickPlay"/>), and its pictures in the account's
/// grid folder. Steam keeps its list in memory and writes it over the file when it changes it or
/// closes, so nothing is written while Steam runs: the user is asked to close it first.
/// </summary>
public sealed class SteamShortcuts
{
    private readonly Func<bool> _steamRunning;
    private readonly byte[] _banner, _icon;

    /// <param name="root">Steam's folder (the one with userdata in it).</param>
    /// <param name="program">What the entries run: the stream program, by its full path.</param>
    /// <param name="steamRunning">Whether Steam runs now.</param>
    /// <param name="banner">The square picture the library's pictures are made from (PNG).</param>
    /// <param name="icon">The app's icon (PNG).</param>
    public SteamShortcuts(string root, string program, Func<bool> steamRunning, byte[] banner, byte[] icon)
    {
        Root = root;
        Program = program;
        _steamRunning = steamRunning;
        _banner = banner;
        _icon = icon;
    }

    public string Root { get; }
    public string Program { get; }

    /// <summary>For the real app: this user's Steam, the app's own pictures, the process list asked; nothing when Steam is not installed.</summary>
    public static SteamShortcuts? ForThisUser(string programLocation)
    {
        if (FindRoot() is not { } root) return null;
        return new SteamShortcuts(root, programLocation.Length > 0 ? programLocation : ProgramRunner.BinaryName, IsSteamRunning, Resource("banner.png"), Resource("icon.png"));
    }

    /// <summary>One of the pictures inside the app (its project embeds them by these names).</summary>
    public static byte[] Resource(string name)
    {
        using Stream stream = typeof(SteamShortcuts).Assembly.GetManifestResourceStream(name) ?? throw new InvalidOperationException($"the app has no {name} inside");
        using var bytes = new MemoryStream();
        stream.CopyTo(bytes);
        return bytes.ToArray();
    }

    /// <summary>Where Steam is installed for this user, the first that has accounts: its own folder, the old link to it, the Flatpak's.</summary>
    public static string? FindRoot()
    {
        string home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile, Environment.SpecialFolderOption.DoNotVerify);
        string? data = Environment.GetEnvironmentVariable("XDG_DATA_HOME");
        if (string.IsNullOrEmpty(data) || !Path.IsPathRooted(data)) data = Path.Combine(home, ".local", "share");
        foreach (string candidate in new[] { Path.Combine(data, "Steam"), Path.Combine(home, ".steam", "steam"), Path.Combine(home, ".var", "app", "com.valvesoftware.Steam", ".local", "share", "Steam") })
        {
            if (Directory.Exists(Path.Combine(candidate, "userdata"))) return candidate;
        }
        return null;
    }

    /// <summary>Whether a Steam client runs for anybody on this machine (a process named steam: read from /proc, nothing is asked of it).</summary>
    public static bool IsSteamRunning()
    {
        try
        {
            foreach (string dir in Directory.EnumerateDirectories("/proc"))
            {
                if (!int.TryParse(Path.GetFileName(dir), out _)) continue;
                try
                {
                    if (File.ReadAllText(Path.Combine(dir, "comm")).Trim() == "steam") return true;
                }
                catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { }
            }
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { }
        return false;
    }

    /// <summary>The program's full path as Steam should start it: a plain name is looked up in the search path.</summary>
    public static string FullProgram(string program)
    {
        if (program.Contains('/')) return Path.GetFullPath(program);
        foreach (string dir in (Environment.GetEnvironmentVariable("PATH") ?? "").Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries))
        {
            string candidate = Path.Combine(dir, program);
            if (File.Exists(candidate)) return candidate;
        }
        return program;
    }

    /// <summary>The account Steam was used with last: of the numbered folders in userdata, the one whose settings changed last.</summary>
    public string? Account()
    {
        try
        {
            return Directory.EnumerateDirectories(Path.Combine(Root, "userdata"))
                            .Where(dir => uint.TryParse(Path.GetFileName(dir), out uint id) && id != 0)
                            .OrderByDescending(dir =>
                            {
                                string settings = Path.Combine(dir, "config", "localconfig.vdf");
                                return File.Exists(settings) ? File.GetLastWriteTimeUtc(settings) : Directory.GetLastWriteTimeUtc(dir);
                            })
                            .FirstOrDefault();
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { return null; }
    }

    private string? ListFile() => Account() is { } account ? Path.Combine(account, "config", "shortcuts.vdf") : null;

    /// <summary>What an entry starts the session with: the console by its hardware address (it stays when the console is renamed).</summary>
    public static string LaunchOptions(RegisteredConsole console) => $"--play {console.Mac}";

    /// <summary>The name in Steam's library.</summary>
    public static string Title(RegisteredConsole console) => $"{(console.Name.Length > 0 ? console.Name : console.Mac)} (PS VulkanPlay)";

    /// <summary>
    /// A shortcut's number: the CRC-32 of what it starts and its name, with the top bit set (what
    /// Steam takes as a shortcut's). Steam names the shortcut's pictures after it.
    /// </summary>
    public static uint ShortcutId(string exe, string title) => Crc32(Encoding.UTF8.GetBytes(exe + title)) | 0x80000000u;

    public bool Has(RegisteredConsole console)
    {
        try
        {
            return ListFile() is { } file && File.Exists(file) && FindEntry(Read(file), console) is not null;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or FormatException) { return false; }
    }

    /// <summary>Adds the console's entry (or makes it new) with its pictures. Says what went wrong, or nothing.</summary>
    public string Add(RegisteredConsole console) => Change(console, true);

    /// <summary>Takes the console's entry and its pictures away. Says what went wrong, or nothing.</summary>
    public string Remove(RegisteredConsole console) => Change(console, false);

    private string Change(RegisteredConsole console, bool add)
    {
        if (ListFile() is not { } file) return $"Steam has no account in {Root} yet: start Steam once and sign in.";
        if (_steamRunning()) return "Close Steam, then press again: Steam writes its list of games over any change made while it runs.";
        try
        {
            VdfMap root = File.Exists(file) ? Read(file) : new VdfMap();
            if (root.Get("shortcuts") is not VdfMap list)
            {
                list = new VdfMap();
                root.Items.RemoveAll(item => item.Key.Equals("shortcuts", StringComparison.OrdinalIgnoreCase));
                root.Add("shortcuts", list);
            }
            string grid = Path.Combine(Path.GetDirectoryName(file)!, "grid");
            // The entry there before, and its pictures, go (a new one is made in its place).
            if (FindEntry(root, console) is { } old)
            {
                if (old.Get("appid") is uint oldId) DeletePictures(grid, oldId);
                list.Items.RemoveAll(item => ReferenceEquals(item.Value, old));
            }
            if (add)
            {
                string exe = Quote(FullProgram(Program));
                string title = Title(console);
                uint id = ShortcutId(exe, title);
                Directory.CreateDirectory(grid);
                WritePictures(grid, id);
                list.Add("", Entry(console, exe, title, id, Path.Combine(grid, $"{id}_icon.png")));
            }
            // The first time, Steam's own file is kept beside it as it was.
            string kept = file + ".before-psvulkanplay";
            if (File.Exists(file) && !File.Exists(kept)) File.Copy(file, kept);
            Directory.CreateDirectory(Path.GetDirectoryName(file)!);
            Renumber(list);
            File.WriteAllBytes(file + ".new", BinaryVdf.Write(root));
            File.Move(file + ".new", file, true);
            return "";
        }
        catch (FormatException ex)
        {
            return $"Steam's list of non-Steam games ({file}) could not be read, so nothing was changed: {ex.Message}.";
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            return $"Steam's list of non-Steam games could not be written ({file}): {ex.Message}";
        }
    }

    private static VdfMap Read(string file) => BinaryVdf.Parse(File.ReadAllBytes(file));

    /// <summary>The console's entry: the one that starts this program with that console.</summary>
    private static VdfMap? FindEntry(VdfMap root, RegisteredConsole console)
    {
        if (root.Get("shortcuts") is not VdfMap list) return null;
        string options = LaunchOptions(console);
        return list.Items.Select(item => item.Value).OfType<VdfMap>().FirstOrDefault(entry =>
            entry.GetString("LaunchOptions") == options && entry.GetString("Exe").Contains(ProgramRunner.BinaryName, StringComparison.Ordinal));
    }

    /// <summary>Steam numbers the entries 0, 1, 2 ... in order: the list as it is, so numbered.</summary>
    private static void Renumber(VdfMap list)
    {
        for (int i = 0; i < list.Items.Count; ++i) list.Items[i] = new(i.ToString(System.Globalization.CultureInfo.InvariantCulture), list.Items[i].Value);
    }

    /// <summary>An entry as Steam writes one for a non-Steam game.</summary>
    private static VdfMap Entry(RegisteredConsole console, string exe, string title, uint id, string icon)
    {
        var entry = new VdfMap();
        entry.Add("appid", id);
        entry.Add("appname", title);
        entry.Add("Exe", exe);
        entry.Add("StartDir", Quote(Path.GetDirectoryName(exe.Trim('"')) + "/"));
        entry.Add("icon", icon);
        entry.Add("ShortcutPath", "");
        entry.Add("LaunchOptions", LaunchOptions(console));
        entry.Add("IsHidden", 0u);
        entry.Add("AllowDesktopConfig", 1u);
        entry.Add("AllowOverlay", 1u);
        entry.Add("OpenVR", 0u);
        entry.Add("Devkit", 0u);
        entry.Add("DevkitGameID", "");
        entry.Add("DevkitOverrideAppID", 0u);
        entry.Add("LastPlayTime", 0u);
        entry.Add("FlatpakAppID", "");
        entry.Add("sortas", "");
        entry.Add("tags", new VdfMap());
        return entry;
    }

    private static string Quote(string path) => $"\"{path}\"";

    // The library's pictures, by the names Steam looks for: the tall capsule (600x900), the wide one
    // (920x430), the hero behind the page (1920x620), the logo over it, and the icon.
    private static readonly (string Suffix, int Width, int Height)[] Pictures = [("p", 600, 900), ("", 920, 430), ("_hero", 1920, 620)];

    private static void DeletePictures(string grid, uint id)
    {
        foreach (string suffix in new[] { "p", "", "_hero", "_logo", "_icon" })
        {
            string path = Path.Combine(grid, $"{id}{suffix}.png");
            if (File.Exists(path)) File.Delete(path);
        }
    }

    private void WritePictures(string grid, uint id)
    {
        using SKBitmap banner = SKBitmap.Decode(_banner) ?? throw new IOException("the app's banner could not be read");
        foreach ((string suffix, int width, int height) in Pictures)
        {
            using var surface = SKSurface.Create(new SKImageInfo(width, height, SKColorType.Rgba8888, SKAlphaType.Premul));
            SKCanvas canvas = surface.Canvas;
            // The app's dark background, a little lighter at the top.
            using (var paint = new SKPaint())
            {
                paint.Shader = SKShader.CreateLinearGradient(new SKPoint(0, 0), new SKPoint(0, height), [new SKColor(34, 38, 46), new SKColor(20, 23, 28)], SKShaderTileMode.Clamp);
                canvas.DrawRect(0, 0, width, height, paint);
            }
            // The banner in the middle (the hero is left plain: the logo goes over it).
            if (suffix != "_hero")
            {
                float side = Math.Min(width, height) * 0.86f;
                var place = SKRect.Create((width - side) / 2, (height - side) / 2, side, side);
                using SKImage image = SKImage.FromBitmap(banner);
                canvas.DrawImage(image, place, new SKSamplingOptions(SKCubicResampler.Mitchell));
            }
            Save(surface, Path.Combine(grid, $"{id}{suffix}.png"));
        }
        File.WriteAllBytes(Path.Combine(grid, $"{id}_logo.png"), _icon);
        File.WriteAllBytes(Path.Combine(grid, $"{id}_icon.png"), _icon);
    }

    private static void Save(SKSurface surface, string path)
    {
        using SKImage image = surface.Snapshot();
        using SKData data = image.Encode(SKEncodedImageFormat.Png, 100);
        using FileStream file = File.Create(path + ".new");
        data.SaveTo(file);
        file.Close();
        File.Move(path + ".new", path, true);
    }

    /// <summary>CRC-32 (IEEE 802.3, reflected, as zlib's).</summary>
    public static uint Crc32(byte[] bytes)
    {
        uint crc = 0xFFFFFFFFu;
        foreach (byte b in bytes)
        {
            crc ^= b;
            for (int k = 0; k < 8; ++k) crc = (crc & 1) != 0 ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
        }
        return ~crc;
    }
}
