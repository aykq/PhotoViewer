# Progress

## Taşınan Özellikler (Eski WinUI 3 → Yeni C++)

Aşağıdaki özellikler yeni C++ yapısına taşınacak:

| Özellik | Durum |
|---------|-------|
| Fotoğraf görüntüleme (merkezi, uygun ölçek) | Planlandı |
| Zoom: fare tekerleği ±10%, %10–%1000 | Planlandı |
| Zoom: çift tıklama 2.5x / reset | Planlandı |
| Zoom indicator overlay (otomatik gizlenen) | Planlandı |
| Pan: sürükleme ile kaydırma | Planlandı |
| Klasör navigasyonu: önceki/sonraki (← →) | Planlandı |
| Döngüsel navigasyon (son ↔ ilk) | Planlandı |
| Explorer-uyumlu sıralama (StrCmpLogicalW) | Planlandı |
| Yükleme iptali (hızlı ardışık geçişte) | Planlandı |
| EXIF metadata paneli | Planlandı |
| Metadata: kamera make/model | Planlandı |
| Metadata: çözünürlük, dosya boyutu, format | Planlandı |
| Metadata: GPS koordinatları + altitude | Planlandı |
| Info panel açma/kapama animasyonu | Planlandı |
| Panel durumu kalıcı saklama | Planlandı |
| Dosya adı pencere başlığında ve panelde | Planlandı |
| Multi-instance (her çift tıklama yeni pencere) | Planlandı |
| Dosya aktivasyonu (Explorer'dan çift tıklama) | Planlandı |
| Format: JPG, PNG, GIF, BMP, WebP, TIFF, ICO | Planlandı |
| Format: HEIC, HEIF (libheif) | Planlandı |
| Format: JXL (libjxl) | Planlandı |
| Format: AVIF | Planlandı |

---

## Phase 0: C++ Proje Kurulumu (Sonraki Adım)

### Yapılacaklar
- [ ] Visual Studio 2022 C++ Win32 projesi oluştur
- [ ] Direct2D, DirectWrite, WIC header'ları ve linkleri
- [ ] Temel `WinMain` + pencere sınıfı (`RegisterClassEx` + `CreateWindowEx`)
- [ ] Mesaj döngüsü (`GetMessage` / `DispatchMessage`)
- [ ] `WM_PAINT` içinde Direct2D HwndRenderTarget kurulumu
- [ ] `WM_SIZE` ile render target yeniden boyutlandırma
- [ ] vcpkg kurulumu (libheif, libjxl için)
- [ ] Proje yapısı: `src/`, `include/`, klasör düzeni

---

## Phase 1: Temel Görüntü Gösterimi

### Yapılacaklar
- [ ] WIC ile JPEG/PNG/BMP decode → `ID2D1Bitmap`
- [ ] `DrawBitmap()` ile ekranda merkezi gösterim (aspect ratio korumalı)
- [ ] Komut satırı argümanından dosya yolu alma
- [ ] Pencere başlığında dosya adı

---

## Phase 2: Zoom ve Pan

### Yapılacaklar
- [ ] `WM_MOUSEWHEEL` → zoom faktörü güncelle (±10%)
- [ ] Cursor pozisyonuna göre zoom anchor (Matrix3x2F)
- [ ] `WM_LBUTTONDOWN/MOVE/UP` → pan (sürükleme)
- [ ] `WM_LBUTTONDBLCLK` → 2.5x zoom veya reset
- [ ] Zoom %10–%1000 clamp, %100'e snap
- [ ] Zoom indicator overlay (DirectWrite, otomatik gizlenen timer ile)

---

## Phase 3: Klasör Navigasyonu

### Yapılacaklar
- [ ] `FolderNavigator`: dizin tarama, uzantı filtresi
- [ ] `StrCmpLogicalW` sıralama
- [ ] `WM_KEYDOWN` ← → ile önceki/sonraki
- [ ] Döngüsel navigasyon (son → ilk, ilk → son)
- [ ] Hızlı geçişte önceki decode iptali (`std::atomic<bool>` cancel flag)
- [ ] Arka plan thread'inde decode (UI donmaması)

---

## Phase 4: Geniş Format Desteği

### Yapılacaklar
- [ ] WIC birincil decode (JPG, PNG, GIF, BMP, WebP, TIFF, ICO)
- [ ] EXIF yönü otomatik uygulama (WIC)
- [ ] libheif entegrasyonu → HEIC, HEIF
- [ ] libjxl entegrasyonu → JXL
- [ ] libavif entegrasyonu → AVIF (opsiyonel)
- [ ] Decode başarısız olunca kullanıcıya bilgi mesajı

---

## Phase 5: Metadata ve Info Panel

### Yapılacaklar
- [ ] WIC `IWICMetadataQueryReader`: kamera make/model, GPS
- [ ] Dosya boyutu (KB/MB formatı)
- [ ] Çözünürlük (`IWICBitmapFrameDecode::GetSize`)
- [ ] Info panel çizimi (DirectWrite, sağ kenar)
- [ ] Panel açma/kapama (klavye kısayolu veya tıklama)
- [ ] Panel açma/kapama animasyonu (frame bazlı genişlik interpolasyonu)
- [ ] Panel durumu kalıcı saklama (registry veya INI dosyası)

---

## Phase 6: Multi-Instance ve Dosya Aktivasyonu

### Davranış
- Explorer'dan fotoğrafa çift tıklama → **her zaman yeni pencere** açılır
- Her pencere kendi klasöründe bağımsız çalışır (← → kendi penceresi içinde)
- Aynı anda birden fazla pencere açık olabilir
- Single-instance kontrolü yok — standart multi-instance uygulama

### Yapılacaklar
- [ ] Komut satırı argümanından dosya yolu alma (`argv[1]`)
- [ ] Her `WinMain` çağrısı bağımsız pencere açar (mutex yok)
- [ ] Explorer dosya ilişkilendirmesi (her tıklama yeni process başlatır)

---

## Proje Kararları

1. **C++23 + Win32 + Direct2D**: Anlık startup, GPU rendering, GC yok
2. **WIC birincil decoder**: OS-native, hızlı, sistem codec'leriyle genişletilebilir
3. **libheif + libjxl**: Sistem codec olmadan HEIC/JXL garantili açılır
4. **vcpkg**: Üçüncü taraf kütüphane yönetimi
5. **Multi-instance**: Her Explorer çift tıklaması yeni pencere açar; pencereler bağımsız çalışır
6. **Eski WinUI 3 / .NET kodu korunmaz**: Tamamen yeni proje
