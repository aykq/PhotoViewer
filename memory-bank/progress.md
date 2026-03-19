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
- Magick.NET-Q8-AnyCPU 14.11.0
- MetadataExtractor 2.9.0

---

## Phase 1: Temel Düzenleme

### Yapılacak Görevler
- [ ] Toolbar ekleme (döndürme, kırpma, silme, kaydetme butonları)
- [ ] Sola/sağa döndürme (90° döndürme)
- [ ] Hızlı kırpma (center crop)
- [ ] Değişiklikleri kaydetme (backup ile)
- [ ] Fotoğraf silme
- [ ] Dizin bazlı navigasyon (ileri/geri geçiş)

### Notlar
- Döndürme ve kırpma Magick.NET ile yapılacak
- Kaydetmeden önce .bak backup dosyası oluşturulacak
- Navigasyon için dizindeki tüm fotoğraf dosyaları listelenecek

---

## Phase 2: Gelişmiş Özellikler

### Yapılacak Görevler
- [ ] Dosya seçici dialog (FileOpenPicker)
- [ ] Sürükle-bırak desteği
- [ ] Tam kırpma aracı (kullanıcı seçimli alan)
- [ ] Filtreler/efektler (parlaklık, kontrast, keskinlik, doygunluk)
- [ ] Kırpma oranı seçenekleri (16:9, 4:3, 1:1, vb.)
- [ ] Undo/Redo sistemi

### Notlar
- Filtreler için Magick.NET kullanılabilir
- Undo/Redo için değişiklikleri bir stack'te tutma

---

## Phase 3: Kullanıcı Deneyimi

### Yapılacak Görevler
- [ ] Slayt gösterisi modu
- [ ] Favoriler/album sistemi
- [ ] Batch işlemler (toplu döndürme, silme, format dönüştürme)
- [ ] Karanlık/açık tema desteği
- [ ] Ayarlar sayfası
- [ ] Dil desteği (localization/internationalization)
- [ ] Klavye kısayolları

### Notlar
- Tema desteği için WinUI 3 ResourceDictionary kullanılabilir
- Localization için .resw dosyaları kullanılacak

---

## Bilinen Sorunlar

1. Info panel animasyonu manuel implementasyon gerektiriyor (GridLength animasyonu WinUI3'te sorunlu)
2. MetadataExtractor bazı formatlarda (HEIC, RAW) tam veri okuyamayabilir

## Proje Kararları

1. **WinUI 3**: Modern Windows UI için doğru seçim
2. **Magick.NET**: Görüntü işleme için güçlü ve platform bağımsız
3. **Single-instance**: AppLifecycle API ile tek kopya çalışma
4. **Backup system**: .bak dosyaları ile değişiklik güvenliği
5. **CommunityToolkit.Mvvm**: [ObservableProperty] ve [RelayCommand] ile kod tekrarını azaltma
