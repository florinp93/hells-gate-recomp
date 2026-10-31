using System.Collections.Generic;
using System.IO;

namespace DantesInferno
{
    public static class XexParser
    {
        public const uint XEX_REGION_NTSCU = 0x000000FF;
        public const uint XEX_REGION_NTSCJ = 0x0000FF00;
        public const uint XEX_REGION_PAL = 0x00FF0000;

        private const uint XEX_HEADER_EXECUTION_INFO = 0x00040006;

        public static readonly Dictionary<uint, string> LanguageNames = new Dictionary<uint, string>
        {
            { 1, "English" },
            { 2, "Japanese" },
            { 3, "German" },
            { 4, "French" },
            { 5, "Spanish" },
            { 6, "Italian" },
            { 7, "Korean" },
            { 8, "Chinese (Traditional)" },
            { 9, "Portuguese" },
            { 10, "Chinese (Simplified)" },
            { 11, "Polish" },
            { 12, "Russian" },
        };

        private static readonly uint[] NtscULanguages = { 1, 4, 5 };
        private static readonly uint[] NtscJLanguages = { 1, 2 };
        private static readonly uint[] PalLanguages = { 1, 3, 4, 5, 6 };

        public class XexInfo
        {
            public uint MediaId;
            public uint TitleId;
            public uint Region;
        }

        public static XexInfo Parse(string xexPath)
        {
            if (!File.Exists(xexPath))
                return null;
            using (var fs = new FileStream(xexPath, FileMode.Open, FileAccess.Read, FileShare.Read))
                return Parse(fs);
        }

        public static XexInfo Parse(Stream stream)
        {
            var reader = new BinaryReader(stream);
            long length = stream.Length;
            if (ReadUInt32BE(reader) != 0x58455832)
                return null;

            stream.Position = 0x10;
            uint securityOffset = ReadUInt32BE(reader);
            uint headerCount = ReadUInt32BE(reader);
            var info = new XexInfo();

            for (int i = 0; i < headerCount && i < 64; i++)
            {
                long headerPos = 0x18 + i * 8;
                if (headerPos + 8 > length)
                    break;
                stream.Position = headerPos;
                uint key = ReadUInt32BE(reader);
                uint value = ReadUInt32BE(reader);
                if (key == XEX_HEADER_EXECUTION_INFO && value > 0 && value + 0x18 <= length)
                {
                    stream.Position = value;
                    info.MediaId = ReadUInt32BE(reader);
                    stream.Position = value + 0x0C;
                    info.TitleId = ReadUInt32BE(reader);
                }
            }

            if (securityOffset > 0 && securityOffset + 0x17C <= length)
            {
                stream.Position = securityOffset + 0x178;
                info.Region = ReadUInt32BE(reader);
            }
            return info;
        }

        public static List<uint> GetSupportedLanguages(uint region)
        {
            if (region == 0xFFFFFFFF)
                return new List<uint>(PalLanguages);

            var languages = new List<uint>();
            void AddAll(uint[] ids)
            {
                foreach (uint id in ids)
                    if (!languages.Contains(id)) languages.Add(id);
            }
            if ((region & XEX_REGION_NTSCU) != 0) AddAll(NtscULanguages);
            if ((region & XEX_REGION_NTSCJ) != 0) AddAll(NtscJLanguages);
            if ((region & XEX_REGION_PAL) != 0) AddAll(PalLanguages);
            if (languages.Count == 0)
                languages.Add(1);
            return languages;
        }

        private static uint ReadUInt32BE(BinaryReader reader)
        {
            byte[] bytes = reader.ReadBytes(4);
            if (bytes.Length < 4) return 0;
            return (uint)((bytes[0] << 24) | (bytes[1] << 16) | (bytes[2] << 8) | bytes[3]);
        }
    }
}
