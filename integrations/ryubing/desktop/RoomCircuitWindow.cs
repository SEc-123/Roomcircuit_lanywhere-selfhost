// SPDX-License-Identifier: MIT
#nullable enable

using Avalonia;
using Avalonia.Controls;
using Avalonia.Input.Platform;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Threading;
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Net.Http;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;

namespace Ryujinx.Ava.UI.RoomCircuit;

internal sealed class RoomCircuitWindow : Window
{
    private readonly string _settingsFile;
    private readonly Action<string> _applyRelay;
    private readonly bool _chinese;
    private readonly CancellationTokenSource _lifetime = new();
    private readonly DispatcherTimer _timer = new() { Interval = TimeSpan.FromSeconds(30) };
    private readonly HashSet<string> _seenOpenRooms = new(StringComparer.Ordinal);
    private RoomCircuitSettings _settings;
    private RoomCircuitClient? _client;
    private RoomCircuitOptions? _options;
    private RoomCircuitAppointment? _current;
    private readonly List<uint> _offsets = new() { 0 };
    private uint? _next;
    private long _serverTime;
    private long _receivedAt;
    private bool _busy;
    private bool _closed;
    private bool _checkingFollows;

    private readonly TextBox _origin = new() { MinWidth = 240, MaxLength = 512 };
    private readonly ComboBox _game = new() { MinWidth = 220 };
    private readonly TextBox _gameId = new() { MinWidth = 180, MaxLength = 16 };
    private readonly TextBox _appId = new() { MinWidth = 180, MaxLength = 16 };
    private readonly DatePicker _date = new();
    private readonly TimePicker _time = new();
    private readonly StackPanel _manual = new() { Spacing = 8 };
    private readonly ListBox _appointments = new() { MinHeight = 108, MaxHeight = 250 };
    private readonly StackPanel _rooms = new() { Spacing = 4 };
    private readonly StackPanel _createForm = new() { Spacing = 8 };
    private readonly TextBlock _selection = new() { TextWrapping = TextWrapping.Wrap };
    private readonly TextBlock _feedback = new() { TextWrapping = TextWrapping.Wrap };
    private readonly TextBlock _endpoint = new() { TextWrapping = TextWrapping.Wrap };
    private readonly Button _ready;
    private readonly Button _follow;
    private readonly Button _find;
    private readonly Button _share;
    private readonly Button _previous;
    private readonly Button _more;
    private readonly Button _configure;
    private readonly Button _create;

