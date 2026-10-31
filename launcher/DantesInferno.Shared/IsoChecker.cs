using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;

namespace DantesInferno
{
    public class IsoFileEntry
    {
        public string Path;
        public long Offset;
        public long Size;
    }

    public class IsoCheckResult
    {
        public bool CanExtract;
        public string Problem;
        public string Release;
        public bool KnownRelease;
        public List<IsoFileEntry> Files = new List<IsoFileEntry>();
        public List<string> Report = new List<string>();
    }

    public static class IsoChecker
    {
        public const uint DantesInfernoTitleId = 0x454108CF;

        private static readonly Dictionary<string, string> KnownXex = new Dictionary<string, string>
        {
            { "213F84F062658B3EED34D211F9DF973FBF569FB2", "Europe (En, De, It)" },
        };

        private static readonly long[] PartitionOffsets = { 0, 0xFD90000, 0x2080000, 0x18300000 };
        private static readonly string[] RequiredFiles = { "default.xex", "bigfile0.viv", "bigfile1.viv" };
        private const int SectorSize = 2048;
        private static readonly byte[] XdvdfsMagic = Encoding.ASCII.GetBytes("MICROSOFT*XBOX*MEDIA");

        public static IsoCheckResult Check(string isoPath)
        {
            var result = new IsoCheckResult();
            result.Report.Add("ISO: " + isoPath);
            if (!File.Exists(isoPath))
                return Fail(result, "The file does not exist.");
            long length = new FileInfo(isoPath).Length;
            result.Report.Add(string.Format(CultureInfo.InvariantCulture, "Size: {0:N0} bytes", length));

            using (var stream = new FileStream(isoPath, FileMode.Open, FileAccess.Read, FileShare.Read))
            {
                long partition = -1;
                foreach (long offset in PartitionOffsets)
                {
                    if (MatchAt(stream, offset + 0x10000, XdvdfsMagic))
                    {
                        partition = offset;
                        break;
                    }
                }
                if (partition < 0)
                {
                    if (MatchAt(stream, 0x800, Encoding.ASCII.GetBytes("PlayStation3")))
                        return Fail(result, "This is a PlayStation 3 disc image. The port needs the Xbox 360 version of Dante's Inferno.");
                    if (MatchAt(stream, 0, Encoding.ASCII.GetBytes("LIVE")) || MatchAt(stream, 0, Encoding.ASCII.GetBytes("CON ")))
                        return Fail(result, "This is an Xbox 360 package (Games on Demand / DLC), not a disc image. The port needs the Xbox 360 disc ISO.");
                    return Fail(result, "This is not an Xbox 360 disc image (no Xbox 360 file system found). The file may be damaged, incomplete or in another format.");
                }
                result.Report.Add("Xbox 360 file system at 0x" + partition.ToString("X"));

                byte[] volume = ReadAt(stream, partition + 0x10000, 0x20);
                uint rootSector = BitConverter.ToUInt32(volume, 20);
                uint rootSize = BitConverter.ToUInt32(volume, 24);
                try
                {
                    ReadDirectory(stream, partition, rootSector, rootSize, "", result.Files, 0);
                }
                catch (EndOfStreamException)
                {
                    return Fail(result, string.Format(CultureInfo.InvariantCulture,
                        "The ISO is incomplete ({0:N0} MB). A full Dante's Inferno image is about 7,476 MB. Download or dump it again.",
                        length / (1024 * 1024)));
                }
                catch (Exception ex)
                {
                    return Fail(result, "The disc file system could not be read (" + ex.Message + "). The image is probably damaged.");
                }
                result.Report.Add("Files: " + result.Files.Count);

                long end = result.Files.Count > 0 ? result.Files.Max(f => f.Offset + f.Size) : 0;
                if (end > length)
                {
                    result.Report.Add(string.Format(CultureInfo.InvariantCulture, "Files end at {0:N0} bytes", end));
                    return Fail(result, string.Format(CultureInfo.InvariantCulture,
                        "The ISO is incomplete: it is {0:N0} MB but its files need {1:N0} MB. Download or dump it again.",
                        length / (1024 * 1024), end / (1024 * 1024)));
                }

                foreach (string required in RequiredFiles)
                {
                    if (!result.Files.Any(f => string.Equals(f.Path, required, StringComparison.OrdinalIgnoreCase)))
                        return Fail(result, required + " is missing from the ISO. This is not a complete Dante's Inferno disc image.");
                }

                IsoFileEntry xex = result.Files.First(f => string.Equals(f.Path, "default.xex", StringComparison.OrdinalIgnoreCase));
                byte[] xexData = ReadAt(stream, xex.Offset, (int)Math.Min(xex.Size, 64L * 1024 * 1024));
                string sha1;
                using (var hasher = SHA1.Create())
                    sha1 = BitConverter.ToString(hasher.ComputeHash(xexData)).Replace("-", "");
                var info = XexParser.Parse(new MemoryStream(xexData));
                if (info == null)
                    return Fail(result, "default.xex in the ISO is damaged (not an Xbox 360 executable).");
                result.Report.Add("default.xex SHA-1: " + sha1);
                result.Report.Add("Title ID: " + info.TitleId.ToString("X8") + ", media ID: " + info.MediaId.ToString("X8") +
                    ", region flags: " + info.Region.ToString("X8"));
                if (info.TitleId != DantesInfernoTitleId)
                    return Fail(result, "This ISO is another game (title ID " + info.TitleId.ToString("X8") +
                        "). The port needs Dante's Inferno (title ID 454108CF).");

                string known;
                result.KnownRelease = KnownXex.TryGetValue(sha1, out known);
                result.Release = result.KnownRelease ? known : RegionName(info.Region);
                result.Report.Add(result.KnownRelease
                    ? "Release: " + known + " (the release the port is built and tested with)"
                    : "Release: " + result.Release + " (not tested; should work with the bundled Title Update)");
                result.CanExtract = true;
                return result;
            }
        }

