using System.Runtime.InteropServices;

namespace PSVulkanPlay.Platform;

internal static class NativeLibs
{
    [DllImport("libc", EntryPoint = "dlopen")] private static extern IntPtr dlopen_c(string path, int flags);
    [DllImport("libdl.so.2", EntryPoint = "dlopen")] private static extern IntPtr dlopen_dl(string path, int flags);
    private const int RTLD_NOW = 2;

    /// <summary>
    /// Loads the app's own HarfBuzz (libHarfBuzzSharp.so, the text shaper) before anything else,
    /// with every call it makes resolved at once.
    ///
    /// That library offers the same hb_* names as the system's libharfbuzz and reaches its own
    /// functions through them. FreeType 2.14 and later opens the system's libharfbuzz for the
    /// whole process the first time fontconfig has it read a font file (a font the font cache has
    /// not seen yet is enough). A libHarfBuzzSharp.so loaded after that, or loaded earlier with its
    /// calls still to be resolved at first use, has them resolved to the system's copy: another
    /// version with other structures, and the app dies in hb_font_create while its window opens.
    /// Loaded first and resolved in full, there is no other copy to resolve to, and the runtime's
    /// own load later finds this one. The runtime itself asks for resolution at first use, which
    /// is why the load is made here, through dlopen.
    /// </summary>
    public static void BindHarfBuzzToItself()
    {
        if (!OperatingSystem.IsLinux()) return;
        // Where the runtime will look, in its order: the folders the host worked out from the
        // app's dependency list, then the app's own.
        var folders = new List<string>();
        if (AppContext.GetData("NATIVE_DLL_SEARCH_DIRECTORIES") is string listed)
            folders.AddRange(listed.Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries));
        folders.Add(AppContext.BaseDirectory);
        foreach (string folder in folders)
        {
            string path = Path.Combine(folder, "libHarfBuzzSharp.so");
            if (!File.Exists(path)) continue;
            IntPtr handle = IntPtr.Zero;
            try { handle = dlopen_c(path, RTLD_NOW); }
            catch (EntryPointNotFoundException)
            {
                // glibc before 2.34 keeps dlopen in libdl.
                try { handle = dlopen_dl(path, RTLD_NOW); }
                catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException) { }
            }
            catch (DllNotFoundException) { }
            HarfBuzzNote = handle != IntPtr.Zero
                ? $"text shaper: own HarfBuzz opened first ({path})"
                : $"text shaper: own HarfBuzz could NOT be opened first ({path})";
            return;
        }
    }

    /// <summary>What <see cref="BindHarfBuzzToItself"/> did, for the diagnostics.</summary>
    public static string HarfBuzzNote { get; private set; } = "text shaper: own HarfBuzz not found beside the app";
}