    public RoomCircuitWindow(string settingsFile, Action<string> applyRelay, string language)
    {
        _settingsFile = settingsFile;
        _applyRelay = applyRelay;
        _chinese = language.StartsWith("zh", StringComparison.OrdinalIgnoreCase);
        _settings = RoomCircuitSettings.Load(settingsFile);
        _origin.Text = _settings.Origin;
        _gameId.PlaceholderText = T("Local communication ID (16 hex digits)", "本地联机 ID（16 位十六进制数）");
        _appId.PlaceholderText = T("Application ID (optional)", "应用 ID（可选）");
        _origin.PlaceholderText = "https://rooms.example.net";
        DateTimeOffset initial = DateTimeOffset.Now.AddMinutes(30);
        initial = initial.AddSeconds(-initial.Second).AddMilliseconds(-initial.Millisecond);
        _date.SelectedDate = initial;
        _time.SelectedTime = initial.TimeOfDay;

        Title = "RoomCircuit";
        Width = 670;
        Height = 740;
        MinWidth = 450;
        MinHeight = 450;
        WindowStartupLocation = WindowStartupLocation.CenterOwner;

        _ready = Button(T("I'm ready", "我已就绪"), SetReadyAsync);
        _follow = Button(T("Follow", "关注"), ToggleFollowAsync);
        _find = Button(T("Find rooms", "找房"), FindRoomsAsync);
        _share = Button(T("Copy link", "复制链接"), CopyLinkAsync);
        _previous = Button(T("Previous", "上一页"), PreviousAsync);
        _more = Button(T("Next", "下一页"), MoreAsync);
        _create = Button(T("Create", "创建"), CreateAsync);
        _configure = Button(T("Use this relay", "使用此中继"), ConfigureAsync);
        Button refresh = Button(T("Refresh", "刷新"), RefreshAsync);
        Button connect = Button(T("Connect", "连接"), ConnectAsync);

        _manual.Children.Add(Wrap(_gameId, _appId));
        _game.SelectionChanged += (_, _) => _manual.IsVisible = _game.SelectedIndex == 0;
        _createForm.Children.Add(Wrap(Label(T("New appointment", "新建约战")), _game));
        _createForm.Children.Add(_manual);
        _createForm.Children.Add(Wrap(Label(T("Local start time", "本地开始时间")), _date, _time, _create));
        _appointments.SelectionChanged += (_, _) =>
        {
            if (_appointments.SelectedItem is Row selected)
            {
                _current = selected.Event;
                _rooms.Children.Clear();
            }
            UpdateActions();
        };

        Content = new ScrollViewer
        {
            Padding = new Thickness(20),
            Content = new StackPanel
            {
                Spacing = 14,
                Children =
                {
                    new TextBlock { Text = T("Appointments", "约战"), FontSize = 22, FontWeight = FontWeight.SemiBold },
                    Wrap(Label(T("Service address", "约战服务地址")), _origin, connect, refresh),
                    Wrap(_endpoint, _configure),
                    _createForm,
                    _appointments,
                    Wrap(_previous, _more),
                    _selection,
                    Wrap(_ready, _follow, _find, _share),
                    _rooms,
                    _feedback,
                },
            },
        };
        _timer.Tick += async (_, _) => await CheckFollowedRoomsAsync();
        Opened += async (_, _) =>
        {
            _timer.Start();
            if (!string.IsNullOrWhiteSpace(_origin.Text))
            {
                await RunAsync(ConnectAsync);
            }
        };
        Closed += (_, _) =>
        {
            _closed = true;
            _timer.Stop();
            _lifetime.Cancel();
            _lifetime.Dispose();
        };
        UpdateActions();
    }

    private string T(string en, string zh) => _chinese ? zh : en;

    private static TextBlock Label(string value) => new()
    {
        Text = value,
        VerticalAlignment = VerticalAlignment.Center,
    };

    private static WrapPanel Wrap(params Control[] items)
    {
        var result = new WrapPanel();
        foreach (Control item in items)
        {
            item.Margin = new Thickness(0, 0, 8, 5);
            result.Children.Add(item);
        }
        return result;
    }

    private Button Button(string title, Func<Task> action)
    {
        var control = new Button { Content = title };
        control.Click += async (_, _) => await RunAsync(action);
        return control;
    }

    private IReadOnlyList<string> Followed => _settings.Followed ?? Array.Empty<string>();
    private long Now => _serverTime + Math.Max(0, Environment.TickCount64 - _receivedAt) / 1000;
    private RoomCircuitEndpoint? Relay => _options?.Endpoints?.FirstOrDefault(endpoint => endpoint.CanConfigure);

    private sealed record Row(RoomCircuitAppointment Event, string Text)
    {
        public override string ToString() => Text;
    }

    private string GameName(RoomCircuitAppointment item)
    {
        var game = _options?.Games?.FirstOrDefault(value =>
            string.Equals(value.Game, item.Game, StringComparison.OrdinalIgnoreCase) &&
            string.Equals(value.App, item.App, StringComparison.OrdinalIgnoreCase));
        return game is null ? item.Game.ToUpperInvariant() : _chinese ? game.Zh ?? game.Name : game.Name;
    }

    private string Describe(RoomCircuitAppointment item)
    {
        string prefix = Followed.Contains(item.Id) ? "★ " : "";
        string title = GameName(item);
        string time = DateTimeOffset.FromUnixTimeSeconds(item.Starts).ToLocalTime().ToString("g", CultureInfo.CurrentCulture);
        return T($"{prefix}{title} · {time} · {item.Interested} ready", $"{prefix}{title} · {time} · {item.Interested} 人就绪");
    }

