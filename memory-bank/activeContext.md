# Active Context

## Current Work Focus

**Saf görüntüleyici** odaklı: düzenleme kaldırıldı; **WIC** ile hızlı decode; aynı klasörde **hızlı gezinti** (buton + ←/→).

## Recent Changes

- **2026-03-19**: Düzenleme tamamen kaldırıldı; Magick.NET kaldırıldı
  - `ImageLoaderService`: `Windows.Graphics.Imaging.BitmapDecoder`, EXIF yönü, max kenar 4096
  - `PhotoFolderService`: klasördeki görüntü uzantıları, sıralı liste, indeks
  - `MainViewModel`: `NextPhotoAsync` / `PreviousPhotoAsync`, yükleme iptali (`CancellationTokenSource`)
  - `MainWindow`: crop/toolbar kaldırıldı; önceki/sonraki; `RootGrid` klavye
  - `PhotoFileHelper.cs` silindi
  - `MetadataService`: `GetExifMetadataAsync`, çözünürlük için WIC
- **Memory bank**: `techContext.md`, `projectbrief.md`, `progress.md`, `systemPatterns.md` güncellendi

## Next Steps

1. İsteğe bağlı: fotoğraf silme
2. İsteğe bağlı: FileOpenPicker, sürükle-bırak, ön yükleme
3. Performans: büyük klasörlerde liste önbelleği veya lazy enumeration değerlendirmesi

## Active Decisions and Considerations

- Single-instance davranışı App.xaml.cs ile yönetiliyor
- Görüntü gösterimi **yalnızca WIC** (sistem codec); JXL/özel formatlar codec yoksa açılmayabilir
- MetadataService: `MetadataExtractor` + `BitmapDecoder` (çözünürlük)

## Learnings and Project Insights

- WinUI 3'te GridLength animasyonu XAML Storyboard ile çalışmıyor, manuel implementasyon gerekiyor
- CommunityToolkit.Mvvm [ObservableProperty] attribute'u kod üretimi yapıyor
- WriteableBitmap WIC BGRA pixel verisi ile dolduruluyor
- Ardışık gezintide önceki decode işlemini iptal etmek yanıt süresini iyileştirir
