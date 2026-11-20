using Avalonia.Controls;
using PSVulkanPlay.Core;

namespace PSVulkanPlay.Platform;

/// <summary>
/// The login page in a window of its own, drawn by the system's web engine (WebKitGTK on Linux).
/// The page is Sony's; this app only learns the address it ends on. Where there is no web engine
/// to draw it, it says so, and the sign-in goes through the user's own browser instead.
/// </summary>
internal sealed class SignInWindow(TopLevel owner) : ISignInWindow
{
    public async Task<SignInAnswer> ShowAsync(Uri loginPage, string redirectAddress)
    {
        if (!OperatingSystem.IsLinux() && !OperatingSystem.IsWindows() && !OperatingSystem.IsMacOS())
            return new SignInAnswer("", "no web engine on this system");
        try
        {
            var options = new WebAuthenticatorOptions(loginPage, new Uri(redirectAddress))
            {
                Mode = WebAuthenticatorMode.NativeWebDialog,
                // Nothing of the sign-in is left in the web engine's own storage afterwards.
                NonPersistent = true,
            };
            WebAuthenticationResult result = await WebAuthenticationBroker.AuthenticateAsync(owner, options);
            return new SignInAnswer(result.CallbackUri?.AbsoluteUri ?? "");
        }
        catch (OperationCanceledException)
        {
            return new SignInAnswer("");
        }
        catch (Exception ex) when (ex is PlatformNotSupportedException or DllNotFoundException or EntryPointNotFoundException or InvalidOperationException or NotSupportedException or TypeInitializationException)
        {
            return new SignInAnswer("", ex.Message);
        }
    }
}
