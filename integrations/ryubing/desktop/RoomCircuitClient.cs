// SPDX-License-Identifier: MIT
#nullable enable

using System;
using System.IO;
using System.Net;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Text.Json.Serialization.Metadata;
using System.Threading;
using System.Threading.Tasks;

namespace Ryujinx.Ava.UI.RoomCircuit;

internal sealed record RoomCircuitGame(string Game, string App, string Name, string? Zh);

internal sealed record RoomCircuitEndpoint(string Id, string Host, int Port, string Transport)
{
    // Stock RyuLDN uses a fixed TCP port and its server field contains a host, not a URL.
    public bool CanConfigure =>
        Id == "ryuldn" && Port == 30456 && string.Equals(Transport, "TCP", StringComparison.OrdinalIgnoreCase) &&
        (Uri.CheckHostName(Host) is UriHostNameType.Dns or UriHostNameType.IPv4) &&
        Host.Length <= 253;
}

internal sealed record RoomCircuitOptions(RoomCircuitGame[] Games, RoomCircuitEndpoint[] Endpoints,
    int ReadyBefore, int ReadyLease, int NativeApi);

internal sealed record RoomCircuitAppointment(string Id, long Starts, string Game, string App, int Interested, bool Ready);

internal sealed record RoomCircuitRoom(string Host, int Players, int Max, bool AppointmentBound, bool Crossplay, bool TitleConfirmed);

internal sealed record RoomCircuitResult(long Now, RoomCircuitAppointment[]? Rows,
    uint? Next, RoomCircuitRoom[]? Rooms, RoomCircuitAppointment? Created, string? Error);

internal sealed class RoomCircuitException(string code) : Exception(code);

internal static class RoomCircuitIdentity
{
    public static bool Valid(string? value) =>
        value?.Length == 32 && IsHex(value) && value.Trim('0').Length > 0;

    public static bool IsHex(string value)
    {
        foreach (char c in value)
        {
            if (!Uri.IsHexDigit(c))
            {
                return false;
            }
        }
        return true;
    }

    public static bool IsGame(string value) => value.Length == 16 && IsHex(value) &&
        value.Trim('0').Length > 0 && value.ToUpperInvariant() != "FFFFFFFFFFFFFFFF";

    public static string Create() => Convert.ToHexString(RandomNumberGenerator.GetBytes(16)).ToLowerInvariant();
}

internal sealed record RoomCircuitSettings(string Origin, string Identity, string[]? Followed = null)
{

    public static RoomCircuitSettings Load(string file)
    {
        try
        {
            if (new FileInfo(file).Length > 4096)
            {
                return new("", RoomCircuitIdentity.Create());
            }

            var stored = JsonSerializer.Deserialize(File.ReadAllText(file), RoomCircuitJson.Default.RoomCircuitSettings);
            if (stored is not null && RoomCircuitIdentity.Valid(stored.Identity))
            {
                return stored with { Origin = stored.Origin ?? "" };
            }
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException)
        {
            // The next successful save can replace an absent or unreadable preference.
        }

        return new("", RoomCircuitIdentity.Create());
    }

    public void Save(string file)
    {
        string tmp = file + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            File.WriteAllText(tmp, JsonSerializer.Serialize(this, RoomCircuitJson.Default.RoomCircuitSettings));
            File.Move(tmp, file, overwrite: true);
        }
        finally
        {
            File.Delete(tmp);
        }
    }
}

internal sealed class RoomCircuitClient
{
    internal const string NativePath = "api/appointments/native";
    private const int MaxResponse = 65536;
    private static readonly HttpClient Http = new(new HttpClientHandler
    {
        AllowAutoRedirect = false,
        UseCookies = false,
    }) { Timeout = TimeSpan.FromSeconds(8) };
    private readonly Uri _origin;
    private readonly string _identity;

    public RoomCircuitClient(Uri origin, string identity)
    {
        _origin = origin;
        _identity = identity;
    }

    public static bool TryOrigin(string text, out Uri? origin)
    {
        origin = null;
        if (text.Length > 512 || !Uri.TryCreate(text.Trim(), UriKind.Absolute, out Uri? value) ||
            value.UserInfo.Length != 0 || value.AbsolutePath != "/" || value.Query.Length != 0 ||
            value.Fragment.Length != 0 || value.Host.Length == 0 || value.Port is < 1 or > 65535)
        {
            return false;
        }

        if (value.Scheme != Uri.UriSchemeHttps && !(value.Scheme == Uri.UriSchemeHttp && PrivateHost(value)))
        {
            return false;
        }

        origin = value;
        return true;
    }

