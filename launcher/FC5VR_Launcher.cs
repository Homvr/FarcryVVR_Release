// Far Cry 5 VR launcher: choose VR or flat screen, the resolution multiplier and the headset, then start the game.
// It lives in the game's bin folder (next to FarCry5.exe and the mod's dxgi.dll) and writes there:
//   fc5vr_launcher.txt   "mode vr" or "mode flat", "headset quest3" and the headset's typical eye size "headsetsize <w> <h>"
//                        (read by the mod at start, src\Main.cpp and FarCry5Stereo.cpp load_forced_resolution)
//   fc5vr_settings.txt   only its "render", "resscale", "dlss" and "loadpopup" lines, the same ones F11 changes
// Built by build_launcher.bat (C# 5, so the compiler in Windows' .NET Framework can build it too).
// After Play in VR it stays minimised until the game closes; if the game closed during the start (VR never ran) or
// crashed, it offers a report zip (FC5VR_report_<date>.zip in bin: logs, crash report, system and Windows event
// details; nothing is uploaded). "Make a report" makes the same zip at any time.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Diagnostics.Eventing.Reader;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.IO.Compression;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Windows.Forms;
using Microsoft.Win32;

[assembly: System.Reflection.AssemblyTitle("Far Cry 5 VR launcher")]
[assembly: System.Reflection.AssemblyProduct("Far Cry 5 VR mod")]

static class Launcher
{
    const string SteamUrl = "steam://rungameid/552520";

    // The headsets in the list (Homard 17 Oct: "add other common headsets"). The mod takes each headset's field of view from
    // its runtime every frame and its eye size from the runtime at start; the size here is only a typical one (per eye at
    // 100% in its usual runtime), used when the headset cannot be asked (not connected at start). Runtimes: the OpenXR
    // runtimes it normally runs on (labels of ActiveRuntime), empty for any.
    public class Headset
    {
        public readonly string Name, Id, Connect;
        public readonly int W, H;
        public readonly string[] Runtimes;
        public Headset(string name, string id, int w, int h, string connect, params string[] runtimes)
        {
            Name = name; Id = id; W = w; H = h; Connect = connect; Runtimes = runtimes;
        }
    }
    const string VDXR = "Virtual Desktop (VDXR)", Link = "Meta Quest Link", SteamVR = "SteamVR", WMR = "Windows Mixed Reality", Pimax = "Pimax",
                 Varjo = "Varjo", Pico = "Pico Connect";
    static readonly Headset[] Headsets = {
        new Headset("Meta Quest 3", "quest3", 2064, 2208, "Virtual Desktop, Quest Link / Air Link or Steam Link.", VDXR, Link, SteamVR),
        new Headset("Meta Quest 3S", "quest3s", 1832, 1920, "Virtual Desktop, Quest Link / Air Link or Steam Link.", VDXR, Link, SteamVR),
        new Headset("Meta Quest 2", "quest2", 1832, 1920, "Virtual Desktop, Quest Link / Air Link or Steam Link.", VDXR, Link, SteamVR),
        new Headset("Meta Quest Pro", "questpro", 1800, 1920, "Virtual Desktop, Quest Link / Air Link or Steam Link.", VDXR, Link, SteamVR),
        new Headset("Pico 4 / 4 Ultra", "pico4", 2160, 2160, "Virtual Desktop, Pico Connect or SteamVR.", VDXR, Pico, SteamVR),
        new Headset("Valve Index", "index", 2016, 2240, "SteamVR.", SteamVR),
        new Headset("HTC Vive Pro 2", "vivepro2", 2448, 2448, "SteamVR.", SteamVR),
        new Headset("HP Reverb G2", "reverbg2", 2160, 2160, "SteamVR, or Windows Mixed Reality on Windows versions that still have it.", WMR, SteamVR),
        new Headset("Bigscreen Beyond", "beyond", 2560, 2560, "SteamVR.", SteamVR),
        new Headset("Pimax Crystal", "pimaxcrystal", 2880, 2880, "Pimax Play (its OpenXR runtime) or SteamVR. The mod renders each eye's own turned view, so parallel projections are not needed.", Pimax, SteamVR),
        new Headset("Varjo Aero", "varjoaero", 2880, 2720, "Varjo Base (its OpenXR runtime) or SteamVR.", Varjo, SteamVR),
        new Headset("Other OpenXR headset", "other", 0, 0, "Any OpenXR runtime set as the system default."),
    };

    public static Headset FindHeadset(string id)
    {
        foreach (var h in Headsets) if (h.Id == id) return h;
        return null;
    }

    [DllImport("user32.dll")] static extern bool SetProcessDPIAware();

    static string Dir;
    static string LauncherFile { get { return Path.Combine(Dir, "fc5vr_launcher.txt"); } }
    static string SettingsFile { get { return Path.Combine(Dir, "fc5vr_settings.txt"); } }

    // Steam's launch options "<launcher path>" %command% make Steam's Play open the launcher with the game's own
    // command line (FarCry5.exe and its arguments); Play then runs that command.
    static string[] GameCommand;

