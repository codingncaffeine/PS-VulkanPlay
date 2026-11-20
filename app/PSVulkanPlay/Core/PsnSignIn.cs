using System.Buffers.Binary;
using System.Net.Http.Headers;
using System.Text;
using System.Text.Json;

namespace PSVulkanPlay.Core;

/// <summary>
/// Signing in with PSN, for one thing only: the number of the account, which a console wants
/// when a computer registers with it. The user logs in on Sony's own page; that page then sends
/// the browser to an address that carries a one-time code; the code is exchanged for the
/// account's number. Nothing else is asked for and nothing but the account ID is kept: no
/// password is ever seen by this program, and no token is stored.
///
/// The sign-in service knows a client by an ID and a secret. The ones used are those of Sony's
/// own Remote Play program for Windows, as every open Remote Play client uses them: there is no
/// other way to learn the account's number from a login. Typing the account ID by hand stays
/// possible, and uses none of this.
/// </summary>
public sealed class PsnSignIn(HttpMessageHandler? handler = null, string? service = null)
{
    public const string RedirectAddress = "https://remoteplay.dl.playstation.net/remoteplay/redirect";
    private const string Service = "https://auth.api.sonyentertainmentnetwork.com/2.0/oauth";
    private const string ClientId = "ba495a24-818c-472b-b12d-ff231c1b5745";
    private const string ClientSecret = "mvaiZkRsAsI1IBkY";

    private readonly string _service = (service ?? Service).TrimEnd('/');

    /// <summary>The page the user logs in on.</summary>
    public Uri LoginPage => new(_service + "/authorize?service_entity=urn:service-entity:psn&response_type=code&client_id=" + ClientId +
                                "&redirect_uri=" + RedirectAddress + "&scope=psn:clientapp&request_locale=en_US&ui=pr&service_logo=ps&layout_type=popup" +
                                "&smcid=remoteplay&prompt=always&PlatformPrivacyWs1=minimal&");

    /// <summary>
    /// The one-time code in the address the login page sent the browser to; nothing when the text
    /// is not that address or carries no code. Takes what a person pastes: spaces around it, and
    /// an address without its scheme.
    /// </summary>
    public static string? CodeFrom(string address)
    {
        string text = address.Trim();
        if (!text.Contains("://", StringComparison.Ordinal)) text = "https://" + text;
        if (!Uri.TryCreate(text, UriKind.Absolute, out Uri? uri)) return null;
        if (!(uri.GetLeftPart(UriPartial.Path)).Equals(RedirectAddress, StringComparison.OrdinalIgnoreCase)) return null;
        foreach (string pair in uri.Query.TrimStart('?').Split('&', StringSplitOptions.RemoveEmptyEntries))
        {
            int equals = pair.IndexOf('=');
            if (equals <= 0 || pair[..equals] != "code") continue;
            string code = Uri.UnescapeDataString(pair[(equals + 1)..]);
            return code.Length > 0 ? code : null;
        }
        return null;
    }

    /// <summary>The account ID as a console takes it: the account's number as eight bytes, lowest first, in base64.</summary>
    public static string AccountIdFrom(ulong number)
    {
        Span<byte> bytes = stackalloc byte[8];
        BinaryPrimitives.WriteUInt64LittleEndian(bytes, number);
        return Convert.ToBase64String(bytes);
    }

    /// <summary>Exchanges the code for the account ID. The error is a sentence for the user.</summary>
    public async Task<(string AccountId, string Error)> AccountIdForAsync(string code, CancellationToken cancel = default)
    {
        using var client = handler is null ? new HttpClient() : new HttpClient(handler, false);
        client.Timeout = TimeSpan.FromSeconds(20);
        var basic = new AuthenticationHeaderValue("Basic", Convert.ToBase64String(Encoding.ASCII.GetBytes(ClientId + ":" + ClientSecret)));
        try
        {
            using var ask = new HttpRequestMessage(HttpMethod.Post, _service + "/token")
            {
                Content = new FormUrlEncodedContent(new Dictionary<string, string>
                {
                    ["grant_type"] = "authorization_code", ["code"] = code, ["redirect_uri"] = RedirectAddress,
                }),
            };
            ask.Headers.Authorization = basic;
            using HttpResponseMessage answer = await client.SendAsync(ask, cancel).ConfigureAwait(false);
            string body = await answer.Content.ReadAsStringAsync(cancel).ConfigureAwait(false);
            if (!answer.IsSuccessStatusCode)
                return ("", $"The sign-in service did not take the sign-in (it answered {(int)answer.StatusCode}). A sign-in is good for a short time and once only: sign in again.");
            string? token = Member(body, "access_token");
            if (string.IsNullOrEmpty(token)) return ("", "The sign-in service's answer was not the one expected (it carries no token). Sign in again, or type the account ID.");

            using var who = new HttpRequestMessage(HttpMethod.Get, _service + "/token/" + Uri.EscapeDataString(token));
            who.Headers.Authorization = basic;
            using HttpResponseMessage account = await client.SendAsync(who, cancel).ConfigureAwait(false);
            body = await account.Content.ReadAsStringAsync(cancel).ConfigureAwait(false);
            if (!account.IsSuccessStatusCode) return ("", $"The sign-in service did not say whose sign-in this is (it answered {(int)account.StatusCode}). Sign in again, or type the account ID.");
            return ulong.TryParse(Member(body, "user_id"), out ulong number) && number != 0
                ? (AccountIdFrom(number), "")
                : ("", "The sign-in service's answer carries no account number. Sign in again, or type the account ID.");
        }
        catch (HttpRequestException ex)
        {
            return ("", $"The sign-in service could not be reached: {ex.Message}");
        }
        catch (TaskCanceledException) when (!cancel.IsCancellationRequested)
        {
            return ("", "The sign-in service did not answer in time. Check the connection to the internet and sign in again.");
        }
    }

    // A member of a JSON object as text, whether it is written as a string or as a number.
    private static string? Member(string json, string name)
    {
        try
        {
            using JsonDocument document = JsonDocument.Parse(json);
            if (document.RootElement.ValueKind != JsonValueKind.Object || !document.RootElement.TryGetProperty(name, out JsonElement value)) return null;
            return value.ValueKind switch
            {
                JsonValueKind.String => value.GetString(),
                JsonValueKind.Number => value.GetRawText(),
                _ => null,
            };
        }
        catch (JsonException) { return null; }
    }
}

/// <summary>What came of showing the user the login page.</summary>
/// <param name="Address">Where the page sent the browser in the end; empty when the user closed it.</param>
/// <param name="Unavailable">Not empty: no window for the page could be opened here, and why.</param>
public sealed record SignInAnswer(string Address, string Unavailable = "");

/// <summary>Shows the login page in a window of its own and says where it led.</summary>
public interface ISignInWindow
{
    Task<SignInAnswer> ShowAsync(Uri loginPage, string redirectAddress);
}
