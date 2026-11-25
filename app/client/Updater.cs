// SPDX-License-Identifier: GPL-2.0-only
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Net.Http;
using System.Reflection;
using System.Security.Cryptography;
using System.Text.Json.Nodes;
using System.Xml.Linq;

namespace NekoDrop;

/// <summary>
/// Software update from the project's GitHub releases: finds a newer
/// nekodrop-X.Y.Z-setup.exe, downloads it, checks its SHA-256 against the
/// digest GitHub publishes for the asset, and runs it (Windows asks for
/// administrator rights). Nothing installs without the user choosing to.
/// </summary>
public static class Updater
{
    const string ReleasesApi = "https://api.github.com/repos/zheznanohana/nekodrop/releases?per_page=15";
    const string ReleasesAtom = "https://github.com/zheznanohana/nekodrop/releases.atom";
    const string TagUrlPrefix = "https://github.com/zheznanohana/nekodrop/releases/tag/";
    public const string ReleasesPage = "https://github.com/zheznanohana/nekodrop/releases";

    public enum CheckStatus { UpToDate, Available, RateLimited, Failed }

    public sealed record Release(Version Version, string Tag, string Url, string Sha256, bool Preview, string Page);

    public readonly record struct DownloadProgress(int Percent, long BytesPerSecond);

    public sealed record CheckResult(
        CheckStatus Status,
        Release? Release = null,
        string? NewerTag = null,
        string? PageUrl = null,
        string? Detail = null);

    static readonly HttpClient Http = MakeClient();

    static HttpClient MakeClient()
    {
        var c = new HttpClient { Timeout = TimeSpan.FromMinutes(10) };
        c.DefaultRequestHeaders.UserAgent.ParseAdd("nekodrop-updater");
        c.DefaultRequestHeaders.Accept.ParseAdd("application/vnd.github+json");
        return c;
    }

