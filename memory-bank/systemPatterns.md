# System Patterns

## Genel Mimari

C++ native uygulaması — MVVM yok. Sınıflar sorumluluk alanlarına göre ayrılmıştır.

```
main.cpp
  └── App (tek kopya kontrolü, Win32 pencere oluşturma)
        └── MainWindow
              ├── Renderer          — Direct2D render döngüsü, zoom/pan transform
              ├── ImageLoader       — WIC + özel decoder fallback
              ├── FolderNavigator   — klasör tarama, sıralama, indeks yönetimi
              ├── MetadataReader    — WIC MetadataQueryReader / libexif
              └── InfoPanel         — EXIF panel çizimi (DirectWrite)
```

---

## Bileşen Sorumlulukları

### App
- Multi-instance: mutex yok, her çalıştırma bağımsız pencere açar
- Komut satırı argümanını işleme (dosya yolu `argv[1]`)
- Ana pencereyi oluşturma ve mesaj döngüsünü başlatma

### MainWindow
- `WM_CREATE`: Direct2D + render target kurulumu
- `WM_PAINT`: Renderer'ı çağırır
- `WM_MOUSEWHEEL`: Zoom delta işleme
- `WM_LBUTTONDOWN/MOVE/UP`: Pan (sürükleme)
- `WM_LBUTTONDBLCLK`: Zoom reset / 2.5x toggle
- `WM_KEYDOWN`: ← → navigasyon
- `WM_SIZE`: Render target yeniden boyutlandırma

### Renderer
- `ID2D1HwndRenderTarget` veya DXGI swap chain
- `ID2D1Bitmap` olarak yüklenen görüntü (GPU memory)
- `D2D1::Matrix3x2F` ile zoom + pan transform
- Zoom indicator overlay (DirectWrite metin)
- Info panel arka planı + metin (DirectWrite)

### ImageLoader
- WIC `IWICBitmapDecoder` ile decode (birincil)
- EXIF yönü otomatik uygulanır
- Başarısızsa format uzantısına göre fallback:
  - `.heic/.heif` → libheif → piksel buffer → `ID2D1Bitmap`
  - `.jxl` → libjxl → piksel buffer → `ID2D1Bitmap`
- Büyük görüntülerde uzun kenar 4096 ile ölçekleme (opsiyonel)

### FolderNavigator
- Açılan dosyanın dizinini tarar
- Desteklenen uzantıları filtreler: `.jpg .jpeg .png .gif .bmp .webp .tiff .heic .heif .jxl .avif .ico`
- `StrCmpLogicalW` ile Explorer-uyumlu sıralama (IMG2 < IMG10)
- Önceki / sonraki indeks; döngüsel (son → ilk)

### MetadataReader
- WIC `IWICMetadataQueryReader`: kamera make/model, GPS koordinatları, altitude
- Dosya boyutu: `GetFileAttributesEx`
- Çözünürlük: WIC `IWICBitmapFrameDecode::GetSize`

---

## Key Technical Decisions

1. **Direct2D HwndRenderTarget**: En basit setup, HWND üzerine doğrudan render
2. **ID2D1Bitmap GPU memory**: Her zoom/pan frame'inde CPU'ya dokunulmaz
3. **Matrix3x2F transform**: Scale + translate tek matrix ile; cursor pozisyonuna göre zoom anchor
4. **Multi-instance**: Mutex yok; Explorer'dan her çift tıklama yeni process = yeni pencere; pencereler birbirinden tamamen bağımsız
5. **CancellationToken muadili**: Hızlı ardışık navigasyonda decode thread'ini iptal etmek için `std::atomic<bool>` cancel flag
6. **WIC MetadataQueryReader**: Ayrı kütüphane gerekmeden EXIF okuma

---

## Kritik Akışlar

### Görüntü Yükleme
```
Dosya yolu
  → ImageLoader::Load(path)
    → WIC BitmapDecoder → EXIF yönü → ID2D1Bitmap
    → (başarısızsa) libheif/libjxl → piksel buffer → ID2D1Bitmap
  → Renderer::SetImage(bitmap)
  → InvalidateRect → WM_PAINT
```

### Navigasyon
```
← / → tuşu veya buton
  → FolderNavigator::Prev() / Next()
  → önceki decode iptal (cancel flag)
  → ImageLoader::Load(yeni yol)  [arka plan thread]
  → MetadataReader::Read(yeni yol)
  → UI güncelle
```

### Zoom
```
WM_MOUSEWHEEL (delta)
  → zoom faktörü güncelle (±10%, clamp %10–%1000)
  → cursor pozisyonuna göre translate hesapla
  → Matrix3x2F güncelle
  → InvalidateRect → WM_PAINT
```
