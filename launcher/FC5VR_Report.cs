// Far Cry 5 VR launcher: watching the game after Play, and the report zip (FC5VR_report_<date>.zip in bin) for when the
// game does not start in VR. Nothing is uploaded. Built with FC5VR_Launcher.cs by build_launcher.bat (C# 5).
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Diagnostics.Eventing.Reader;
using System.Globalization;
using System.IO;
using System.IO.Compression;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using Microsoft.Win32;

// The game after Play (VR): found by name (Steam or Ubisoft Connect start it, and may restart it), waited for.
static class GameWatch
{
    [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll")] static extern uint WaitForSingleObject(IntPtr h, uint ms);
    [DllImport("kernel32.dll")] static extern bool GetExitCodeProcess(IntPtr h, out uint code);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    const uint SYNCHRONIZE = 0x00100000, PROCESS_QUERY_LIMITED_INFORMATION = 0x1000;

    // The handle is opened as soon as the game is seen, so its exit code can still be read after it closed.
    static IntPtr OpenGame(TimeSpan within)
    {
        var until = DateTime.Now + within;
        while (DateTime.Now < until) {
            foreach (var p in Process.GetProcessesByName("FarCry5")) {
                var h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, false, p.Id);
                if (h != IntPtr.Zero) return h;
            }
            Thread.Sleep(250);
        }
        return IntPtr.Zero;
    }

    // Blocks until the game has closed. found: it started at all (within 3 min); code: the last process's exit code.
    public static void WaitForGame(out bool found, out uint code)
    {
        found = false;
        code = 0;
        var h = OpenGame(TimeSpan.FromMinutes(3));
        while (h != IntPtr.Zero) {
            found = true;
            WaitForSingleObject(h, 0xFFFFFFFF);
            GetExitCodeProcess(h, out code);
            CloseHandle(h);
            // A relaunch (Steam -> Ubisoft Connect -> the game again) comes within seconds.
            h = OpenGame(TimeSpan.FromSeconds(20));
        }
    }
}

static class Report
{
    const long Cut = 5L * 1024 * 1024;

