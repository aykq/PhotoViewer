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
        // Zoom indicator + ok glipleri: Segoe UI Semi-Bold, 16 DIP, ortalı
        m_dwriteFactory->CreateTextFormat(
            L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            16.0f, L"", &m_textFormat
        );
        if (m_textFormat)
        {
            m_textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            m_textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }

        // Panel etiket: 13 DIP Regular, sola dayalı
        m_dwriteFactory->CreateTextFormat(
            L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            13.0f, L"", &m_labelFormat
        );
        if (m_labelFormat)
        {
            m_labelFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            m_labelFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }

        // Panel değer: 13 DIP Semi-Bold, sola dayalı
        m_dwriteFactory->CreateTextFormat(
            L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            13.0f, L"", &m_valueFormat
        );
        if (m_valueFormat)
        {
            m_valueFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            m_valueFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }

        // Index bar: 14 DIP Semi-Bold, ortalı
        m_dwriteFactory->CreateTextFormat(
            L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            14.0f, L"", &m_indexFormat
        );
        if (m_indexFormat)
        {
            m_indexFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            m_indexFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }
}

Renderer::~Renderer()
{
    DiscardDeviceResources();

    if (m_indexFormat)   { m_indexFormat->Release();   m_indexFormat = nullptr; }
    if (m_valueFormat)   { m_valueFormat->Release();   m_valueFormat = nullptr; }
    if (m_labelFormat)   { m_labelFormat->Release();   m_labelFormat = nullptr; }
    if (m_textFormat)    { m_textFormat->Release();    m_textFormat = nullptr; }
    if (m_dwriteFactory) { m_dwriteFactory->Release(); m_dwriteFactory = nullptr; }
    if (m_wicFactory)    { m_wicFactory->Release();    m_wicFactory = nullptr; }
    if (m_factory)       { m_factory->Release();       m_factory = nullptr; }
}

HRESULT Renderer::CreateDeviceResources()
{
    if (m_renderTarget) return S_OK;

    RECT rc;
    GetClientRect(m_hwnd, &rc);
    D2D1_SIZE_U size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);

    HRESULT hr = m_factory->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(m_hwnd, size),
        &m_renderTarget
    );
    if (FAILED(hr)) return hr;

    m_renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &m_whiteBrush
    );
    m_renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.65f), &m_overlayBrush
    );

    return S_OK;
}

void Renderer::DiscardDeviceResources()
{
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

    D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
    );

    HRESULT hr = m_renderTarget->CreateBitmap(
        D2D1::SizeU(width, height),
        pixels,
        width * 4,
        props,
        &m_bitmap
    );

    if (SUCCEEDED(hr))
    {
        m_imagePath = path;
        return true;
    }
    return false;
}

