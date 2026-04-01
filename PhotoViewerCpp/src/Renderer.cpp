#include "Renderer.h"
#include <cwchar>

Renderer::Renderer(HWND hwnd) : m_hwnd(hwnd)
{
    // D2D1Factory: Direct2D'nin giriş noktası.
    // SINGLE_THREADED: render işlemleri tek iş parçacığından yapılacak (UI thread).
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &m_factory);

    // DirectWrite fabrikası: cihazdan bağımsız, uygulama boyunca tek örnek.
    // SHARED: birden fazla bileşen aynı fabrikayı paylaşabilir.
    DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(&m_dwriteFactory)
    );

    if (m_dwriteFactory)
    {
        // Zoom indicator metin formatı: Segoe UI Semi-Bold, 16 DIP (~12pt @ 96 DPI)
        m_dwriteFactory->CreateTextFormat(
            L"Segoe UI",
            nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            16.0f,
            L"",
            &m_textFormat
        );

        if (m_textFormat)
        {
            // Metin dikdörtgen içinde yatay ve dikey ortala
            m_textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            m_textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }
}

Renderer::~Renderer()
{
    DiscardDeviceResources();

    if (m_textFormat)    { m_textFormat->Release();    m_textFormat = nullptr; }
    if (m_dwriteFactory) { m_dwriteFactory->Release(); m_dwriteFactory = nullptr; }
    if (m_wicFactory)    { m_wicFactory->Release();    m_wicFactory = nullptr; }
    if (m_factory)       { m_factory->Release();       m_factory = nullptr; }
}

HRESULT Renderer::CreateDeviceResources()
{
    // Render target zaten varsa tekrar oluşturma
    if (m_renderTarget) return S_OK;

    RECT rc;
    GetClientRect(m_hwnd, &rc);
    D2D1_SIZE_U size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);

    // HwndRenderTarget: doğrudan Win32 HWND üzerine GPU ile çizer.
    HRESULT hr = m_factory->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(m_hwnd, size),
        &m_renderTarget
    );
    if (FAILED(hr)) return hr;

    // Zoom indicator fırçaları: render target'a bağlı GPU kaynakları
    m_renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f),    // Opak beyaz — metin
        &m_whiteBrush
    );
    m_renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.65f),   // %65 saydam siyah — arka plan
        &m_overlayBrush
    );

    return S_OK;
}

void Renderer::DiscardDeviceResources()
{
    // ID2D1Bitmap ve fırçalar render target'a bağlı GPU kaynaklarıdır;
    // render target yok edilince hepsi geçersiz olur.
    if (m_bitmap)       { m_bitmap->Release();       m_bitmap = nullptr; }
    if (m_whiteBrush)   { m_whiteBrush->Release();   m_whiteBrush = nullptr; }
    if (m_overlayBrush) { m_overlayBrush->Release(); m_overlayBrush = nullptr; }
    if (m_renderTarget) { m_renderTarget->Release(); m_renderTarget = nullptr; }
}

bool Renderer::LoadImageFromPixels(const uint8_t* pixels, UINT width, UINT height,
                                   const std::wstring& path)
{
    if (m_bitmap) { m_bitmap->Release(); m_bitmap = nullptr; }
    if (FAILED(CreateDeviceResources())) return false;

    // 32bppPBGRA: Direct2D'nin beklediği pre-multiplied BGRA formatı.
    // Arka plan thread'inde WIC converter aynı formatı üretir.
    D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
    );

    HRESULT hr = m_renderTarget->CreateBitmap(
        D2D1::SizeU(width, height),
        pixels,
        width * 4,   // stride: her satır = genişlik × 4 byte (BGRA)
        props,
        &m_bitmap
    );

    if (SUCCEEDED(hr))
    {
        m_imagePath = path;  // D2DERR_RECREATE_TARGET'ta yeniden yüklemek için sakla
        return true;
    }
    return false;
}

