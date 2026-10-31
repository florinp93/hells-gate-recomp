using System;
using System.IO;
using System.Reflection;

namespace DantesInferno
{
    public static class PathHelper
    {
        public static string ExecutableDirectory
        {
            get
            {
                string path = Assembly.GetExecutingAssembly().Location;
                return Path.GetDirectoryName(path);
            }
        }

        public static string GetGameExecutablePath(string installDirectory)
        {
            return Path.Combine(installDirectory, "dantes_inferno.exe");
        }

        public static string GetGameConfigPath(string installDirectory)
        {
            return Path.Combine(installDirectory, "dantes_inferno.toml");
        }

        public static string GetGameDataPath(string installDirectory)
        {
            return Path.Combine(installDirectory, "game");
        }

        public static string GetVersionPath(string installDirectory)
        {
            return Path.Combine(installDirectory, "version.txt");
        }

        public static string GetLogsPath(string installDirectory)
        {
            return Path.Combine(installDirectory, "logs");
        }

        public static string GetDlcPath(string installDirectory)
        {
            return Path.Combine(installDirectory, "dlc");
        }

        public static string GetTitleUpdatePath(string gameDataDirectory)
        {
            return Path.Combine(gameDataDirectory, "default.xexp");
        }

        // Outside game\, so replacing the game files never loses it.
        public static string GetBundledTitleUpdatePath(string installDirectory)
        {
            return Path.Combine(installDirectory, "tu", "default.xexp");
        }

        // portable.txt next to the game keeps saves and the cache in userdata\
        // there instead of Documents (src/user_paths.cpp).
        public static string GetPortableMarkerPath(string installDirectory)
        {
            return Path.Combine(installDirectory, "portable.txt");
        }

        public static string GetPortableDataPath(string installDirectory)
        {
            return Path.Combine(installDirectory, "userdata");
        }

        public static string GetDocumentsDataPath()
        {
            return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "dantes_inferno");
        }

        public static string GetExtractXisoPath(string installDirectory)
        {
            return Path.Combine(installDirectory, "extract-xiso.exe");
        }
    }
}