    // The mod writes next to FarCry5.exe, or in %APPDATA%\VR\FarCry5 when bin is not writable.
    static IEnumerable<string> LogDirs(string bin)
    {
        yield return bin;
        yield return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), @"VR\FarCry5");
    }

    static string Newest(string dir, string pattern)
    {
        try {
            if (!Directory.Exists(dir)) return null;
            string best = null;
            var t = DateTime.MinValue;
            foreach (var f in Directory.GetFiles(dir, pattern)) {
                var w = File.GetLastWriteTime(f);
                if (w > t) { t = w; best = f; }
            }
            return best;
        } catch { return null; }
    }

    // The newest crash report the mod wrote (fc5vr_logs\crash_*.txt).
    public static string NewestCrash(string bin)
    {
        string best = null;
        foreach (var d in LogDirs(bin)) {
            var f = Newest(Path.Combine(d, "fc5vr_logs"), "crash_*.txt");
            if (f != null && (best == null || File.GetLastWriteTime(f) > File.GetLastWriteTime(best))) best = f;
        }
        return best;
    }

    public static string LogPath(string bin)
    {
        foreach (var d in LogDirs(bin)) {
            var p = Path.Combine(d, "fc5vr_log.txt");
            if (File.Exists(p)) return p;
        }
        return Path.Combine(bin, "fc5vr_log.txt");
    }

    static IEnumerable<string> Lines(string path)
    {
        using (var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
        using (var r = new StreamReader(fs)) {
            string l;
            while ((l = r.ReadLine()) != null) yield return l;
        }
    }

    // The success line of the mod's start ("[Boot] VR running: first frame in the headset"), in a log written since a time.
    public static bool LogSaysVrRan(string log, DateTime since)
    {
        try {
            if (!File.Exists(log) || File.GetLastWriteTime(log) < since) return false;
            foreach (var l in Lines(log))
                if (l.Contains("[Boot] VR running")) return true;
        } catch { }
        return false;
    }

    // The last start step in the log ("step 17 OpenXR: session (+21034 ms, thread 1234)").
    public static string LastBootStep(string log)
    {
        string last = null;
        try {
            foreach (var l in Lines(log)) {
                int i = l.IndexOf("[Boot] step ", StringComparison.Ordinal);
                if (i >= 0) last = l.Substring(i + 7);
            }
        } catch { }
        return last;
    }

    static byte[] ReadShared(string path)
    {
        using (var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete)) {
            var len = fs.Length;
            if (len <= 2 * Cut) {
                var b = new byte[len];
                int got = 0;
                while (got < len) { int n = fs.Read(b, got, (int)(len - got)); if (n <= 0) break; got += n; }
                return b;
            }
            // Large: the first and the last 5 MB.
            var head = new byte[Cut];
            var tail = new byte[Cut];
            ReadAll(fs, head);
            fs.Seek(len - Cut, SeekOrigin.Begin);
            ReadAll(fs, tail);
            var mid = Encoding.ASCII.GetBytes("\r\n\r\n[... " + (len - 2 * Cut) + " bytes cut by the launcher ...]\r\n\r\n");
            var all = new byte[head.Length + mid.Length + tail.Length];
            Buffer.BlockCopy(head, 0, all, 0, head.Length);
            Buffer.BlockCopy(mid, 0, all, head.Length, mid.Length);
            Buffer.BlockCopy(tail, 0, all, head.Length + mid.Length, tail.Length);
            return all;
        }
    }

    static void ReadAll(Stream s, byte[] b)
    {
        int got = 0;
        while (got < b.Length) { int n = s.Read(b, got, b.Length - got); if (n <= 0) break; got += n; }
    }

    static void Add(ZipArchive zip, string path, string name, StringBuilder notes)
    {
        try {
            if (path == null || !File.Exists(path)) { notes.AppendLine("not found: " + (path ?? name)); return; }
            var data = ReadShared(path);
            var e = zip.CreateEntry(name, CompressionLevel.Optimal);
            using (var s = e.Open()) s.Write(data, 0, data.Length);
        } catch (Exception ex) { notes.AppendLine("could not add " + path + ": " + ex.Message); }
    }

    static void AddText(ZipArchive zip, string name, string text)
    {
        var e = zip.CreateEntry(name, CompressionLevel.Optimal);
        using (var w = new StreamWriter(e.Open(), new UTF8Encoding(false))) w.Write(text);
    }

    static string Reg(RegistryKey root, string key, string value)
    {
        try {
            using (var k = root.OpenSubKey(key)) {
                var v = k == null ? null : k.GetValue(value);
                return v == null ? null : v.ToString();
            }
        } catch { return null; }
    }

    static void Layers(StringBuilder sb, RegistryKey root, string label)
    {
        try {
            using (var k = root.OpenSubKey(@"SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit")) {
                if (k == null || k.GetValueNames().Length == 0) { sb.AppendLine("OpenXR implicit layers (" + label + "): none"); return; }
                foreach (var n in k.GetValueNames()) {
                    var v = k.GetValue(n);
                    string lname = null;
                    try {
                        if (File.Exists(n)) {
                            var m = Regex.Match(File.ReadAllText(n), "\"name\"\\s*:\\s*\"([^\"]*)\"");
                            if (m.Success) lname = m.Groups[1].Value;
                        }
                    } catch { }
                    sb.AppendLine("OpenXR implicit layer (" + label + "): " + n + " \"" + (lname ?? "?") + "\" " +
                                  (v is int && (int)v == 0 ? "ENABLED" : "disabled") + (File.Exists(n) ? "" : " (manifest missing)"));
                }
            }
        } catch (Exception e) { sb.AppendLine("OpenXR implicit layers (" + label + "): " + e.Message); }
    }

    static void Gpus(StringBuilder sb)
    {
        try {
            const string cls = @"SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}";
            using (var k = Registry.LocalMachine.OpenSubKey(cls)) {
                if (k == null) return;
                foreach (var sub in k.GetSubKeyNames()) {
                    if (sub.Length == 0 || !char.IsDigit(sub[0])) continue;
                    try {
                        using (var g = k.OpenSubKey(sub)) {
                            if (g == null || g.GetValue("DriverDesc") == null) continue;
                            long vram = 0;
                            var q = g.GetValue("HardwareInformation.qwMemorySize");
                            if (q is long) vram = (long)q;
                            else if (q is byte[] && ((byte[])q).Length >= 8) vram = BitConverter.ToInt64((byte[])q, 0);
                            else {
                                var m = g.GetValue("HardwareInformation.MemorySize");
                                if (m is int) vram = (uint)(int)m;
                                else if (m is byte[] && ((byte[])m).Length >= 4) vram = BitConverter.ToUInt32((byte[])m, 0);
                            }
                            sb.AppendLine("GPU " + sub + ": " + g.GetValue("DriverDesc") + ", driver " + g.GetValue("DriverVersion") + " (" + g.GetValue("DriverDate") + "), " +
                                          vram / (1024 * 1024) + " MB");
                        }
                    } catch { }
                }
            }
        } catch { }
    }

    static string SystemText(string bin, string exitInfo)
    {
        var sb = new StringBuilder();
        sb.AppendLine("Far Cry 5 VR report, " + DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture));
        if (!string.IsNullOrEmpty(exitInfo)) sb.AppendLine(exitInfo);
        const string nt = @"SOFTWARE\Microsoft\Windows NT\CurrentVersion";
        sb.AppendLine("Windows: " + Reg(Registry.LocalMachine, nt, "ProductName") + " " + Reg(Registry.LocalMachine, nt, "DisplayVersion") + " build " +
                      Reg(Registry.LocalMachine, nt, "CurrentBuild") + "." + Reg(Registry.LocalMachine, nt, "UBR"));
        var json = Launcher.ActiveRuntimeJson();
        string name, lib;
        bool ok = Launcher.RuntimeManifest(json, out name, out lib);
        sb.AppendLine("OpenXR runtime (system): " + (json ?? "none") + (json == null ? "" : " \"" + (name ?? "?") + "\", library " + (lib ?? "?") + (ok ? " exists" : " MISSING")));
        var env = Environment.GetEnvironmentVariable("XR_RUNTIME_JSON");
        if (!string.IsNullOrEmpty(env)) sb.AppendLine("XR_RUNTIME_JSON: " + env);
        var test = Path.Combine(bin, "fc5vr_xr_runtime.txt");
        try { if (File.Exists(test)) sb.AppendLine("fc5vr_xr_runtime.txt (test runtime): " + File.ReadAllText(test).Trim()); } catch { }
        Layers(sb, Registry.LocalMachine, "HKLM");
        Layers(sb, Registry.CurrentUser, "HKCU");
        Gpus(sb);
        var vc = Reg(Registry.LocalMachine, @"SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64", "Version");
        sb.AppendLine("VC++ runtime x64: " + (vc ?? "not installed"));
        sb.AppendLine("ViGEmBus: " + (Launcher.ViGEmBusInstalled() ? "installed" : "NOT installed"));
        sb.AppendLine("DLLs in bin:");
        try {
            foreach (var f in Directory.GetFiles(bin, "*.dll")) {
                var vi = FileVersionInfo.GetVersionInfo(f);
                sb.AppendLine("  " + Path.GetFileName(f) + "  " + new FileInfo(f).Length + " bytes  " + (vi.FileVersion ?? "") + "  " + (vi.ProductName ?? "") + "  " +
                              File.GetLastWriteTime(f).ToString("yyyy-MM-dd HH:mm", CultureInfo.InvariantCulture));
            }
        } catch { }
        foreach (var w in Launcher.StartChecks()) sb.AppendLine("Warning: " + w);
        var opts = Launcher.WrongGameOptions();
        if (opts == null) sb.AppendLine("Game options: no gamerprofile.xml read");
        else if (opts.Count == 0) sb.AppendLine("Game options: all set as the mod needs");
        else foreach (var g in opts) sb.AppendLine("Game option not as the mod needs: " + g.Label + " (" + g.Element + " " + g.Attr + "=" + (g.Have ?? "missing") + ")");
        return sb.ToString();
    }

    static string EventsText()
    {
        var sb = new StringBuilder();
        // Application: 1000 Application Error, 1001 Windows Error Reporting, 1002 Application Hang, 1026 .NET Runtime; System: 26 Application Popup.
        Query(sb, "Application", "*[System[(EventID=1000 or EventID=1001 or EventID=1002 or EventID=1026) and TimeCreated[timediff(@SystemTime) <= 86400000]]]", true);
        Query(sb, "System", "*[System[EventID=26 and TimeCreated[timediff(@SystemTime) <= 86400000]]]", false);
        return sb.ToString();
    }

    static void Query(StringBuilder sb, string log, string xpath, bool onlyGame)
    {
        sb.AppendLine("=== " + log + " (last 24 h) ===");
        try {
            using (var r = new EventLogReader(new EventLogQuery(log, PathType.LogName, xpath) { ReverseDirection = true })) {
                int n = 0;
                for (EventRecord e = r.ReadEvent(); e != null && n < 40; e = r.ReadEvent()) {
                    using (e) {
                        string msg = null;
                        try { msg = e.FormatDescription(); } catch { }
                        if (msg == null) {
                            var parts = new List<string>();
                            foreach (var p in e.Properties) parts.Add(p.Value == null ? "" : p.Value.ToString());
                            msg = string.Join(" | ", parts);
                        }
                        if (onlyGame && msg.IndexOf("FarCry5", StringComparison.OrdinalIgnoreCase) < 0) continue;
                        sb.AppendLine("--- " + (e.TimeCreated.HasValue ? e.TimeCreated.Value.ToString("yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture) : "?") + " event " + e.Id +
                                      " " + e.ProviderName);
                        sb.AppendLine(msg.Trim());
                        ++n;
                    }
                }
                if (n == 0) sb.AppendLine("none");
            }
        } catch (Exception ex) { sb.AppendLine("could not read: " + ex.Message); }
        sb.AppendLine();
    }

    // Writes the zip into bin; returns its path.
    public static string Make(string bin, string exitInfo)
    {
        var path = Path.Combine(bin, "FC5VR_report_" + DateTime.Now.ToString("yyyy-MM-dd_HHmmss", CultureInfo.InvariantCulture) + ".zip");
        var notes = new StringBuilder();
        using (var fs = new FileStream(path, FileMode.Create))
        using (var zip = new ZipArchive(fs, ZipArchiveMode.Create)) {
            Add(zip, LogPath(bin), "fc5vr_log.txt", notes);
            var crash = NewestCrash(bin);
            if (crash != null) {
                Add(zip, crash, Path.GetFileName(crash), notes);
                var dmp = Path.ChangeExtension(crash, ".dmp");
                if (!File.Exists(dmp)) dmp = Newest(Path.GetDirectoryName(crash), "crash_*.dmp");
                if (dmp != null) Add(zip, dmp, Path.GetFileName(dmp), notes);
            }
            try {
                foreach (var f in Directory.GetFiles(bin, "fc5vr_*.txt")) {
                    var n = Path.GetFileName(f);
                    if (n.Equals("fc5vr_log.txt", StringComparison.OrdinalIgnoreCase) || new FileInfo(f).Length > 2 * 1024 * 1024) continue;
                    Add(zip, f, "bin/" + n, notes);
                }
            } catch { }
            Add(zip, Path.Combine(bin, "vr_config.txt"), "bin/vr_config.txt", notes);
            Add(zip, @"C:\ProgramData\Virtual Desktop\OpenXR.log", "VirtualDesktop_OpenXR.log", notes);
            var steam = Reg(Registry.CurrentUser, @"Software\Valve\Steam", "SteamPath");
            if (!string.IsNullOrEmpty(steam)) Add(zip, Path.Combine(steam.Replace('/', '\\'), @"logs\vrserver.txt"), "SteamVR_vrserver.txt", notes);
            AddText(zip, "system.txt", SystemText(bin, exitInfo) + (notes.Length > 0 ? "\r\nFiles:\r\n" + notes : ""));
            AddText(zip, "windows_events.txt", EventsText());
        }
        return path;
    }

    public static void ShowInExplorer(string path)
    {
        try { Process.Start("explorer.exe", "/select,\"" + path + "\""); } catch { }
    }
}
