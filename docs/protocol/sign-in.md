# Signing in with PSN, for the account ID

A console registers a client for one PSN account, named by a number ([registration.md](registration.md): the
account ID is that number as eight bytes, lowest first, in base64). A user does not know that number. It can
be learned from a login, which is all this client signs in for: play on the home network needs no sign-in,
and nothing but the account ID is kept.

**[read]** How the reference learns it (its `scripts/psn-account-id.py`); this client does the same:

1. The user logs in on the sign-in service's own page, opened with a client ID, a redirect address and the
   scope `psn:clientapp`:
   `https://auth.api.sonyentertainmentnetwork.com/2.0/oauth/authorize?service_entity=urn:service-entity:psn&response_type=code&client_id=…&redirect_uri=https://remoteplay.dl.playstation.net/remoteplay/redirect&scope=psn:clientapp&request_locale=en_US&ui=pr&service_logo=ps&layout_type=popup&smcid=remoteplay&prompt=always&PlatformPrivacyWs1=minimal&`
2. After the login the page sends the browser to the redirect address with `code=…` in its query: a one-time
   code. (The page at that address shows little more than the word "redirect".)
3. `POST https://auth.api.sonyentertainmentnetwork.com/2.0/oauth/token` with HTTP basic authentication (the
   client ID and its secret) and the form fields `grant_type=authorization_code`, `code` and `redirect_uri`
   answers with JSON whose `access_token` is a token.
4. `GET https://auth.api.sonyentertainmentnetwork.com/2.0/oauth/token/TOKEN`, with the same authentication,
   answers with JSON whose `user_id` is the account's number.

The client ID and secret are those of the console maker's own Remote Play program for Windows, which is how
every open client signs in; the service knows no other client that may ask for this.

This client (the app, `app/PSVulkanPlay/Core/PsnSignIn.cs`): the login page is shown in a window of its own
by the system's web engine, and the app learns only the address the page ends on; where there is no web
engine the page opens in the user's browser and the last address is taken from the clipboard or pasted. An
address that is not the redirect address is not taken for a sign-in. The password is typed into the service's
page and nowhere else, the token is used for the one request and dropped, and typing the account ID by hand
stays possible. **Not seen yet** with a real account: the checks run against a stand-in for the two requests.
