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

// Piksel decode YAPMADAN sadece metadata alanlarını doldurur:
// width, height, format, dateTaken, cameraMake, cameraModel, aperture,
// shutterSpeed, iso, GPS alanları, iccProfileName.
// DecodeImage'dan ~10-20× daha hızlı; info paneli için WM_META_DONE aşamasında kullanılır.
bool ExtractImageMeta(const std::wstring& path, DecodeOutput& out);

// Thumbnail için optimize edilmiş decode: metadata atlanır, WIC scaler pipeline kullanılır.
// JPEG/PNG/BMP/TIFF/GIF/ICO için büyük buffer tahsis etmeden doğrudan hedef boyuta ölçekler.
// WebP/HEIC/JXL/AVIF için tam decode + ölçekleme yedek yolu kullanılır.
// Başarılıysa true döner; pixelsOut BGRA pre-multiplied, boyut widthOut × heightOut.
bool DecodeImageForThumbnail(const std::wstring& path, UINT targetH,
                              std::vector<uint8_t>& pixelsOut,
                              UINT& widthOut, UINT& heightOut);

// Nominatim reverse geocoding: koordinatları konum adına çevirir.
// Başarılıysa L"Şehir, İl, Ülke" formatında döner; hata durumunda boş wstring.
std::wstring FetchLocationName(double latDecimal, double lonDecimal);

// Web Mercator tile koordinatını hesaplar (zoom 0–19).
// tx, ty: OSM tile indeksleri; [0, 2^zoom) aralığında clamp edilir.
void LatLonToTileXY(double lat, double lon, int zoom, int& tx, int& ty);

// Tile içindeki piksel ofsetini hesaplar (tile boyutu 256×256 kabul edilir).
// px, py: tile'ın sol üst köşesine göre [0, 255] aralığında piksel konumu.
void LatLonToPixelInTile(double lat, double lon, int zoom, int tileX, int tileY,
                         int& px, int& py);

// WinHTTP ile tile.openstreetmap.org'dan PNG tile indirir.
// Başarısızsa boş vector döner.
std::vector<uint8_t> FetchOsmTile(int zoom, int x, int y);

// Ham PNG byte'larını BGRA pre-multiplied piksel dizisine dönüştürür.
// WIC kullanır; çağıran thread'de CoInitialize yapılmış olmalı.
// outW/outH doldurulur; başarısızsa false döner.
bool DecodePngToPixels(const uint8_t* pngBytes, size_t byteCount,
                       std::vector<uint8_t>& outPixels, UINT& outW, UINT& outH);

// HEIC/HEIF dosyasındaki gömülü küçük resmi hızlıca decode eder (~5-20ms).
// iPhone/kamera HEIC dosyalarında genellikle bulunur; yoksa false döner.
// Başarılıysa pixelsOut BGRA pre-mul, boyut widthOut × heightOut.
bool ExtractHEICEmbeddedPreview(const std::wstring& path,
                                 std::vector<uint8_t>& pixelsOut,
                                 UINT& widthOut, UINT& heightOut);
