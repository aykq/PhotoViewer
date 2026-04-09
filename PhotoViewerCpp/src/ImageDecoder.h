#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>

// Animasyon frame'i — BGRA pre-multiplied pikseller + süre
struct AnimFrame
{
    std::vector<uint8_t> pixels;  // BGRA pre-multiplied
    UINT width      = 0;
    UINT height     = 0;
    int  durationMs = 100;        // frame gösterim süresi (ms)
};

// Decode sonucu: BGRA pre-multiplied pikseller + EXIF metadata
struct DecodeOutput
{
    std::vector<uint8_t> pixels;   // BGRA pre-multiplied, satır sırası, width * height * 4 byte
    UINT         width      = 0;
    UINT         height     = 0;
    std::wstring format;           // ör. L"JPEG", L"WebP", L"HEIC"

    // EXIF metadata — yoksa boş
    std::wstring dateTaken;
    std::wstring cameraMake;
    std::wstring cameraModel;
    std::wstring aperture;         // ör. L"f/2.8"
    std::wstring shutterSpeed;     // ör. L"1/500s"
    std::wstring iso;
    // GPS — yoksa boş
    std::wstring gpsLatitude;      // ör. L"40°26′47.12″N"
    std::wstring gpsLongitude;     // ör. L"79°58′30.45″W"
    std::wstring gpsAltitude;      // ör. L"123.4 m"
    // GPS ondalık derece; hasGpsDecimal false ise geçersiz
    bool         hasGpsDecimal  = false;
    double       gpsLatDecimal  = 0.0;  // N = pozitif, S = negatif
    double       gpsLonDecimal  = 0.0;  // E = pozitif, W = negatif
    // Nominatim reverse geocoding sonucu — ör. L"Merzifon, Amasya, Türkiye"
    std::wstring gpsLocationName;
    // ICC renk profili adı — ör. L"Adobe RGB (1998)"; yoksa boş
    std::wstring iccProfileName;
    // Animasyon frame'leri — boş = statik görüntü (pixels kullanılır); dolu = animated
    std::vector<AnimFrame> frames;
};

// Desteklenen tüm formatları 32bpp BGRA pre-multiplied piksellere decode eder.
// EXIF yönelimi (Orientation tag) otomatik uygulanır.
// Başarılıysa true döner ve out.pixels dolu olur.
bool DecodeImage(const std::wstring& path, DecodeOutput& out);

// Nominatim reverse geocoding: koordinatları konum adına çevirir.
// Başarılıysa L"Şehir, İl, Ülke" formatında döner; hata durumunda boş wstring.
std::wstring FetchLocationName(double latDecimal, double lonDecimal);
