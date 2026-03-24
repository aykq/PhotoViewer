# Progress

## Phase 0: Temel Görüntüleme (Tamamlandı)

### Tamamlanan Görevler
- [x] Ana pencere ve WinUI 3 kurulumu
- [x] Fotoğraf görüntüleme (merkezi, uniform stretch)
- [x] EXIF metadata paneli (kamera, çözünürlük, dosya boyutu, koordinatlar, GPS altitude, format)
- [x] Yakınlaştırma (fare tekerleği + çift tıklama)
- [x] Kaydırma/pan (sürükleme ile)
- [x] Zoom indicator
- [x] Info panel animasyonu (açılma/kapama)
- [x] Panel durumu persistence (local settings)
- [x] Single-instance çalışma
- [x] Dosya aktivasyonu (komut satırı argümanı ile dosya açma)

### Kullanılan Kütüphaneler
- Microsoft.WindowsAppSDK 1.8.260209005
- CommunityToolkit.Mvvm 8.4.0
- MetadataExtractor 2.9.0
- Görüntü decode: **Windows.Graphics.Imaging (WIC)** — Magick.NET yok

---

## Phase 1: Hızlı görüntüleme (güncel)

### Tamamlanan Görevler
- [x] **Düzenleme kaldırıldı** (döndürme, kırpma, diske yazma, `.bak` yok)
- [x] **WIC** ile decode (sistem codec’leri; daha hızlı açılış)
- [x] Büyük görsellerde uzun kenar **4096** ile UI ölçekleme
- [x] **Dizin gezintisi**: aynı klasördeki görüntüler, önceki/sonraki butonlar, **← / →** tuşları
- [x] Yükleme iptali (hızlı ardışık geçişte önceki decode iptal)

### Yapılacak Görevler
- [ ] Fotoğraf silme
- [ ] İsteğe bağlı: önbellek / ön yükleme (bir sonraki görsel için)
- [ ] İsteğe bağlı: FileOpenPicker, sürükle-bırak

### Notlar
- Gezinti: `PhotoFolderService` uzantı listesi (jpg, png, webp, heic, jxl, …)
- `StatusMessage`: codec yoksa veya dosya açılamazsa kullanıcıya bilgi

---

## Phase 2: Gelişmiş Özellikler

### Yapılacak Görevler
- [ ] Dosya seçici dialog (FileOpenPicker)
- [ ] Sürükle-bırak desteği
- [ ] (İstenirse) Düzenleme araçları — ayrı kütüphane / harici uygulama ile değerlendirme
- [ ] Filtreler/efektler
- [ ] Undo/Redo (düzenleme varsa)

### Notlar
- Düzenleme tekrar eklenirse: Magick, SkiaSharp veya GPU tabanlı seçenekler ayrıca değerlendirilir

---

## Phase 3: Kullanıcı Deneyimi

### Yapılacak Görevler
- [ ] Slayt gösterisi modu
- [ ] Favoriler/album sistemi
- [ ] Batch işlemler
- [ ] Karanlık/açık tema desteği
- [ ] Ayarlar sayfası
- [ ] Dil desteği (localization/internationalization)
- [ ] Ek klavye kısayolları (zoom, panel, vb.)

### Notlar
- Tema: WinUI 3 ResourceDictionary
- Localization: .resw

---

## Bilinen Sorunlar

1. Info panel animasyonu manuel implementasyon gerektiriyor (GridLength animasyonu WinUI3'te sorunlu)
2. MetadataExtractor bazı formatlarda (HEIC, RAW) tam veri okuyamayabilir
3. **HEIC**: Windows’ta HEIF uzantısı gerekir; codec yoksa `BitmapDecoder` başarısız olabilir
4. **JXL / bazı formatlar**: Sistemde WIC codec yoksa görüntü açılmaz (Magick fallback yok)

## Proje Kararları

1. **WinUI 3**: Modern Windows UI
2. **WIC (BitmapDecoder)**: Varsayılan görüntü yolu — hız ve sistem entegrasyonu
3. **Magick.NET kaldırıldı**: Düzenleme yok; ağır native kütüphane bağımlılığı yok
4. **Single-instance**: AppLifecycle API
5. **CommunityToolkit.Mvvm**: [ObservableProperty] ve [RelayCommand]
