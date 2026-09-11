# Builds the FM8+ icon by extracting FM8's own program icon and compositing icon_overlay.ico (the
# "+") over it, and optionally embeds the result into FM8.plus.exe as its icon resource.
#
# IMPORTANT: the result contains Native Instruments' artwork, so it must NEVER be committed to this
# repo or shipped in the installer. The installer runs this script on the END USER's machine against
# their own licensed FM8.exe (see MakeIcon / EmbedIcon in installer\FM8.plus.iss), which is also why
# the icon is injected into their installed copy of FM8.plus.exe rather than built into the binary we
# distribute. Only our "+" overlay ships. installer\FM8.plus.ico is gitignored for this reason.
#
#   # composite only
#   powershell -ExecutionPolicy Bypass -File tools\make_icon.ps1 -Out <path.ico> [-Fm8Exe <path>]
#   # composite and embed into an executable
#   powershell -ExecutionPolicy Bypass -File tools\make_icon.ps1 -Out <path.ico> -EmbedInto <path.exe>
#   # embed an already-built icon (used after the installer has copied the launcher into place)
#   powershell -ExecutionPolicy Bypass -File tools\make_icon.ps1 -EmbedOnly -Out <path.ico> -EmbedInto <path.exe>
param(
  [string]$Fm8Exe  = "C:\Program Files\Native Instruments\FM8\FM8.exe",
  [string]$Overlay = "$PSScriptRoot\..\installer\icon_overlay.ico",
  [string]$Out     = "$PSScriptRoot\..\installer\FM8.plus.ico",
  [string]$EmbedInto = "",
  [switch]$EmbedOnly
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

Add-Type @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;

public static class IconMaker {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern int PrivateExtractIcons(string path, int index, int cx, int cy, IntPtr[] hicon, int[] id, int count, int flags);
    [DllImport("user32.dll")] static extern bool DestroyIcon(IntPtr h);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr BeginUpdateResource(string pFileName, bool bDeleteExistingResources);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool UpdateResource(IntPtr hUpdate, IntPtr lpType, IntPtr lpName, ushort wLanguage, byte[] lpData, uint cbData);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool EndUpdateResource(IntPtr hUpdate, bool fDiscard);

    const int RT_ICON = 3, RT_GROUP_ICON = 14;

    // Largest FM8 program icon available at or below 256px, as a 256x256 bitmap.
    static Bitmap ExtractBase(string exe) {
        foreach (int s in new[] { 256, 128, 96, 64, 48, 32 }) {
            IntPtr[] h = new IntPtr[1]; int[] id = new int[1];
            if (PrivateExtractIcons(exe, 0, s, s, h, id, 1, 0) > 0 && h[0] != IntPtr.Zero) {
                using (Icon ic = (Icon)Icon.FromHandle(h[0]).Clone())
                using (Bitmap raw = ic.ToBitmap()) {
                    DestroyIcon(h[0]);
                    Bitmap canvas = new Bitmap(256, 256, PixelFormat.Format32bppArgb);
                    using (Graphics g = Graphics.FromImage(canvas)) {
                        g.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.HighQualityBicubic;
                        g.DrawImage(raw, 0, 0, 256, 256);
                    }
                    return canvas;
                }
            }
        }
        throw new Exception("No icon found in " + exe);
    }

    public static void Build(string exe, string overlay, string outPath) {
        using (Bitmap baseImg = ExtractBase(exe))
        using (Bitmap ov = new Bitmap(overlay))
        using (Graphics g = Graphics.FromImage(baseImg)) {
            g.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.HighQualityBicubic;
            g.DrawImage(ov, 0, 0, 256, 256);   // overlay is a full-canvas 256x256 "+" mark
            WriteIco(baseImg, outPath, new[] { 256, 64, 48, 32, 16 });
        }
    }

    // Multi-size .ico, every entry a PNG frame (supported on Vista+).
    static void WriteIco(Bitmap src, string outPath, int[] sizes) {
        var pngs = new System.Collections.Generic.List<byte[]>();
        foreach (int s in sizes) {
            using (Bitmap b = new Bitmap(s, s, PixelFormat.Format32bppArgb))
            using (Graphics g = Graphics.FromImage(b)) {
                g.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.HighQualityBicubic;
                g.DrawImage(src, 0, 0, s, s);
                using (var ms = new MemoryStream()) { b.Save(ms, ImageFormat.Png); pngs.Add(ms.ToArray()); }
            }
        }
        using (var fs = new FileStream(outPath, FileMode.Create))
        using (var w = new BinaryWriter(fs)) {
            w.Write((short)0); w.Write((short)1); w.Write((short)sizes.Length);   // ICONDIR
            int offset = 6 + 16 * sizes.Length;
            for (int i = 0; i < sizes.Length; i++) {
                int s = sizes[i];
                w.Write((byte)(s >= 256 ? 0 : s));
                w.Write((byte)(s >= 256 ? 0 : s));
                w.Write((byte)0);
                w.Write((byte)0);
                w.Write((short)1);
                w.Write((short)32);
                w.Write(pngs[i].Length);
                w.Write(offset);
                offset += pngs[i].Length;
            }
            foreach (var p in pngs) w.Write(p);
        }
    }

    // Inject an .ico into a PE as RT_ICON frames + an RT_GROUP_ICON directory, so Explorer shows it.
    // Existing resources (notably the manifest) are preserved.
    public static void Embed(string icoPath, string exePath) {
        byte[] ico = File.ReadAllBytes(icoPath);
        int count = BitConverter.ToUInt16(ico, 4);
        if (BitConverter.ToUInt16(ico, 2) != 1 || count < 1) throw new Exception("Not an icon file: " + icoPath);

        var images = new byte[count][];
        var group = new MemoryStream();
        var gw = new BinaryWriter(group);
        gw.Write((short)0); gw.Write((short)1); gw.Write((short)count);   // GRPICONDIR

        for (int i = 0; i < count; i++) {
            int e = 6 + 16 * i;
            int bytesInRes = BitConverter.ToInt32(ico, e + 8);
            int imageOffset = BitConverter.ToInt32(ico, e + 12);
            images[i] = new byte[bytesInRes];
            Buffer.BlockCopy(ico, imageOffset, images[i], 0, bytesInRes);
            // GRPICONDIRENTRY: same as ICONDIRENTRY but a 2-byte resource id replaces the offset.
            gw.Write(ico[e]); gw.Write(ico[e + 1]); gw.Write(ico[e + 2]); gw.Write(ico[e + 3]);
            gw.Write(BitConverter.ToUInt16(ico, e + 4));   // planes
            gw.Write(BitConverter.ToUInt16(ico, e + 6));   // bit count
            gw.Write(bytesInRes);
            gw.Write((ushort)(i + 1));                     // RT_ICON resource id
        }
        gw.Flush();

        IntPtr h = BeginUpdateResource(exePath, false);
        if (h == IntPtr.Zero) throw new Exception("BeginUpdateResource failed (" + Marshal.GetLastWin32Error() + ") on " + exePath);
        try {
            for (int i = 0; i < count; i++)
                if (!UpdateResource(h, (IntPtr)RT_ICON, (IntPtr)(i + 1), 0, images[i], (uint)images[i].Length))
                    throw new Exception("UpdateResource(RT_ICON " + (i + 1) + ") failed (" + Marshal.GetLastWin32Error() + ")");
            byte[] g = group.ToArray();
            if (!UpdateResource(h, (IntPtr)RT_GROUP_ICON, (IntPtr)1, 0, g, (uint)g.Length))
                throw new Exception("UpdateResource(RT_GROUP_ICON) failed (" + Marshal.GetLastWin32Error() + ")");
        } catch {
            EndUpdateResource(h, true);   // discard
            throw;
        }
        if (!EndUpdateResource(h, false))
            throw new Exception("EndUpdateResource failed (" + Marshal.GetLastWin32Error() + ")");
    }
}
'@ -ReferencedAssemblies System.Drawing

if (-not $EmbedOnly) {
  if (-not (Test-Path $Fm8Exe))  { throw "FM8.exe not found: $Fm8Exe" }
  if (-not (Test-Path $Overlay)) { throw "Overlay not found: $Overlay" }
  [IconMaker]::Build($Fm8Exe, (Resolve-Path $Overlay).Path, $Out)
  Write-Host "Wrote $Out"
}

if ($EmbedInto) {
  if (-not (Test-Path $Out))       { throw "Icon not found: $Out" }
  if (-not (Test-Path $EmbedInto)) { throw "Executable not found: $EmbedInto" }
  [IconMaker]::Embed((Resolve-Path $Out).Path, (Resolve-Path $EmbedInto).Path)
  Write-Host "Embedded $Out into $EmbedInto"
}
