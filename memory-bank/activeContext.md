# Active Context

## Current Work Focus

Proje temizliği ve stabilizasyon çalışmaları tamamlandı. Memory Bank Phase 0-1-2-3 formatında güncellendi.

## Recent Changes

- **2026-03-18**: Temizlik ve stabilizasyon
  - NullToVisibilityConverter.cs silindi (boş dosya)
  - MainWindow.xaml.cs temizlendi (kullanılmayan kodlar kaldırıldı)
  - MainViewModel.cs temizlendi (kullanılmayan property'ler kaldırıldı)
  - MetadataService.cs temizlendi (aşırı metadata tag collection döngüsü kaldırıldı)
  - AnimatePanel metodu sadeleştirildi (kullanılmayan DoubleAnimation/Storyboard kodları kaldırıldı)
  - progress.md Phase 0-1-2-3 formatında güncellendi
  - systemPatterns.md güncellendi (mevcut yapıya uygun)
- **2026-03-19**: Derleme warning'leri giderildi
  - `Magick.NET-Q8-AnyCPU` güncellendi (NU1902/NU1903)
  - `MVVMTK0045` için `ObservableProperty` alanları `partial property` formuna çevrildi
  - `CS8622` için `App_Activated` nullability imzası düzeltildi
- **2026-03-19**: Phase 1 başlatıldı
  - Toolbar eklendi ve komutlar bağlandı
  - Sola/Sağa 90 derece döndürme eklendi (direkt orijinal dosyayı günceller)
- **2026-03-19**: Crop UX eklendi
  - Tek `Crop` butonu: crop modunda `check` ikonuna geçiyor
  - Görsel üzerinde sürüklenebilir `üst/alt/sol/sağ` kırpma göstergeleri
  - `check` ile kırpma uygulanıyor ve orijinal dosyanın üstüne yazılıyor
  - Döndürme işlemleri de orijinal dosyanın üstüne yazıyor

## Next Steps

1. Phase 1 için toolbar ekleme (döndürme, kırpma, silme, kaydetme)
2. Phase 1 için navigasyon sistemi ekleme
3. Memory Bank'i her değişiklikten sonra güncelle

## Active Decisions and Considerations

- Single-instance davranışı App.xaml.cs ile yönetiliyor
- Magick.NET görüntü işleme için hazır (Phase 1'de kullanılacak)
- MetadataService sadece gerekli alanları çıkarıyor (Camera, Resolution, FileSize, Coordinates, GpsAltitude, Format)
- Panel animasyonları manuel easing ile yapılıyor

## Learnings and Project Insights

- WinUI 3'te GridLength animasyonu XAML Storyboard ile çalışmıyor, manuel implementasyon gerekiyor
- CommunityToolkit.Mvvm [ObservableProperty] attribute'u kod üretimi yapıyor
- WriteableBitmap Magick.NET görüntülerini UI'da göstermek için kullanılıyor
- MetadataExtractor JPEG, PNG, TIFF, HEIC/HEIF formatlarını destekliyor
