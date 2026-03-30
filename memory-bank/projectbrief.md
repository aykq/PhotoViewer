# Project Brief

PhotoViewer, Windows masaüstü için geliştirilmiş **native C++** tabanlı, **anlık açılan**, stabil bir fotoğraf görüntüleyici uygulamasıdır.

## Neden Yeniden Yazım?

WinUI 3 / .NET tabanlı ilk sürüm işlevsel olmakla birlikte:
- CLR + WinUI 3 startup gecikmesi (~300–500ms) hissedilir biçimde yavaştı
- Rendering CPU-side (WriteableBitmap) olduğundan zoom/pan GC pauzlarından etkileniyordu
- Bazı formatlarda (HEIC, JXL) codec yoksa görüntü açılamıyordu

Yeni hedef: **<10ms startup**, **GPU rendering**, **geniş format desteği**.

---

## Core Requirements

- **Anlık açılış**: <10ms startup, native binary
- **Hızlı fotoğraf görüntüleme**: Direct2D GPU rendering
- **Geniş format desteği**: JPG, PNG, GIF, BMP, WebP, TIFF, HEIC, HEIF, JXL, AVIF, ICO — sorunsuz
- **EXIF / metadata görüntüleme**: kamera, çözünürlük, boyut, GPS
- **Zoom ve pan**: fare tekerleği zoom, sürükleme ile kaydırma
- **Klasör navigasyonu**: önceki/sonraki, klavye okları (← →), döngüsel
- **Multi-instance**: Explorer'dan her çift tıklama yeni pencere açar; pencereler birbirinden bağımsız
- **Klasör navigasyonu pencere başına**: ← → her pencerenin kendi klasöründe çalışır
- **Dosya aktivasyonu**: komut satırı argümanı ile dosya açma

---

## Scope

- Windows 10/11 native desktop uygulaması
- C++23 + Win32 + Direct2D + WIC
- Özel decoderlar: libheif (HEIC), libjxl (JXL)
- Metadata: WIC MetadataQueryReader veya libexif

---

## Out of Scope (şu an)

- Düzenleme araçları (döndürme, kırpma, kaydetme)
- Slayt gösterisi
- Favoriler / albüm sistemi
- Filtreler / efektler
