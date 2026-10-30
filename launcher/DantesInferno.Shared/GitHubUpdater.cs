using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Web.Script.Serialization;

namespace DantesInferno
{
    public class ReleaseAsset
    {
        public string Name { get; set; }
        public string BrowserDownloadUrl { get; set; }
    }

    public class ReleaseInfo
    {
        public string TagName { get; set; }
        public string Name { get; set; }
        public string Body { get; set; }
        public string HtmlUrl { get; set; }
        public List<ReleaseAsset> Assets { get; } = new List<ReleaseAsset>();

        public SemanticVersion Version => SemanticVersion.Parse(TagName);
    }

    public class SemanticVersion : IComparable<SemanticVersion>
    {
        private readonly int[] _numbers = new int[4];

        public string Prerelease { get; private set; }

        public static SemanticVersion Parse(string input)
        {
            var result = new SemanticVersion();
            if (string.IsNullOrWhiteSpace(input))
                return result;

            string text = input.Trim().ToLowerInvariant().TrimStart('v');
            int plus = text.IndexOf('+');
            if (plus >= 0)
                text = text.Substring(0, plus);
            int dash = text.IndexOf('-');
            if (dash >= 0)
            {
                result.Prerelease = text.Substring(dash + 1);
                text = text.Substring(0, dash);
            }

            var parts = text.Split('.');
            for (int i = 0; i < parts.Length && i < result._numbers.Length; i++)
                int.TryParse(parts[i], out result._numbers[i]);
            return result;
        }

        public int CompareTo(SemanticVersion other)
        {
            if (other == null) return 1;
            for (int i = 0; i < _numbers.Length; i++)
            {
                int result = _numbers[i].CompareTo(other._numbers[i]);
                if (result != 0) return result;
            }

            bool release = string.IsNullOrEmpty(Prerelease);
            bool otherRelease = string.IsNullOrEmpty(other.Prerelease);
            if (release || otherRelease)
                return release == otherRelease ? 0 : (release ? 1 : -1);

            var a = Prerelease.Split('.');
            var b = other.Prerelease.Split('.');
            for (int i = 0; i < Math.Max(a.Length, b.Length); i++)
            {
                if (i >= a.Length) return -1;
                if (i >= b.Length) return 1;
                int result = int.TryParse(a[i], out int av) && int.TryParse(b[i], out int bv)
                    ? av.CompareTo(bv)
                    : string.Compare(a[i], b[i], StringComparison.Ordinal);
                if (result != 0) return result;
            }
            return 0;
        }

        public override string ToString()
        {
            int count = _numbers[3] > 0 ? 4 : _numbers[2] > 0 ? 3 : 2;
            string v = string.Join(".", Array.ConvertAll(_numbers, n => n.ToString()), 0, count);
            return string.IsNullOrEmpty(Prerelease) ? v : v + "-" + Prerelease;
        }

        public static bool operator >(SemanticVersion a, SemanticVersion b) { return a.CompareTo(b) > 0; }
        public static bool operator <=(SemanticVersion a, SemanticVersion b) { return a.CompareTo(b) <= 0; }
        public static bool operator <(SemanticVersion a, SemanticVersion b) { return a.CompareTo(b) < 0; }
        public static bool operator >=(SemanticVersion a, SemanticVersion b) { return a.CompareTo(b) >= 0; }
    }

    public static class GitHubUpdater
    {
        private const string ApiUrl = "https://api.github.com/repos/florinp93/hells-gate-recomp/releases";
        private const string UserAgent = "DantesInfernoLauncher/1.0";

        static GitHubUpdater()
        {
            try { ServicePointManager.SecurityProtocol = (SecurityProtocolType)3072; }
            catch { }
        }

        public static ReleaseInfo CheckLatest()
        {
            using (var client = new WebClient())
            {
                client.Headers["User-Agent"] = UserAgent;
                var releases = new JavaScriptSerializer().Deserialize<List<object>>(client.DownloadString(ApiUrl));
                if (releases == null || releases.Count == 0)
                    throw new InvalidOperationException("No releases found.");
                if (!(releases[0] is Dictionary<string, object> latest))
                    throw new InvalidOperationException("Unexpected release list format.");
                return MapRelease(latest);
            }
        }

        public static SemanticVersion GetLocalVersion(string installDirectory)
        {
            string versionPath = PathHelper.GetVersionPath(installDirectory);
            return SemanticVersion.Parse(File.Exists(versionPath) ? File.ReadAllText(versionPath) : null);
        }

        public static string DownloadAsset(ReleaseAsset asset, string destDir, Action<long, long> progress = null)
        {
            if (asset == null || string.IsNullOrEmpty(asset.BrowserDownloadUrl))
                throw new InvalidOperationException("Asset has no download URL.");

            Directory.CreateDirectory(destDir);
            string destPath = Path.Combine(destDir, asset.Name ?? "download.bin");
            using (var client = new WebClient())
            {
                client.Headers["User-Agent"] = UserAgent;
                if (progress != null)
                    client.DownloadProgressChanged += (s, e) => progress(e.BytesReceived, e.TotalBytesToReceive);
                client.DownloadFile(asset.BrowserDownloadUrl, destPath);
            }
            return destPath;
        }

        private static ReleaseInfo MapRelease(Dictionary<string, object> raw)
        {
            var info = new ReleaseInfo
            {
                TagName = raw.GetString("tag_name"),
                Name = raw.GetString("name"),
                Body = raw.GetString("body"),
                HtmlUrl = raw.GetString("html_url"),
            };
            if (raw.TryGetValue("assets", out var assets) && assets is IEnumerable list)
            {
                foreach (var item in list)
                {
                    if (item is Dictionary<string, object> asset)
                        info.Assets.Add(new ReleaseAsset
                        {
                            Name = asset.GetString("name"),
                            BrowserDownloadUrl = asset.GetString("browser_download_url"),
                        });
                }
            }
            return info;
        }

        private static string GetString(this Dictionary<string, object> dict, string key)
        {
            return dict.TryGetValue(key, out var value) ? value as string : null;
        }
    }
}
