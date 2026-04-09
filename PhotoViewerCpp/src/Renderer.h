#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <string>
#include <cstdint>
#include <vector>
#include "ImageDecoder.h"

// Direct2D, DirectWrite ve WIC kütüphanelerini otomatik linkle
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")

// Navigation arrow pill boyutları — hem Renderer hem main.cpp tarafından kullanılır
namespace ArrowLayout {
    constexpr float ZoneW  = 48.0f;  // tıklanabilir bölge genişliği (tam yükseklik)
    constexpr float PillH  = 80.0f;  // pill yüksekliği (sadece görsel)
    constexpr float Radius = 8.0f;   // köşe yarıçapı
}

// Info panel boyutları — hem Renderer hem main.cpp tarafından kullanılır
namespace PanelLayout {
    constexpr float Width = 280.0f;  // panel genişliği
    constexpr float PadX  = 16.0f;  // panel içi yatay dolgu
}

// Info button boyutları — hem Renderer hem main.cpp tarafından kullanılır
namespace InfoButton {
    constexpr float Size   = 32.0f;  // buton genişlik/yükseklik
    constexpr float Margin = 12.0f;  // pencere kenarından mesafe
}

// Görüntüye ait metadata (decode thread'de doldurulur, UI thread'de okunur)
struct ImageInfo
{
    std::wstring filename;
    int          width         = 0;
    int          height        = 0;
    int64_t      fileSizeBytes = 0;
    std::wstring format;           // e.g. L"JPEG"
    // EXIF alanları — mevcut değilse boş wstring
    std::wstring dateTaken;
    std::wstring cameraMake;
    std::wstring cameraModel;
    std::wstring aperture;         // e.g. L"f/2.8"
    std::wstring shutterSpeed;     // e.g. L"1/500s"
    std::wstring iso;
    // GPS — mevcut değilse boş
    std::wstring gpsLatitude;
    std::wstring gpsLongitude;
    std::wstring gpsAltitude;
    // GPS ondalık derece — OSM linki için; hasGpsDecimal false ise geçersiz
    bool         hasGpsDecimal  = false;
    double       gpsLatDecimal  = 0.0;
    double       gpsLonDecimal  = 0.0;
    // Nominatim konum adı — ör. L"Merzifon, Amasya, Türkiye"
    std::wstring gpsLocationName;
    // ICC renk profili adı — ör. L"Adobe RGB (1998)"; yoksa boş
    std::wstring iccProfileName;
    // Decode hatası — boş değilse ekranda gösterilir
    std::wstring errorMessage;
};

// Görüntünün ekrandaki dönüşüm durumu.
// WndProc fare/klavye olaylarında günceller; Renderer sadece okur.
struct ViewState
{
    float zoomFactor        = 1.0f;   // 1.0 = fit-to-window, >1 = yakın, <1 = uzak
    float panX              = 0.0f;   // Piksel cinsinden yatay kaydırma
    float panY              = 0.0f;   // Piksel cinsinden dikey kaydırma
    bool  showZoomIndicator = false;  // Zoom overlay gösterilsin mi?
    bool  showInfoPanel     = false;  // I tuşuyla toggle — navigasyonda sıfırlanmaz
    float panelAnimWidth    = 0.0f;   // Animasyonlu panel genişliği (0.0–PanelLayout::Width)
    bool  use12HourTime     = false;  // T tuşuyla toggle — 12h/24h saat gösterimi
    int   imageIndex        = 0;      // 1-based; 0 = klasör yok
    int   imageTotal        = 0;      // 0 = klasör yok
};

// Renderer: Direct2D render target yönetimi + WIC görüntü yükleme
// Görev: pencereye GPU üzerinden çizim yapmak
class Renderer
{
public:
    explicit Renderer(HWND hwnd);
    ~Renderer();

    // Renderer kopyalanamaz (COM nesneleri paylaşılamaz)
    Renderer(const Renderer&)            = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Ana çizim fonksiyonu — WM_PAINT'te çağrılır
    void Render(const ViewState& vs, const ImageInfo* info);

    // Pencere boyutlandığında render target'ı güncelle — WM_SIZE'da çağrılır
    void Resize(UINT width, UINT height);

    // Arka plan decode sonucu: ham piksel buffer'ından GPU bitmap oluştur (UI thread'de çağrılır)
    bool LoadImageFromPixels(const uint8_t* pixels, UINT width, UINT height,
                             const std::wstring& path);

