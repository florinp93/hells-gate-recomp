using System;
using System.Collections.Generic;
using System.IO;

namespace DantesInferno
{
    /// <summary>
    /// Parses Xbox 360 XEX file headers to extract game metadata
    /// (region, media ID, title ID) for language detection.
    /// </summary>
    public static class XexParser
    {
        // XEX region flags (from xex2_info.h)
        public const uint XEX_REGION_NTSCU = 0x000000FF;
        public const uint XEX_REGION_NTSCJ = 0x0000FF00;
        public const uint XEX_REGION_PAL = 0x00FF0000;
        public const uint XEX_REGION_OTHER = 0xFF000000;

        // XEX optional header IDs
        private const uint XEX_HEADER_EXECUTION_INFO = 0x00040006;

        // Xbox 360 language IDs (from XLanguage enum)
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

        public class XexInfo
        {
            public uint MediaId;
            public uint TitleId;
            public uint Region;
            public string XexPath;
        }

        /// <summary>
        /// Parses a XEX file and extracts game metadata.
        /// XEX header layout (all big-endian):
        ///   0x00: magic "XEX2"
        ///   0x04: module_flags
        ///   0x08: header_size
        ///   0x0C: reserved
        ///   0x10: security_offset (file offset to xex2_security_info)
        ///   0x14: header_count (number of optional headers)
        ///   0x18: optional headers (each 8 bytes: key + value/offset)
        /// </summary>
        public static XexInfo Parse(string xexPath)
        {
            if (!File.Exists(xexPath))
                return null;

            using (var fs = new FileStream(xexPath, FileMode.Open, FileAccess.Read, FileShare.Read))
            using (var reader = new BinaryReader(fs))
            {
                // Read magic
                uint magic = ReadUInt32BE(reader);
                if (magic != 0x58455832) // "XEX2"
                    return null;

                // Read header fields
                reader.BaseStream.Position = 0x10;
                uint securityOffset = ReadUInt32BE(reader);
                uint headerCount = ReadUInt32BE(reader);

                var info = new XexInfo { XexPath = xexPath };

                // Search optional headers for execution info
                // Optional headers start at 0x18, each is 8 bytes (key + value)
                for (int i = 0; i < headerCount && i < 64; i++)
                {
                    long headerPos = 0x18 + i * 8;
                    if (headerPos + 8 > reader.BaseStream.Length)
                        break;

                    reader.BaseStream.Position = headerPos;
                    uint key = ReadUInt32BE(reader);
                    uint value = ReadUInt32BE(reader);

                    if (key == XEX_HEADER_EXECUTION_INFO)
                    {
                        // value is the file offset to xex2_opt_execution_info
                        if (value > 0 && value + 0x18 <= reader.BaseStream.Length)
                        {
                            reader.BaseStream.Position = value;
                            // xex2_opt_execution_info:
                            // 0x00: media_id
                            // 0x04: version_value
                            // 0x08: base_version_value
                            // 0x0C: title_id
                            info.MediaId = ReadUInt32BE(reader);
                            reader.BaseStream.Position = value + 0x0C;
                            info.TitleId = ReadUInt32BE(reader);
                        }
                    }
                }

                // Parse security info for region
                // xex2_security_info has region at offset 0x178
                if (securityOffset > 0 && securityOffset + 0x17C <= reader.BaseStream.Length)
                {
                    reader.BaseStream.Position = securityOffset + 0x178;
                    info.Region = ReadUInt32BE(reader);
                }

                return info;
            }
        }

        /// <summary>
        /// Returns the list of languages supported by the game based on its region.
        /// NTSC-U: English, French, Spanish
        /// NTSC-J: Japanese, English
        /// PAL: English, French, German, Spanish, Italian
        /// Region-free (0xFFFFFFFF / all bits): treat as full MULTI set (PAL + NTSC).
        /// </summary>
        public static List<uint> GetSupportedLanguages(uint region)
        {
            var languages = new List<uint>();

            // Fully unlocked / region-free dumps (common MULTI5 scene tags).
            if (region == 0xFFFFFFFF)
            {
                languages.Add(1); // English
                languages.Add(3); // German
                languages.Add(4); // French
                languages.Add(5); // Spanish
                languages.Add(6); // Italian
                return languages;
            }

            bool isNtscU = (region & XEX_REGION_NTSCU) != 0;
            bool isNtscJ = (region & XEX_REGION_NTSCJ) != 0;
            bool isPal = (region & XEX_REGION_PAL) != 0;

            if (isNtscU)
            {
                languages.Add(1);  // English
                languages.Add(4);  // French
                languages.Add(5);  // Spanish
            }

            if (isNtscJ)
            {
                if (!languages.Contains(1)) languages.Add(1);  // English
                if (!languages.Contains(2)) languages.Add(2);  // Japanese
            }

            if (isPal)
            {
                if (!languages.Contains(1)) languages.Add(1);  // English
                if (!languages.Contains(3)) languages.Add(3);  // German
                if (!languages.Contains(4)) languages.Add(4);  // French
                if (!languages.Contains(5)) languages.Add(5);  // Spanish
                if (!languages.Contains(6)) languages.Add(6);  // Italian
            }

            if (languages.Count == 0)
            {
                // Unknown region, default to English
                languages.Add(1);
            }

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