    private void UpdateActions()
    {
        var item = _current;
        var relay = Relay;
        _create.IsEnabled = _client is not null;
        _previous.IsEnabled = _client is not null && _offsets.Count > 1;
        _more.IsEnabled = _client is not null && _next.HasValue;
        _ready.Content = item?.Ready == true ? T("Cancel ready", "取消就绪") : T("I'm ready", "我已就绪");
        _ready.IsEnabled = item is not null && (item.Ready || Now < item.Starts + 7200 && item.Starts - Now <= (_options?.ReadyBefore ?? 1800));
        _follow.Content = item is not null && Followed.Contains(item.Id) ? T("Unfollow", "取消关注") : T("Follow", "关注");
        _follow.IsEnabled = item is not null;
        _find.IsEnabled = _share.IsEnabled = item is not null && _client is not null;
        _selection.Text = item is null ? "" : Describe(item);
        _endpoint.Text = relay is null ? "" : $"RyuLDN · {relay.Host}:{relay.Port}";
        _configure.IsEnabled = relay is not null;
    }

    private async Task RunAsync(Func<Task> action)
    {
        if (_busy || _closed)
        {
            return;
        }
        _busy = true;
        if (Content is Control control)
        {
            control.IsEnabled = false;
        }
        try
        {
            await action();
        }
        catch (OperationCanceledException) when (_closed) { }
        catch (RoomCircuitException error)
        {
            _feedback.Text = error.Message switch
            {
                "limit" => T("Too many requests. Try again shortly.", "请求过多，请稍后重试。"),
                "not_found" => T("This appointment is no longer available.", "该约战已失效。"),
                "invalid" => T("Check this appointment or its ready time.", "请检查约战信息或就绪时间。"),
                "game_running" => T("Close the game before changing relays.", "关闭游戏后再切换中继。"),
                _ => T("RoomCircuit is unavailable. Check the service address.", "约战服务不可用，请检查服务地址。"),
            };
        }
        catch (Exception error) when (error is HttpRequestException or OperationCanceledException or IOException or
            UnauthorizedAccessException or JsonException or ArgumentException or InvalidOperationException)
        {
            _feedback.Text = T("Could not complete the request. Check the address and try again.", "操作未完成，请检查地址后重试。");
        }
        finally
        {
            _busy = false;
            if (!_closed && Content is Control restored)
            {
                restored.IsEnabled = true;
                UpdateActions();
            }
        }
    }

    private async Task ConnectAsync()
    {
        _feedback.Text = "";
        if (!RoomCircuitClient.TryOrigin(_origin.Text ?? "", out Uri? origin))
        {
            _feedback.Text = T("Enter an HTTPS service address or a private LAN HTTP address.", "输入 HTTPS 服务地址或局域网 HTTP 地址。");
            return;
        }

        var trial = new RoomCircuitClient(origin!, _settings.Identity);
        var options = await trial.GetOptionsAsync(_lifetime.Token);
        if (options.NativeApi != 1)
        {
            _feedback.Text = T("This relay needs the RoomCircuit native appointments update.", "请升级中继以支持 RoomCircuit 原生约战。");
            return;
        }
        var first = await trial.ListAsync(0, _lifetime.Token);
        var configured = origin!.GetLeftPart(UriPartial.Authority);
        var settings = _settings with
        {
            Origin = configured,
            Followed = _settings.Origin == configured ? _settings.Followed : null,
        };
        settings.Save(_settingsFile);
        _settings = settings;
        _origin.Text = configured;
        _client = trial;
        _options = options;
        _seenOpenRooms.Clear();
        _offsets.Clear();
        _offsets.Add(0);
        _game.ItemsSource = new[] { T("Enter game IDs", "输入游戏 ID") }.Concat(
            (options.Games ?? Array.Empty<RoomCircuitGame>()).Select(game => _chinese ? game.Zh ?? game.Name : game.Name)).ToArray();
        _game.SelectedIndex = options.Games?.Length > 0 ? 1 : 0;
        RenderPage(first, null);
        _feedback.Text = _appointments.ItemCount == 0 ? T("No appointments yet.", "暂无约战。") : "";
    }