    [STAThread]
    static void Main(string[] args)
    {
        try { SetProcessDPIAware(); } catch { }
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Dir = AppDomain.CurrentDomain.BaseDirectory;
        if (args.Length > 0 && args[0].EndsWith(".exe", StringComparison.OrdinalIgnoreCase) && File.Exists(args[0])) GameCommand = args;
        if (!File.Exists(Path.Combine(Dir, "FarCry5.exe"))) {
            MessageBox.Show("Put FC5VR_Launcher.exe in Far Cry 5's bin folder, next to FarCry5.exe and the mod's dxgi.dll.",
                            "Far Cry 5 VR", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return;
        }
        Application.Run(new LauncherForm());
    }

    // ---- files ----

    static string ResolutionFile { get { return Path.Combine(Dir, "fc5vr_resolution.txt"); } }

    // DLSS render scale, as F11 Display > Image quality "DLSS render scale" (fc5vr_resolution.txt "auto <ratio>").
    public static readonly string[] DlssModes = { "Native (DLAA)", "Quality (0.67)", "Balanced (0.58)", "Performance (0.50)" };
    public static readonly float[] DlssRatios = { 1.00f, 0.67f, 0.58f, 0.50f };

    // DLSS: fc5vr_settings.txt "dlss on|off" (as F11's DLSS checkbox) and fc5vr_resolution.txt "auto <ratio>".
    // A resolution file that is not "auto" (a fixed size, or "off": test setups) is left alone unless the mode is changed;
    // dlssMode is -1 then.
    public static void ReadDlss(out bool on, out int mode)
    {
        on = true;
        mode = 0;
        try {
            if (File.Exists(SettingsFile))
                foreach (var raw in File.ReadAllLines(SettingsFile)) {
                    var l = raw.Trim();
                    if (l.StartsWith("dlss ")) on = !(l == "dlss off" || l == "dlss 0");
                }
            if (File.Exists(ResolutionFile)) {
                var parts = File.ReadAllText(ResolutionFile).Split(new[] { ' ', '\t', '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries);
                float r;
                if (parts.Length >= 2 && parts[0] == "auto" && float.TryParse(parts[1], NumberStyles.Float, CultureInfo.InvariantCulture, out r)) {
                    for (int i = 0; i < DlssRatios.Length; ++i) if (Math.Abs(DlssRatios[i] - r) < 0.02f) mode = i;
                } else if (parts.Length >= 1 && parts[0] != "auto") mode = -1;
            }
        } catch { }
    }

    // Rendering: fc5vr_settings.txt "render stereo|afw" (F11 Display "Rendering", F8). No line: AFW, the mod's start mode.
    public static bool ReadStereo()
    {
        bool stereo = false;
        try {
            if (File.Exists(SettingsFile))
                foreach (var raw in File.ReadAllLines(SettingsFile)) {
                    var l = raw.Trim();
                    if (l.StartsWith("render ")) stereo = l.Substring(7).Trim() == "stereo";
                }
        } catch { }
        return stereo;
    }

    // Loading popup: fc5vr_settings.txt "loadpopup off" (F11 HUD and menus > Loading popup). No line: shown, the mod's default.
    public static bool ReadSkipPopup()
    {
        bool skip = false;
        try {
            if (File.Exists(SettingsFile))
                foreach (var raw in File.ReadAllLines(SettingsFile)) {
                    var l = raw.Trim();
                    if (l.StartsWith("loadpopup ")) skip = l == "loadpopup off" || l == "loadpopup 0";
                }
        } catch { }
        return skip;
    }

    public static void ReadChoice(out bool flat, out string headset, out float scale)
    {
        flat = false;
        headset = Headsets[0].Id;
        scale = 1.0f;
        try {
            if (File.Exists(LauncherFile)) {
                foreach (var raw in File.ReadAllLines(LauncherFile)) {
                    var l = raw.Trim();
                    if (l.StartsWith("mode ")) flat = l.Substring(5).Trim() == "flat";
                    if (l.StartsWith("headset ")) headset = l.Substring(8).Trim();
                }
            }
            if (File.Exists(SettingsFile)) {
                foreach (var raw in File.ReadAllLines(SettingsFile)) {
                    var l = raw.Trim();
                    float v;
                    if (l.StartsWith("resscale ") && float.TryParse(l.Substring(9).Trim(), NumberStyles.Float, CultureInfo.InvariantCulture, out v))
                        scale = Math.Max(1.0f, Math.Min(2.0f, v));
                }
            }
        } catch { }
    }

    // Replaces the line starting with "<key> " (the first one; later ones removed), or adds it.
    static void SetLine(List<string> lines, string key, string line)
    {
        bool found = false;
        for (int i = 0; i < lines.Count; ++i) {
            if (lines[i].Trim().StartsWith(key + " ")) {
                if (found) { lines.RemoveAt(i--); continue; }
                lines[i] = line;
                found = true;
            }
        }
        if (!found) lines.Add(line);
    }

    // dlssMode -1: fc5vr_resolution.txt left as it is.
    public static void WriteChoice(bool flat, string headset, bool stereo, float scale, bool dlss, int dlssMode, bool skipPopup)
    {
        var hs = FindHeadset(headset);
        File.WriteAllText(LauncherFile, "mode " + (flat ? "flat" : "vr") + "\r\nheadset " + headset + "\r\n" +
                          (hs != null && hs.W > 0 ? "headsetsize " + hs.W + " " + hs.H + "\r\n" : ""), new UTF8Encoding(false));
        if (dlssMode >= 0)
            File.WriteAllText(ResolutionFile, "auto " + DlssRatios[dlssMode].ToString("0.00", CultureInfo.InvariantCulture) + "\n", new UTF8Encoding(false));
        // The settings file is the mod's: only the render, resscale and dlss lines change (added if missing), the rest is kept as it is.
        var lines = new List<string>();
        if (File.Exists(SettingsFile)) lines.AddRange(File.ReadAllLines(SettingsFile));
        else lines.Add("# Far Cry 5 VR settings: the same commands as fc5vr_cmd.txt, replayed at start (edited by the mod).");
        lines.RemoveAll(l => l.Trim().StartsWith("afw "));   // the old form of the render line (the mod drops it too)
        SetLine(lines, "render", stereo ? "render stereo" : "render afw");
        SetLine(lines, "resscale", "resscale " + scale.ToString("0.0", CultureInfo.InvariantCulture));
        SetLine(lines, "dlss", dlss ? "dlss on" : "dlss off");
        // The loading popup's line only when it is skipped or already there (no line: shown).
        if (skipPopup || lines.Exists(l => l.Trim().StartsWith("loadpopup "))) SetLine(lines, "loadpopup", skipPopup ? "loadpopup off" : "loadpopup on");
        // The mod writes with \n; keep it.
        var tmp = SettingsFile + ".launcher.tmp";
        File.WriteAllText(tmp, string.Join("\n", lines) + "\n", new UTF8Encoding(false));
        if (File.Exists(SettingsFile)) File.Replace(tmp, SettingsFile, null);
        else File.Move(tmp, SettingsFile);
    }

    // The headset's recommended eye size, written by the mod once VR has started (fc5vr_xr_size.txt "<w> <h> <headset>";
    // without a headset it was the Quest 3's).
    public static bool HeadsetEyeSize(out int w, out int h, out string headset)
    {
        w = h = 0;
        headset = "quest3";
        try {
            var p = Path.Combine(Dir, "fc5vr_xr_size.txt");
            if (!File.Exists(p)) return false;
            var parts = File.ReadAllText(p).Split(new[] { ' ', '\t', '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length >= 3) headset = parts[2];
            return parts.Length >= 2 && int.TryParse(parts[0], out w) && int.TryParse(parts[1], out h) && w > 0 && h > 0;
        } catch { return false; }
    }

    // The eye size for a headset: the mod's measured one when it is that headset's, else its typical one.
    public static bool EyeSizeFor(string id, out int w, out int h, out bool measured)
    {
        string tag;
        measured = HeadsetEyeSize(out w, out h, out tag) && tag == id;
        if (measured) return true;
        var hs = FindHeadset(id);
        w = hs != null ? hs.W : 0;
        h = hs != null ? hs.H : 0;
        return w > 0;
    }

    // What the headset reported in the mod's last VR run (fc5vr_xr_view.txt, "<key> <values>" lines), if it was this headset.
    public static Dictionary<string, string> LastView(string id)
    {
        var d = new Dictionary<string, string>();
        try {
            var p = Path.Combine(Dir, "fc5vr_xr_view.txt");
            if (!File.Exists(p)) return null;
            foreach (var raw in File.ReadAllLines(p)) {
                var l = raw.Trim();
                int s = l.IndexOf(' ');
                if (s > 0) d[l.Substring(0, s)] = l.Substring(s + 1).Trim();
            }
        } catch { return null; }
        string hs;
        return d.TryGetValue("headset", out hs) && hs == id ? d : null;
    }

    // ---- the game's own options (Documents\My Games\Far Cry 5\gamerprofile.xml) ----
    // The game reads this file when it starts and writes it when its options change, so it is checked and set only
    // while the game is closed. The values are the README's "Required game options" (Homard's own file, 24 Oct):
    // VideoModeProfile 1 = the window profile, whose windowMode 3 is Borderless (0 = the exclusive Fullscreen profile),
    // AntiAliasingMode 1 = SMAA (DLSS runs inside the game's SMAA pass), HelpCrosshair 0 = Interface > Reticle off.
    public class GameOption
    {
        public readonly string Element, Attr, Want, Label;
        public string Have;   // the file's value, null when the attribute is missing
        public GameOption(string element, string attr, string want, string label) { Element = element; Attr = attr; Want = want; Label = label; }
    }

    static GameOption[] RequiredOptions()
    {
        return new[] {
            new GameOption("RenderProfile", "VideoModeProfile", "1", "Window mode: Borderless"),
            new GameOption("VideoModeProfileWindow", "windowMode", "3", "Window mode: Borderless"),
            new GameOption("RenderProfile", "FOV", "120", "Field of view: 120"),
            new GameOption("RenderProfile", "AntiAliasingMode", "1", "Anti-aliasing: SMAA (DLSS runs in its place)"),
            new GameOption("RenderProfile", "VSyncPreset", "off", "V-Sync: Off"),
            new GameOption("RenderProfile", "FrameRateLockEnabled", "0", "Frame rate limit: Off"),
            new GameOption("RenderProfile", "MotionBlur", "0", "Motion blur: Off"),
            new GameOption("RenderProfile", "ResolutionScale", "1", "Resolution scale: 100 %"),
            new GameOption("RenderProfile", "DynamicResolutionEnabled", "0", "Dynamic resolution: Off"),
            new GameOption("VideoModeProfileWindow", "hdr", "0", "HDR: Off"),
            new GameOption("ProfileSpecificGameProfile", "HelpCrosshair", "0", "Crosshair (Interface > Reticle): Off"),
        };
    }

    public static string GameProfilePath
    {
        get { return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), @"My Games\Far Cry 5\gamerprofile.xml"); }
    }

    // The span of an attribute's value inside the element's opening tag, or false.
    static bool FindAttr(string t, string element, string attr, out int start, out int length)
    {
        start = length = 0;
        int tag = t.IndexOf("<" + element + " ", StringComparison.Ordinal);
        if (tag < 0) return false;
        int end = t.IndexOf('>', tag);
        if (end < 0) return false;
        var m = new Regex("\\s" + Regex.Escape(attr) + "=\"([^\"]*)\"").Match(t, tag, end - tag);
        if (!m.Success) return false;
        start = m.Groups[1].Index;
        length = m.Groups[1].Length;
        return true;
    }

    static bool SameValue(string have, string want)
    {
        double a, b;
        if (double.TryParse(have, NumberStyles.Float, CultureInfo.InvariantCulture, out a) && double.TryParse(want, NumberStyles.Float, CultureInfo.InvariantCulture, out b))
            return Math.Abs(a - b) < 1e-3;
        return string.Equals(have, want, StringComparison.OrdinalIgnoreCase);
    }

    // The required options the game's file does not have (an option missing from the file counts as wrong only when
    // the file has its element). Null when there is no file yet (the game has not run) or it cannot be read.
    public static List<GameOption> WrongGameOptions()
    {
        try {
            var p = GameProfilePath;
            if (!File.Exists(p)) return null;
            var t = File.ReadAllText(p);
            var wrong = new List<GameOption>();
            foreach (var o in RequiredOptions()) {
                int s, n;
                if (FindAttr(t, o.Element, o.Attr, out s, out n)) o.Have = t.Substring(s, n);
                else if (t.IndexOf("<" + o.Element + " ", StringComparison.Ordinal) < 0) continue;
                if (o.Have == null || !SameValue(o.Have, o.Want)) wrong.Add(o);
            }
            return wrong;
        } catch { return null; }
    }

    // The wrong options' labels, one each (both window mode values are one option in the game's menu).
    public static List<string> WrongLabels(List<GameOption> wrong)
    {
        var l = new List<string>();
        if (wrong != null) foreach (var o in wrong) if (!l.Contains(o.Label)) l.Add(o.Label);
        return l;
    }

    // Sets the required options in the game's file (the game must be closed). The first time, the file as it was is kept
    // beside it as gamerprofile.xml.fc5vr_backup. Missing attributes are left alone (the game writes them itself).
    public static void FixGameOptions()
    {
        var p = GameProfilePath;
        var t = File.ReadAllText(p);
        foreach (var o in RequiredOptions()) {
            int s, n;
            if (FindAttr(t, o.Element, o.Attr, out s, out n) && !SameValue(t.Substring(s, n), o.Want)) t = t.Substring(0, s) + o.Want + t.Substring(s + n);
        }
        var bak = p + ".fc5vr_backup";
        if (!File.Exists(bak)) File.Copy(p, bak);
        var attrs = File.GetAttributes(p);
        if ((attrs & FileAttributes.ReadOnly) != 0) File.SetAttributes(p, attrs & ~FileAttributes.ReadOnly);
        File.WriteAllText(p, t, new UTF8Encoding(false));
    }

    // The system's OpenXR runtime (the one the headset uses), from the Khronos registry key.
    public static string ActiveRuntime()
    {
        try {
            using (var k = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\Khronos\OpenXR\1")) {
                var v = k == null ? null : k.GetValue("ActiveRuntime") as string;
                if (string.IsNullOrEmpty(v)) return null;
                var low = v.ToLowerInvariant();
                if (low.Contains("simulator")) return "Meta XR Simulator";
                if (low.Contains("virtual desktop") || low.Contains("virtualdesktop")) return VDXR;
                if (low.Contains("oculus") || low.Contains("meta")) return Link;
                if (low.Contains("steamvr") || low.Contains("steamxr")) return SteamVR;
                if (low.Contains("mixedreality")) return WMR;
                if (low.Contains("pimax") || low.Contains("piopenxr")) return Pimax;
                if (low.Contains("varjo")) return Varjo;
                if (low.Contains("pico")) return Pico;
                return Path.GetFileNameWithoutExtension(v);
            }
        } catch { return null; }
    }

    // The runtime manifest's name and DLL (library_path, relative to the manifest), and whether that DLL exists.
    public static bool RuntimeManifest(string json, out string name, out string library)
    {
        name = library = null;
        try {
            if (string.IsNullOrEmpty(json) || !File.Exists(json)) return false;
            var t = File.ReadAllText(json);
            var n = Regex.Match(t, "\"name\"\\s*:\\s*\"((?:[^\"\\\\]|\\\\.)*)\"");
            var l = Regex.Match(t, "\"library_path\"\\s*:\\s*\"((?:[^\"\\\\]|\\\\.)*)\"");
            if (n.Success) name = n.Groups[1].Value.Replace("\\\\", "\\");
            if (!l.Success) return false;
            library = l.Groups[1].Value.Replace("\\\\", "\\").Replace("/", "\\");
            if (!Path.IsPathRooted(library)) library = Path.GetFullPath(Path.Combine(Path.GetDirectoryName(json), library));
            return File.Exists(library);
        } catch { return false; }
    }

    public static string ActiveRuntimeJson()
    {
        try {
            using (var k = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\Khronos\OpenXR\1"))
                return k == null ? null : k.GetValue("ActiveRuntime") as string;
        } catch { return null; }
    }

    // DLLs in bin that other mods or overlays use to load themselves into the game, and ReShade.
    public static List<string> ProxyDlls()
    {
        var found = new List<string>();
        foreach (var n in new[] { "openvr_api.dll", "d3d11.dll", "dinput8.dll", "version.dll", "winmm.dll", "d3d9.dll" })
            if (File.Exists(Path.Combine(Dir, n))) found.Add(n);
        try {
            foreach (var f in Directory.GetFiles(Dir, "*.dll")) {
                var vi = FileVersionInfo.GetVersionInfo(f);
                var p = (vi.ProductName ?? "") + " " + (vi.FileDescription ?? "");
                if (p.IndexOf("ReShade", StringComparison.OrdinalIgnoreCase) >= 0) found.Add(Path.GetFileName(f) + " (ReShade)");
            }
        } catch { }
        if (File.Exists(Path.Combine(Dir, "ReShade.ini"))) found.Add("ReShade.ini (ReShade)");
        return found;
    }

    // The VC++ 2015-2022 x64 runtime the installer registers ("v14.40.33810.00").
    public static Version VcRuntime()
    {
        try {
            using (var k = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64")) {
                if (k == null) return null;
                var v = k.GetValue("Version") as string;
                if (string.IsNullOrEmpty(v)) return null;
                Version r;
                return Version.TryParse(v.TrimStart('v', 'V'), out r) ? r : null;
            }
        } catch { return null; }
    }

    public static bool ViGEmBusInstalled()
    {
        try {
            using (var k = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Services\ViGEmBus")) return k != null;
        } catch { return false; }
    }

    // What would stop VR from starting, as warnings for the headset text.
    public static List<string> StartChecks()
    {
        var w = new List<string>();
        var json = ActiveRuntimeJson();
        string name, lib;
        if (!string.IsNullOrEmpty(json) && !RuntimeManifest(json, out name, out lib))
            w.Add("the system's OpenXR runtime (" + json + ") is missing or its DLL is: reinstall the headset's app or set its OpenXR runtime again.");
        var proxies = ProxyDlls();
        if (proxies.Count > 0)
            w.Add("bin has " + string.Join(", ", proxies) + ": other mods or overlays load into the game through these and can stop the VR mod (openvr_api.dll makes it try SteamVR's old API first). Move them out if VR does not start.");
        var vc = VcRuntime();
        if (vc == null) w.Add("the Microsoft Visual C++ 2015-2022 x64 runtime is not installed: install vc_redist.x64.exe from Microsoft, or the game may close at start.");
        else if (vc < new Version(14, 40)) w.Add("the Microsoft Visual C++ x64 runtime is old (" + vc + "): install the latest vc_redist.x64.exe from Microsoft.");
        if (!ViGEmBusInstalled()) w.Add("the ViGEmBus driver is not installed: the motion controllers drive the game through it (install it from its GitHub releases).");
        return w;
    }

    public static bool GameRunning() { return Process.GetProcessesByName("FarCry5").Length > 0; }

    public static bool FromSteam { get { return GameCommand != null; } }

    // What to put in Steam > Far Cry 5 > Properties > Launch options.
    public static string SteamLaunchOption { get { return "\"" + Path.Combine(Dir, "FC5VR_Launcher.exe") + "\" %command%"; } }

    static string Quote(string a) { return a.Length > 0 && a.IndexOfAny(new[] { ' ', '\t', '"' }) < 0 ? a : "\"" + a.Replace("\"", "\\\"") + "\""; }

    public static void StartGame()
    {
        if (GameCommand != null) {
            // Started by Steam's Play: run the command Steam gave (its environment, SteamAppId included, is inherited).
            var rest = new List<string>();
            for (int i = 1; i < GameCommand.Length; ++i) rest.Add(Quote(GameCommand[i]));
            Process.Start(new ProcessStartInfo(GameCommand[0], string.Join(" ", rest)) { UseShellExecute = false, WorkingDirectory = Path.GetDirectoryName(GameCommand[0]) });
            return;
        }
        // The Steam copy starts through Steam (as from the library); another copy starts its own exe.
        if (Dir.ToLowerInvariant().Contains(@"\steamapps\")) Process.Start(SteamUrl);
        else Process.Start(new ProcessStartInfo(Path.Combine(Dir, "FarCry5.exe")) { WorkingDirectory = Dir });
    }

    public static Headset[] HeadsetList { get { return Headsets; } }
    public static string GameDir { get { return Dir; } }
}

class LauncherForm : Form
{
    readonly RadioButton vr = new RadioButton();
    readonly RadioButton flat = new RadioButton();
    readonly ComboBox scale = new ComboBox();
    readonly ComboBox headset = new ComboBox();
    readonly Label scaleInfo = new Label();
    readonly Label headsetInfo = new Label();
    readonly Label modeInfo = new Label();
    readonly ComboBox render = new ComboBox();
    readonly Label renderInfo = new Label();
    readonly CheckBox dlss = new CheckBox();
    readonly ComboBox dlssMode = new ComboBox();
    readonly Label dlssInfo = new Label();
    readonly CheckBox skipPopup = new CheckBox();
    readonly Label popupInfo = new Label();
    bool dlssKeepFile;   // fc5vr_resolution.txt is not "auto" (a test setup): kept unless the mode is changed
    readonly Button play = new Button();

    public LauncherForm()
    {
        Text = "Far Cry 5 VR";
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = false;
        StartPosition = FormStartPosition.CenterScreen;
        AutoScaleMode = AutoScaleMode.Dpi;
        AutoScaleDimensions = new SizeF(96f, 96f);
        Font = new Font("Segoe UI", 9.5f);
        try { Icon = Icon.ExtractAssociatedIcon(Path.Combine(Launcher.GameDir, "FarCry5.exe")); } catch { }

        var table = new TableLayoutPanel { ColumnCount = 2, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, Padding = new Padding(14), Dock = DockStyle.Fill };
        table.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        table.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 300));

        // Mode
        var modes = new FlowLayoutPanel { AutoSize = true, Margin = new Padding(0), WrapContents = false };
        vr.Text = "VR"; vr.AutoSize = true;
        flat.Text = "Flatscreen"; flat.AutoSize = true; flat.Margin = new Padding(16, 3, 3, 3);
        modes.Controls.Add(vr);
        modes.Controls.Add(flat);
        AddRow(table, "Mode", modes);
        AddInfo(table, modeInfo);

        // Rendering: F11 Display "Rendering" (F8 in game).
        render.DropDownStyle = ComboBoxStyle.DropDownList;
        render.Width = 120;
        render.Items.AddRange(new object[] { "Stereo", "AFW" });
        AddRow(table, "Rendering", render);
        AddInfo(table, renderInfo);

        // Resolution multiplier: x1.0 to x2.0 in steps of 0.1, as F11 Display > Image quality.
        scale.DropDownStyle = ComboBoxStyle.DropDownList;
        scale.Width = 120;
        for (int s = 10; s <= 20; ++s) scale.Items.Add("x" + (s / 10.0).ToString("0.0", CultureInfo.InvariantCulture));
        AddRow(table, "Resolution multiplier", scale);
        AddInfo(table, scaleInfo);

        // DLSS: on/off (F11 Display > Image quality "DLSS") and the render scale ("DLSS render scale").
        var dl = new FlowLayoutPanel { AutoSize = true, Margin = new Padding(0), WrapContents = false };
        dlss.Text = "On"; dlss.AutoSize = true; dlss.Margin = new Padding(0, 5, 12, 0);
        dlssMode.DropDownStyle = ComboBoxStyle.DropDownList;
        dlssMode.Width = 160;
        dlssMode.Items.AddRange(Launcher.DlssModes);
        dl.Controls.Add(dlss);
        dl.Controls.Add(dlssMode);
        AddRow(table, "DLSS", dl);
        AddInfo(table, dlssInfo);

        // Loading popup: F11 HUD and menus > Loading popup ("loadpopup off" skips it).
        skipPopup.Text = "Skip the loading popup"; skipPopup.AutoSize = true;
        AddRow(table, "After a load", skipPopup);
        AddInfo(table, popupInfo);

        // Headset
        headset.DropDownStyle = ComboBoxStyle.DropDownList;
        headset.Width = 200;
        foreach (var h in Launcher.HeadsetList) headset.Items.Add(h.Name);
        AddRow(table, "Headset", headset);
        AddInfo(table, headsetInfo);

        // Buttons
        var buttons = new FlowLayoutPanel { AutoSize = true, FlowDirection = FlowDirection.RightToLeft, Dock = DockStyle.Fill, Margin = new Padding(0, 12, 0, 0) };
        var close = new Button { Text = "Close", AutoSize = true, DialogResult = DialogResult.Cancel };
        play.Text = "Play"; play.AutoSize = true; play.MinimumSize = new Size(90, 0);
        var report = new Button { Text = "Make a report", AutoSize = true };
        report.Click += delegate { MakeReport(null); };
        buttons.Controls.Add(close);
        buttons.Controls.Add(play);
        buttons.Controls.Add(report);
        table.Controls.Add(buttons, 0, table.RowCount);
        table.SetColumnSpan(buttons, 2);
        table.RowCount++;

        Controls.Add(table);
        AutoSize = true;
        AutoSizeMode = AutoSizeMode.GrowAndShrink;
        AcceptButton = play;
        CancelButton = close;

        bool isFlat; string hs; float sc;
        Launcher.ReadChoice(out isFlat, out hs, out sc);
        vr.Checked = !isFlat;
        flat.Checked = isFlat;
        scale.SelectedIndex = Math.Max(0, Math.Min(10, (int)Math.Round(sc * 10) - 10));
        headset.SelectedIndex = 0;
        for (int i = 0; i < Launcher.HeadsetList.Length; ++i) if (Launcher.HeadsetList[i].Id == hs) headset.SelectedIndex = i;
        render.SelectedIndex = Launcher.ReadStereo() ? 0 : 1;
        bool dlssOn; int dm;
        Launcher.ReadDlss(out dlssOn, out dm);
        dlss.Checked = dlssOn;
        dlssKeepFile = dm < 0;
        dlssMode.SelectedIndex = Math.Max(0, dm);
        skipPopup.Checked = Launcher.ReadSkipPopup();

        vr.CheckedChanged += delegate { UpdateInfo(); };
        scale.SelectedIndexChanged += delegate { UpdateInfo(); };
        render.SelectedIndexChanged += delegate { UpdateInfo(); };
        dlss.CheckedChanged += delegate { UpdateInfo(); };
        skipPopup.CheckedChanged += delegate { UpdateInfo(); };
        dlssMode.SelectedIndexChanged += delegate { dlssKeepFile = false; UpdateInfo(); };
        headset.SelectedIndexChanged += delegate { UpdateInfo(); };
        play.Click += delegate { Play(); };
        UpdateInfo();
    }

    static void AddRow(TableLayoutPanel t, string label, Control c)
    {
        var l = new Label { Text = label, AutoSize = true, Anchor = AnchorStyles.Left, Margin = new Padding(0, 6, 16, 0) };
        c.Margin = new Padding(0, 3, 0, 0);
        t.Controls.Add(l, 0, t.RowCount);
        t.Controls.Add(c, 1, t.RowCount);
        t.RowCount++;
    }

    static void AddInfo(TableLayoutPanel t, Label info)
    {
        info.AutoSize = true;
        info.MaximumSize = new Size(300, 0);
        info.ForeColor = SystemColors.GrayText;
        info.Margin = new Padding(0, 2, 0, 8);
        t.Controls.Add(info, 1, t.RowCount);
        t.RowCount++;
    }

    float Multiplier() { return (10 + Math.Max(0, scale.SelectedIndex)) / 10.0f; }

    Launcher.Headset Chosen() { return Launcher.HeadsetList[Math.Max(0, headset.SelectedIndex)]; }
    string HeadsetId() { return Chosen().Id; }

    // The headset row's text: how it connects, whether the system's runtime is one it uses, what the last run measured.
    string HeadsetText()
    {
        var hs = Chosen();
        var rt = Launcher.ActiveRuntime();
        var sb = new StringBuilder("Connects through " + hs.Connect.TrimEnd('.') + ". ");
        if (rt == null) sb.Append("No OpenXR runtime is set as the system default: set the headset's app (Virtual Desktop Streamer: OpenXR Runtime VDXR; SteamVR: Settings > OpenXR) as the default. ");
        else if (hs.Runtimes.Length > 0 && Array.IndexOf(hs.Runtimes, rt) < 0 && rt != "Meta XR Simulator")
            sb.Append("The system's OpenXR runtime is " + rt + ", which the " + hs.Name + " does not usually use: set " + hs.Runtimes[0] + " as the OpenXR runtime. ");
        else sb.Append("OpenXR runtime: " + rt + ". ");
        var view = Launcher.LastView(hs.Id);
        string v;
        if (view != null && view.TryGetValue("span", out v)) {
            var p = v.Split(' ');
            float sh, sv;
            if (p.Length >= 2 && float.TryParse(p[0], NumberStyles.Float, CultureInfo.InvariantCulture, out sh) && float.TryParse(p[1], NumberStyles.Float, CultureInfo.InvariantCulture, out sv)) {
                string sys;
                view.TryGetValue("system", out sys);
                sb.Append(string.Format(CultureInfo.InvariantCulture, "Last run: {0}, view {1:0} x {2:0} deg. ", string.IsNullOrEmpty(sys) ? hs.Name : sys, sh, sv));
                if (sh > 116) sb.Append("That is wider than the game's own view (120 deg at most): objects at the far sides can pop in. ");
            }
        }
        var wrong = Launcher.WrongLabels(Launcher.WrongGameOptions());
        if (wrong.Count > 0) sb.Append("Game options the VR mod needs, not set yet: " + string.Join("; ", wrong) + ". Play offers to set them. ");
        foreach (var w in Launcher.StartChecks()) sb.Append("Warning: " + w + " ");
        sb.Append("Connect the headset before pressing Play.");
        return sb.ToString();
    }

    void UpdateInfo()
    {
        bool isVr = vr.Checked;
        scale.Enabled = isVr;
        headset.Enabled = isVr;
        modeInfo.Text = isVr ? "Stereo 3D in the headset with motion controls. F11 in game opens the mod's menu."
                             : "The game runs on the monitor without the mod (as if it was not installed).";
        int w, h;
        bool measured;
        bool known = Launcher.EyeSizeFor(HeadsetId(), out w, out h, out measured);
        if (!isVr) scaleInfo.Text = "Not used in Flatscreen.";
        else if (known && measured)
            scaleInfo.Text = string.Format(CultureInfo.InvariantCulture, "Eye images {0}x{1} (headset {2}x{3}). Higher is sharper and slower. F11 can change it in game.",
                                           (int)(w * Multiplier()) & ~7, (int)(h * Multiplier()) & ~7, w, h);
        else if (known)
            scaleInfo.Text = string.Format(CultureInfo.InvariantCulture, "Eye images about {0}x{1} (typical for this headset {2}x{3}; the mod asks the headset for its own size at start). Higher is sharper and slower. F11 can change it in game.",
                                           (int)(w * Multiplier()) & ~7, (int)(h * Multiplier()) & ~7, w, h);
        else scaleInfo.Text = "Eye image size on top of the headset's recommended size (the mod asks the headset at start). Higher is sharper and slower. F11 can change it in game.";
        render.Enabled = isVr;
        renderInfo.Text = !isVr ? "Not used in Flatscreen."
                        : render.SelectedIndex == 0 ? "Both eyes drawn every game frame: sharpest, heaviest. F8 switches in game."
                                                    : "One eye per game frame, the other rebuilt from depth: faster. F8 switches in game.";
        dlss.Enabled = isVr;
        dlssMode.Enabled = isVr && dlss.Checked;
        int ew = (int)(w * Multiplier()) & ~7, eh = (int)(h * Multiplier()) & ~7;
        float ratio = Launcher.DlssRatios[Math.Max(0, dlssMode.SelectedIndex)];
        if (!isVr) dlssInfo.Text = "Not used in Flatscreen (the game's own options apply).";
        else if (!dlss.Checked) dlssInfo.Text = "Off: the game's own anti-aliasing, rendered at the eye image size.";
        else if (dlssKeepFile) dlssInfo.Text = "The render size is a test setting (fc5vr_resolution.txt): kept unless you pick a mode.";
        else if (ratio > 0.99f) dlssInfo.Text = "DLAA: the game renders at the eye image size, DLSS smooths it (sharpest, slowest).";
        else if (known) dlssInfo.Text = string.Format(CultureInfo.InvariantCulture, "The game renders at {0}x{1} and DLSS scales it up to the eye images. Lower is faster and softer.",
                                                     (int)(ew * ratio) & ~7, (int)(eh * ratio) & ~7);
        else dlssInfo.Text = "The game renders smaller and DLSS scales it up to the eye images. Lower is faster and softer.";
        dlssInfo.Text += " Turning DLSS on or off in F11 needs a game restart.";
        skipPopup.Enabled = isVr;
        popupInfo.Text = !isVr ? "Not used in Flatscreen."
                       : skipPopup.Checked ? "Play starts at once after a load; the first seconds can hitch and the sidearm shows once the mod has seen it."
                                           : "After a load, \"VR mod loading\" counts down in the headset while the mod gets your weapons ready; the controllers wait until it closes.";
        headsetInfo.Text = isVr ? HeadsetText() : "";
    }

    void Play()
    {
        if (Launcher.GameRunning()) {
            MessageBox.Show(this, "Far Cry 5 is already running. Close it first: the choice applies when the game starts.", "Far Cry 5 VR", MessageBoxButtons.OK, MessageBoxIcon.Information);
            return;
        }
        if (vr.Checked && !CheckGameOptions()) return;
        try {
            Launcher.WriteChoice(flat.Checked, HeadsetId(), render.SelectedIndex == 0, Multiplier(),
                                 dlss.Checked, dlssKeepFile ? -1 : Math.Max(0, dlssMode.SelectedIndex), skipPopup.Checked);
        } catch (Exception e) {
            MessageBox.Show(this, "Could not save the choice in " + Launcher.GameDir + ":\n" + e.Message, "Far Cry 5 VR", MessageBoxButtons.OK, MessageBoxIcon.Error);
            return;
        }
        var started = DateTime.Now;
        try {
            Launcher.StartGame();
        } catch (Exception e) {
            MessageBox.Show(this, "Saved, but the game did not start:\n" + e.Message + "\nStart Far Cry 5 from Steam or Ubisoft Connect.", "Far Cry 5 VR", MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }
        if (flat.Checked) {
            Close();
            return;
        }
        // VR: stay minimised until the game closes, then say so if VR never started or the game crashed.
        play.Enabled = false;
        WindowState = FormWindowState.Minimized;
        var watch = new Thread(delegate() {
            bool found;
            uint code;
            GameWatch.WaitForGame(out found, out code);
            try { BeginInvoke(new Action(delegate { GameClosed(started, found, code); })); } catch { }
        });
        watch.IsBackground = true;
        watch.Start();
    }

    // VR: the game's own options the mod needs (README "Required game options"), set in the game's file before it starts.
    // False: the player went back to the launcher.
    bool CheckGameOptions()
    {
        var wrong = Launcher.WrongGameOptions();
        if (wrong == null || wrong.Count == 0) return true;
        var list = new StringBuilder();
        foreach (var o in wrong)
            list.AppendLine("- " + o.Label + "   (" + o.Attr + " is " + (o.Have == null ? "missing" : "\"" + o.Have + "\"") + ")");
        var answer = MessageBox.Show(this, "These Far Cry 5 options are not the ones the VR mod needs:\n\n" + list +
                                     "\nSet them now? Yes changes them in the game's options file (" + Launcher.GameProfilePath + "); " +
                                     "the first time, a copy of the file as it was is kept as gamerprofile.xml.fc5vr_backup.\n" +
                                     "No starts the game with your options unchanged. Cancel goes back to the launcher.",
                                     "Far Cry 5 VR: game options", MessageBoxButtons.YesNoCancel, MessageBoxIcon.Warning);
        if (answer == DialogResult.Cancel) return false;
        if (answer == DialogResult.Yes) {
            try {
                Launcher.FixGameOptions();
            } catch (Exception e) {
                MessageBox.Show(this, "Could not change the game's options file:\n" + e.Message + "\nSet them in the game's Options instead.", "Far Cry 5 VR",
                                MessageBoxButtons.OK, MessageBoxIcon.Error);
                return false;
            }
            UpdateInfo();
        }
        return true;
    }

    void GameClosed(DateTime started, bool found, uint code)
    {
        var bin = Launcher.GameDir;
        var log = Report.LogPath(bin);
        var crash = Report.NewestCrash(bin);
        bool newCrash = crash != null && File.GetLastWriteTime(crash) >= started;
        bool vrRan = Report.LogSaysVrRan(log, started);
        bool badCode = code >= 0xC0000000u;
        if (found && vrRan && !newCrash && !badCode) {
            Close();   // a normal session
            return;
        }
        var why = new StringBuilder();
        if (!found) why.AppendLine("Far Cry 5 was not seen running within 3 minutes of Play.");
        else why.AppendLine(string.Format(CultureInfo.InvariantCulture, "Exit code 0x{0:X8}{1}.", code, badCode ? " (a crash)" : ""));
        if (newCrash) why.AppendLine("The mod wrote a crash report: " + Path.GetFileName(crash) + ".");
        if (!vrRan) {
            var step = Report.LastBootStep(log);
            why.AppendLine("VR did not start" + (step != null ? ": the last start step in the log was " + step : " (no start steps in the log).") );
        }
        var exitInfo = "Last game run: " + (found ? string.Format(CultureInfo.InvariantCulture, "exit code 0x{0:X8}", code) : "the game was not seen running") +
                       ", Play at " + started.ToString("yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture) + (vrRan ? ", VR ran" : ", VR did not start");
        WindowState = FormWindowState.Normal;
        Activate();
        var title = vrRan ? "Far Cry 5 crashed" : "Far Cry 5 closed during start / VR did not start";
        if (MessageBox.Show(this, why + "\nSave a report (FC5VR_report_<date>.zip in the game's bin folder) to send to the mod's author? It holds the mod's log and crash report, " +
                                  "the VR runtime's log, system details and Windows' error events. Nothing is uploaded.",
                            title, MessageBoxButtons.YesNo, MessageBoxIcon.Warning) == DialogResult.Yes)
            MakeReport(exitInfo);
        Close();
    }

    void MakeReport(string exitInfo)
    {
        try {
            Cursor = Cursors.WaitCursor;
            var path = Report.Make(Launcher.GameDir, exitInfo);
            Cursor = Cursors.Default;
            Report.ShowInExplorer(path);
        } catch (Exception e) {
            Cursor = Cursors.Default;
            MessageBox.Show(this, "Could not write the report:\n" + e.Message, "Far Cry 5 VR", MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
    }
}