    static string CachePath =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "nekodrop", "update-cache.json");

    static (string? Etag, string? Json) ReadCache()
    {
        try
        {
            if (!File.Exists(CachePath)) return (null, null);
            var node = JsonNode.Parse(File.ReadAllText(CachePath));
            return ((string?)node?["etag"], (string?)node?["json"]);
        }
        catch (Exception) { return (null, null); }
    }

    static void WriteCache(string? etag, string json)
    {
        try
        {
            var dir = Path.GetDirectoryName(CachePath);
            if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
            var obj = new JsonObject { ["etag"] = etag ?? "", ["json"] = json };
            File.WriteAllText(CachePath, obj.ToJsonString());
        }
        catch (Exception) { }
    }

    static Release? ParseReleases(string json, bool includePreview)
    {
        Release? best = null;
        foreach (var r in JsonNode.Parse(json)?.AsArray() ?? [])
        {
            if (r is null || (bool?)r["draft"] == true) continue;
            var preview = (bool?)r["prerelease"] == true;
            if (preview && !includePreview) continue;
            var tag = (string?)r["tag_name"] ?? "";
            if (!Version.TryParse(tag.TrimStart('v'), out var v) || v <= Current) continue;
            foreach (var a in r["assets"]?.AsArray() ?? [])
            {
                var name = (string?)a?["name"] ?? "";
                var digest = (string?)a?["digest"] ?? "";
                if (!name.StartsWith("nekodrop-", StringComparison.Ordinal) || !name.EndsWith("-setup.exe", StringComparison.Ordinal)) continue;
                // An installer without a published digest is never run.
                if (!digest.StartsWith("sha256:", StringComparison.Ordinal)) continue;
                var url = (string?)a?["browser_download_url"] ?? "";
                if (!url.StartsWith("https://github.com/zheznanohana/nekodrop/releases/download/", StringComparison.Ordinal)) continue;
                if (best is null || v > best.Version)
                    best = new Release(v, tag, url, digest["sha256:".Length..].ToLowerInvariant(), preview, (string?)r["html_url"] ?? "");
            }
        }
        return best;
    }

    /// <summary>
    /// Fallback when api.github.com is rate-limited (e.g. shared proxy IP, #4):
    /// reads releases.atom (outside the 60/hr REST API quota) only to detect if a
    /// newer tag exists. Never auto-installs because Atom carries no SHA-256 digest.
    /// </summary>
    static async Task<(string? Tag, string? Url)> FindNewerFromAtom(CancellationToken ct)
    {
        try
        {
            using var req = new HttpRequestMessage(HttpMethod.Get, ReleasesAtom);
            req.Headers.Accept.Clear();
            req.Headers.Accept.ParseAdd("application/atom+xml");
            using var resp = await Http.SendAsync(req, ct);
            if (!resp.IsSuccessStatusCode) return (null, null);
            var xml = await resp.Content.ReadAsStringAsync(ct);
            var doc = XDocument.Parse(xml);
            XNamespace atom = "http://www.w3.org/2005/Atom";
            Version? bestVer = null;
            string? bestTag = null, bestUrl = null;
            foreach (var entry in doc.Descendants(atom + "entry"))
            {
                var href = entry.Element(atom + "link")?.Attribute("href")?.Value ?? "";
                if (!href.StartsWith(TagUrlPrefix, StringComparison.Ordinal)) continue;
                var tag = href[TagUrlPrefix.Length..].Split('?', '#')[0];
                if (!Version.TryParse(tag.TrimStart('v'), out var v) || v <= Current) continue;
                if (bestVer is null || v > bestVer)
                {
                    bestVer = v;
                    bestTag = tag;
                    bestUrl = href;
                }
            }
            return (bestTag, bestUrl);
        }
        catch (Exception) { return (null, null); }
    }

    /// <summary>This build's version (0.1.16 of "0.1.16+commit").</summary>
    public static Version Current
    {
        get
        {
            var info = Assembly.GetEntryAssembly()?.GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion;
            var text = (info ?? "0.0.0").Split('+')[0];
            return Version.TryParse(text, out var v) ? v : new Version(0, 0, 0);
        }
    }

    /// <summary>
    /// Checks GitHub releases with ETag caching (304 responses do not consume the
    /// 60/hr unauthenticated quota) and distinguishes HTTP 403 rate limits behind
    /// shared proxies from "already up to date" (#4).
    /// </summary>
    public static async Task<CheckResult> Check(bool includePreview)
    {
        var (cachedEtag, cachedJson) = ReadCache();
        try
        {
            using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(15));
            using var req = new HttpRequestMessage(HttpMethod.Get, ReleasesApi);
            if (!string.IsNullOrWhiteSpace(cachedEtag)) req.Headers.IfNoneMatch.TryParseAdd(cachedEtag);
            using var resp = await Http.SendAsync(req, cts.Token);

            if (resp.StatusCode == HttpStatusCode.NotModified && !string.IsNullOrEmpty(cachedJson))
            {
                var best = ParseReleases(cachedJson, includePreview);
                return best is not null ? new CheckResult(CheckStatus.Available, best) : new CheckResult(CheckStatus.UpToDate);
            }

            if (resp.IsSuccessStatusCode)
            {
                var json = await resp.Content.ReadAsStringAsync(cts.Token);
                WriteCache(resp.Headers.ETag?.ToString(), json);
                var best = ParseReleases(json, includePreview);
                return best is not null ? new CheckResult(CheckStatus.Available, best) : new CheckResult(CheckStatus.UpToDate);
            }

            if (resp.StatusCode is HttpStatusCode.Forbidden or HttpStatusCode.TooManyRequests)
            {
                var rateLimited = resp.StatusCode == HttpStatusCode.TooManyRequests ||
                    (resp.Headers.TryGetValues("X-RateLimit-Remaining", out var rem) && rem.FirstOrDefault() == "0");
                if (rateLimited)
                {
                    if (!string.IsNullOrEmpty(cachedJson) && ParseReleases(cachedJson, includePreview) is { } cached)
                        return new CheckResult(CheckStatus.Available, cached);
                    var (tag, url) = await FindNewerFromAtom(cts.Token);
                    return new CheckResult(CheckStatus.RateLimited, NewerTag: tag, PageUrl: url ?? ReleasesPage);
                }
            }

            return new CheckResult(CheckStatus.Failed, Detail: $"HTTP {(int)resp.StatusCode} ({resp.ReasonPhrase})");
        }
        catch (OperationCanceledException)
        {
            return new CheckResult(CheckStatus.Failed, Detail: "timeout");
        }
        catch (Exception e)
        {
            return new CheckResult(CheckStatus.Failed, Detail: e.Message);
        }
    }

    /// <summary>The newest release above this build with an installer and a digest; null if none.</summary>
    public static async Task<Release?> FindNewer(bool includePreview) => (await Check(includePreview)).Release;

    /// <summary>Downloads, verifies and starts the installer. Returns "" or why not.</summary>
    public static async Task<string> Install(Release r, IProgress<DownloadProgress>? progress = null, bool deleteAfterInstall = true)
    {
        var dir = Path.Combine(Path.GetTempPath(), "nekodrop-update");
        Directory.CreateDirectory(dir);
        var path = Path.Combine(dir, $"nekodrop-{r.Version}-setup.exe");
        using (var resp = await Http.GetAsync(r.Url, HttpCompletionOption.ResponseHeadersRead))
        {
            resp.EnsureSuccessStatusCode();
            var total = resp.Content.Headers.ContentLength ?? 0;
            await using var src = await resp.Content.ReadAsStreamAsync();
            await using var dst = File.Create(path);
            var buf = new byte[1 << 16];
            long done = 0;
            int n;
            var clock = Stopwatch.StartNew();
            long lastDone = 0;
            long lastMs = 0;
            void Report()
            {
                var now = clock.ElapsedMilliseconds;
                var elapsed = now - lastMs;
                if (elapsed < 1) return;
                var bps = (done - lastDone) * 1000 / elapsed;
                if (total > 0) progress?.Report(new((int)(done * 100 / total), bps));
                lastDone = done;
                lastMs = now;
            }
            System.Threading.Timer? timer = null;
            if (progress is not null)
                timer = new System.Threading.Timer(_ => { try { Report(); } catch (Exception) { } }, null, 500, 500);
            try
            {
                while ((n = await src.ReadAsync(buf)) > 0)
                {
                    await dst.WriteAsync(buf.AsMemory(0, n));
                    done += n;
                }
            }
            finally
            {
                timer?.Dispose();
            }
            if (progress is not null && total > 0)
            {
                var avg = clock.ElapsedMilliseconds > 0 ? done * 1000 / clock.ElapsedMilliseconds : 0;
                progress.Report(new(100, avg));
            }
        }
        string hash;
        await using (var f = File.OpenRead(path))
            hash = Convert.ToHexString(await SHA256.HashDataAsync(f)).ToLowerInvariant();
        if (hash != r.Sha256)
        {
            File.Delete(path);
            return "the download does not match the published SHA-256";
        }
        // The installer closes this app itself (CloseApplications) and starts it again afterwards.
        StartInstaller(path, deleteAfterInstall);
        return "";
    }

    /// <summary>
    /// Starts the installer. With deleteAfterInstall, a detached PowerShell helper
    /// waits for the installer and removes the setup file only after a successful
    /// exit. The app is closed by the installer itself, so the helper must live
    /// outside this process.
    /// </summary>
    static void StartInstaller(string path, bool deleteAfterInstall)
    {
        if (!deleteAfterInstall)
        {
            Process.Start(new ProcessStartInfo(path, "/SILENT /NORESTART") { UseShellExecute = true });
            return;
        }

        string? helper = null;
        try
        {
            helper = Path.Combine(Path.GetDirectoryName(path)!, $"{Path.GetFileNameWithoutExtension(path)}-cleanup.ps1");
            File.WriteAllText(helper, CleanupScript);
            var powershell = Path.Combine(Environment.SystemDirectory, "WindowsPowerShell", "v1.0", "powershell.exe");
            var psi = new ProcessStartInfo(powershell)
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                WindowStyle = ProcessWindowStyle.Hidden,
            };
            psi.ArgumentList.Add("-NoProfile");
            psi.ArgumentList.Add("-NonInteractive");
            psi.ArgumentList.Add("-ExecutionPolicy");
            psi.ArgumentList.Add("Bypass");
            psi.ArgumentList.Add("-File");
            psi.ArgumentList.Add(helper);
            psi.ArgumentList.Add("-Installer");
            psi.ArgumentList.Add(path);
            Process.Start(psi);
        }
        catch (Exception)
        {
            // If the helper cannot start, still run the update; leaving the file is
            // better than blocking the installation.
            if (helper is not null)
            {
                try { File.Delete(helper); } catch (Exception) { }
            }
            Process.Start(new ProcessStartInfo(path, "/SILENT /NORESTART") { UseShellExecute = true });
        }
    }

    const string CleanupScript = """
param([Parameter(Mandatory = $true)][string]$Installer)
$ErrorActionPreference = 'SilentlyContinue'
try {
    $p = Start-Process -FilePath $Installer -ArgumentList '/SILENT', '/NORESTART' -PassThru -ErrorAction Stop
    if ($p.WaitForExit(1800000) -and $p.ExitCode -eq 0) {
        Remove-Item -LiteralPath $Installer -Force
    }
} catch {
}
Remove-Item -LiteralPath $PSCommandPath -Force
""";
}
