<div align="center">
  <img src="Assets/Square150x150Logo.scale-400.png" alt="Lumina" width="120">

  # Lumina

  **Windows için hızlı, modern fotoğraf görüntüleyici**

  [![Son Sürüm](https://img.shields.io/github/v/release/aykq/Lumina?label=indir&color=blue)](https://github.com/aykq/Lumina/releases/latest)
  [![Lisans: MIT](https://img.shields.io/badge/lisans-MIT-green)](LICENSE)
  [![Platform](https://img.shields.io/badge/Windows-10%2F11%20x64-0078D4?logo=windows&logoColor=white)](https://github.com/aykq/Lumina/releases)

  [English](README.md)
</div>

---

Lumina, C++23, Win32 ve Direct2D ile yazılmış yerli bir Windows fotoğraf görüntüleyicisidir. 10 ms'den kısa sürede açılır, GPU üzerinde render eder ve ek sistem codec'i gerektirmeden HEIC, JPEG XL ve AVIF dahil tüm yaygın görüntü formatlarını destekler.

## Özellikler

- **Anlık açılış** — <10 ms başlangıç, splash screen yok, runtime yükü yok
- **GPU render** — Direct2D sıfır-kopya görüntüleme; zoom ve pan CPU'ya dokunmaz
- **Geniş format desteği** — aşağıdaki tabloya bakın; animasyonlu WebP, JXL, AVIF ve GIF dahil
- **EXIF metadata paneli** — kamera modeli, lens, enstantane hızı, diyafram, ISO, GPS koordinatları
- **OpenStreetMap önizlemesi** — konum etiketli fotoğraflarda satır içi harita; Google Maps bağlantısı
- **Prefetch cache** — anında klasör gezintisi için ±8 fotoğraf önbelleği
- **Filmstrip çubuğu** — altta kaydırılabilir küçük resim şeridi (**F** tuşu ile aç/kapat)
- **Fotoğraf düzenleme** — 90° döndürme, serbest açı ufuk düzeltme, kırpma (serbest + 1:1 / 4:3 / 16:9 / …), yeniden boyutlandırma
- **Explorer küçük resimleri** — `LuminaShell.dll`, HEIC, JXL, AVIF ve WebP için Explorer önizlemesi ekler
- **Çoklu örnek** — Explorer'dan her çift tıklama bağımsız bir pencere açar

## Desteklenen Formatlar

| Format | Uzantı | Animasyon | EXIF / GPS |
|--------|--------|:---------:|:----------:|
| JPEG | `.jpg` `.jpeg` | — | ✅ |
| PNG | `.png` | — | ✅ |
| BMP | `.bmp` | — | — |
| GIF | `.gif` | ✅ | — |
| TIFF | `.tif` `.tiff` | — | ✅ |
| ICO | `.ico` | — | — |
| WebP | `.webp` | ✅ | ✅ |
| HEIC / HEIF | `.heic` `.heif` | ✅ | ✅ |
| JPEG XL | `.jxl` | ✅ | ✅ |
| AVIF | `.avif` | ✅ | ✅ |

## Ekran Görüntüleri

<!-- screenshot: ana görüntüleyici -->
<!-- screenshot: GPS haritası ile bilgi paneli -->
<!-- screenshot: filmstrip çubuğu -->
<!-- screenshot: Explorer küçük resimleri (HEIC/JXL) -->

## Kurulum

Son sürümü **[GitHub Releases](https://github.com/aykq/Lumina/releases/latest)** sayfasından indirin.

| Paket | Açıklama |
|-------|----------|
| `Lumina-*-x64-Setup.exe` | Önerilen — rehberli kurulum (Inno Setup) |
| `Lumina-*-x64.msi` | Kurumsal / Grup İlkesi dağıtımı için MSI |
| `Lumina-*-x64-Portable.zip` | Kurulum gerektirmez; `LuminaCpp.exe`'yi doğrudan çalıştırın |

**Sistem gereksinimleri:** Windows 10 sürüm 1809 (build 17763) veya üzeri, x64.

### SmartScreen uyarısı

Yükleyici Authenticode ile imzalanmamıştır. İlk çalıştırmada Windows Defender SmartScreen *Bilinmeyen yayımcı* uyarısı gösterebilir.

1. **Daha fazla bilgi → Yine de çalıştır**'a tıklayın
2. Dosya bütünlüğünü doğrulayın: `certutil -hashfile Lumina-*-Setup.exe SHA256` çıktısını `SHA256SUMS.txt` ile karşılaştırın
3. SLSA kanıtlaması: `gh attestation verify <artifact> --repo aykq/Lumina`



Kaydı silmek için: `regsvr32 /u "C:\LuminaShell.dll konumu\LuminaShell.dll"`

## Klavye Kısayolları

| Tuş | İşlem |
|-----|-------|
| `←` / `→` | Önceki / sonraki fotoğraf |
| `Ctrl+←` / `Ctrl+→` | 90° sola / sağa döndür |
| `[` / `]` | 90° sola / sağa döndür (alternatif) |
| `Delete` | Mevcut fotoğrafı sil |
| `I` | Bilgi / metadata panelini aç/kapat |
| `F` | Filmstrip çubuğunu aç/kapat |
| `T` | 12 sa / 24 sa saat formatını değiştir |
| `Esc` / `Ctrl+W` | Pencereyi kapat |
| Çift tıklama | 2,5× zoom / sığdır sıfırla |
| Fare tekerleği | Yaklaştır / uzaklaştır (%10 adımlarla) |
| Sürükleme | Kaydır (pan) |

## Kaynak Koddan Derleme

**Gereksinimler**

- [Visual Studio 2022](https://visualstudio.microsoft.com/) — **C++ ile masaüstü geliştirme** iş yükü
- Windows 10 SDK ≥ 10.0.19041
- [vcpkg](https://github.com/microsoft/vcpkg) (güncel herhangi bir sürüm; bağımlılıklar `vcpkg.json` içinde tanımlıdır)

```powershell
# Repoyu klonlayın
git clone https://github.com/aykq/Lumina.git
cd Lumina

# vcpkg'yi MSBuild ile entegre edin (makine başına bir kez)
vcpkg integrate install

# Derleme — vcpkg.json ilk derlemede tüm bağımlılıkları otomatik indirir
msbuild LuminaCpp\LuminaCpp.vcxproj /p:Configuration=Release /p:Platform=x64 /m
msbuild LuminaShell\LuminaShell.vcxproj /p:Configuration=Release /p:Platform=x64 /m
```

Çıktı dosyaları `x64\Release\` dizinine yazılır.

**Shell uzantısını kaydedin (isteğe bağlı)**

```powershell
# Yönetici olarak çalıştırın
regsvr32 "x64\Release\LuminaShell.dll"
```

**HEIC decode notu:** Lumina önce işletim sistemi codec'ini (HEIF Image Extensions, GPU hızlandırmalı, çoğu Windows 11 22H2+ sistemine kurulu gelir) dener. Yoksa otomatik olarak dahili libheif decoder'a geçer.

## Katkı

Sorun raporları ve pull request'ler memnuniyetle karşılanır. Kapsamlı bir değişikliğe başlamadan önce yaklaşımı görüşmek için lütfen önce bir issue açın.

## Lisans

MIT © 2025 AykQ — bkz. [LICENSE](LICENSE).
