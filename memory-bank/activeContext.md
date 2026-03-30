# Active Context

## Mevcut Durum

Proje, WinUI 3 / .NET tabanlı yapıdan **C++23 + Win32 + Direct2D + WIC** tabanlı native yapıya taşınmak üzere kararlaştırıldı.
Eski kod henüz silinmedi — yeni proje kurulumundan önce referans olarak tutulabilir.

## Güncel Odak

**Bir sonraki adım: Phase 0 — C++ proje altyapısının kurulması**

1. Visual Studio 2022'de yeni C++ Win32 projesi oluşturma
2. Direct2D + WIC bağımlılıklarını ekleme
3. Temel pencere + mesaj döngüsü
4. vcpkg kurulumu (libheif, libjxl)

## Alınan Kararlar

- **Dil**: C++23
- **Rendering**: Direct2D (GPU, zero-copy)
- **Pencere**: Win32 API (WinUI 3 değil)
- **Decoder**: WIC birincil + libheif + libjxl fallback
- **Mimari**: Service-benzeri sınıflar (MVVM yok)
- **Paket yönetimi**: vcpkg
- **Referans**: [QuickView](https://github.com/justnullname/QuickView) (C++23 + Direct2D + özel decoderlar)

## Taşınan Özellikler

Eski WinUI 3 sürümündeki tüm temel özellikler yeni yapıya taşınacak:
zoom/pan, klasör navigasyonu, EXIF paneli, multi-instance, dosya aktivasyonu.
Detay için `progress.md` → "Taşınan Özellikler" tablosuna bakınız.

## Notlar

- Kullanıcının C++ veya Rust deneyimi yok — ilk C++ projesi olacak
- Rust ile C++ arasında kalındı; startup hızı ve Windows ekosistemi olgunluğu nedeniyle C++ seçildi
