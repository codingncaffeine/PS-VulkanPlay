using System.Reflection;
using Avalonia.Controls;

namespace PSVulkanPlay.Platform;

/// <summary>
/// Tells the desktop which application a window belongs to, so the taskbar and the window switcher
/// show the app's own name and icon, from its desktop entry.
/// </summary>
/// <remarks>
/// On Wayland that is the toplevel's app id, which names the desktop entry; on X11 the entry's
/// <c>StartupWMClass</c> matches the window's class, and Avalonia also puts the icon on the window.
/// Avalonia 12.1's Wayland backend sends neither an app id nor an icon, so the app id is sent here:
/// on the backend's own Wayland thread, found by reflection on its internals. Should those change,
/// the window stays as it was.
/// </remarks>
internal static class DesktopIdentity
{
    // The same id as the stream window's and the desktop entry's: one application to the desktop.
    public const string AppId = "io.github.codingncaffeine.PSVulkanPlay";

    private const BindingFlags Members = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;

    /// <summary>Why the app id could not be sent, when it could not; for the diagnostics.</summary>
    public static string Problem { get; private set; } = "";

    /// <summary>Names the window's application. Call it each time the window opens: showing it again may make a new toplevel.</summary>
    public static void Apply(Window window)
    {
        try
        {
            object? impl = window.PlatformImpl;
            if (impl?.GetType().FullName != "Avalonia.Wayland.WindowImpl") return;

            object? proxy = Field(impl, "_surfaceProxy");
            object? target = Field(proxy, "_target");
            if (target is null || Field(proxy, "_marshaller") is not Delegate marshaller || marshaller.GetType().GetGenericArguments() is not [_, var priority])
            {
                Problem = "the window's Wayland toplevel was not found";
                return;
            }

            // The backend's marshaller runs the call on its Wayland thread, in order with its own requests.
            marshaller.DynamicInvoke((Action)(() => Send(target)), Enum.Parse(priority, "Normal"));
        }
        catch (Exception ex) when (ex is TargetInvocationException or ArgumentException or MemberAccessException or InvalidOperationException)
        {
            Problem = ex.Message;
        }
    }

    private static void Send(object target)
    {
        try
        {
            object? toplevel = Field(target, "_xdgTopLevel");
            if (toplevel?.GetType().GetMethod("SetAppId", [typeof(string)]) is not { } setAppId)
            {
                Problem = "the toplevel has no app id request";
                return;
            }
            setAppId.Invoke(toplevel, [AppId]);
        }
        catch (Exception ex) when (ex is TargetInvocationException or ArgumentException or MemberAccessException)
        {
            Problem = ex.Message;
        }
    }

    private static object? Field(object? owner, string name)
    {
        for (Type? type = owner?.GetType(); type is not null; type = type.BaseType)
            if (type.GetField(name, Members) is { } field) return field.GetValue(owner);
        return null;
    }
}
