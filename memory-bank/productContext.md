# Product Context

## Why This Project Exists

PhotoViewer, Windows kullanıcıları için hızlı, stabil ve geniş format destekli bir fotoğraf görüntüleyicidir.
Windows'un yerleşik görüntüleyicisinin yavaş açılması ve bazı formatlarda (HEIC, JXL) sorun yaşaması bu projenin çıkış noktasıdır.

## Problems It Solves

1. **Yavaş açılış**: WinUI 3 / .NET tabanlı görüntüleyicilerin ~300–500ms startup gecikmesi — yeni yapıyla <10ms
2. **Format sorunları**: HEIC, JXL, AVIF gibi modern formatların sistem codec olmadan açılamaması — özel decoderlarla çözüldü
3. **EXIF Bilgisi Eksikliği**: Kamera modeli, GPS koordinatları, çözünürlük bilgisi sade bir panel ile sunulacak
4. **Albüm Gezinme Zorluğu**: Klasör içinde ← → ile hızlı gezinti
5. **Esnek Çoklu Pencere**: Explorer'dan her çift tıklama yeni bağımsız pencere açar; aynı anda birden fazla fotoğraf karşılaştırılabilir

## How It Should Work

- Kullanıcı Explorer'dan bir fotoğrafa çift tıklar → uygulama **anında** (<10ms) açılır
- Fotoğraf GPU üzerinde merkezi olarak gösterilir
- Fare tekerleği ile zoom, sürükleme ile pan
- ← → tuşları veya kenar alanları ile klasördeki fotoğraflar arasında gezinme
- Sağ panel: EXIF bilgileri (kamera, çözünürlük, boyut, GPS)
- Panel açılıp kapatılabilir, durum kalıcı

## User Experience Goals

- **Anlık açılış** — en kritik hedef
- Minimalist, modern arayüz
- Sorunsuz format desteği (HEIC, JXL dahil)
- Sezgisel navigasyon
- Bilgilendirici ama kalabalık olmayan metadata paneli
