# System Patterns

## System Architecture

PhotoViewer, MVVM (Model-View-ViewModel) mimarisi ile geliştirilmiş WinUI 3 uygulamasıdır.

```
Views (XAML + Code-behind)
    └── MainWindow.xaml / MainWindow.xaml.cs
            │
            ▼
ViewModels
    └── MainViewModel.cs (ObservableObject + RelayCommand)
            │
            ├── Services
            │   ├── MetadataService.cs
            │   ├── ImageLoaderService.cs   ← WIC BitmapDecoder
            │   └── PhotoFolderService.cs   ← klasördeki görüntü listesi
            │
            └── Models
                └── ExifData.cs
```

## Key Technical Decisions

1. **Single-Instance Pattern**: App.xaml.cs içinde AppLifecycle API kullanılarak tek kopya çalışma garantisi
2. **WriteableBitmap Display**: `BitmapDecoder` + `GetPixelDataAsync` (BGRA8) → `WriteableBitmap` — **Magick yok**
3. **Panel Animations**: Manuel easing ile GridLength animasyonu (WinUI3'te XAML animasyonu sorunlu)
4. **Metadata**: MetadataExtractor (EXIF) + `BitmapDecoder` başlığı (çözünürlük)
5. **Gezinti**: Açılan dosyanın klasörü taranır; sıralı liste; önceki/sonraki; `CancellationToken` ile hızlı geçişte iptal

## Design Patterns in Use

- **MVVM**: CommunityToolkit.Mvvm ile [ObservableProperty] ve [RelayCommand] attribute'ları
- **Service Layer**: MetadataService, ImageLoaderService, PhotoFolderService
- **Single Instance**: AppInstance.FindOrRegisterForKey ile tek kopya kontrolü

## Component Relationships

- **MainWindow**: Zoom/pan, info paneli, klavye ile gezinti (← →)
- **MainViewModel**: `LoadPhotoAsync`, `NextPhotoAsync`, `PreviousPhotoAsync`, metadata bağlantıları
- **MetadataService**: `GetExifMetadataAsync`
- **ImageLoaderService**: WIC ile decode
- **PhotoFolderService**: statik yardımcı — sıralı dosya listesi
- **ExifData**: Metadata modeli

## Critical Implementation Paths

1. **Fotoğraf Yükleme**: Dosya aktivasyonu → `LoadPhotoAsync` → `PhotoFolderService` (liste + indeks) → `ImageLoaderService` (WIC) → `WriteableBitmap` → UI
2. **Metadata**: `GetExifMetadataAsync` — MetadataExtractor + isteğe bağlı `BitmapDecoder` ile çözünürlük
3. **Gezinti**: `NextPhotoAsync` / `PreviousPhotoAsync` veya `LoadPhotoAsync` ile yeni yol — önceki yükleme `CancellationToken` ile iptal
