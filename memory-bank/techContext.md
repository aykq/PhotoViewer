# Tech Context

## Technologies Used

| Teknoloji | Versiyon | Kullanım Alanı |
|-----------|----------|----------------|
| .NET | 8.0 | Runtime |
| Windows App SDK | 1.8.260209005 | WinUI 3 UI Framework |
| CommunityToolkit.Mvvm | 8.4.0 | MVVM pattern implementasyonu |
| **Windows.Graphics.Imaging (WIC)** | OS codec | Görüntü **decode** (BitmapDecoder → WriteableBitmap); sistem codec’leri (JPEG, PNG, HEIC*, WebP, vb.) |
| MetadataExtractor | 2.9.0 | EXIF metadata okuma |
| WinUI 3 | - | Kullanıcı arayüzü |

\* HEIC: Windows’ta HEIF Image Extensions yüklü olmalı.

**Not:** Magick.NET kaldırıldı; düzenleme yok. Ağır encode/decode yükü ortadan kalktı.

## Development Setup

### Gereksinimler
- Visual Studio 2022 veya .NET 8 SDK
- Windows 10/11
- Windows App SDK workload

### Derleme
```bash
dotnet build
dotnet run
```

### Platform Hedefleri
- x86, x64, ARM64

### Target Framework
- net8.0-windows10.0.19041.0
- Minimum: Windows 10 1809 (build 17763)

## Technical Constraints

- WinUI 3'te GridLength animasyonu XAML Storyboard ile çalışmıyor, manuel easing kullanılıyor
- Görüntü gösterimi **WIC** ile; bazı formatlar (ör. JXL) için sistemde codec yoksa açılmayabilir
- MetadataExtractor JPEG, PNG, TIFF, HEIC/HEIF vb. için EXIF okur; çözünürlük satırı için `BitmapDecoder` (hızlı başlık okuma)
- Single-instance için AppLifecycle API kullanılıyor

## Dependencies

```
CommunityToolkit.Mvvm
MetadataExtractor
Microsoft.WindowsAppSDK
```

## Tool Usage Patterns

- **Git**: Versiyon kontrolü
- **Visual Studio**: IDE
- **dotnet CLI**: Build ve test
- **NuGet**: Paket yönetimi
