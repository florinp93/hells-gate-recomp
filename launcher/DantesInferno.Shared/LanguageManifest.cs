using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;

namespace DantesInferno
{
    /// <summary>
    /// The language manifest in bigfile*.viv: the text and audio languages on
    /// the disc. The XEX region flags list more than some discs carry (the
    /// "pal_it" disc only has English, German and Italian).
    /// </summary>
    public static class LanguageManifest
    {
        private const uint BigMagic = 0x42494748; // "BIGH" big-endian

        // Manifest signature: first line is a small number, second line is
        // the title id "454108CF-XX".
        private static readonly Regex Signature =
            new Regex("^\\d+\\r?\\n[0-9A-Fa-f]{8}-[0-9A-Fa-f]{2}",
                      RegexOptions.Compiled);

        public class Entry
        {
            public uint Id;
            public string Name;
        }

        /// <summary>
        /// The disc's text languages, or null without a readable manifest.
        /// </summary>
        public static List<Entry> GetDiscTextLanguages(string gameDir)
        {
            if (string.IsNullOrEmpty(gameDir) || !Directory.Exists(gameDir))
                return null;

            foreach (string vivPath in Directory.GetFiles(gameDir, "bigfile*.viv"))
            {
                var result = TryReadManifest(vivPath);
                if (result != null && result.Count > 0)
                    return result;
            }
            return null;
        }

        private static List<Entry> TryReadManifest(string vivPath)
        {
            try
            {
                using (var fs = new FileStream(vivPath, FileMode.Open,
                                             FileAccess.Read, FileShare.Read))
                using (var reader = new BinaryReader(fs))
                {
                    if (reader.BaseStream.Length < 16)
                        return null;
                    if (ReadUInt32BE(reader) != BigMagic)
                        return null;
                    reader.ReadUInt32(); // total size (little-endian, unused)
                    uint numFiles = ReadUInt32BE(reader);
                    reader.ReadUInt32(); // header size

                    for (uint i = 0; i < numFiles; i++)
                    {
                        uint offset = ReadUInt32BE(reader);
                        uint size = ReadUInt32BE(reader);
                        reader.ReadUInt32(); // name hash

                        if (size < 32 || size > 8192)
                            continue;
                        if (offset + size > reader.BaseStream.Length)
                            continue;

                        long saved = reader.BaseStream.Position;
                        reader.BaseStream.Position = offset;
                        byte[] data = reader.ReadBytes((int)size);
                        reader.BaseStream.Position = saved;

                        string text = Encoding.ASCII.GetString(data);
                        if (!Signature.IsMatch(text))
                            continue;

                        var parsed = ParseManifest(text);
                        if (parsed != null)
                            return parsed;
                    }
                }
            }
            catch
            {
            }
            return null;
        }

        private static List<Entry> ParseManifest(string text)
        {
            string[] lines = text.Split('\n');
            int p = Array.FindIndex(lines,
                l => l.Trim('\r') == "default");
            if (p < 0 || p + 2 >= lines.Length)
                return null;

            if (!int.TryParse(lines[p + 1].Trim('\r'), out int textCount) ||
                textCount <= 0 || textCount > 32)
                return null;

            var result = new List<Entry>();
            int cursor = p + 3; // skip "default", text count, audio count
            for (int i = 0; i < textCount; i++)
            {
                // Text record: code, display name, numeric id, tag.
                if (cursor + 3 >= lines.Length)
                    return null;
                if (!uint.TryParse(lines[cursor + 2].Trim('\r'), out uint id))
                    return null;
                string name = lines[cursor + 1].Trim('\r');
                if (id >= 1 && id <= 12 && name.Length > 0)
                    result.Add(new Entry { Id = id, Name = name });
                cursor += 4;
            }
            return result.Count > 0 ? result : null;
        }

        private static uint ReadUInt32BE(BinaryReader reader)
        {
            byte[] b = reader.ReadBytes(4);
            if (b.Length < 4) return 0;
            return (uint)((b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]);
        }
    }
}