    private static bool PrivateHost(Uri uri)
    {
        if (uri.IsLoopback)
        {
            return true;
        }

        if (!IPAddress.TryParse(uri.Host, out IPAddress? address) || address.AddressFamily != AddressFamily.InterNetwork)
        {
            return false;
        }

        byte[] ip = address.GetAddressBytes();
        return ip[0] == 10 || ip[0] == 172 && ip[1] >= 16 && ip[1] <= 31 || ip[0] == 192 && ip[1] == 168;
    }

    public Task<RoomCircuitOptions> GetOptionsAsync(CancellationToken token) =>
        SendAsync(HttpMethod.Get, "api/appointments/options", null, RoomCircuitJson.Default.RoomCircuitOptions, token);

    public Task<RoomCircuitResult> ListAsync(uint offset, CancellationToken token) =>
        ActionAsync("list", token, ("offset", offset));

    public Task<RoomCircuitResult> EventAsync(string id, CancellationToken token) =>
        ActionAsync("event", token, ("id", id));

    public Task<RoomCircuitResult> RoomsAsync(string id, CancellationToken token) =>
        ActionAsync("rooms", token, ("id", id));

    public Task<RoomCircuitResult> SetReadyAsync(string id, bool ready, CancellationToken token) =>
        ActionAsync(ready ? "ready" : "leave", token, ("id", id));

    public Task<RoomCircuitResult> CreateAsync(long starts, string game, string? app, CancellationToken token) =>
        app is null ? ActionAsync("create", token, ("starts", starts), ("game", game)) :
            ActionAsync("create", token, ("starts", starts), ("game", game), ("app", app));

    private Task<RoomCircuitResult> ActionAsync(string action, CancellationToken token, params (string Key, object Value)[] fields)
    {
        using var bytes = new MemoryStream();
        using (var json = new Utf8JsonWriter(bytes))
        {
            json.WriteStartObject();
            json.WriteString("action", action);
            json.WriteString("identity", _identity);
            foreach (var field in fields)
            {
                switch (field.Value)
                {
                    case string text: json.WriteString(field.Key, text); break;
                    case uint offset: json.WriteNumber(field.Key, offset); break;
                    case long seconds: json.WriteNumber(field.Key, seconds); break;
                    default: throw new ArgumentException("Unknown appointment field");
                }
            }
            json.WriteEndObject();
        }
        return SendAsync(HttpMethod.Post, NativePath, bytes.ToArray(), RoomCircuitJson.Default.RoomCircuitResult, token);
    }

    private async Task<T> SendAsync<T>(HttpMethod method, string path, byte[]? body, JsonTypeInfo<T> type, CancellationToken token)
    {
        using var request = new HttpRequestMessage(method, new Uri(_origin, path));
        if (body is not null)
        {
            request.Content = new ByteArrayContent(body);
            request.Content.Headers.ContentType = new MediaTypeHeaderValue("application/json");
            request.Headers.Add("X-RoomCircuit-Client", "native-v1");
        }

        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(token);
        deadline.CancelAfter(TimeSpan.FromSeconds(8));
        using HttpResponseMessage response = await Http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, deadline.Token).ConfigureAwait(false);
        if (response.Content.Headers.ContentLength > MaxResponse ||
            response.Content.Headers.ContentType?.MediaType != "application/json")
        {
            throw new RoomCircuitException("invalid_response");
        }

        await using Stream stream = await response.Content.ReadAsStreamAsync(deadline.Token).ConfigureAwait(false);
        using var bytes = new MemoryStream();
        byte[] buffer = new byte[4096];
        for (int count = await stream.ReadAsync(buffer, deadline.Token).ConfigureAwait(false);
             count > 0;
             count = await stream.ReadAsync(buffer, deadline.Token).ConfigureAwait(false))
        {
            if (bytes.Length + count > MaxResponse)
            {
                throw new RoomCircuitException("invalid_response");
            }

            bytes.Write(buffer, 0, count);
        }

        if (!response.IsSuccessStatusCode)
        {
            var result = JsonSerializer.Deserialize(bytes.ToArray(), RoomCircuitJson.Default.RoomCircuitResult);
            throw new RoomCircuitException(result?.Error ?? "unavailable");
        }

        return JsonSerializer.Deserialize(bytes.ToArray(), type) ?? throw new RoomCircuitException("invalid_response");
    }
}

[JsonSourceGenerationOptions(PropertyNamingPolicy = JsonKnownNamingPolicy.CamelCase, PropertyNameCaseInsensitive = true)]
[JsonSerializable(typeof(RoomCircuitOptions))]
[JsonSerializable(typeof(RoomCircuitResult))]
[JsonSerializable(typeof(RoomCircuitSettings))]
internal sealed partial class RoomCircuitJson : JsonSerializerContext;