        public static string InstalledRelease(string gameDir)
        {
            try
            {
                string xex = System.IO.Path.Combine(gameDir, "default.xex");
                string sha1;
                using (var stream = File.OpenRead(xex))
                using (var hasher = SHA1.Create())
                    sha1 = BitConverter.ToString(hasher.ComputeHash(stream)).Replace("-", "");
                string known;
                if (KnownXex.TryGetValue(sha1, out known))
                    return known;
                var info = XexParser.Parse(xex);
                if (info == null)
                    return "damaged default.xex";
                if (info.TitleId != DantesInfernoTitleId)
                    return "not Dante's Inferno: title " + info.TitleId.ToString("X8");
                return RegionName(info.Region) + ", untested";
            }
            catch
            {
                return null;
            }
        }

        public static List<string> VerifyExtracted(IsoCheckResult check, string gameDir)
        {
            var problems = new List<string>();
            foreach (IsoFileEntry file in check.Files)
            {
                string path = System.IO.Path.Combine(gameDir, file.Path.Replace('/', '\\'));
                if (!File.Exists(path))
                    problems.Add(file.Path + ": missing");
                else if (new FileInfo(path).Length != file.Size)
                    problems.Add(string.Format(CultureInfo.InvariantCulture, "{0}: {1:N0} bytes instead of {2:N0}",
                        file.Path, new FileInfo(path).Length, file.Size));
            }
            return problems;
        }

        private static string RegionName(uint region)
        {
            var names = new List<string>();
            if ((region & XexParser.XEX_REGION_PAL) != 0) names.Add("PAL");
            if ((region & XexParser.XEX_REGION_NTSCU) != 0) names.Add("NTSC-U");
            if ((region & XexParser.XEX_REGION_NTSCJ) != 0) names.Add("NTSC-J");
            return names.Count == 0 ? "unknown region" : string.Join("/", names) + " release";
        }

        private static IsoCheckResult Fail(IsoCheckResult result, string problem)
        {
            result.CanExtract = false;
            result.Problem = problem;
            result.Report.Add("Problem: " + problem);
            return result;
        }

        private static void ReadDirectory(Stream stream, long partition, uint sector, uint size, string prefix,
            List<IsoFileEntry> files, int depth)
        {
            if (depth > 32 || size == 0)
                return;
            if (size > 16 * 1024 * 1024)
                throw new InvalidDataException("directory too large");
            byte[] dir = ReadAt(stream, partition + (long)sector * SectorSize, (int)size);
            var visited = new HashSet<int>();
            var pending = new Stack<int>();
            pending.Push(0);
            while (pending.Count > 0)
            {
                int offset = pending.Pop();
                if (offset + 14 > dir.Length || !visited.Add(offset))
                    continue;
                ushort left = BitConverter.ToUInt16(dir, offset);
                ushort right = BitConverter.ToUInt16(dir, offset + 2);
                if (left == 0xFFFF && right == 0xFFFF)
                    continue;
                uint entrySector = BitConverter.ToUInt32(dir, offset + 4);
                uint entrySize = BitConverter.ToUInt32(dir, offset + 8);
                byte attributes = dir[offset + 12];
                int nameLength = dir[offset + 13];
                if (offset + 14 + nameLength > dir.Length)
                    throw new InvalidDataException("bad directory entry");
                string name = Encoding.ASCII.GetString(dir, offset + 14, nameLength);
                string path = prefix.Length == 0 ? name : prefix + "/" + name;
                if ((attributes & 0x10) != 0)
                    ReadDirectory(stream, partition, entrySector, entrySize, path, files, depth + 1);
                else
                    files.Add(new IsoFileEntry { Path = path, Offset = partition + (long)entrySector * SectorSize, Size = entrySize });
                if (left != 0)
                    pending.Push(left * 4);
                if (right != 0)
                    pending.Push(right * 4);
            }
        }

        private static bool MatchAt(Stream stream, long offset, byte[] expected)
        {
            if (offset + expected.Length > stream.Length)
                return false;
            byte[] data = ReadAt(stream, offset, expected.Length);
            return data.SequenceEqual(expected);
        }

        private static byte[] ReadAt(Stream stream, long offset, int count)
        {
            if (offset < 0 || offset + count > stream.Length)
                throw new EndOfStreamException("read past the end of the image");
            stream.Position = offset;
            var data = new byte[count];
            int read = 0;
            while (read < count)
            {
                int n = stream.Read(data, read, count - read);
                if (n <= 0)
                    throw new EndOfStreamException("read past the end of the image");
                read += n;
            }
            return data;
        }
    }
}
