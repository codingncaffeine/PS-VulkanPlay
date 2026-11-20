using System.Net;
using System.Text;
using PSVulkanPlay.Core;
using Xunit;

namespace PSVulkanPlay.Tests;

/// <summary>A stand-in for the sign-in service: keeps what it was asked and answers as told.</summary>
public sealed class FakeService : HttpMessageHandler
{
    public List<(string Method, string Url, string Authorization, string Body)> Asked { get; } = [];
    public Func<HttpRequestMessage, string, HttpResponseMessage> Answer { get; set; } = (request, _) =>
        request.Method == HttpMethod.Post
            ? Json("""{"access_token":"tok/en+1","token_type":"bearer","expires_in":3599}""")
            : Json("""{"scopes":"psn:clientapp","user_id":"1234567890123","client_id":"x"}""");

    public static HttpResponseMessage Json(string body, HttpStatusCode status = HttpStatusCode.OK) =>
        new(status) { Content = new StringContent(body, Encoding.UTF8, "application/json") };

    protected override async Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancel)
    {
        string body = request.Content is null ? "" : await request.Content.ReadAsStringAsync(cancel);
        Asked.Add((request.Method.Method, request.RequestUri!.AbsoluteUri, request.Headers.Authorization?.ToString() ?? "", body));
        return Answer(request, body);
    }
}

public class SignInTests
{
    [Fact]
    public void The_account_id_is_the_number_as_eight_bytes_lowest_first()
    {
        // The pair the stream program's own tests use for the same account.
        Assert.Equal("ywT7cR8BAAA=", PsnSignIn.AccountIdFrom(1234567890123));
        Assert.Equal("AQAAAAAAAAA=", PsnSignIn.AccountIdFrom(1));
        Assert.Equal("//////////8=", PsnSignIn.AccountIdFrom(ulong.MaxValue));
    }

    [Theory]
    [InlineData("https://remoteplay.dl.playstation.net/remoteplay/redirect?code=AbC123&cid=00000000-0000", "AbC123")]
    [InlineData("  https://remoteplay.dl.playstation.net/remoteplay/redirect?cid=1&code=x%2Fy%3D \n", "x/y=")]
    [InlineData("remoteplay.dl.playstation.net/remoteplay/redirect?code=bare", "bare")]
    public void The_code_is_read_from_the_address_the_login_led_to(string address, string code) =>
        Assert.Equal(code, PsnSignIn.CodeFrom(address));

    [Theory]
    [InlineData("")]
    [InlineData("not an address")]
    [InlineData("https://remoteplay.dl.playstation.net/remoteplay/redirect")]
    [InlineData("https://remoteplay.dl.playstation.net/remoteplay/redirect?code=")]
    [InlineData("https://remoteplay.dl.playstation.net/remoteplay/redirect?error=access_denied")]
    [InlineData("https://example.com/remoteplay/redirect?code=stolen")]
    [InlineData("https://remoteplay.dl.playstation.net.example.com/remoteplay/redirect?code=stolen")]
    [InlineData("https://auth.api.sonyentertainmentnetwork.com/2.0/oauth/authorize?code=wrong-page")]
    public void An_address_that_is_not_the_logins_last_one_gives_no_code(string address) =>
        Assert.Null(PsnSignIn.CodeFrom(address));

    [Fact]
    public void The_login_page_is_the_services_own_with_the_redirect_address_in_it()
    {
        Uri page = new PsnSignIn().LoginPage;
        Assert.Equal("auth.api.sonyentertainmentnetwork.com", page.Host);
        Assert.Equal("https", page.Scheme);
        Assert.Contains("response_type=code", page.Query);
        Assert.Contains("redirect_uri=" + PsnSignIn.RedirectAddress, page.Query);
        Assert.Contains("scope=psn:clientapp", page.Query);
    }

    [Fact]
    public async Task The_code_is_exchanged_for_the_account_id_in_two_requests()
    {
        var service = new FakeService();
        var (id, error) = await new PsnSignIn(service, "https://service.test/oauth").AccountIdForAsync("the code/1", TestContext.Current.CancellationToken);
        Assert.Equal("", error);
        Assert.Equal("ywT7cR8BAAA=", id);
        Assert.Equal(2, service.Asked.Count);
        Assert.Equal(("POST", "https://service.test/oauth/token"), (service.Asked[0].Method, service.Asked[0].Url));
        Assert.StartsWith("Basic ", service.Asked[0].Authorization);
        Assert.Contains("grant_type=authorization_code", service.Asked[0].Body);
        Assert.Contains("code=the+code%2F1", service.Asked[0].Body);
        Assert.Contains("redirect_uri=https%3A%2F%2Fremoteplay.dl.playstation.net%2Fremoteplay%2Fredirect", service.Asked[0].Body);
        // The token is part of the second address, written so that it stays one part of it.
        Assert.Equal(("GET", "https://service.test/oauth/token/tok%2Fen%2B1"), (service.Asked[1].Method, service.Asked[1].Url));
        Assert.Equal(service.Asked[0].Authorization, service.Asked[1].Authorization);
    }

    [Fact]
    public async Task A_number_written_as_a_number_is_taken_too()
    {
        var service = new FakeService();
        service.Answer = (request, _) => request.Method == HttpMethod.Post ? FakeService.Json("""{"access_token":"t"}""") : FakeService.Json("""{"user_id":1}""");
        var (id, error) = await new PsnSignIn(service, "https://service.test/oauth").AccountIdForAsync("c", TestContext.Current.CancellationToken);
        Assert.Equal(("AQAAAAAAAAA=", ""), (id, error));
    }

    [Fact]
    public async Task What_goes_wrong_is_said_in_a_sentence_and_gives_no_id()
    {
        var refused = new FakeService { Answer = (_, _) => FakeService.Json("""{"error":"invalid_grant"}""", HttpStatusCode.BadRequest) };
        var (id, error) = await new PsnSignIn(refused, "https://service.test/oauth").AccountIdForAsync("used", TestContext.Current.CancellationToken);
        Assert.Equal("", id);
        Assert.Contains("answered 400", error);
        Assert.Single(refused.Asked);   // no second request without a token

        var odd = new FakeService { Answer = (_, _) => FakeService.Json("<html>maintenance</html>") };
        (id, error) = await new PsnSignIn(odd, "https://service.test/oauth").AccountIdForAsync("c", TestContext.Current.CancellationToken);
        Assert.Equal("", id);
        Assert.Contains("carries no token", error);

        var nobody = new FakeService();
        nobody.Answer = (request, _) => request.Method == HttpMethod.Post ? FakeService.Json("""{"access_token":"t"}""") : FakeService.Json("""{"user_id":"none"}""");
        (id, error) = await new PsnSignIn(nobody, "https://service.test/oauth").AccountIdForAsync("c", TestContext.Current.CancellationToken);
        Assert.Equal("", id);
        Assert.Contains("no account number", error);

        var down = new FakeService { Answer = (_, _) => throw new HttpRequestException("Name or service not known") };
        (id, error) = await new PsnSignIn(down, "https://service.test/oauth").AccountIdForAsync("c", TestContext.Current.CancellationToken);
        Assert.Equal("", id);
        Assert.StartsWith("The sign-in service could not be reached", error);
    }
}
