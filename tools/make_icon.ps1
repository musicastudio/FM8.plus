# Builds installer\FM8.plus.ico by extracting FM8's own program icon and compositing
# installer\icon_overlay.ico (the "+") over it. FM8's icon is identical for every 2022-12-23 install,
# so this is a one-time asset step; the resulting .ico is committed and embedded into FM8.plus.exe.
#
#   powershell -ExecutionPolicy Bypass -File tools\make_icon.ps1 [-Fm8Exe <path>]
param(
  [string]$Fm8Exe  = "C:\Program Files\Native Instruments\FM8\FM8.exe",
  [string]$Overlay = "$PSScriptRoot\..\installer\icon_overlay.ico",
  [string]$Out     = "$PSScriptRoot\..\installer\FM8.plus.ico"
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
                w.Write((byte)(s >= 256 ? 0 : s));  // width
                w.Write((byte)(s >= 256 ? 0 : s));  // height
                w.Write((byte)0);                   // colours
                w.Write((byte)0);                   // reserved
                w.Write((short)1);                  // planes
                w.Write((short)32);                 // bit count
                w.Write(pngs[i].Length);            // bytes in resource
                w.Write(offset);                    // image offset
                offset += pngs[i].Length;
            }
            foreach (var p in pngs) w.Write(p);
        }
    }
}
'@ -ReferencedAssemblies System.Drawing

if (-not (Test-Path $Fm8Exe))  { throw "FM8.exe not found: $Fm8Exe" }
if (-not (Test-Path $Overlay)) { throw "Overlay not found: $Overlay" }
[IconMaker]::Build($Fm8Exe, (Resolve-Path $Overlay), $Out)
Write-Host "Wrote $Out"