bool Renderer::LoadImage(const std::wstring& path)
{
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

// ─── Navigation Arrows ────────────────────────────────────────────────────────

void Renderer::DrawNavArrows(const ViewState& vs)
{
    if (vs.imageTotal <= 0) return;
    if (!m_overlayBrush || !m_whiteBrush || !m_textFormat) return;

    D2D1_SIZE_F sz = m_renderTarget->GetSize();
    float midY  = sz.height * 0.5f;
    float halfH = ArrowLayout::PillH * 0.5f;

    // Sol ok pill — sol köşeler pencere dışında kalır; yalnızca sağ köşeler yuvarlak görünür
    D2D1_ROUNDED_RECT leftRR = {
        D2D1::RectF(-ArrowLayout::Radius, midY - halfH,
                     ArrowLayout::ZoneW,  midY + halfH),
        ArrowLayout::Radius, ArrowLayout::Radius
    };
    m_renderTarget->FillRoundedRectangle(leftRR, m_overlayBrush);

    D2D1_RECT_F leftText = D2D1::RectF(0, midY - halfH, ArrowLayout::ZoneW, midY + halfH);
    m_renderTarget->DrawText(L"\u2039", 1, m_textFormat, leftText, m_whiteBrush);

    // Sağ ok pill — sağ köşeler pencere dışında kalır; yalnızca sol köşeler yuvarlak görünür
    D2D1_ROUNDED_RECT rightRR = {
        D2D1::RectF(sz.width - ArrowLayout::ZoneW, midY - halfH,
                    sz.width + ArrowLayout::Radius, midY + halfH),
        ArrowLayout::Radius, ArrowLayout::Radius
    };
    m_renderTarget->FillRoundedRectangle(rightRR, m_overlayBrush);

    D2D1_RECT_F rightText = D2D1::RectF(
        sz.width - ArrowLayout::ZoneW, midY - halfH, sz.width, midY + halfH);
    m_renderTarget->DrawText(L"\u203A", 1, m_textFormat, rightText, m_whiteBrush);
}

// ─── Index Bar ────────────────────────────────────────────────────────────────

void Renderer::DrawIndexBar(const ViewState& vs)
{
    if (vs.imageTotal <= 0) return;
    if (!m_indexFormat || !m_overlayBrush || !m_whiteBrush) return;

    wchar_t text[32];
    swprintf_s(text, L"%d / %d", vs.imageIndex, vs.imageTotal);

    D2D1_SIZE_F sz = m_renderTarget->GetSize();
    constexpr float kW      = 100.0f;
    constexpr float kH      = 34.0f;
    constexpr float kMargin = 12.0f;

    D2D1_RECT_F bgRect = D2D1::RectF(
        (sz.width - kW) * 0.5f,   sz.height - kH - kMargin,
        (sz.width + kW) * 0.5f,   sz.height - kMargin
    );

    D2D1_ROUNDED_RECT rr = { bgRect, 6.0f, 6.0f };
    m_renderTarget->FillRoundedRectangle(rr, m_overlayBrush);
    m_renderTarget->DrawText(
        text, static_cast<UINT32>(wcslen(text)), m_indexFormat, bgRect, m_whiteBrush
    );
}

// ─── Info Panel ───────────────────────────────────────────────────────────────

void Renderer::DrawInfoPanel(const ViewState& vs, const ImageInfo* info)
{
    if (!m_overlayBrush || !m_whiteBrush) return;

    D2D1_SIZE_F sz = m_renderTarget->GetSize();
    constexpr float kPanelW = 280.0f;
    constexpr float kPadX   = 16.0f;

    // Panel arka planı — sağ kenara yapışık, tam yükseklik
    D2D1_RECT_F bg = D2D1::RectF(sz.width - kPanelW, 0.0f, sz.width, sz.height);
    m_renderTarget->FillRectangle(bg, m_overlayBrush);

    if (!info || !m_labelFormat || !m_valueFormat) return;

    float x0 = sz.width - kPanelW + kPadX;
    float x1 = sz.width - kPadX;
    float y  = 24.0f;

    constexpr float kLabelH = 17.0f;
    constexpr float kGap    = 4.0f;
    constexpr float kValueH = 18.0f;
    constexpr float kRowH   = kLabelH + kGap + kValueH + 8.0f;  // ~47px per row

    // Etiket + değer satırı çizen lambda; boş değerler atlanır
    auto DrawRow = [&](const wchar_t* label, const std::wstring& value)
    {
        if (value.empty()) return;
        m_renderTarget->DrawText(
            label, static_cast<UINT32>(wcslen(label)),
            m_labelFormat, D2D1::RectF(x0, y, x1, y + kLabelH),
            m_whiteBrush
        );
        m_renderTarget->DrawText(
            value.c_str(), static_cast<UINT32>(value.size()),
            m_valueFormat, D2D1::RectF(x0, y + kLabelH + kGap, x1, y + kLabelH + kGap + kValueH),
            m_whiteBrush
        );
        y += kRowH;
    };

    // Dosya adı — büyük başlık (valueFormat, tek satır)
    if (!info->filename.empty())
    {
        m_renderTarget->DrawText(
            info->filename.c_str(), static_cast<UINT32>(info->filename.size()),
            m_valueFormat, D2D1::RectF(x0, y, x1, y + kValueH),
            m_whiteBrush
        );
        y += kValueH + 14.0f;
    }

    // İnce ayraç
    m_renderTarget->DrawLine(
        D2D1::Point2F(x0, y), D2D1::Point2F(x1, y), m_whiteBrush, 0.5f
    );
    y += 10.0f;

    // Temel bilgiler
    if (info->width > 0 && info->height > 0)
    {
        wchar_t dimBuf[32];
        swprintf_s(dimBuf, L"%d \u00d7 %d px", info->width, info->height);
        DrawRow(L"Çözünürlük", dimBuf);
    }

    if (info->fileSizeBytes > 0)
    {
        wchar_t szBuf[32];
        if (info->fileSizeBytes >= 1024LL * 1024)
            swprintf_s(szBuf, L"%.1f MB", info->fileSizeBytes / (1024.0 * 1024.0));
        else
            swprintf_s(szBuf, L"%lld KB", info->fileSizeBytes / 1024);
        DrawRow(L"Dosya Boyutu", szBuf);
    }

    DrawRow(L"Format", info->format);

    // EXIF bölümü — sadece en az bir alan doluysa
    bool hasExif = !info->dateTaken.empty()  || !info->cameraMake.empty()  ||
                   !info->cameraModel.empty() || !info->aperture.empty()    ||
                   !info->shutterSpeed.empty()|| !info->iso.empty();

    if (hasExif)
    {
        y += 4.0f;
        m_renderTarget->DrawLine(
            D2D1::Point2F(x0, y), D2D1::Point2F(x1, y), m_whiteBrush, 0.5f
        );
        y += 10.0f;

        DrawRow(L"Tarih", info->dateTaken);

        // Kamera marka + model tek satırda
        if (!info->cameraMake.empty() || !info->cameraModel.empty())
        {
            std::wstring cam = info->cameraMake;
            if (!info->cameraModel.empty())
            {
                if (!cam.empty()) cam += L" ";
                cam += info->cameraModel;
            }
            DrawRow(L"Kamera", cam);
        }

        DrawRow(L"Diyafram", info->aperture);
        DrawRow(L"Enstantane", info->shutterSpeed);
        DrawRow(L"ISO", info->iso);
    }
}

// ─── Ana Render ───────────────────────────────────────────────────────────────

void Renderer::Render(const ViewState& vs, const ImageInfo* info)
{
    if (FAILED(CreateDeviceResources())) return;

    // D2DERR_RECREATE_TARGET sonrası bitmap kaybolmuşsa yeniden yükle
    if (!m_bitmap && !m_imagePath.empty())
        LoadImage(m_imagePath);

    m_renderTarget->BeginDraw();

    // Arka plan: koyu gri (#1E1E1E)
    m_renderTarget->Clear(D2D1::ColorF(0.118f, 0.118f, 0.118f));

    if (m_bitmap)
    {
        D2D1_SIZE_F wndSize = m_renderTarget->GetSize();
        D2D1_SIZE_F imgSize = m_bitmap->GetSize();

        float fitScale   = min(wndSize.width / imgSize.width, wndSize.height / imgSize.height);
        float finalScale = fitScale * vs.zoomFactor;

        float destW = imgSize.width  * finalScale;
        float destH = imgSize.height * finalScale;
        float destX = (wndSize.width  - destW) * 0.5f + vs.panX;
        float destY = (wndSize.height - destH) * 0.5f + vs.panY;

        m_renderTarget->DrawBitmap(
            m_bitmap,
            D2D1::RectF(destX, destY, destX + destW, destY + destH)
        );
    }

    // Overlaylar (arka plandan öne doğru)
    DrawNavArrows(vs);
    DrawIndexBar(vs);
    if (vs.showInfoPanel) DrawInfoPanel(vs, info);

    // Zoom indicator: sağ alt köşe
    if (vs.showZoomIndicator && m_textFormat && m_whiteBrush && m_overlayBrush)
    {
        D2D1_SIZE_F wndSize = m_renderTarget->GetSize();
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

        D2D1_ROUNDED_RECT rr = { bgRect, 6.0f, 6.0f };
        m_renderTarget->FillRoundedRectangle(rr, m_overlayBrush);
        m_renderTarget->DrawText(
            text, static_cast<UINT32>(wcslen(text)), m_textFormat, bgRect, m_whiteBrush
        );
    }

    HRESULT hr = m_renderTarget->EndDraw();

    if (hr == D2DERR_RECREATE_TARGET)
        DiscardDeviceResources();
}

void Renderer::Resize(UINT width, UINT height)
{
    if (m_renderTarget)
        m_renderTarget->Resize(D2D1::SizeU(width, height));
}