bool Renderer::LoadImage(const std::wstring& path)
{
    // Bu yol yalnızca GPU cihazı kayıp kurtarması için çağrılır (D2DERR_RECREATE_TARGET).
    // Normal yükleme arka plan thread'inde yapılır → LoadImageFromPixels ile tamamlanır.
    if (m_bitmap) { m_bitmap->Release(); m_bitmap = nullptr; }

    if (!m_wicFactory)
    {
        HRESULT hr = CoCreateInstance(
            CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&m_wicFactory)
        );
        if (FAILED(hr)) return false;
    }

    IWICBitmapDecoder* decoder = nullptr;
    HRESULT hr = m_wicFactory->CreateDecoderFromFilename(
        path.c_str(), nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnLoad, &decoder
    );
    if (FAILED(hr)) return false;

    IWICBitmapFrameDecode* frame = nullptr;
    hr = decoder->GetFrame(0, &frame);
    decoder->Release();
    if (FAILED(hr)) return false;

    IWICFormatConverter* converter = nullptr;
    hr = m_wicFactory->CreateFormatConverter(&converter);
    if (FAILED(hr)) { frame->Release(); return false; }

    hr = converter->Initialize(
        frame, GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone, nullptr, 0.0f, WICBitmapPaletteTypeMedianCut
    );
    frame->Release();
    if (FAILED(hr)) { converter->Release(); return false; }

    if (FAILED(CreateDeviceResources())) { converter->Release(); return false; }

    hr = m_renderTarget->CreateBitmapFromWicBitmap(converter, nullptr, &m_bitmap);
    converter->Release();

    if (SUCCEEDED(hr)) { m_imagePath = path; return true; }
    return false;
}

void Renderer::Render(const ViewState& vs)
{
    if (FAILED(CreateDeviceResources())) return;

    // D2DERR_RECREATE_TARGET sonrası render target yeniden oluşturuldu ama bitmap gitti —
    // kaydedilen yoldan senkron olarak yeniden yükle (nadir durum: GPU sürücüsü sıfırlandı)
    if (!m_bitmap && !m_imagePath.empty())
        LoadImage(m_imagePath);

    m_renderTarget->BeginDraw();

    // Arka plan: koyu gri (#1E1E1E)
    m_renderTarget->Clear(D2D1::ColorF(0.118f, 0.118f, 0.118f));

    if (m_bitmap)
    {
        D2D1_SIZE_F wndSize = m_renderTarget->GetSize();
        D2D1_SIZE_F imgSize = m_bitmap->GetSize();

        // Aspect ratio korumalı "fit" ölçeği (zoomFactor=1 → tam ekrana sığdır)
        float fitScale   = min(wndSize.width / imgSize.width, wndSize.height / imgSize.height);
        float finalScale = fitScale * vs.zoomFactor;

        float destW = imgSize.width  * finalScale;
        float destH = imgSize.height * finalScale;

        // Fit konumu (pencere ortası) + pan ofseti
        float destX = (wndSize.width  - destW) * 0.5f + vs.panX;
        float destY = (wndSize.height - destH) * 0.5f + vs.panY;

        m_renderTarget->DrawBitmap(
            m_bitmap,
            D2D1::RectF(destX, destY, destX + destW, destY + destH)
        );

        // Zoom indicator: sağ alt köşede yarı saydam yuvarlak dikdörtgen + "%150" metni
        if (vs.showZoomIndicator && m_textFormat && m_whiteBrush && m_overlayBrush)
        {
            wchar_t text[16];
            swprintf_s(text, L"%.0f%%", vs.zoomFactor * 100.0f);

            constexpr float kW      = 80.0f;
            constexpr float kH      = 34.0f;
            constexpr float kMargin = 12.0f;

            D2D1_RECT_F bgRect = D2D1::RectF(
                wndSize.width  - kW - kMargin,
                wndSize.height - kH - kMargin,
                wndSize.width  - kMargin,
                wndSize.height - kMargin
            );

            // Yuvarlak kenar — köşe yarıçapı 6 DIP
            D2D1_ROUNDED_RECT rr = { bgRect, 6.0f, 6.0f };
            m_renderTarget->FillRoundedRectangle(rr, m_overlayBrush);

            m_renderTarget->DrawText(
                text, static_cast<UINT32>(wcslen(text)),
                m_textFormat, bgRect, m_whiteBrush
            );
        }
    }

    // Phase 5: EXIF paneli (DirectWrite metin) buraya eklenecek

    HRESULT hr = m_renderTarget->EndDraw();

    if (hr == D2DERR_RECREATE_TARGET)
    {
        // GPU cihazı kayboldu. Render target ve bitmap'i serbest bırak.
        // Bir sonraki Render() çağrısında m_imagePath'ten yeniden oluşturulur.
        DiscardDeviceResources();
    }
}

void Renderer::Resize(UINT width, UINT height)
{
    if (m_renderTarget)
    {
        // Render target'ı yeni pencere boyutuna göre güncelle.
        // ID2D1Bitmap'ler geçersiz olmaz, sadece viewport boyutu değişir.
        m_renderTarget->Resize(D2D1::SizeU(width, height));
    }
}
