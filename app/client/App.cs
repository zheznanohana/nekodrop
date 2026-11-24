// SPDX-License-Identifier: GPL-2.0-only
using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Security.Principal;
using System.Text;
using System.Text.Json.Nodes;
using System.Windows;
using System.Windows.Threading;
using Forms = System.Windows.Forms;

namespace NekoDrop;

public sealed class App : Application
{
    [STAThread]
    public static int Main(string[] args)
    {
        if (args.Contains("--unregister")) { Shell.Unregister(); return 0; }
        // Chosen in the Share sheet: send what was shared, like Explorer's verb.
        if (args.Length == 0 && Shell.SharedFiles() is { Length: > 0 } shared) args = ["--send", .. shared];
        var pipeName = "nekodrop-ui-" + WindowsIdentity.GetCurrent().User?.Value;
        using var single = new Mutex(true, @"Local\nekodrop-ui", out var first);
        if (!first && !args.Contains("--snapshot"))
        {
            // Hand the arguments (files to send, a notification's answer) to the running copy.
            try
            {
                using var client = new NamedPipeClientStream(".", pipeName, PipeDirection.Out);
                client.Connect(3000);
                using var w = new StreamWriter(client, new UTF8Encoding(false));
                w.WriteLine(string.Join("\u001f", args));
            }
            catch (Exception) { }
            return 0;
        }
        var app = new App { ShutdownMode = ShutdownMode.OnExplicitShutdown };
        app._pipeName = pipeName;
        app._startArgs = args;
        return app.Run();
    }

