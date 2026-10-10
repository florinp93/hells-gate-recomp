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

        public static string GetTitleUpdatePath(string installDirectory)
        {
            return Path.Combine(installDirectory, "game", "default.xexp");
        }
    }
}
