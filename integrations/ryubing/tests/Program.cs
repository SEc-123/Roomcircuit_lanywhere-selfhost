// SPDX-License-Identifier: MIT

using Avalonia;
using Avalonia.Headless;
using Avalonia.Themes.Fluent;
using Ryujinx.Ava.UI.RoomCircuit;
using System;
using System.IO;
using System.Linq;
using System.Threading;

namespace RoomCircuitNativeProbe;

internal sealed class ProbeApplication : Application
{
    public override void Initialize() => Styles.Add(new FluentTheme());
}

internal static class Program
{
    private static void Expect(bool valid, string explanation)
    {
        if (!valid)
        {
            throw new InvalidOperationException(explanation);
        }
    }

    public static int Main(string[] args)
    {
        var workspace = Path.Combine(Path.GetTempPath(), "roomcircuit-csharp-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(workspace);
        try
        {
            CheckSettings(workspace);
            AppBuilder.Configure<ProbeApplication>()
                .UseHeadless(new AvaloniaHeadlessPlatformOptions { UseHeadlessDrawing = true })
                .SetupWithoutStarting();
            var panel = new RoomCircuitWindow(Path.Combine(workspace, "panel.json"), _ => { }, "zh-CN");
            panel.Show();
            Avalonia.Threading.Dispatcher.UIThread.RunJobs();
            Expect(panel.IsVisible && panel.Title == "RoomCircuit", "Cannot construct the actual Avalonia panel");
            panel.Close();
            if (args.Length > 0)
            {
                CheckRelay(args[0]);
            }
            Console.WriteLine("RoomCircuit preferences, native Avalonia window and relay lifecycle passed.");
            return 0;
        }
        finally
        {
            Directory.Delete(workspace, recursive: true);
        }
    }

    private static void CheckSettings(string workspace)
    {
        Expect(RoomCircuitClient.TryOrigin("https://rooms.example.net", out _), "Valid HTTPS origin rejected");
        Expect(RoomCircuitClient.TryOrigin("http://192.168.0.1:11464", out _), "Valid private LAN origin rejected");
        Expect(!RoomCircuitClient.TryOrigin("http://rooms.example.net", out _), "Public cleartext origin accepted");
        Expect(!RoomCircuitClient.TryOrigin("https://member@rooms.example.net/", out _), "User-info accepted");
        Expect(!RoomCircuitClient.TryOrigin("https://rooms.example.net/appointments/", out _), "Sub-path origin accepted");
        string file = Path.Combine(workspace, "settings.json");
        string identity = RoomCircuitSettings.Load(file).Identity;
        new RoomCircuitSettings("http://localhost", identity, new[] { "42" }).Save(file);
        var stored = RoomCircuitSettings.Load(file);
        Expect(stored.Identity == identity && stored.Followed!.Single() == "42", "Follow or identity persistence failed");
    }

    private static void CheckRelay(string endpoint)
    {
        Expect(RoomCircuitClient.TryOrigin(endpoint, out var address), "Relay test origin is invalid");
        var client = new RoomCircuitClient(address!, RoomCircuitIdentity.Create());
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(40));
        CancellationToken token = timeout.Token;

        var options = client.GetOptionsAsync(token).GetAwaiter().GetResult();
        Expect(options.NativeApi == 1 && options.Games.Length > 0, "Native relay capability missing");
        var page = client.ListAsync(0, token).GetAwaiter().GetResult();
        var game = options.Games[0];
        var result = client.CreateAsync(page.Now + 600, game.Game, game.App, token).GetAwaiter().GetResult();
        string id = result.Created?.Id ?? throw new InvalidOperationException("Create did not return its precise appointment");
        Expect(client.EventAsync(id, token).GetAwaiter().GetResult().Rows!.Single().Id == id, "Exact event not found");
        Expect(client.SetReadyAsync(id, true, token).GetAwaiter().GetResult().Rows!.Single(row => row.Id == id).Ready,
            "Native ready did not update");
        Expect(client.RoomsAsync(id, token).GetAwaiter().GetResult().Rooms is not null, "Native room lookup absent");
        Expect(!client.SetReadyAsync(id, false, token).GetAwaiter().GetResult().Rows!.Single(row => row.Id == id).Ready,
            "Native clear did not update");
    }
}