    string _pipeName = "";
    string[] _startArgs = [];
    public static App Instance => (App)Current;
    public AppModel Model { get; } = new();
    public Settings Settings { get; } = Settings.Load();
    public ServiceLink Service { get; } = new();
    public Engine Engine { get; } = new();
    readonly Wake _wake = new();
    Forms.NotifyIcon? _tray;
    MainWindow? _window;
    readonly Dictionary<string, Request> _requests = [];
    readonly Dictionary<string, string> _sendTarget = [];

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        L.Use(Settings.Language);
        var snap = Array.IndexOf(_startArgs, "--snapshot");
        if (snap >= 0 && snap + 1 < _startArgs.Length) { Snapshot(_startArgs[snap + 1]); return; }
        Shell.SetIdentity();
        try { Shell.Register(Settings.StartWithWindows); } catch (Exception) { }
        ApplyTheme();
        CreateTray();
        ApplyHotkey();
        KeepAppleIdFresh();
        Service.State += o => Dispatcher.BeginInvoke(() => OnServiceState(o));
        Service.Connected += up => Dispatcher.BeginInvoke(() =>
        {
            if (up) return;
            Model.Status = L.T("status.noService");
            Model.Air = "service_stopped";
            Model.RadioOn = false;
        });
        Model.Allow = Settings.Discoverable;
        Service.Start();
        Engine.Event += o => Dispatcher.BeginInvoke(() => OnEngine(o));
        Engine.Event += o => EngineLog(o.ToJsonString());
        Engine.Log += EngineLog;
        if (Settings.Discoverable) StartEngine();
        _wake.AirDropNearby += () => Dispatcher.BeginInvoke(() =>
        {
            // effectiveMode (0.1.36+): a card that runs on demand although Concurrent is
            // chosen (RTL8852CE) was never woken by a nearby iPhone (#32).
            var mode = (string?)Service.Last?["effectiveMode"] ?? (string?)Service.Last?["mode"];
            if (mode == "ondemand") Service.Wake();
        });
        _wake.Start();
        var watchdog = new System.Windows.Threading.DispatcherTimer { Interval = TimeSpan.FromSeconds(5) };
        watchdog.Tick += (_, _) => EngineWatchdog();
        watchdog.Start();
        // Updates: a look a minute after start, then every 6 hours.
        var updates = new System.Windows.Threading.DispatcherTimer { Interval = TimeSpan.FromMinutes(1) };
        updates.Tick += (_, _) => { updates.Interval = TimeSpan.FromHours(6); _ = CheckForUpdate(false); };
        updates.Start();
        _ = Task.Run(ListenForArgs);
        HandleArgs(_startArgs, fromStart: true);
    }

    void StartEngine()
    {
        if (Engine.Running) return;
        try { Identity.Ensure(); } catch (Exception e) { Model.Detail = L.T("err.identity") + e.Message; }
        _engineIf = AwdlIndex();
        _notReceivingSince = null;
        if (!Engine.Start(Settings, out var error)) Model.Detail = L.T("err.engine") + error;
    }

    int _engineIf = -1;

    static readonly object EngineLogLock = new();

    /// <summary>The engine's events and stderr, kept in %LocalAppData%\nekodrop\engine.log (1 MB, one old copy).</summary>
    static void EngineLog(string line)
    {
        try
        {
            var dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "nekodrop");
            var path = Path.Combine(dir, "engine.log");
            lock (EngineLogLock)
            {
                Directory.CreateDirectory(dir);
                if (File.Exists(path) && new FileInfo(path).Length > 1024 * 1024) File.Move(path, path + ".old", true);
                File.AppendAllText(path, DateTime.Now.ToString("HH:mm:ss.fff ") + line + Environment.NewLine);
            }
        }
        catch (Exception) { }
    }

    /// <summary>awdl0's interface index (-1 when absent): a new one means awdld re-created it.</summary>
    static int AwdlIndex()
    {
        try
        {
            foreach (var n in System.Net.NetworkInformation.NetworkInterface.GetAllNetworkInterfaces())
                if (n.Name == "awdl0" && n.OperationalStatus == System.Net.NetworkInformation.OperationalStatus.Up)
                    return n.GetIPProperties().GetIPv6Properties().Index;
        }
        catch (Exception) { }
        return -1;
    }

    /// <summary>
    /// Keeps AirDrop discoverable without anyone restarting anything: the engine
    /// comes back if it exits, and starts over when awdl0 is re-created (a radio
    /// restart gives it a new interface the old sockets do not belong to).
    /// </summary>
    void EngineWatchdog()
    {
        if (!Settings.Discoverable || (string?)Service.Last?["radio"] != "on") return;
        var now = AwdlIndex();
        if (now < 0) return; /* no awdl0 yet: wait for awdld */
        if (!Engine.Running)
        {
            StartEngine();
        }
        else if (_engineIf < 0)
        {
            // The engine started before awdl0 existed and binds to it by itself (its
            // link watcher polls every second). Restarting it here killed it just as
            // the iPhone that woke the card had found it: the PC dropped out of the
            // share sheet until it was reopened (#32, #35).
            _engineIf = now;
        }
        else if (now != _engineIf
                 || _notReceivingSince is { } since && DateTime.UtcNow - since > TimeSpan.FromSeconds(30))
        {
            Engine.Dispose();
            StartEngine();
        }
    }

    DateTime? _notReceivingSince;

    void CreateTray()
    {
        _tray = new Forms.NotifyIcon
        {
            Text = "nekodrop",
            Icon = System.Drawing.Icon.ExtractAssociatedIcon(Environment.ProcessPath!) ?? System.Drawing.SystemIcons.Application,
            Visible = true,
        };
        // Left: the main window. Right: the design's menu at the pointer.
        _tray.MouseClick += (_, a) =>
        {
            if (a.Button == Forms.MouseButtons.Right) ShowTrayMenu();
            else if (a.Button != Forms.MouseButtons.Left) return;
            // A click on the icon of an open panel closes it (a double click keeps it open).
            else if (_window is { IsVisible: true } w && DateTime.UtcNow - w.ShownAt > NekoDrop.MainWindow.Grace) w.Shut();
            else ShowWindow();
        };
    }

    void ShowTrayMenu()
    {
        var at = Forms.Cursor.Position;
        // The pointer is in device pixels; WPF places windows in 96-dpi units.
        double scale;
        using (var g = System.Drawing.Graphics.FromHwnd(IntPtr.Zero)) scale = g.DpiX / 96.0;
        new TrayMenu().ShowAt(new Point(at.X / scale, at.Y / scale));
    }

    public void QuitApp() => Quit();

    /// <summary>
    /// The diagnostic report (report.ps1 beside the app): it asks for
    /// administrator rights itself and writes the redacted report and logs to
    /// the Desktop, so it also works when the service does not.
    /// </summary>
    public static void RunReport()
    {
        var script = Path.Combine(AppContext.BaseDirectory, "report.ps1");
        if (!File.Exists(script))
        {
            MessageBox.Show(L.T("report.missing"), "nekodrop");
            return;
        }
        try
        {
            Process.Start(new ProcessStartInfo("powershell.exe",
                $"-NoProfile -ExecutionPolicy Bypass -File \"{script}\"") { UseShellExecute = true });
        }
        catch (Exception e) { MessageBox.Show(e.Message, "nekodrop"); }
    }

    public MainWindow? Window => _window;

    async Task LiveSnapshot(string dir, (string Name, Window W)[] windows)
    {
        foreach (var (name, w) in windows)
        {
            w.WindowStartupLocation = WindowStartupLocation.CenterScreen;
            w.Topmost = true;
            w.Show();
            w.Activate();
            await Task.Delay(1500);
            var src = PresentationSource.FromVisual(w);
            var m = src?.CompositionTarget?.TransformToDevice ?? System.Windows.Media.Matrix.Identity;
            int x = (int)(w.Left * m.M11), y = (int)(w.Top * m.M22);
            int cw = (int)(w.ActualWidth * m.M11), ch = (int)(w.ActualHeight * m.M22);
            using (var bmp = new System.Drawing.Bitmap(cw, ch))
            {
                using (var g = System.Drawing.Graphics.FromImage(bmp)) g.CopyFromScreen(x, y, 0, 0, bmp.Size);
                bmp.Save(Path.Combine(dir, name + "-live.png"), System.Drawing.Imaging.ImageFormat.Png);
            }
            w.Close();
        }
        Shutdown();
    }

    /// <summary>
    /// --snapshot DIR [--lang zh|en]: renders the send, main and settings windows
    /// with sample devices to DIR\*.png off screen and exits. For checking the
    /// UI without the service, the radio or a visible window.
    /// </summary>
    void Snapshot(string dir)
    {
        var lang = Array.IndexOf(_startArgs, "--lang");
        if (lang >= 0 && lang + 1 < _startArgs.Length) L.Use(_startArgs[lang + 1]);
        _theme = Theme.Build(Settings.Accent, _startArgs.Contains("--dark") ? true : _startArgs.Contains("--light") ? false : null,
                             _startArgs.Contains("--colourful"));
        Resources.MergedDictionaries.Add(_theme);
        EnsureStyles();
        Directory.CreateDirectory(dir);
        Model.Status = L.F("status.on", 149);
        Model.RadioOn = true;
        Model.Air = "ready";
        Model.Mode = "concurrent";
        Model.Channel = 149;
        Model.Ssid = "Office-5G";
        Model.Update = "0.1.40";
        Motion.ForceOn = true;
        Model.Devices.Add(new Device { Id = "a", Name = "iPhone", Model = "iPhone15,2" });
        Model.Devices.Add(new Device { Id = "b", Name = "MacBook Air", Model = "Mac14,2", Busy = true, Progress = 0.42, State = "sending", Status = L.F("send.sending", 0.42) });
        Model.Devices.Add(new Device { Id = "c", Name = "iPad Pro", Model = "iPad14,5", State = "sent", Status = L.T("send.sent") });
        Model.Devices.Add(new Device { Id = "d", Name = "iPhone 15 Pro Max", Model = "iPhone16,2", State = "declined", Status = L.T("send.declined") });
        var asking = new Request { Id = "r", Sender = "iPhone", Model = "iPhone15,2", What = L.F("what.photos", 3) };
        Model.Requests.Add(asking);
        Model.Devices[0].Ask = asking; // the bubble under its icon
        Model.ReceivedFiles.Add(new Received { Name = "IMG_0042.HEIC", Path = "", From = "iPhone" });
        Model.ReceivedFiles.Add(new Received { Name = L.F("what.videos", 1), Path = "", From = "MacBook Air", Failed = true });
        Model.Files.Add(@"C:\Users\me\Pictures\IMG_2048.HEIC");
        Model.Files.Add(@"C:\Users\me\Documents\Plan.pdf");
        var wifiState = new JsonObject
        {
            ["radio"] = "on", ["mode"] = "concurrent", ["wifi"] = "connected", ["ssid"] = "Office-5G",
            ["networks"] = new JsonArray(
                new JsonObject { ["ssid"] = "Office-5G", ["dbm"] = -48, ["security"] = "wpa2-psk", ["saved"] = true, ["connected"] = true },
                new JsonObject { ["ssid"] = "Home-WiFi", ["dbm"] = -61, ["security"] = "wpa2/3-psk", ["saved"] = true },
                new JsonObject { ["ssid"] = "Guest-5G", ["dbm"] = -70, ["security"] = "wpa2-psk" },
                new JsonObject { ["ssid"] = "CoffeeShop-Free", ["dbm"] = -80, ["security"] = "open" }),
            ["known"] = new JsonArray(
                new JsonObject { ["ssid"] = "Office-5G", ["auto"] = true },
                new JsonObject { ["ssid"] = "Home-WiFi", ["auto"] = false }),
        };
        Window Main(string air, string detail = "") { Model.Air = air; Model.Detail = detail; return new MainWindow(); }
        // Each window is built just before it is drawn, so a variant can change the
        // settings and the model first. A height draws more than fits on the build
        // machine's screen (the main window's whole content).
        var windows = new (string Name, Func<Window> Make, double Height)[]
        {
            ("send", () => new SendWindow(), 0),
            ("main", () => Main("ready"), 1000),
            ("main-list", () => { Settings.DeviceView = "list"; return Main("ready"); }, 1000),
            ("settings-general", () => new SettingsWindow("general"), 0),
            ("settings-network", () => new SettingsWindow("network"), 0),
            ("settings-appleid", () => new SettingsWindow("appleid"), 0),
            ("settings-hardware", () => new SettingsWindow("hardware"), 0),
            ("settings-updates", () => new SettingsWindow("updates"), 0),
            ("wifi", () => { var w = new WifiWindow(); w.Render(wifiState); return w; }, 0),
            ("wifi-windows", () => { var w = new WifiWindow(); wifiState["mode"] = "ondemand"; w.Render(wifiState); return w; }, 0),
            ("tray", () => { var m = new TrayMenu(); m.Loaded += (_, _) => m.OpenModes(); return m; }, 0),
            ("main-receiving", () => { Settings.DeviceView = "radar"; asking.Receiving = true; asking.Progress = 0.6; return Main("ready"); }, 0),
            ("main-done", () => { asking.Receiving = false; var w = Main("ready"); w.Loaded += (_, _) => Dispatcher.BeginInvoke(Model.RaiseStored); return w; }, 0),
            ("main-empty", () => { Settings.DeviceView = "radar"; Model.Devices.Clear(); Model.Requests.Clear(); return Main("ready"); }, 0),
            ("main-starting", () => Main("starting"), 0),
            ("main-standby", () => Main("standby"), 0),
            ("main-off", () => Main("off"), 0),
            ("main-error", () => Main("error", "bring-up failed: out of DMA memory for the firmware image"), 0),
            ("main-noservice", () => Main("service_stopped"), 0),
            ("main-nocard", () => Main("no_adapter"), 0),
        };
        if (_startArgs.Contains("--live"))
        {
            // On screen, captured from the screen: shows the real backdrop (Mica).
            _ = LiveSnapshot(dir, windows.Select(x => (x.Name, x.Make())).ToArray());
            return;
        }
        foreach (var (name, make, tall) in windows)
        {
            var w = make();
            w.WindowStartupLocation = WindowStartupLocation.Manual;
            w.Left = -20000; w.Top = -20000; w.ShowActivated = false; w.ShowInTaskbar = false;
            w.Show();
            w.UpdateLayout();
            // Let the radar's first ripple grow out from behind the hub, so the
            // picture shows where it is centered.
            Pump(TimeSpan.FromMilliseconds(1200));
            if (w.Content is not FrameworkElement root) continue;
            var width = root.ActualWidth;
            var height = tall > 0 ? tall : root.ActualHeight;
            if (tall > 0)
            {
                root.Measure(new Size(width, height));
                root.Arrange(new Rect(0, 0, width, height));
                root.UpdateLayout();
            }
            Dispatcher.Invoke(() => { }, DispatcherPriority.ApplicationIdle);
            // The background, then the content as laid out (a VisualBrush would stretch
            // it to the picture when the layout is taller than the window).
            var bmp = new System.Windows.Media.Imaging.RenderTargetBitmap((int)width, (int)height, 96, 96, System.Windows.Media.PixelFormats.Pbgra32);
            if (w is not TrayMenu)
            {
                var back = new System.Windows.Media.DrawingVisual();
                using (var dc = back.RenderOpen())
                    dc.DrawRectangle((System.Windows.Media.Brush)Resources["MicaSolid"], null, new Rect(0, 0, width, height));
                bmp.Render(back);
            }
            bmp.Render(root);
            var png = new System.Windows.Media.Imaging.PngBitmapEncoder();
            png.Frames.Add(System.Windows.Media.Imaging.BitmapFrame.Create(bmp));
            using (var fs = File.Create(Path.Combine(dir, name + ".png"))) png.Save(fs);
            w.Close();
        }
        MotionStrips(dir);
        Shutdown();
    }

    /// <summary>
    /// For --snapshot: the send and receive animations as strips of frames (the
    /// radar only, every ~80 ms), so a build shows the motion without a screen.
    /// </summary>
    void MotionStrips(string dir)
    {
        Settings.DeviceView = "radar";
        Model.Requests.Clear();
        Model.Devices.Clear();
        var mac = new Device { Id = "m", Name = "MacBook Air", Model = "Mac14,2", State = "sending", Busy = true, Progress = 0.3, Status = L.F("send.sending", 0.3) };
        var phone = new Device { Id = "p", Name = "iPhone", Model = "iPhone15,2" };
        Model.Devices.Add(phone);
        Model.Devices.Add(mac);
        var ask = new Request { Id = "q", Sender = "iPhone", Model = "iPhone15,2", What = L.F("what.photos", 3), Receiving = true, Progress = 0.4 };
        Model.Requests.Add(ask);
        phone.Ask = ask;
        Model.Air = "ready";

        Strip(dir, "motion-send", () => { mac.Progress = 1; },
              (360, () => { mac.State = "sent"; mac.Status = L.T("send.sent"); }),
              [0, 120, 240, 360, 440, 520, 600, 700, 820, 1000]);
        Strip(dir, "motion-receive", () => { ask.Progress = 1; },
              (360, () => { Model.Requests.Remove(ask); phone.Ask = null; Model.RaiseStored(); }),
              [0, 120, 240, 360, 440, 520, 600, 700, 820, 1000]);
    }

    void Strip(string dir, string name, Action start, (int At, Action Do) then, int[] times)
    {
        var w = new MainWindow { WindowStartupLocation = WindowStartupLocation.Manual, Left = -20000, Top = -20000, ShowActivated = false, ShowInTaskbar = false };
        w.Show();
        w.UpdateLayout();
        Pump(TimeSpan.FromMilliseconds(1200));
        var radar = FindChild<RadarView>(w);
        if (radar is null) { w.Close(); return; }
        var clock = System.Diagnostics.Stopwatch.StartNew();
        start();
        var done = false;
        var frames = new List<System.Windows.Media.Imaging.RenderTargetBitmap>();
        foreach (var t in times)
        {
            if (!done && t >= then.At) { while (clock.ElapsedMilliseconds < then.At) Pump(TimeSpan.FromMilliseconds(5)); then.Do(); done = true; }
            while (clock.ElapsedMilliseconds < t) Pump(TimeSpan.FromMilliseconds(5));
            int fw = (int)radar.ActualWidth, fh = (int)radar.ActualHeight;
            var bmp = new System.Windows.Media.Imaging.RenderTargetBitmap(fw, fh, 96, 96, System.Windows.Media.PixelFormats.Pbgra32);
            var back = new System.Windows.Media.DrawingVisual();
            using (var dc = back.RenderOpen())
                dc.DrawRectangle((System.Windows.Media.Brush)Resources["MicaSolid"], null, new Rect(0, 0, fw, fh));
            bmp.Render(back);
            bmp.Render(radar);
            frames.Add(bmp);
        }
        w.Close();
        // Side by side, with each frame's time on top.
        double fwd = frames[0].Width, fht = frames[0].Height;
        var sheet = new System.Windows.Media.DrawingVisual();
        using (var dc = sheet.RenderOpen())
        {
            dc.DrawRectangle((System.Windows.Media.Brush)Resources["MicaSolid"], null, new Rect(0, 0, fwd * frames.Count, fht + 22));
            for (var i = 0; i < frames.Count; i++)
            {
                dc.DrawImage(frames[i], new Rect(i * fwd, 22, fwd, fht));
                var label = new System.Windows.Media.FormattedText($"{times[i]} ms", System.Globalization.CultureInfo.InvariantCulture,
                    FlowDirection.LeftToRight, new System.Windows.Media.Typeface("Segoe UI"), 13, (System.Windows.Media.Brush)Resources["Text2"], 1);
                dc.DrawText(label, new Point(i * fwd + 8, 3));
            }
        }
        var out_ = new System.Windows.Media.Imaging.RenderTargetBitmap((int)(fwd * frames.Count), (int)(fht + 22), 96, 96, System.Windows.Media.PixelFormats.Pbgra32);
        out_.Render(sheet);
        var png = new System.Windows.Media.Imaging.PngBitmapEncoder();
        png.Frames.Add(System.Windows.Media.Imaging.BitmapFrame.Create(out_));
        using var fs = File.Create(Path.Combine(dir, name + ".png"));
        png.Save(fs);
    }

    static T? FindChild<T>(DependencyObject root) where T : DependencyObject
    {
        for (var i = 0; i < System.Windows.Media.VisualTreeHelper.GetChildrenCount(root); i++)
        {
            var c = System.Windows.Media.VisualTreeHelper.GetChild(root, i);
            if (c is T t) return t;
            if (FindChild<T>(c) is { } found) return found;
        }
        return null;
    }

    /// <summary>Runs the dispatcher (animations, layout) for a while, for --snapshot.</summary>
    void Pump(TimeSpan time)
    {
        var frame = new DispatcherFrame();
        var timer = new DispatcherTimer { Interval = time };
        timer.Tick += (_, _) => { timer.Stop(); frame.Continue = false; };
        timer.Start();
        Dispatcher.PushFrame(frame);
    }

    SendWindow? _send;
    DateTime _lastSend = DateTime.MinValue;

    /// <summary>The share sheet: what is being sent, and who to send it to.</summary>
    public void ShowSendWindow()
    {
        Service.Wake();
        if (_send is null)
        {
            var w = new SendWindow();
            w.Closed += (_, _) => { if (_send == w) _send = null; if (_window is not { IsVisible: true }) StopLookingLater(); };
            _send = w;
        }
        _send.Refresh();
        _send.Show();
        if (_send.WindowState == WindowState.Minimized) _send.WindowState = WindowState.Normal;
        _send.Activate();
        Engine.Look(true);
    }

    ResourceDictionary? _styles;

    /// <summary>The design's control styles (Styles.xaml), after the colours they use.</summary>
    void EnsureStyles()
    {
        if (_styles is not null) return;
        _styles = new ResourceDictionary { Source = new Uri("/nekodrop;component/Styles.xaml", UriKind.Relative) };
        Resources.MergedDictionaries.Add(_styles);
    }

    SettingsWindow? _settings;

    /// <summary>The settings, at a tab: general, network, hardware or updates.</summary>
    public void ShowSettings(string tab)
    {
        _settings?.Close();
        _settings = new SettingsWindow(tab);
        _settings.Closed += (s, _) => { if (_settings == s) _settings = null; };
        _settings.Show();
        _settings.Activate();
    }

    /// <summary>Starts NekoDropService (Windows asks for administrator rights).</summary>
    public static void StartService()
    {
        try
        {
            Process.Start(new ProcessStartInfo("sc.exe", "start NekoDropService")
                { UseShellExecute = true, Verb = "runas", WindowStyle = ProcessWindowStyle.Hidden });
        }
        catch (Exception) { /* the user said no */ }
    }

    Hotkey? _hotkey;

    /// <summary>The global shortcut on or off, with its keys, after the settings. False when Windows would not register it (taken).</summary>
    public bool ApplyHotkey()
    {
        _hotkey?.Dispose();
        _hotkey = null;
        if (!Settings.Hotkey) return true;
        _hotkey = Hotkey.Register(Settings.HotkeyKeys, () =>
        {
            var selected = Shell.ExplorerSelection();
            if (selected.Length == 0) { ShowWindow(); return; }
            Model.Files.Clear();
            foreach (var f in selected) Model.Files.Add(f);
            ShowSendWindow();
        });
        return _hotkey is not null;
    }

    ResourceDictionary? _theme;

    /// <summary>The colours, again after the accent setting changes: every window follows (DynamicResource).</summary>
    public void ApplyTheme()
    {
        var fresh = Theme.Build(Settings.Accent);
        if (_theme is not null) Resources.MergedDictionaries.Remove(_theme);
        Resources.MergedDictionaries.Insert(0, fresh);
        _theme = fresh;
        EnsureStyles();
    }

    /// <summary>After the language setting changes: menus, Explorer verb and open windows follow.</summary>
    public void ApplyLanguage()
    {
        L.Use(Settings.Language);

        try { Shell.Register(Settings.StartWithWindows); } catch (Exception) { }
        if (_window is not null) { var w = _window; var shown = w.IsVisible; _window = null; w.Close(); if (shown) ShowWindow(); }
    }

    /// <summary>The engine again (a new identity, or Contacts Only switched): it reads both when it starts.</summary>
    public void RestartEngine()
    {
        Engine.Dispose();
        if (Settings.Discoverable) StartEngine();
    }

    System.Windows.Threading.DispatcherTimer? _appleIdTimer;

    /// <summary>
    /// Keeps a signed-in Apple ID's certificate and record fresh (appleid.exe
    /// renews them ahead of expiry, and re-reads the iCloud contacts): soon
    /// after start, then every 12 hours. A new certificate restarts the engine.
    /// </summary>
    void KeepAppleIdFresh()
    {
        _appleIdTimer = new System.Windows.Threading.DispatcherTimer { Interval = TimeSpan.FromMinutes(2) };
        _appleIdTimer.Tick += async (_, _) =>
        {
            _appleIdTimer.Interval = TimeSpan.FromHours(12);
            if (!Settings.AppleIdOn || AppleId.Source != "appleid") return;
            var before = AppleId.CertificateUntil;
            var result = await AppleId.Refresh();
            if (result == 0 && AppleId.CertificateUntil != before) RestartEngine();
        };
        _appleIdTimer.Start();
    }

    /// <summary>Who can send to this PC, from the AirDrop mark: "off", "contacts" or "everyone".</summary>
    public void SetReceive(string mode)
    {
        if (mode == "off") { SetDiscoverable(false); return; }
        var changed = Settings.Receive != mode;
        Settings.Receive = mode;
        Settings.Save();
        if (!Model.Allow) SetDiscoverable(true); // starts the engine, which reads the mode
        else if (changed) RestartEngine();
        _window?.ShowMark();
        _settings?.ShowReceive();
    }

    public void SetDiscoverable(bool on)
    {
        Model.Allow = on;
        Settings.Discoverable = on;
        Settings.Save();
        if (on) { StartEngine(); Service.Wake(); }
        else Engine.Dispose();
    }

    public void ShowWindow()
    {
        Service.Wake();
        if (_window is null)
        {
            var w = new MainWindow();
            w.Closed += (_, _) => { if (_window == w) _window = null; };
            _window = w;
        }
        // A click on the tray icon while the panel is open first takes the
        // focus from it (it goes away); that click means "close", not "open again".
        else if (!_window.IsVisible && (DateTime.UtcNow - _window.ShutAt).TotalMilliseconds < 400) return;
        _window.DockRight();
        var opening = !_window.IsVisible;
        _window.Show();
        if (_window.WindowState == WindowState.Minimized) _window.WindowState = WindowState.Normal;
        _window.Activate();
        if (opening) _window.Reveal();
        _lookTimer?.Stop();
        Engine.Look(true);
    }

    public void WindowClosed() { if (_send is null) StopLookingLater(); }

    System.Windows.Threading.DispatcherTimer? _lookTimer;

    /// <summary>
    /// The panel or the send window went away: keep looking a while (90 s), and
    /// for as long as a send or a request is under way, so devices, a send's
    /// progress and its outcome are all still there when the panel opens again.
    /// Looking wakes every Apple device nearby, so it does stop in the end.
    /// </summary>
    void StopLookingLater()
    {
        _lookTimer?.Stop();
        _lookTimer = new System.Windows.Threading.DispatcherTimer { Interval = TimeSpan.FromSeconds(90) };
        _lookTimer.Tick += (_, _) =>
        {
            if (_send is not null || _window is { IsVisible: true }) { _lookTimer.Stop(); return; }
            if (Model.Devices.Any(d => d.Busy) || Model.Requests.Count > 0) return; // check again in 90 s
            _lookTimer.Stop();
            Engine.Look(false);
        };
        _lookTimer.Start();
    }

    void Quit()
    {
        _wake.Dispose();
        Engine.Dispose();
        Service.Dispose();
        if (_tray is not null) _tray.Visible = false;
        Shutdown();
    }

    async Task ListenForArgs()
    {
        while (true)
        {
            try
            {
                using var server = new NamedPipeServerStream(_pipeName, PipeDirection.In, 1, PipeTransmissionMode.Byte,
                                                             PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);
                await server.WaitForConnectionAsync();
                using var r = new StreamReader(server, Encoding.UTF8);
                var line = await r.ReadLineAsync() ?? "";
                var args = line.Length == 0 ? [] : line.Split('\u001f');
                await Dispatcher.BeginInvoke(() => HandleArgs(args, fromStart: false));
            }
            catch (Exception) { await Task.Delay(500); }
        }
    }

    /// <summary>--send FILE…, --background, or a nekodrop: URI from a notification.</summary>
    void HandleArgs(string[] args, bool fromStart)
    {
        if (args.Length > 0 && args[0].StartsWith("nekodrop:", StringComparison.OrdinalIgnoreCase))
        {
            HandleUri(new Uri(args[0]));
            return;
        }
        var files = args.SkipWhile(a => a != "--send").Skip(1).Where(a => File.Exists(a) || Directory.Exists(a)).ToList();
        if (files.Count > 0)
        {
            // A new right-click send replaces what was queued, except that Explorer
            // starts one process per selected item for classic verbs: those arrive
            // within moments of each other and add up.
            if (DateTime.UtcNow - _lastSend > TimeSpan.FromSeconds(2)) Model.Files.Clear();
            _lastSend = DateTime.UtcNow;
            foreach (var f in files) if (!Model.Files.Contains(f)) Model.Files.Add(f);
            ShowSendWindow();
        }
        else if (args.Contains("--send")) ShowSendWindow();
        else if (!(fromStart && args.Contains("--background"))) ShowWindow();
    }

    void HandleUri(Uri uri)
    {
        var q = System.Web.HttpUtility.ParseQueryString(uri.Query);
        switch (uri.AbsolutePath.ToLowerInvariant())
        {
            case "answer":
                var id = q["id"] ?? "";
                Answer(id, q["accept"] == "1");
                break;
            case "update":
                _ = InstallUpdate();
                break;
            case "folder":
                var path = q["path"];
                if (path is not null && Directory.Exists(path)) Process.Start("explorer.exe", $"\"{path}\"");
                break;
            default:
                ShowWindow();
                break;
        }
    }

    Updater.Release? _update;

    /// <summary>Looks for a newer release; tells the user once per version. Returns the detailed result.</summary>
    public async Task<Updater.CheckResult> CheckForUpdateDetailed(bool asked)
    {
        if (!asked && !Settings.CheckUpdates) return new Updater.CheckResult(Updater.CheckStatus.UpToDate);
        var res = await Updater.Check(Settings.PreviewUpdates);
        if (res.Status == Updater.CheckStatus.Available && res.Release is { } found)
        {
            if (Settings.SkipVersion != found.Version.ToString())
            {
                Model.Update = found.Version.ToString();
                Model.UpdatePreview = found.Preview;
            }
            var announce = !asked && (_update is null || _update.Version != found.Version) &&
                           Settings.SkipVersion != found.Version.ToString();
            _update = found;
            if (announce) Shell.UpdateToast(found.Version.ToString(), found.Preview);
        }
        return res;
    }

    /// <summary>Looks for a newer release; tells the user once per version. Returns what it found.</summary>
    public async Task<Updater.Release?> CheckForUpdate(bool asked) => (await CheckForUpdateDetailed(asked)).Release;

    /// <summary>Downloads, verifies and runs the installer of the release found. Returns "" or why not.</summary>
    public async Task<string> InstallUpdate(IProgress<int>? progress = null)
    {
        var r = _update ?? await CheckForUpdate(true);
        if (r is null) return L.T("update.none");
        try
        {
            var why = await Updater.Install(r, progress, Settings.DeleteInstallerAfterInstall);
            if (why != "") Shell.DoneToast("update", L.T("update.failed"), why, null);
            return why;
        }
        catch (Exception e)
        {
            Shell.DoneToast("update", L.T("update.failed"), e.Message, null);
            return e.Message;
        }
    }

    public void Answer(string id, bool accept)
    {
        Engine.Answer(id, accept);
        if (_requests.TryGetValue(id, out var r))
        {
            r.Status = accept ? L.T("req.receiving") : L.T("req.declined");
            r.Receiving = accept;
        }
        if (!accept && _requests.Remove(id, out var gone)) { Model.Requests.Remove(gone); DetachAsk(gone); }
    }

    /// <summary>
    /// A request goes under its sender's icon on the radar: the device of the
    /// same name, or one put there for it (the sender was not among the devices found).
    /// </summary>
    void AttachAsk(Request r)
    {
        var d = Model.Devices.FirstOrDefault(x => string.Equals(x.Name, r.Sender, StringComparison.OrdinalIgnoreCase) && x.Ask is null);
        if (d is null) Model.Devices.Add(d = new Device { Id = "ask:" + r.Id, Name = r.Sender, Model = r.Model, ForAsk = true });
        d.Ask = r;
    }

    void DetachAsk(Request r)
    {
        var d = Model.Devices.FirstOrDefault(x => x.Ask == r);
        if (d is null) return;
        d.Ask = null;
        if (d.ForAsk) Model.Devices.Remove(d);
    }

    public void SendTo(Device d)
    {
        if (Model.Files.Count == 0) return;
        var id = Guid.NewGuid().ToString("N")[..12];
        _sendTarget[id] = d.Id;
        d.Busy = true;
        d.State = "asking";
        d.Status = L.T("send.asking");
        d.Progress = 0;
        Service.Touch();
        Engine.SendFiles(id, d.Id, Model.Files.ToList());
    }

    string _lastRadio = "", _lastWifi = "";
    WifiWindow? _wifiWindow;

    public void ShowWifi()
    {
        if (_wifiWindow is null)
        {
            _wifiWindow = new WifiWindow();
            _wifiWindow.Closed += (_, _) => _wifiWindow = null;
        }
        _wifiWindow.Show();
        _wifiWindow.Activate();
    }

    /// <summary>
    /// Networks that want a web sign-in first (hotels, campuses): after joining,
    /// ask Windows' connectivity probe; anything but its answer means a portal,
    /// which opens in the browser.
    /// </summary>
    async Task CheckPortal(string ssid)
    {
        await Task.Delay(TimeSpan.FromSeconds(6)); /* DHCP on the nekodrop adapter */
        try
        {
            using var http = new System.Net.Http.HttpClient(new System.Net.Http.HttpClientHandler { AllowAutoRedirect = false })
                { Timeout = TimeSpan.FromSeconds(8) };
            var r = await http.GetAsync("http://www.msftconnecttest.com/connecttest.txt");
            var body = r.IsSuccessStatusCode ? await r.Content.ReadAsStringAsync() : "";
            if (body.Trim() == "Microsoft Connect Test") { Model.Portal = ""; return; }
            Model.Portal = ssid;
            Process.Start(new ProcessStartInfo("http://www.msftconnecttest.com/redirect") { UseShellExecute = true });
            Shell.DoneToast("portal", L.F("wifi.portal.title", ssid), L.T("wifi.portal.body"), null);
        }
        catch (Exception) { /* no answer at all: not a portal question */ }
    }


    void OnServiceState(JsonObject o)
    {
        var radio = (string?)o["radio"];
        // A radio (re)start recreates awdl0: the engine's sockets belong to the old
        // interface, so it starts over on the new one.
        // Not when the engine has no awdl0 yet (it binds to the new one by itself):
        // on a wake it was started a moment earlier, and a second start only sent a
        // goodbye to the iPhone (#32, #35).
        if (radio == "on" && _lastRadio != "on" && _lastRadio != "" && Settings.Discoverable &&
            !(Engine.Running && _engineIf < 0))
        {
            Engine.Dispose();
            StartEngine();
        }
        _lastRadio = radio ?? "";
        Model.RadioOn = radio == "on";
        var stateMode = (string?)o["effectiveMode"] ?? (string?)o["mode"] ?? "";
        var card = (string?)o["card"] ?? "";
        Model.Mode = (string?)o["mode"] ?? "";
        Model.Channel = (int?)o["channel"] ?? 0;
        Model.Ssid = (string?)o["wifi"] == "connected" ? (string?)o["ssid"] ?? "" : "";
        Model.Air = card == "" && stateMode != "off" ? "no_adapter" : radio switch
        {
            "on" => "ready",
            "starting" => "starting",
            "error" => "error",
            _ => stateMode == "off" ? "off" : "standby",
        };
        var wifiNow = (string?)o["wifi"] ?? "";
        if (wifiNow == "connected" && _lastWifi != "connected") _ = CheckPortal((string?)o["ssid"] ?? "");
        _lastWifi = wifiNow;
        var channel = (int?)o["channel"] ?? 0;
        var wifi = (string?)o["wifi"] ?? "";
        var mode = (string?)o["mode"];
        Model.Status = radio switch
        {
            "on" => L.F("status.on", channel),
            "starting" => L.T("status.starting"),
            "error" => L.T("status.error"),
            _ => mode == "off" ? L.T("status.off") : L.T("status.standby"),
        };
        Model.Detail = radio == "error" ? (string?)o["detail"] ?? "" :
            wifi == "" ? "" : wifi.StartsWith("unsupported") ? "Wi-Fi：" + wifi[13..] : "Wi-Fi：" + WifiText(wifi);
        if (_tray is not null) _tray.Text = "nekodrop — " + NekoDrop.MainWindow.Status(Model.Air).Label;
    }

    static string WifiText(string s) => s switch
    {
        "connected" => L.T("wifi.connected"),
        "failed" => L.T("wifi.failed"),
        "waiting for a beacon" or "authenticating" or "associating" or "4-way handshake" => L.T("wifi.connecting"),
        _ => s,
    };

    void OnEngine(JsonObject o)
    {
        var ev = (string?)o["event"];
        var id = (string?)o["id"] ?? "";
        switch (ev)
        {
            case "ready":
                /* A new engine (the watchdog starts one over when the radio
                 * moves channel and re-creates awdl0) knows no peers and is not
                 * looking: the old one's devices go, and an open window looks
                 * again (#9: the send window stayed empty after a move to 149). */
                foreach (var stale in Model.Devices.Where(x => !x.Busy).ToList()) Model.Devices.Remove(stale);
                if (_send is not null || _window is { IsVisible: true }) Engine.Look(true);
                break;
            case "receiving":
                /* Not receiving (ports or mDNS held, a socket error): the watchdog starts over after a while. */
                _notReceivingSince = (bool?)o["on"] == true ? null : _notReceivingSince ?? DateTime.UtcNow;
                break;
            case "peer":
            {
                var d = Model.Devices.FirstOrDefault(x => x.Id == id);
                if (d is null) Model.Devices.Add(d = new Device { Id = id });
                d.Name = (string?)o["name"] ?? L.T("dev.apple");
                d.Model = (string?)o["model"] ?? "";
                break;
            }
            case "peer_gone":
                var gone = Model.Devices.FirstOrDefault(x => x.Id == id);
                if (gone is not null && !gone.Busy) Model.Devices.Remove(gone);
                break;
            case "asked":
            {
                Service.Touch();
                var sender = (string?)o["sender"] ?? L.T("dev.someone");
                var model = (string?)o["model"] ?? "";
                var files = o["files"] as JsonArray;
                var what = Describe(files);
                if (Settings.AlwaysAccept.Contains($"{sender}|{model}")) { Engine.Answer(id, true); break; }
                var r = new Request { Id = id, Sender = sender, Model = model, What = what };
                _requests[id] = r;
                Model.Requests.Insert(0, r);
                AttachAsk(r);
                Shell.AskToast(id, sender, what);
                break;
            }
            case "incoming":
                if (_requests.TryGetValue(id, out var inc))
                {
                    var total = (double?)o["total"] ?? 0;
                    inc.Progress = total > 0 ? ((double?)o["done"] ?? 0) / total : 0;
                    inc.Status = L.T("req.receiving");
                    inc.Receiving = true;
                }
                Service.Touch();
                break;
            case "stored":
            {
                var folder = (string?)o["folder"] ?? Settings.SaveFolder;
                var from = _requests.TryGetValue(id, out var st) ? st.Sender : "AirDrop";
                if (st is not null) { _requests.Remove(id); Model.Requests.Remove(st); DetachAsk(st); }
                if (o["files"] is JsonArray stored)
                    foreach (var f in stored)
                    {
                        var name = f is JsonObject fo ? (string?)fo["name"] ?? "" : (string?)f ?? "";
                        Model.ReceivedFiles.Insert(0, new Received { Name = name, Path = Path.Combine(folder, name), From = from });
                    }
                Model.RaiseStored();
                Shell.DoneToast(id, L.F("done.received", from), L.F("done.savedTo", folder), folder);
                break;
            }
            case "lost":
                if (_requests.Remove(id, out var lost))
                {
                    Model.Requests.Remove(lost);
                    DetachAsk(lost);
                    Model.ReceivedFiles.Insert(0, new Received { Name = lost.What, Path = "", From = lost.Sender, Failed = true });
                    Shell.DoneToast(id, L.F("done.lost", lost.Sender), (string?)o["reason"] ?? "", null);
                }
                break;
            case "send":
            {
                var target = _sendTarget.GetValueOrDefault(id);
                var d = Model.Devices.FirstOrDefault(x => x.Id == target);
                if (d is null) break;
                var state = (string?)o["state"];
                d.State = state is "done" ? "sent" : state ?? "";
                var total = (double?)o["total"] ?? 0;
                var done = (double?)o["done"] ?? (double?)o["sent"] ?? 0;
                d.Progress = total > 0 ? done / total : d.Progress;
                d.Status = state switch
                {
                    "asking" => L.T("send.asking"),
                    "sending" => L.F("send.sending", d.Progress),
                    "sent" or "done" => L.T("send.sent"),
                    "declined" => L.T("send.declined"),
                    "failed" => L.T("send.failed") + ((string?)o["reason"] ?? ""),
                    _ => state ?? "",
                };
                if (state is "sent" or "done" or "declined" or "failed")
                {
                    d.Busy = false;
                    // A device that took the files is ready for more a moment later.
                    if (state is "sent" or "done")
                        _ = Task.Delay(TimeSpan.FromSeconds(3.5)).ContinueWith(_ => Dispatcher.BeginInvoke(() =>
                        {
                            if (d.State != "sent") return;
                            d.State = "";
                            d.Progress = 0;
                            d.Status = L.T("dev.ready");
                        }));
                    if (state is "sent" or "done") Shell.DoneToast(id, L.F("done.sent", d.Name), L.F("done.items", Model.Files.Count), null);
                }
                Service.Touch();
                break;
            }
            case "looking":
                Model.Looking = (bool?)o["on"] ?? false;
                break;
            case "link":
                if ((bool?)o["up"] == true) Model.Detail = Model.Detail.Replace(L.T("err.engine"), "");
                break;
            case "fatal":
                Model.Detail = L.T("err.engineStopped") + ((string?)o["detail"] ?? (string?)o["reason"] ?? "");
                break;
        }
    }

    static string Describe(JsonArray? files)
    {
        if (files is null || files.Count == 0) return L.T("what.some");
        int photos = 0, videos = 0, other = 0;
        foreach (var f in files)
        {
            var type = (f as JsonObject)?["type"]?.ToString() ?? "";
            if (type.Contains("image") || type.Contains("jpeg") || type.Contains("png") || type.Contains("heic")) photos++;
            else if (type.Contains("movie") || type.Contains("video") || type.Contains("mpeg")) videos++;
            else other++;
        }
        var parts = new List<string>();
        if (photos > 0) parts.Add(L.F("what.photos", photos));
        if (videos > 0) parts.Add(L.F("what.videos", videos));
        if (other > 0) parts.Add(L.F("what.files", other));
        return string.Join(L.T("what.join"), parts);
    }
}
