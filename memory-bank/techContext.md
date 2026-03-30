# Tech Context

## Karar: C++ ile Sıfırdan Yeniden Yazım

Proje, WinUI 3 / .NET tabanlı yapıdan **C++23 + Direct2D + WIC** tabanlı native bir yapıya taşınmaktadır.
Sebep: CLR/WinUI 3 startup gecikmesi (~300–500ms), GC pauzları ve rendering pipeline'ın CPU-side olması.
Referans proje: [QuickView](https://github.com/justnullname/QuickView) (C++23 + Direct2D + WIC + özel decoderlar)

---

## Yeni Tech Stack

| Bileşen | Teknoloji | Notlar |
|---------|-----------|--------|
| **Dil** | C++23 | Modern, native |
| **Pencere / Mesaj döngüsü** | Win32 API | `CreateWindowEx`, `WM_` mesajları |
| **Rendering** | Direct2D | GPU-accelerated, zero-copy |
| **Metin / UI** | DirectWrite | Font render |
| **Görüntü decode (birincil)** | WIC (Windows Imaging Component) | OS-native, JPEG/PNG/BMP/GIF/WebP/TIFF/ICO |
| **HEIC / HEIF** | libheif | WIC codec yoksa devreye girer |
| **JXL** | libjxl | WIC codec yoksa devreye girer |
| **AVIF** | libavif / dav1d | Opsiyonel |
| **EXIF metadata** | WIC MetadataQueryReader | veya libexif |
| **IDE** | Visual Studio 2022 | C++ Desktop Development workload |
| **Build** | MSBuild / CMake | TBD |

---

## Startup Performansı

| Stack | Startup süresi |
|-------|----------------|
| WinUI 3 / .NET 8 (eski) | ~300–500ms |
| C++ Win32 + Direct2D (yeni) | **<10ms** |

---

## Rendering Mimarisi

```
Win32 HWND
  └── Direct2D HwndRenderTarget  (veya DXGI SwapChain)
        └── ID2D1Bitmap  (WIC'ten yüklenen görüntü)
              └── DrawBitmap() — GPU transform (zoom/pan)
```

- Zoom ve pan: `D2D1::Matrix3x2F` scale + translate — CPU'ya dokunmaz
- Animasyonlar: `WM_TIMER` veya DWM flush ile frame bazlı

---

## Format Desteği Stratejisi

```
Dosya açılır
  ├── WIC BitmapDecoder dene (JPEG, PNG, BMP, GIF, TIFF, WebP, ICO, HEIC*, JXL*)
  │     * sistem codec yüklüyse
  └── Başarısızsa → format uzantısına göre özel decoder
        ├── .heic / .heif  → libheif
        ├── .jxl           → libjxl
        └── .avif          → libavif
```

---

## Development Setup

### Gereksinimler
- Visual Studio 2022 (C++ Desktop Development workload)
- Windows 10/11 SDK (10.0.19041+)
- vcpkg (libheif, libjxl, libavif paket yönetimi için)

### Platform Hedefleri
- x64 (birincil), x86, ARM64

### Minimum Windows
- Windows 10 1809 (build 17763) — Direct2D 1.1 + WIC garantili

---

## Eski Stack (Kaldırıldı)

- .NET 8, WinUI 3, CommunityToolkit.Mvvm
- Magick.NET, MetadataExtractor
- WriteableBitmap, XAML rendering