    // Mevcut bitmap'i serbest bırak (decode hatası veya gezinme öncesi temizlik)
    void ClearImage();

    // Date toggle rect — DrawInfoPanel her çizimde günceller;
    // WndProc WM_LBUTTONUP'ta tıklama kontrolü için kullanılır.
    D2D1_RECT_F GetDateToggleRect()    const { return m_dateToggleRect; }
    bool        IsDateToggleVisible()  const { return m_dateToggleVisible; }

    // GPS link rect — tıklanabilir koordinat satırı; hasGpsDecimal varsa görünür.
    D2D1_RECT_F GetGpsLinkRect()    const { return m_gpsLinkRect; }
    bool        IsGpsLinkVisible()  const { return m_gpsLinkVisible; }

    // Animasyon — WM_DECODE_DONE'dan sonra çağrılır
    void LoadAnimationFrames(const std::vector<AnimFrame>& frames);
    void ClearAnimation();
    bool IsAnimated()            const { return !m_animBitmaps.empty(); }
    int  AdvanceFrame();               // bir sonraki frame'e geç, yeni frame süresi (ms) döner
    int  GetCurrentFrameDuration() const;

private:
    // GPU cihazına bağlı kaynakları oluştur
    HRESULT CreateDeviceResources();
    // GPU cihazı kaybolunca kaynakları serbest bırak (D2DERR_RECREATE_TARGET)
    void    DiscardDeviceResources();
    // GPU cihazı kurtarma: senkron WIC yükleme — sadece D2DERR_RECREATE_TARGET sonrası çağrılır
    bool    LoadImage(const std::wstring& path);

    // Overlay çizim yardımcıları
    void DrawNavArrows(const ViewState& vs);
    void DrawIndexBar(const ViewState& vs);
    void DrawInfoPanel(const ViewState& vs, const ImageInfo* info);
    void DrawInfoButton(const ViewState& vs);

    HWND                   m_hwnd         = nullptr;
    ID2D1Factory*          m_factory      = nullptr;   // Direct2D fabrikası (cihazdan bağımsız)
    ID2D1HwndRenderTarget* m_renderTarget = nullptr;   // HWND'e bağlı render target (GPU)

    // DirectWrite — cihazdan bağımsız
    IDWriteFactory*        m_dwriteFactory = nullptr;
    IDWriteTextFormat*     m_textFormat    = nullptr;  // 16 DIP Semi-Bold, center — zoom indicator + ok glipleri
    IDWriteTextFormat*     m_labelFormat   = nullptr;  // 13 DIP Regular, left — panel etiketler
    IDWriteTextFormat*     m_valueFormat   = nullptr;  // 13 DIP Semi-Bold, left — panel değerler
    IDWriteTextFormat*     m_indexFormat   = nullptr;  // 14 DIP Semi-Bold, center — index bar
    IDWriteTextFormat*     m_toggleFormat  = nullptr;  // 10 DIP Regular,    center — time toggle pill

    // Fırçalar (cihaza bağlı, render target ile birlikte oluşturulur/yok edilir)
    ID2D1SolidColorBrush*  m_whiteBrush       = nullptr;   // Metin için
    ID2D1SolidColorBrush*  m_overlayBrush     = nullptr;   // Yarı saydam siyah arka plan
    ID2D1SolidColorBrush*  m_activeBrush      = nullptr;   // Info button aktif durumu
    ID2D1SolidColorBrush*  m_toggleFillBrush  = nullptr;   // Segmented toggle aktif segment dolgusu

    ID2D1Bitmap*           m_bitmap       = nullptr;   // GPU'ya yüklenmiş görüntü
    std::wstring           m_imagePath;                // D2DERR_RECREATE_TARGET'ta yeniden yüklemek için

    // Date toggle badge — DrawInfoPanel tarafından doldurulur
    D2D1_RECT_F            m_dateToggleRect    = {};
    bool                   m_dateToggleVisible = false;

    // GPS link rect — DrawInfoPanel tarafından doldurulur
    D2D1_RECT_F            m_gpsLinkRect    = {};
    bool                   m_gpsLinkVisible = false;

    // Animasyon bitmap dizisi (cihaza bağlı) + frame süresi
    std::vector<ID2D1Bitmap*> m_animBitmaps;
    std::vector<int>          m_animDurations;  // ms, m_animBitmaps ile 1:1
    int                       m_animFrameIdx = 0;
};
