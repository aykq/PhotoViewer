<div align="center">
  <img src="Assets/Square150x150Logo.scale-400.png" alt="Lumina" width="120">

  # Lumina

  **Fast, modern photo viewer for Windows**

  [![Latest Release](https://img.shields.io/github/v/release/aykq/Lumina?label=download&color=blue)](https://github.com/aykq/Lumina/releases/latest)
  [![License: MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)
  [![Platform](https://img.shields.io/badge/Windows-10%2F11%20x64-0078D4?logo=windows&logoColor=white)](https://github.com/aykq/Lumina/releases)

  [Türkçe](README-TR.md)
</div>

---

Lumina is a native Windows photo viewer built with C++23, Win32, and Direct2D. It opens in under 10 ms, renders via the GPU, and supports every major image format—including HEIC, JPEG XL, and AVIF—without requiring extra system codecs.

## Features

- **Instant startup** — <10 ms launch, no splash screen, no runtime overhead
- **GPU rendering** — Direct2D zero-copy display; zoom and pan never touch the CPU
- **Wide format support** — see table below, including animated WebP, JXL, AVIF, and GIF
- **EXIF metadata panel** — camera model, lens, shutter speed, aperture, ISO, GPS coordinates
- **OpenStreetMap preview** — inline map tile rendering for geotagged photos, click to open Google Maps
- **Prefetch cache** — ±8-photo lookahead for instant folder navigation
- **Filmstrip bar** — scrollable thumbnail strip at the bottom (toggle with **F**)
- **Photo editing** — 90° rotation, free-angle horizon correction, crop (free + fixed ratio 1:1 / 4:3 / 16:9 / …), resize
- **Explorer thumbnails** — `LuminaShell.dll` adds thumbnail support for HEIC, JXL, AVIF, and WebP
- **Multi-instance** — every Explorer double-click opens an independent, isolated window

## Supported Formats

| Format | Extension(s) | Animated | EXIF / GPS |
|--------|-------------|:--------:|:----------:|
| JPEG | `.jpg` `.jpeg` | — | ✅ |
| PNG | `.png` | — | ✅ |
| BMP | `.bmp` | — | — |
| GIF | `.gif` | ✅ | — |
| TIFF | `.tif` `.tiff` | — | ✅ |
| ICO | `.ico` | — | — |
| WebP | `.webp` | ✅ | ✅ |
| HEIC / HEIF | `.heic` `.heif` | ✅ | ✅ |
| JPEG XL | `.jxl` | ✅ | ✅ |
| AVIF | `.avif` | ✅ | ✅ |

## Screenshots

<!-- screenshot: main viewer -->
<!-- screenshot: info panel with GPS map -->
<!-- screenshot: filmstrip bar -->
<!-- screenshot: Explorer thumbnails (HEIC/JXL) -->

## Installation

Download the latest release from **[GitHub Releases](https://github.com/aykq/Lumina/releases/latest)**.

| Package | Description |
|---------|-------------|
| `Lumina-*-x64-Setup.exe` | Recommended — guided installer (Inno Setup) |
| `Lumina-*-x64.msi` | MSI for enterprise / Group Policy deployment |
| `Lumina-*-x64-Portable.zip` | No install required; run `LuminaCpp.exe` directly |

**System requirements:** Windows 10 version 1809 (build 17763) or later, x64.

### SmartScreen notice

The installer is not Authenticode-signed. Windows Defender SmartScreen may show an *Unknown publisher* warning on first run.

1. Click **More info → Run anyway**
2. Verify integrity: `certutil -hashfile Lumina-*-Setup.exe SHA256` and compare against `SHA256SUMS.txt`
3. SLSA provenance: `gh attestation verify <artifact> --repo aykq/Lumina`

## Keyboard Shortcuts

| Key | Action |
|-----|--------|
| `←` / `→` | Previous / next photo |
| `Ctrl+←` / `Ctrl+→` | Rotate 90° CCW / CW |
| `[` / `]` | Rotate 90° CCW / CW (alternate) |
| `Delete` | Delete current photo |
| `I` | Toggle info / metadata panel |
| `F` | Toggle filmstrip bar |
| `T` | Toggle 12 h / 24 h time format |
| `Esc` / `Ctrl+W` | Close window |
| Double-click | 2.5× zoom / reset to fit |
| Scroll wheel | Zoom in / out (10% steps) |
| Drag | Pan |

## Building from Source

**Prerequisites**

- [Visual Studio 2022](https://visualstudio.microsoft.com/) with the **Desktop development with C++** workload
- Windows 10 SDK ≥ 10.0.19041
- [vcpkg](https://github.com/microsoft/vcpkg) (any recent version; dependencies are declared in `vcpkg.json`)

```powershell
# Clone the repository
git clone https://github.com/aykq/Lumina.git
cd Lumina

# Integrate vcpkg with MSBuild (once per machine)
vcpkg integrate install

# Build — vcpkg.json pulls all dependencies automatically on first build
msbuild LuminaCpp\LuminaCpp.vcxproj /p:Configuration=Release /p:Platform=x64 /m
msbuild LuminaShell\LuminaShell.vcxproj /p:Configuration=Release /p:Platform=x64 /m
```

Output lands in `x64\Release\`.

**Register the shell extension (optional)**

```powershell
# Run as Administrator
regsvr32 "x64\Release\LuminaShell.dll"
```

**HEIC decoding note:** Lumina first tries the OS codec (HEIF Image Extensions, GPU-accelerated, pre-installed on most Windows 11 22H2+ systems). If unavailable, it falls back to the bundled libheif decoder automatically.

## Contributing

Issues and pull requests are welcome. Please open an issue before starting a significant change so we can discuss the approach.

## License

MIT © 2025 AykQ — see [LICENSE](LICENSE).
