using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;

namespace DantesInferno
{
    public class DlcPackage
    {
        public string Path { get; set; }
        public string Magic { get; set; }
        public uint ContentType { get; set; }
        public uint TitleId { get; set; }
        public string DisplayName { get; set; }
        public string ContentId { get; set; }
    }

    public class DlcInstallResult
    {
        public List<string> Installed = new List<string>();
        public List<string> AlreadyInstalled = new List<string>();
        public List<string> Skipped = new List<string>();
        public List<string> Errors = new List<string>();
        public int UnknownFiles;
    }

    public static class DlcInstaller
    {
        public const uint DantesInfernoTitleId = 0x454108CF;
        public const uint MarketplaceContent = 0x00000002;
        public const uint SavedGame = 0x00000001;
        public const uint TitleUpdate = 0x000B0000;

        private static readonly string[] ArchiveExtensions = { ".zip", ".7z", ".rar", ".tar", ".gz", ".xz" };

        public static string PackageFolder(string dlcRoot)
        {
            return Path.Combine(dlcRoot, "454108CF", "00000002");
        }

        public static DlcPackage ReadPackage(string path)
        {
            try
            {
                var header = new byte[0x491];
                using (var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
                {
                    if (stream.Length < header.Length)
                        return null;
                    int read = 0;
                    while (read < header.Length)
                    {
                        int n = stream.Read(header, read, header.Length - read);
                        if (n <= 0)
                            return null;
                        read += n;
                    }
                }
                string magic = Encoding.ASCII.GetString(header, 0, 4);
                if (magic != "LIVE" && magic != "PIRS" && magic != "CON ")
                    return null;
                string name = Encoding.BigEndianUnicode.GetString(header, 0x411, 0x80);
                int end = name.IndexOf('\0');
                if (end >= 0)
                    name = name.Substring(0, end);
                return new DlcPackage
                {
                    Path = path,
                    Magic = magic.Trim(),
                    ContentType = ReadUInt32BE(header, 0x344),
                    TitleId = ReadUInt32BE(header, 0x360),
                    DisplayName = name.Trim(),
                    ContentId = BitConverter.ToString(header, 0x32C, 20).Replace("-", ""),
                };
            }
            catch
            {
                return null;
            }
        }

        public static List<DlcPackage> ListInstalled(string dlcRoot)
        {
            var packages = new List<DlcPackage>();
            if (!Directory.Exists(dlcRoot))
                return packages;
            foreach (string file in Directory.GetFiles(dlcRoot, "*", SearchOption.AllDirectories))
            {
                DlcPackage package = ReadPackage(file);
                if (package != null && package.TitleId == DantesInfernoTitleId &&
                    package.ContentType == MarketplaceContent)
                    packages.Add(package);
            }
            return packages;
        }

        public static DlcInstallResult Install(IEnumerable<string> inputs, string dlcRoot)
        {
            var result = new DlcInstallResult();
            var temps = new List<string>();
            try
            {
                var files = new List<string>();
                foreach (string input in inputs)
                    Collect(input, files, temps, result);
                string target = PackageFolder(dlcRoot);
                var installed = ListInstalled(dlcRoot).ToDictionary(p => p.ContentId, p => p);
                foreach (string file in files)
                {
                    DlcPackage package = ReadPackage(file);
                    if (package == null)
                    {
                        result.UnknownFiles++;
                        continue;
                    }
                    string label = string.IsNullOrEmpty(package.DisplayName)
                        ? System.IO.Path.GetFileName(file)
                        : package.DisplayName;
                    if (package.TitleId != DantesInfernoTitleId)
                    {
                        result.Skipped.Add(label + ": not a Dante's Inferno package (title " +
                            package.TitleId.ToString("X8") + ")");
                        continue;
                    }
                    if (package.ContentType == TitleUpdate)
                    {
                        result.Skipped.Add(label + ": title update, already included with the game");
                        continue;
                    }
                    if (package.ContentType == SavedGame)
                    {
                        result.Skipped.Add(label + ": a save game, not DLC");
                        continue;
                    }
                    if (package.ContentType != MarketplaceContent)
                    {
                        result.Skipped.Add(label + ": not DLC (content type " + package.ContentType.ToString("X8") + ")");
                        continue;
                    }
                    if (installed.ContainsKey(package.ContentId))
                    {
                        result.AlreadyInstalled.Add(label);
                        continue;
                    }
                    try
                    {
                        Directory.CreateDirectory(target);
                        string name = System.IO.Path.GetFileName(file);
                        if (!IsPackageName(name))
                            name = package.ContentId;
                        File.Copy(file, System.IO.Path.Combine(target, name), true);
                        installed[package.ContentId] = package;
                        result.Installed.Add(label);
                    }
                    catch (Exception ex)
                    {
                        result.Errors.Add(label + ": " + ex.Message);
                    }
                }
            }
            finally
            {
                foreach (string temp in temps)
                {
                    try
                    {
                        Directory.Delete(temp, true);
                    }
                    catch { }
                }
            }
            return result;
        }

        private static void Collect(string input, List<string> files, List<string> temps, DlcInstallResult result)
        {
            if (Directory.Exists(input))
            {
                foreach (string file in Directory.GetFiles(input, "*", SearchOption.AllDirectories))
                    CollectFile(file, files, temps, result);
            }
            else if (File.Exists(input))
            {
                CollectFile(input, files, temps, result);
            }
        }

        private static void CollectFile(string file, List<string> files, List<string> temps, DlcInstallResult result)
        {
            string extension = System.IO.Path.GetExtension(file).ToLowerInvariant();
            if (!ArchiveExtensions.Contains(extension) || ReadPackage(file) != null)
            {
                files.Add(file);
                return;
            }
            string temp = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "dantes_inferno_dlc_" + Guid.NewGuid().ToString("N"));
            temps.Add(temp);
            Directory.CreateDirectory(temp);
            string error;
            if (!Extract(file, temp, out error))
            {
                result.Errors.Add(System.IO.Path.GetFileName(file) + ": could not open the archive (" + error +
                    "). Extract it yourself and add the files inside.");
                return;
            }
            foreach (string inner in Directory.GetFiles(temp, "*", SearchOption.AllDirectories))
                CollectFile(inner, files, temps, result);
        }

        private static bool Extract(string archive, string destination, out string error)
        {
            error = null;
            try
            {
                var psi = new ProcessStartInfo
                {
                    FileName = System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "tar.exe"),
                    Arguments = "-x -f \"" + archive + "\" -C \"" + destination + "\"",
                    UseShellExecute = false,
                    CreateNoWindow = true,
                    RedirectStandardError = true,
                };
                using (var process = Process.Start(psi))
                {
                    string stderr = process.StandardError.ReadToEnd();
                    process.WaitForExit();
                    if (process.ExitCode == 0)
                        return true;
                    error = string.IsNullOrWhiteSpace(stderr) ? "exit code " + process.ExitCode : stderr.Trim();
                    return false;
                }
            }
            catch (Exception ex)
            {
                error = ex.Message;
                return false;
            }
        }

        private static bool IsPackageName(string name)
        {
            return name.Length >= 40 && name.Length <= 42 && name.All(Uri.IsHexDigit);
        }

        private static uint ReadUInt32BE(byte[] data, int offset)
        {
            return (uint)(data[offset] << 24 | data[offset + 1] << 16 | data[offset + 2] << 8 | data[offset + 3]);
        }
    }
}