    private void RenderPage(RoomCircuitResult result, string? selected)
    {
        _serverTime = result.Now;
        _receivedAt = Environment.TickCount64;
        _next = result.Next;
        Row[] list = (result.Rows ?? Array.Empty<RoomCircuitAppointment>()).Select(row => new Row(row, Describe(row))).ToArray();
        _appointments.ItemsSource = list;
        _appointments.SelectedItem = list.FirstOrDefault(row => row.Event.Id == selected) ?? list.FirstOrDefault();
        _current = (_appointments.SelectedItem as Row)?.Event;
        _rooms.Children.Clear();
    }

    private async Task RefreshAsync()
    {
        if (_client is null)
        {
            await ConnectAsync();
            return;
        }
        RenderPage(await _client.ListAsync(_offsets[^1], _lifetime.Token), _current?.Id);
    }

    private async Task PreviousAsync()
    {
        if (_client is not null && _offsets.Count > 1)
        {
            uint prior = _offsets[^2];
            var page = await _client.ListAsync(prior, _lifetime.Token);
            _offsets.RemoveAt(_offsets.Count - 1);
            RenderPage(page, null);
        }
    }

    private async Task MoreAsync()
    {
        if (_client is not null && _next is uint next)
        {
            var page = await _client.ListAsync(next, _lifetime.Token);
            _offsets.Add(next);
            RenderPage(page, null);
        }
    }

    private async Task CreateAsync()
    {
        if (_client is null || _date.SelectedDate is not DateTimeOffset date || _time.SelectedTime is not TimeSpan time)
        {
            return;
        }

        DateTime local = DateTime.SpecifyKind(date.Date + time, DateTimeKind.Unspecified);
        if (TimeZoneInfo.Local.IsInvalidTime(local) || TimeZoneInfo.Local.IsAmbiguousTime(local))
        {
            _feedback.Text = T("Choose an unambiguous local start time.", "请选择没有时区歧义的开始时间。");
            return;
        }
        long starts = new DateTimeOffset(local, TimeZoneInfo.Local.GetUtcOffset(local)).ToUnixTimeSeconds();
        if (starts < Now || starts > Now + 7 * 86400)
        {
            _feedback.Text = T("Choose a start time within the next seven days.", "请选择未来七天内的开始时间。");
            return;
        }

        RoomCircuitGame? known = _game.SelectedIndex > 0 ? _options?.Games?[_game.SelectedIndex - 1] : null;
        string game = known?.Game ?? (_gameId.Text ?? "").Trim();
        string app = known?.App ?? (_appId.Text ?? "").Trim();
        if (!RoomCircuitIdentity.IsGame(game) || app.Length > 0 && (app.Length != 16 || !RoomCircuitIdentity.IsHex(app)))
        {
            _feedback.Text = T("Check the game IDs.", "请检查游戏 ID。");
            return;
        }

        var result = await _client.CreateAsync(starts, game, app.Length == 0 ? null : app, _lifetime.Token);
        _offsets.Clear();
        _offsets.Add(0);
        RenderPage(result, result.Created?.Id);
        if (result.Created is { } created && _current?.Id != created.Id)
        {
            // A late appointment may live outside the first page. Keep it selected for sharing.
            _appointments.ItemsSource = ((IEnumerable<Row>)_appointments.ItemsSource!).Prepend(new Row(created, Describe(created))).ToArray();
            _appointments.SelectedIndex = 0;
        }
        _feedback.Text = T("Appointment created.", "约战已创建。");
    }

    private async Task SetReadyAsync()
    {
        if (_client is not null && _current is { } selected)
        {
            var result = await _client.SetReadyAsync(selected.Id, !selected.Ready, _lifetime.Token);
            _offsets.Clear();
            _offsets.Add(0);
            RenderPage(result, selected.Id);
            _feedback.Text = selected.Ready ? T("Ready cleared.", "已取消就绪。") :
                T("Ready for five minutes.", "就绪有效期五分钟。");
        }
    }

