#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <string>
#include <cstdint>

// Direct2D, DirectWrite ve WIC kütüphanelerini otomatik linkle
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")

// Görüntünün ekrandaki dönüşüm durumu.
// WndProc fare/klavye olaylarında günceller; Renderer sadece okur.
struct ViewState
{
    float zoomFactor        = 1.0f;   // 1.0 = fit-to-window, >1 = yakın, <1 = uzak
    float panX              = 0.0f;   // Piksel cinsinden yatay kaydırma
    float panY              = 0.0f;   // Piksel cinsinden dikey kaydırma
    bool  showZoomIndicator = false;  // Zoom overlay gösterilsin mi?
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
    void Render(const ViewState& vs);

    // Pencere boyutlandığında render target'ı güncelle — WM_SIZE'da çağrılır
    void Resize(UINT width, UINT height);

    // Arka plan decode sonucu: ham piksel buffer'ından GPU bitmap oluştur (UI thread'de çağrılır)
    bool LoadImageFromPixels(const uint8_t* pixels, UINT width, UINT height,
                             const std::wstring& path);

private:
    // GPU cihazına bağlı kaynakları oluştur
    HRESULT CreateDeviceResources();
    // GPU cihazı kaybolunca kaynakları serbest bırak (D2DERR_RECREATE_TARGET)
    void    DiscardDeviceResources();
    // GPU cihazı kurtarma: senkron WIC yükleme — sadece D2DERR_RECREATE_TARGET sonrası çağrılır
    bool    LoadImage(const std::wstring& path);

    HWND                   m_hwnd         = nullptr;
    ID2D1Factory*          m_factory      = nullptr;   // Direct2D fabrikası (cihazdan bağımsız)
    ID2D1HwndRenderTarget* m_renderTarget = nullptr;   // HWND'e bağlı render target (GPU)

    // Zoom indicator — DirectWrite (cihazdan bağımsız)
    IDWriteFactory*        m_dwriteFactory = nullptr;
    IDWriteTextFormat*     m_textFormat    = nullptr;

    // Zoom indicator — fırçalar (cihaza bağlı, render target ile birlikte oluşturulur/yok edilir)
    ID2D1SolidColorBrush*  m_whiteBrush   = nullptr;   // "%150" metni için
    ID2D1SolidColorBrush*  m_overlayBrush = nullptr;   // Yarı saydam siyah arka plan

    IWICImagingFactory*    m_wicFactory   = nullptr;   // WIC fabrikası (GPU kurtarma için)
    ID2D1Bitmap*           m_bitmap       = nullptr;   // GPU'ya yüklenmiş görüntü
    std::wstring           m_imagePath;                // D2DERR_RECREATE_TARGET'ta yeniden yüklemek için
};