    private Task ToggleFollowAsync()
    {
        if (_current is { } row)
        {
            var followed = Followed.ToList();
            if (!followed.Remove(row.Id))
            {
                if (followed.Count >= 12)
                {
                    _feedback.Text = T("You can follow up to 12 appointments.", "最多关注 12 个约战。");
                    return Task.CompletedTask;
                }
                followed.Add(row.Id);
            }
            var changed = _settings with { Followed = followed.ToArray() };
            changed.Save(_settingsFile);
            _settings = changed;
            var page = _appointments.ItemsSource as IEnumerable<Row>;
            if (page is not null)
            {
                var updated = page.Select(item => new Row(item.Event, Describe(item.Event))).ToArray();
                _appointments.ItemsSource = updated;
                _appointments.SelectedItem = updated.FirstOrDefault(item => item.Event.Id == row.Id);
            }
        }
        return Task.CompletedTask;
    }

    private async Task CheckFollowedRoomsAsync()
    {
        var client = _client;
        if (_closed || _busy || _checkingFollows || client is null || Followed.Count == 0)
        {
            return;
        }

        _checkingFollows = true;
        try
        {
            var opened = await Task.WhenAll(Followed.Take(12).Select(async id =>
                (Id: id, Result: await client.RoomsAsync(id, _lifetime.Token))));
            if (_closed || client != _client)
            {
                return;
            }
            foreach (var (id, result) in opened)
            {
                if (!Followed.Contains(id))
                {
                    continue;
                }
                if (result.Rooms?.Any(room => room.Crossplay) == true)
                {
                    if (_seenOpenRooms.Add(id))
                    {
                        _feedback.Text = T("A followed appointment has an open room.", "关注的约战有房间开放。");
                    }
                }
                else
                {
                    _seenOpenRooms.Remove(id);
                }
            }
        }
        catch (Exception error) when (error is RoomCircuitException or HttpRequestException or
            OperationCanceledException or IOException or JsonException)
        {
            // Background polling retries without replacing a foreground result.
        }
        finally
        {
            _checkingFollows = false;
        }
    }

    private async Task FindRoomsAsync()
    {
        if (_client is null || _current is null)
        {
            return;
        }

        var result = await _client.RoomsAsync(_current.Id, _lifetime.Token);
        _rooms.Children.Clear();
        foreach (RoomCircuitRoom room in result.Rooms?.Where(room => room.Crossplay) ?? Enumerable.Empty<RoomCircuitRoom>())
        {
            string detail = room.AppointmentBound ? T("Appointment host", "约战房主") :
                room.TitleConfirmed ? T("Same game", "同游戏") : T("LCID match", "按联机 ID 匹配");
            _rooms.Children.Add(new TextBlock { Text = $"{room.Host} · {room.Players}/{room.Max} · {detail}",
                TextWrapping = TextWrapping.Wrap });
        }
        _rooms.Children.Add(new TextBlock
        {
            Text = _rooms.Children.Count == 0 ? T("No compatible rooms are open.", "暂无可加入的跨端房间。") :
                T("Open the same game and join from its Local Wireless menu.", "打开同款游戏，从游戏的本地通信菜单加入。"),
            TextWrapping = TextWrapping.Wrap,
        });
    }

    private async Task CopyLinkAsync()
    {
        if (_current is { } row && Clipboard is { } clipboard && RoomCircuitClient.TryOrigin(_settings.Origin, out Uri? origin))
        {
            await clipboard.SetTextAsync(new Uri(origin!, $"appointments/?event={Uri.EscapeDataString(row.Id)}").AbsoluteUri);
            _feedback.Text = T("Link copied.", "链接已复制。");
        }
    }

    private Task ConfigureAsync()
    {
        if (Relay is { } relay)
        {
            _applyRelay(relay.Host);
            _feedback.Text = T("Relay saved. Start the game and open Local Wireless.", "已保存中继。启动游戏后打开本地通信。");
        }
        return Task.CompletedTask;
    }
}
