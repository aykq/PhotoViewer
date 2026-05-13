#include "Renderer.h"
#include "ImageDecoder.h"
#include <cwchar>
#include <algorithm>

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

        // Time toggle pill: 10 DIP Regular, ortalı
        m_dwriteFactory->CreateTextFormat(
            L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            10.0f, L"", &m_toggleFormat
        );
        if (m_toggleFormat)
        {
            m_toggleFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            m_toggleFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }

        // Save bar butonları: 13 DIP Semi-Bold, ortalı
        m_dwriteFactory->CreateTextFormat(
            L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            13.0f, L"", &m_btnFormat
        );
        if (m_btnFormat)
        {
            m_btnFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            m_btnFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }
}

Renderer::~Renderer()
{
    DiscardDeviceResources();

    if (m_btnFormat)     { m_btnFormat->Release();     m_btnFormat = nullptr; }
    if (m_toggleFormat)  { m_toggleFormat->Release();  m_toggleFormat = nullptr; }
    if (m_indexFormat)   { m_indexFormat->Release();   m_indexFormat = nullptr; }
    if (m_valueFormat)   { m_valueFormat->Release();   m_valueFormat = nullptr; }
    if (m_labelFormat)   { m_labelFormat->Release();   m_labelFormat = nullptr; }
    if (m_textFormat)    { m_textFormat->Release();    m_textFormat = nullptr; }
    if (m_dwriteFactory) { m_dwriteFactory->Release(); m_dwriteFactory = nullptr; }
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
    m_renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.22f), &m_activeBrush
    );
    m_renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.38f), &m_toggleFillBrush
    );
    m_renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(0.667f, 0.667f, 0.667f, 1.0f), &m_grayBrush      // #AAAAAA
    );
    m_renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(0.102f, 0.102f, 0.102f, 1.0f), &m_panelBgBrush   // #1A1A1A
    );
    m_renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(0.200f, 0.200f, 0.200f, 1.0f), &m_separatorBrush // #333333
    );
    m_renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(0.298f, 0.686f, 0.314f, 1.0f), &m_saveBtnBrush   // #4CAF50 — Kaydet
    );
    m_renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(0.776f, 0.157f, 0.157f, 1.0f), &m_deleteBrush    // #C62828 — Sil tehlike
    );
    if (!m_dialogLayer)
        m_renderTarget->CreateLayer(nullptr, &m_dialogLayer);
    return S_OK;
}

void Renderer::DiscardDeviceResources()
{
    for (auto* bmp : m_animBitmaps) if (bmp) bmp->Release();
    m_animBitmaps.clear();
    m_animDurations.clear();
    m_animFrameIdx = 0;

    // Thumbnail cache'ini serbest bırak (cihaza bağlı bitmap'ler)
    for (auto& [path, bmp] : m_thumbCache) if (bmp) bmp->Release();
    m_thumbCache.clear();
    m_thumbOrder.clear();

    // OSM tile cache'ini serbest bırak
    for (auto& [key, bmp] : m_mapTileCache) if (bmp) bmp->Release();
    m_mapTileCache.clear();

    if (m_dialogLayer)      { m_dialogLayer->Release();      m_dialogLayer = nullptr; }
    if (m_bitmap)           { m_bitmap->Release();           m_bitmap = nullptr; }
    if (m_deleteBrush)      { m_deleteBrush->Release();      m_deleteBrush = nullptr; }
    if (m_saveBtnBrush)     { m_saveBtnBrush->Release();     m_saveBtnBrush = nullptr; }
    if (m_separatorBrush)   { m_separatorBrush->Release();   m_separatorBrush = nullptr; }
    if (m_panelBgBrush)     { m_panelBgBrush->Release();     m_panelBgBrush = nullptr; }
    if (m_grayBrush)        { m_grayBrush->Release();        m_grayBrush = nullptr; }
    if (m_toggleFillBrush)  { m_toggleFillBrush->Release();  m_toggleFillBrush = nullptr; }
    if (m_activeBrush)      { m_activeBrush->Release();      m_activeBrush = nullptr; }
    if (m_whiteBrush)       { m_whiteBrush->Release();       m_whiteBrush = nullptr; }
    if (m_overlayBrush)     { m_overlayBrush->Release();     m_overlayBrush = nullptr; }
    if (m_renderTarget)     { m_renderTarget->Release();     m_renderTarget = nullptr; }
}


void Renderer::LoadAnimationFrames(const std::vector<AnimFrame>& frames)
{
    ClearAnimation();
    if (frames.empty()) return;
    if (FAILED(CreateDeviceResources()) || !m_renderTarget) return;

    D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
    );

    for (const auto& f : frames)
    {
        ID2D1Bitmap* bmp = nullptr;
        HRESULT hr = m_renderTarget->CreateBitmap(
            D2D1::SizeU(f.width, f.height),
            f.pixels.data(), f.width * 4, props, &bmp);
        if (SUCCEEDED(hr) && bmp)
        {
            m_animBitmaps.push_back(bmp);
            m_animDurations.push_back(f.durationMs);
        }
    }

    if (m_animBitmaps.empty()) return;
    m_animFrameIdx = 0;
}

void Renderer::ClearAnimation()
{
    for (auto* bmp : m_animBitmaps) if (bmp) bmp->Release();
    m_animBitmaps.clear();
    m_animDurations.clear();
    m_animFrameIdx = 0;
}


int Renderer::AdvanceFrame()
{
    if (m_animBitmaps.empty()) return 100;
    m_animFrameIdx = (m_animFrameIdx + 1) % static_cast<int>(m_animBitmaps.size());
    return m_animDurations[m_animFrameIdx];
}

int Renderer::GetCurrentFrameDuration() const
{
    if (m_animDurations.empty()) return 100;
    return m_animDurations[m_animFrameIdx];
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

    DecodeOutput decoded;
    if (!DecodeImage(path, decoded) || decoded.pixels.empty())
        return false;

    return LoadImageFromPixels(decoded.pixels.data(), decoded.width, decoded.height, path);
}

void Renderer::ClearImage()
{
    if (m_bitmap) { m_bitmap->Release(); m_bitmap = nullptr; }
    m_imagePath.clear();
}

// ─── Navigation Arrows ────────────────────────────────────────────────────────

void Renderer::DrawNavArrows(const ViewState& vs)
{
    if (vs.imageTotal <= 0) return;
    if (!m_overlayBrush || !m_whiteBrush || !m_textFormat) return;

    D2D1_SIZE_F sz = m_renderTarget->GetSize();
    float availW = sz.width - vs.panelAnimWidth;
    float midY   = sz.height * 0.5f;

    // Floating rounded square oklar — 40×40px, 10px köşe, kenardan 8px boşluk
    constexpr float kSqSize   = 40.0f;
    constexpr float kSqRadius = 10.0f;
    constexpr float kSqMargin = 8.0f;

    // Sol ok (basılıysa açık tint, değilse koyu)
    D2D1_ROUNDED_RECT leftRR = {
        D2D1::RectF(kSqMargin,            midY - kSqSize * 0.5f,
                    kSqMargin + kSqSize,   midY + kSqSize * 0.5f),
        kSqRadius, kSqRadius
    };
    m_renderTarget->FillRoundedRectangle(leftRR, vs.leftArrowPressed  ? m_activeBrush : m_panelBgBrush);
    m_renderTarget->DrawText(L"\u2039", 1, m_textFormat, leftRR.rect, m_whiteBrush);

    // Sağ ok (basılıysa açık tint, değilse koyu)
    D2D1_ROUNDED_RECT rightRR = {
        D2D1::RectF(availW - kSqMargin - kSqSize, midY - kSqSize * 0.5f,
                    availW - kSqMargin,            midY + kSqSize * 0.5f),
        kSqRadius, kSqRadius
    };
    m_renderTarget->FillRoundedRectangle(rightRR, vs.rightArrowPressed ? m_activeBrush : m_panelBgBrush);
    m_renderTarget->DrawText(L"\u203A", 1, m_textFormat, rightRR.rect, m_whiteBrush);
}

// ─── Index Bar ────────────────────────────────────────────────────────────────

void Renderer::DrawIndexBar(const ViewState& vs)
{
    if (vs.imageTotal <= 0) return;
    if (vs.editDirty) return;       // save bar açıkken index bar gizlenir
    if (vs.indexBarAlpha <= 0.01f) return;
    if (!m_indexFormat || !m_overlayBrush || !m_whiteBrush) return;

    wchar_t text[32];
    swprintf_s(text, L"%d / %d", vs.imageIndex, vs.imageTotal);

    D2D1_SIZE_F sz = m_renderTarget->GetSize();
    constexpr float kW      = 100.0f;
    constexpr float kH      = 34.0f;
    constexpr float kMargin = 12.0f;

    float availW      = sz.width - vs.panelAnimWidth;
    float bottomOff   = vs.stripAnimHeight + kMargin;  // strip açıksa yukarı kayar
    D2D1_RECT_F bgRect = D2D1::RectF(
        (availW - kW) * 0.5f,   sz.height - kH - bottomOff,
        (availW + kW) * 0.5f,   sz.height - bottomOff
    );

    // Alpha uygulamak için brush opaklıklarını geçici olarak ayarla
    float savedOverlayOp = m_overlayBrush->GetOpacity();
    float savedWhiteOp   = m_whiteBrush->GetOpacity();
    m_overlayBrush->SetOpacity(savedOverlayOp * vs.indexBarAlpha);
    m_whiteBrush->SetOpacity(savedWhiteOp   * vs.indexBarAlpha);

    D2D1_ROUNDED_RECT rr = { bgRect, 6.0f, 6.0f };
    m_renderTarget->FillRoundedRectangle(rr, m_overlayBrush);
    m_renderTarget->DrawText(
        text, static_cast<UINT32>(wcslen(text)), m_indexFormat, bgRect, m_whiteBrush
    );

    m_overlayBrush->SetOpacity(savedOverlayOp);
    m_whiteBrush->SetOpacity(savedWhiteOp);
}

// ─── Tarih biçimlendirme yardımcısı ──────────────────────────────────────────

// EXIF ham string: "YYYY:MM:DD HH:MM:SS"
// 24h: "15-07-2023  14:30"   12h: "15-07-2023  2:30 PM"
static std::wstring FormatDateTaken(const std::wstring& raw, bool use12h)
{
    int yr = 0, mo = 0, dy = 0, hr = 0, mi = 0, se = 0;
    if (swscanf_s(raw.c_str(), L"%d:%d:%d %d:%d:%d",
                  &yr, &mo, &dy, &hr, &mi, &se) == 6)
    {
        wchar_t buf[48];
        if (use12h)
        {
            const wchar_t* ampm = (hr >= 12) ? L"PM" : L"AM";
            int hr12 = hr % 12;
            if (hr12 == 0) hr12 = 12;
            swprintf_s(buf, L"%02d-%02d-%04d  %d:%02d %ls", dy, mo, yr, hr12, mi, ampm);
        }
        else
        {
            swprintf_s(buf, L"%02d-%02d-%04d  %02d:%02d", dy, mo, yr, hr, mi);
        }
        return buf;
    }
    return raw;  // ayrıştırılamadıysa ham veriyi göster
}

// ─── Info Panel ───────────────────────────────────────────────────────────────

void Renderer::DrawInfoPanel(const ViewState& vs, const ImageInfo* info)
{
    m_dateToggleVisible  = false;
    m_gpsLinkVisible     = false;
    m_mapPreviewVisible  = false;
    m_mapCopyBtnVisible  = false;
    if (!m_panelBgBrush || !m_whiteBrush) return;

    D2D1_SIZE_F sz = m_renderTarget->GetSize();

    // Panel arka planı — opak koyu (#1A1A1A), sağ kenara yapışık
    D2D1_RECT_F bg = D2D1::RectF(sz.width - vs.panelAnimWidth, 0.0f, sz.width, sz.height);
    m_renderTarget->FillRectangle(bg, m_panelBgBrush);

    // Sol kenar çizgisi (#333333, 1px)
    m_renderTarget->DrawLine(
        D2D1::Point2F(sz.width - vs.panelAnimWidth, 0.0f),
        D2D1::Point2F(sz.width - vs.panelAnimWidth, sz.height),
        m_separatorBrush, 1.0f
    );

    // Animasyon sırasında metin taşmasını önlemek için clip bölgesi
    m_renderTarget->PushAxisAlignedClip(bg, D2D1_ANTIALIAS_MODE_ALIASED);

    if (!info || !m_labelFormat || !m_valueFormat)
    {
        m_renderTarget->PopAxisAlignedClip();
        return;
    }

    // Metin koordinatları her zaman tam panel genişliğine sabitlenir.
    // Clip bölgesi (bg) animasyon sırasında fazlayı keser; böylece
    // panel daralırken metin yeniden kırılmaz.
    float x0 = sz.width - PanelLayout::Width + PanelLayout::PadX;
    float x1 = sz.width - PanelLayout::PadX;

    // ── "Image Details" başlık bölümü (sabit — scrolllanmaz) ────────────────
    constexpr float kHeaderH   = PanelLayout::HeaderH;
    constexpr float kHeaderTxt = 13.0f;  // m_valueFormat font boyutu
    float headerTxtY = (kHeaderH - kHeaderTxt) * 0.5f;
    m_renderTarget->DrawText(
        L"Image Details",
        static_cast<UINT32>(wcslen(L"Image Details")),
        m_valueFormat,
        D2D1::RectF(x0, headerTxtY, x1, headerTxtY + kHeaderTxt + 4.0f),
        m_whiteBrush
    );
    // Başlık altı ayraç
    m_renderTarget->DrawLine(
        D2D1::Point2F(sz.width - PanelLayout::Width, kHeaderH),
        D2D1::Point2F(sz.width, kHeaderH),
        m_separatorBrush, 1.0f
    );

    // ── Kaydırılabilir içerik bölgesi (başlık altından pencere sonuna) ───────
    D2D1_RECT_F contentClip = D2D1::RectF(
        sz.width - vs.panelAnimWidth, kHeaderH, sz.width, sz.height);
    m_renderTarget->PushAxisAlignedClip(contentClip, D2D1_ANTIALIAS_MODE_ALIASED);

    // vs.panelScrollY piksel kadar yukarı kaydır
    float y = kHeaderH + PanelLayout::PadX - vs.panelScrollY;

    constexpr float kLabelH = 17.0f;
    constexpr float kGap    = 4.0f;
    constexpr float kValueH = 18.0f;
    constexpr float kRowH   = kLabelH + kGap + kValueH + 8.0f;  // ~47px per row

    // Etiket + değer satırı çizen lambda; boş değerler atlanır
    auto DrawRow = [&](const wchar_t* label, const std::wstring& value)
    {
        if (value.empty()) return;
        // Etiket: gri (#AAAAAA)
        m_renderTarget->DrawText(
            label, static_cast<UINT32>(wcslen(label)),
            m_labelFormat, D2D1::RectF(x0, y, x1, y + kLabelH),
            m_grayBrush
        );
        // Değer: beyaz
        m_renderTarget->DrawText(
            value.c_str(), static_cast<UINT32>(value.size()),
            m_valueFormat, D2D1::RectF(x0, y + kLabelH + kGap, x1, y + kLabelH + kGap + kValueH),
            m_whiteBrush
        );
        y += kRowH;
    };

    // Dosya adı — tek satırsa ortala, taşarsa word-wrap ile gerçek yüksekliğe göre genişle
    if (!info->filename.empty() && m_dwriteFactory)
    {
        float textW = x1 - x0;
        IDWriteTextLayout* layout = nullptr;
        m_dwriteFactory->CreateTextLayout(
            info->filename.c_str(),
            static_cast<UINT32>(info->filename.size()),
            m_valueFormat,
            textW, 1000.0f,
            &layout
        );
        if (layout)
        {
            layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            DWRITE_TEXT_METRICS metrics{};
            layout->GetMetrics(&metrics);
            m_renderTarget->DrawTextLayout(D2D1::Point2F(x0, y), layout, m_whiteBrush);
            layout->Release();
            y += metrics.height + 6.0f;
        }
    }

    // Dosya adı altı ayraç
    m_renderTarget->DrawLine(
        D2D1::Point2F(x0, y), D2D1::Point2F(x1, y), m_separatorBrush, 1.0f
    );
    y += 12.0f;

    // Temel bilgiler
    if (info->width > 0 && info->height > 0)
    {
        wchar_t dimBuf[32];
        swprintf_s(dimBuf, L"%d \u00d7 %d px", info->width, info->height);
        DrawRow(L"Resolution", dimBuf);
    }

    if (info->fileSizeBytes > 0)
    {
        wchar_t szBuf[32];
        if (info->fileSizeBytes >= 1024LL * 1024)
            swprintf_s(szBuf, L"%.1f MB", info->fileSizeBytes / (1024.0 * 1024.0));
        else
            swprintf_s(szBuf, L"%lld KB", info->fileSizeBytes / 1024);
        DrawRow(L"File Size", szBuf);
    }

    DrawRow(L"Format", info->format);
    DrawRow(L"Color Profile", info->iccProfileName);

    // EXIF section — only if at least one field is populated
    bool hasExif = !info->dateTaken.empty()  || !info->cameraMake.empty()  ||
                   !info->cameraModel.empty() || !info->aperture.empty()    ||
                   !info->shutterSpeed.empty()|| !info->iso.empty();

    if (hasExif)
    {
        y += 4.0f;
        m_renderTarget->DrawLine(
            D2D1::Point2F(x0, y), D2D1::Point2F(x1, y), m_separatorBrush, 1.0f
        );
        y += 12.0f;

        // Date Taken — etiket (+ inline toggle) + değer
        if (!info->dateTaken.empty())
        {
            // ── Inline segmented pill: [ 24h | 12h ] — "Date Taken" metninin hemen sağında
            constexpr float kPillW = 54.0f;
            constexpr float kPillH = 14.0f;
            constexpr float kPillR = 7.0f;
            constexpr float kSegW  = kPillW * 0.5f;

            const float px = x1 - kPillW;                  // sağa yaslanmış
            const float py = y + (kLabelH - kPillH) * 0.5f;
            const bool  leftActive = !vs.use12HourTime;

            // Etiket — pill'in soluna kadar (gri)
            m_renderTarget->DrawText(
                L"Date Taken", static_cast<UINT32>(wcslen(L"Date Taken")),
                m_labelFormat, D2D1::RectF(x0, y, px - 6.0f, y + kLabelH), m_grayBrush
            );

            // Aktif segment dolgusu
            D2D1_ROUNDED_RECT activeRR;
            if (leftActive)
                activeRR = { D2D1::RectF(px,         py, px + kSegW,  py + kPillH), kPillR, kPillR };
            else
                activeRR = { D2D1::RectF(px + kSegW, py, px + kPillW, py + kPillH), kPillR, kPillR };
            m_renderTarget->FillRoundedRectangle(activeRR, m_toggleFillBrush);

            // Pill dış çerçevesi
            D2D1_ROUNDED_RECT pillRR = { D2D1::RectF(px, py, px + kPillW, py + kPillH), kPillR, kPillR };
            m_whiteBrush->SetOpacity(0.28f);
            m_renderTarget->DrawRoundedRectangle(pillRR, m_whiteBrush, 1.0f);

            // Orta ayraç çizgisi
            m_whiteBrush->SetOpacity(0.20f);
            m_renderTarget->DrawLine(
                D2D1::Point2F(px + kSegW, py + 3.0f),
                D2D1::Point2F(px + kSegW, py + kPillH - 3.0f),
                m_whiteBrush, 0.75f
            );

            // "24h" / "12h" metinleri — 10px toggle formatı
            m_whiteBrush->SetOpacity(leftActive ? 1.0f : 0.40f);
            m_renderTarget->DrawText(
                L"24h", 3, m_toggleFormat,
                D2D1::RectF(px, py, px + kSegW, py + kPillH), m_whiteBrush
            );
            m_whiteBrush->SetOpacity(leftActive ? 0.40f : 1.0f);
            m_renderTarget->DrawText(
                L"12h", 3, m_toggleFormat,
                D2D1::RectF(px + kSegW, py, px + kPillW, py + kPillH), m_whiteBrush
            );
            m_whiteBrush->SetOpacity(1.0f);

            // Hit-test rect'ini kaydet (tüm pill)
            m_dateToggleRect    = D2D1::RectF(px, py, px + kPillW, py + kPillH);
            m_dateToggleVisible = true;

            // Değer
            std::wstring dateFormatted = FormatDateTaken(info->dateTaken, vs.use12HourTime);
            m_renderTarget->DrawText(
                dateFormatted.c_str(), static_cast<UINT32>(dateFormatted.size()),
                m_valueFormat,
                D2D1::RectF(x0, y + kLabelH + kGap, x1, y + kLabelH + kGap + kValueH),
                m_whiteBrush
            );
            y += kRowH;
        }

        // Camera make + model on one line
        if (!info->cameraMake.empty() || !info->cameraModel.empty())
        {
            std::wstring cam = info->cameraMake;
            if (!info->cameraModel.empty())
            {
                if (!cam.empty()) cam += L" ";
                cam += info->cameraModel;
            }
            DrawRow(L"Camera", cam);
        }

        DrawRow(L"Aperture", info->aperture);
        DrawRow(L"Shutter Speed", info->shutterSpeed);
        DrawRow(L"ISO", info->iso);

        // GPS section — sadece koordinat varsa
        m_locationConsentVisible = false;
        m_locationToggleVisible  = false;
        if (!info->gpsLatitude.empty() || !info->gpsLongitude.empty())
        {
            y += 4.0f;
            m_renderTarget->DrawLine(
                D2D1::Point2F(x0, y), D2D1::Point2F(x1, y), m_separatorBrush, 1.0f
            );
            y += 12.0f;

            bool bothCoords = !info->gpsLatitude.empty() && !info->gpsLongitude.empty();
            std::wstring coords = bothCoords
                ? (info->gpsLatitude + L"  " + info->gpsLongitude)
                : (info->gpsLatitude.empty() ? info->gpsLongitude : info->gpsLatitude);

            // GPS Location etiket satırı
            {
                std::wstring label = info->hasGpsDecimal
                    ? L"GPS Location \u2197"   // ↗ = koordinata tıklanabilir
                    : L"GPS Location";
                m_renderTarget->DrawText(
                    label.c_str(), static_cast<UINT32>(label.size()),
                    m_labelFormat,
                    D2D1::RectF(x0, y, x1, y + kLabelH),
                    m_grayBrush
                );
            }

            // Koordinat değeri — harita linki varsa altı çizili; sağda kopyala butonu
            constexpr float kCopyBtnSize = 22.0f;
            constexpr float kCopyBtnGap  =  6.0f;
            float valueY = y + kLabelH + kGap;
            if (info->hasGpsDecimal && m_dwriteFactory)
            {
                const float coordX1 = x1 - kCopyBtnSize - kCopyBtnGap;
                DWRITE_TEXT_METRICS tm{};
                tm.height = kValueH;  // layout başarısız olursa varsayılan
                IDWriteTextLayout* layout = nullptr;
                if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
                    coords.c_str(), static_cast<UINT32>(coords.size()),
                    m_valueFormat, coordX1 - x0, kValueH * 2.0f, &layout)))
                {
                    DWRITE_TEXT_RANGE all = { 0, static_cast<UINT32>(coords.size()) };
                    layout->SetUnderline(TRUE, all);
                    m_renderTarget->DrawTextLayout(D2D1::Point2F(x0, valueY), layout, m_whiteBrush);

                    layout->GetMetrics(&tm);
                    m_gpsLinkRect    = D2D1::RectF(x0, valueY, x0 + tm.widthIncludingTrailingWhitespace, valueY + tm.height);
                    m_gpsLinkVisible = true;
                    layout->Release();
                }

                // Kopyala butonu — koordinat satırının sağında, dikey olarak ortalanmış
                const float bx0 = x1 - kCopyBtnSize;
                const float by0 = valueY + (tm.height - kCopyBtnSize) * 0.5f;
                const float bx1 = x1;
                const float by1 = by0 + kCopyBtnSize;
                D2D1_RECT_F       btnRect = D2D1::RectF(bx0, by0, bx1, by1);
                D2D1_ROUNDED_RECT btnRR   = { btnRect, 4.0f, 4.0f };

                m_mapCopyBtnRect    = btnRect;
                m_mapCopyBtnVisible = true;

                if (m_separatorBrush)
                    m_renderTarget->DrawRoundedRectangle(btnRR, m_separatorBrush, 1.0f);

                const bool copied = (m_mapCopiedAt != 0) &&
                                    (GetTickCount64() - m_mapCopiedAt < 1500ULL);
                if (copied && m_toggleFormat)
                {
                    ID2D1SolidColorBrush* greenBrush = nullptr;
                    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0x4CAF50), &greenBrush);
                    if (greenBrush)
                    {
                        m_renderTarget->DrawText(L"\u2713", 1, m_toggleFormat, btnRect, greenBrush);
                        greenBrush->Release();
                    }
                }
                else if (m_grayBrush && m_toggleFormat)
                {
                    // ⧉ U+29C9 — kopyala simgesi
                    m_renderTarget->DrawText(L"\u29C9", 1, m_toggleFormat, btnRect, m_grayBrush);
                }
            }
            else
            {
                m_renderTarget->DrawText(
                    coords.c_str(), static_cast<UINT32>(coords.size()),
                    m_valueFormat, D2D1::RectF(x0, valueY, x1, valueY + kValueH),
                    m_whiteBrush
                );
            }
            y += kRowH;

            if (vs.locationConsent == LocationConsent::NotAsked && info->hasGpsDecimal)
            {
                // Onay istenmemiş — kullanıcıdan izin al
                constexpr float kPromptH = 38.0f;
                const wchar_t* kPromptText =
                    L"Konum adı için GPS koordinatları photon.komoot.io adresine gönderilecek.";
                m_renderTarget->DrawText(
                    kPromptText, static_cast<UINT32>(wcslen(kPromptText)),
                    m_labelFormat,
                    D2D1::RectF(x0, y, x1, y + kPromptH),
                    m_grayBrush
                );
                y += kPromptH + 8.0f;

                constexpr float kBtnH   = 28.0f;
                constexpr float kBtnGap =  8.0f;
                float btnW = (x1 - x0 - kBtnGap) * 0.5f;

                D2D1_RECT_F       yesRect = D2D1::RectF(x0,                   y, x0 + btnW, y + kBtnH);
                D2D1_RECT_F       noRect  = D2D1::RectF(x0 + btnW + kBtnGap,  y, x1,        y + kBtnH);
                D2D1_ROUNDED_RECT yesRR   = { yesRect, 5.0f, 5.0f };
                D2D1_ROUNDED_RECT noRR    = { noRect,  5.0f, 5.0f };

                // Evet butonu — yeşil outline + hover/press fill
                ID2D1SolidColorBrush* greenBrush = nullptr;
                m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0x4CAF50), &greenBrush);
                if (greenBrush)
                {
                    if (vs.locationConsentPress == 1)
                        m_renderTarget->FillRoundedRectangle(yesRR, greenBrush);
                    else if (vs.locationConsentHover == 1)
                    {
                        ID2D1SolidColorBrush* hoverBrush = nullptr;
                        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0x4CAF50, 0.18f), &hoverBrush);
                        if (hoverBrush)
                        {
                            m_renderTarget->FillRoundedRectangle(yesRR, hoverBrush);
                            hoverBrush->Release();
                        }
                    }
                    m_renderTarget->DrawRoundedRectangle(yesRR, greenBrush, 1.0f);
                    ID2D1SolidColorBrush* textBrush = (vs.locationConsentPress == 1) ? m_whiteBrush : greenBrush;
                    m_renderTarget->DrawText(L"Evet", 4, m_btnFormat, yesRect, textBrush);
                    greenBrush->Release();
                }

                // Hayır butonu — gray outline + hover/press fill
                if (m_separatorBrush && m_grayBrush)
                {
                    if (vs.locationConsentPress == 2)
                    {
                        float savedOp = m_overlayBrush->GetOpacity();
                        m_overlayBrush->SetOpacity(0.22f);
                        m_renderTarget->FillRoundedRectangle(noRR, m_overlayBrush);
                        m_overlayBrush->SetOpacity(savedOp);
                    }
                    else if (vs.locationConsentHover == 2)
                    {
                        float savedOp = m_overlayBrush->GetOpacity();
                        m_overlayBrush->SetOpacity(0.10f);
                        m_renderTarget->FillRoundedRectangle(noRR, m_overlayBrush);
                        m_overlayBrush->SetOpacity(savedOp);
                    }
                    m_renderTarget->DrawRoundedRectangle(noRR, m_separatorBrush, 1.0f);
                    m_renderTarget->DrawText(L"Hayır", 5, m_btnFormat, noRect, m_grayBrush);
                }

                m_locationYesRect        = yesRect;
                m_locationNoRect         = noRect;
                m_locationConsentVisible = true;
                y += kBtnH + 8.0f;
            }
            else if (vs.locationConsent == LocationConsent::Enabled)
            {
                // Konum adı ve harita
                DrawRow(L"Location", info->gpsLocationName);
                DrawRow(L"Altitude", info->gpsAltitude);

                if (info->hasGpsDecimal)
                {
                    y += 8.0f;
                    DrawMapPreview(x0, x1, y, vs, info);
                    y += 150.0f + 8.0f;
                }

                // "Kapat" toggle linki
                if (m_dwriteFactory && m_grayBrush)
                {
                    m_renderTarget->DrawText(
                        L"Konum araması", 13, m_labelFormat,
                        D2D1::RectF(x0, y, x1 - 52.0f, y + kLabelH),
                        m_grayBrush
                    );
                    IDWriteTextLayout* layout = nullptr;
                    if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
                        L"Kapat", 5, m_valueFormat, 52.0f, kLabelH, &layout)))
                    {
                        DWRITE_TEXT_RANGE all = { 0, 5 };
                        layout->SetUnderline(TRUE, all);
                        float tx = x1 - 50.0f;
                        m_renderTarget->DrawTextLayout(D2D1::Point2F(tx, y), layout, m_grayBrush);
                        DWRITE_TEXT_METRICS tm{};
                        layout->GetMetrics(&tm);
                        m_locationToggleRect    = D2D1::RectF(tx, y, tx + tm.widthIncludingTrailingWhitespace, y + tm.height);
                        m_locationToggleVisible = true;
                        layout->Release();
                    }
                    y += kLabelH + 8.0f;
                }
            }
            else  // Disabled (veya NotAsked && decimal koordinat yok)
            {
                // Harita yok, konum araması kapalı
                DrawRow(L"Altitude", info->gpsAltitude);

                if (vs.locationConsent == LocationConsent::Disabled && m_dwriteFactory && m_grayBrush)
                {
                    m_renderTarget->DrawText(
                        L"Konum araması", 13, m_labelFormat,
                        D2D1::RectF(x0, y, x1 - 30.0f, y + kLabelH),
                        m_grayBrush
                    );
                    IDWriteTextLayout* layout = nullptr;
                    if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
                        L"Aç", 2, m_valueFormat, 30.0f, kLabelH, &layout)))
                    {
                        DWRITE_TEXT_RANGE all = { 0, 2 };
                        layout->SetUnderline(TRUE, all);
                        float tx = x1 - 28.0f;
                        m_renderTarget->DrawTextLayout(D2D1::Point2F(tx, y), layout, m_grayBrush);
                        DWRITE_TEXT_METRICS tm{};
                        layout->GetMetrics(&tm);
                        m_locationToggleRect    = D2D1::RectF(tx, y, tx + tm.widthIncludingTrailingWhitespace, y + tm.height);
                        m_locationToggleVisible = true;
                        layout->Release();
                    }
                    y += kLabelH + 8.0f;
                }
            }
        }
    }

    // Toplam içerik yüksekliğini kaydet (scroll sınırlaması için)
    m_infoPanelContentH = (y + vs.panelScrollY) - (kHeaderH + PanelLayout::PadX);

    m_renderTarget->PopAxisAlignedClip();  // içerik scroll clip
    m_renderTarget->PopAxisAlignedClip();  // panel animasyon clip
}

// ─── Info Button ─────────────────────────────────────────────────────────────

void Renderer::DrawInfoButton(const ViewState& vs)
{
    if (!m_overlayBrush || !m_activeBrush || !m_whiteBrush || !m_textFormat) return;

    D2D1_SIZE_F sz = m_renderTarget->GetSize();
    constexpr float kSize   = InfoButton::Size;
    constexpr float kMargin = InfoButton::Margin;

    // Panel genişliğine göre buton sola kayar (animasyonla birlikte)
    float xOffset = vs.panelAnimWidth;
    float x0 = sz.width - kMargin - kSize - xOffset;
    float y0 = kMargin;
    float x1 = sz.width - kMargin - xOffset;
    float y1 = kMargin + kSize;

    D2D1_RECT_F rect = D2D1::RectF(x0, y0, x1, y1);
    D2D1_ROUNDED_RECT rr = { rect, 8.0f, 8.0f };

    // Sadece fiziksel basışta (infoBtnPressed) açık tint; diğer zamanlarda koyu
    auto* fillBrush = vs.infoBtnPressed ? m_activeBrush : m_panelBgBrush;
    m_renderTarget->FillRoundedRectangle(rr, fillBrush);

    // Kenar çizgisi
    m_renderTarget->DrawRoundedRectangle(rr, m_separatorBrush, 1.0f);

    // "i" glyph centered in the button
    m_renderTarget->DrawText(L"i", 1, m_textFormat, rect, m_whiteBrush);

    // ── Delete butonu — info butonunun solunda, aynı boyutta ────────────────
    const float delX0 = x0 - 6.0f - kSize;  // 6px boşluk + buton genişliği
    const float delX1 = x0 - 6.0f;
    m_deleteBtnRect = D2D1::RectF(delX0, y0, delX1, y1);
    m_deleteBtnVisible = true;

    D2D1_ROUNDED_RECT delRR = { m_deleteBtnRect, 8.0f, 8.0f };
    // Basılıysa kırmızı, hover-benzeri arka plan; normal halde koyu panel rengi
    auto* delFill = vs.deleteBtnPressed ? m_deleteBrush : m_panelBgBrush;
    m_renderTarget->FillRoundedRectangle(delRR, delFill);
    // Kenar: normal halde ince kırmızı ton, basılıysa daha parlak
    m_renderTarget->DrawRoundedRectangle(delRR, vs.deleteBtnPressed ? m_deleteBrush : m_separatorBrush, 1.0f);

    // Çöp kutusu simgesi — dolu gövde + kapak (path geometry)
    {
        auto* tr = m_renderTarget;
        float cx = (delX0 + delX1) * 0.5f;
        float cy = (y0 + y1) * 0.5f;
        const float sw = 1.8f;

        // Sap: U şekli, kapak üzerinde
        const float hW = 3.5f, hTop = cy - 11.0f, lidY = cy - 7.5f;
        tr->DrawLine({cx - hW, lidY}, {cx - hW, hTop}, m_whiteBrush, sw);
        tr->DrawLine({cx - hW, hTop}, {cx + hW, hTop}, m_whiteBrush, sw);
        tr->DrawLine({cx + hW, hTop}, {cx + hW, lidY}, m_whiteBrush, sw);

        // Kapak: kalın yatay çizgi
        tr->DrawLine({cx - 9.0f, lidY}, {cx + 9.0f, lidY}, m_whiteBrush, sw + 0.6f);

        // Gövde: dolu yuvarlatılmış dikdörtgen
        const float bW = 7.5f, bTop = lidY + 2.5f, bBot = cy + 9.0f;
        D2D1_RECT_F bodyR = D2D1::RectF(cx - bW, bTop, cx + bW, bBot);
        D2D1_ROUNDED_RECT bodyRR = { bodyR, 2.5f, 2.5f };
        {
            float op = m_whiteBrush->GetOpacity();
            m_whiteBrush->SetOpacity(op * 0.85f);
            tr->FillRoundedRectangle(bodyRR, m_whiteBrush);
            m_whiteBrush->SetOpacity(op);
        }

        // Gövde üstü kapak ile birleşim — ince dikdörtgen
        tr->FillRectangle(D2D1::RectF(cx - bW, bTop - 1.0f, cx + bW, bTop + 2.0f), m_whiteBrush);

        // Gövde içi dikey çizgiler (koyu, gövde dolgusunun üzerinde)
        auto* bg = vs.deleteBtnPressed ? m_deleteBrush : m_panelBgBrush;
        const float lineTop = bTop + 3.5f, lineBot = bBot - 2.0f;
        tr->DrawLine({cx - 2.5f, lineTop}, {cx - 2.5f, lineBot}, bg, 1.5f);
        tr->DrawLine({cx + 2.5f, lineTop}, {cx + 2.5f, lineBot}, bg, 1.5f);
    }
}

// ─── Ana Render ───────────────────────────────────────────────────────────────

void Renderer::Render(const ViewState& vs, const ImageInfo* info)
{
    if (FAILED(CreateDeviceResources())) return;

    // D2DERR_RECREATE_TARGET sonrası bitmap kaybolmuşsa yeniden yükle
    if (!m_bitmap && !m_animBitmaps.empty() && !m_imagePath.empty())
        LoadImage(m_imagePath);  // animasyon için fallback (nadiren gerekir)
    else if (!m_bitmap && m_animBitmaps.empty() && !m_imagePath.empty())
        LoadImage(m_imagePath);

    m_renderTarget->BeginDraw();

    // Arka plan: çok koyu (#0F0F0F)
    m_renderTarget->Clear(D2D1::ColorF(0.059f, 0.059f, 0.059f));

    // Aktif bitmap: animasyonsa mevcut frame, değilse statik bitmap
    ID2D1Bitmap* activeBitmap = nullptr;
    if (!m_animBitmaps.empty() && m_animFrameIdx < static_cast<int>(m_animBitmaps.size()))
        activeBitmap = m_animBitmaps[m_animFrameIdx];
    else
        activeBitmap = m_bitmap;

    // Bitmap yoksa ve decode hatası varsa hata mesajı göster
    if (!activeBitmap && info && !info->errorMessage.empty() &&
        m_textFormat && m_whiteBrush)
    {
        D2D1_SIZE_F sz = m_renderTarget->GetSize();
        m_renderTarget->DrawText(
            info->errorMessage.c_str(),
            static_cast<UINT32>(info->errorMessage.size()),
            m_textFormat,
            D2D1::RectF(0, 0, sz.width, sz.height),
            m_whiteBrush
        );
    }

    if (activeBitmap)
    {
        D2D1_SIZE_F wndSize = m_renderTarget->GetSize();
        D2D1_SIZE_F imgSize = activeBitmap->GetSize();

        float availW = wndSize.width  - vs.panelAnimWidth;
        float availH = wndSize.height - vs.stripAnimHeight;
        float fitScale   = min(availW / imgSize.width, availH / imgSize.height);
        float finalScale = fitScale * vs.zoomFactor;

        float destW = imgSize.width  * finalScale;
        float destH = imgSize.height * finalScale;
        float destX = (availW - destW) * 0.5f + vs.panX;
        float destY = (availH - destH) * 0.5f + vs.panY;

        m_imageDisplayRect = D2D1::RectF(destX, destY, destX + destW, destY + destH);

        if (vs.showRotateFreeDialog && fabsf(vs.rotateFreeAngle) > 0.01f)
        {
            float imgCX = destX + destW * 0.5f;
            float imgCY = destY + destH * 0.5f;
            auto rot = D2D1::Matrix3x2F::Rotation(vs.rotateFreeAngle, D2D1::Point2F(imgCX, imgCY));
            m_renderTarget->SetTransform(rot);
            m_renderTarget->DrawBitmap(activeBitmap, D2D1::RectF(destX, destY, destX + destW, destY + destH));
            m_renderTarget->SetTransform(D2D1::Matrix3x2F::Identity());
        }
        else
        {
            m_renderTarget->DrawBitmap(activeBitmap, D2D1::RectF(destX, destY, destX + destW, destY + destH));
        }
    }

    // Overlaylar (arka plandan öne doğru)
    DrawNavArrows(vs);
    if (vs.stripAnimHeight > 0.0f) DrawThumbnailStrip(vs);
    DrawIndexBar(vs);
    DrawStripToggle(vs);
    if (vs.panelAnimWidth > 0.0f) DrawInfoPanel(vs, info);
    DrawInfoButton(vs);  // Info panelinin üstünde çizilir
    DrawEditToolbar(vs); // Görüntü üstü merkez — hover'da beliren döndür butonları
    DrawSaveBar(vs);     // Alt orta — kaydedilmemiş değişiklik varsa görünür
    DrawDeleteConfirmDialog(vs, info); // Modal dialog — en üst katman
    DrawResizeDialog(vs);              // Resize modal — en üst katman
    DrawRotateFreeDialog(vs);          // Serbest döndürme modal — en üst katman
    DrawCropDialog(vs);                // Kırpma modu — en üst katman

    // Zoom indicator: sağ alt köşe (alpha fade)
    if (vs.zoomIndicatorAlpha > 0.01f && m_textFormat && m_whiteBrush && m_overlayBrush)
    {
        D2D1_SIZE_F wndSize = m_renderTarget->GetSize();
        wchar_t text[16];
        swprintf_s(text, L"%.0f%%", vs.zoomFactor * 100.0f);

        constexpr float kW      = 80.0f;
        constexpr float kH      = 34.0f;
        constexpr float kMargin = 12.0f;

        float rightEdge  = wndSize.width - vs.panelAnimWidth;
        float bottomBase = wndSize.height - vs.stripAnimHeight - kMargin;
        D2D1_RECT_F bgRect = D2D1::RectF(
            rightEdge - kW - kMargin,
            bottomBase - kH,
            rightEdge - kMargin,
            bottomBase
        );

        float savedOverlayOp = m_overlayBrush->GetOpacity();
        float savedWhiteOp   = m_whiteBrush->GetOpacity();
        m_overlayBrush->SetOpacity(savedOverlayOp * vs.zoomIndicatorAlpha);
        m_whiteBrush->SetOpacity(savedWhiteOp   * vs.zoomIndicatorAlpha);

        D2D1_ROUNDED_RECT rr = { bgRect, 6.0f, 6.0f };
        m_renderTarget->FillRoundedRectangle(rr, m_overlayBrush);
        m_renderTarget->DrawText(
            text, static_cast<UINT32>(wcslen(text)), m_textFormat, bgRect, m_whiteBrush
        );

        m_overlayBrush->SetOpacity(savedOverlayOp);
        m_whiteBrush->SetOpacity(savedWhiteOp);
    }

    HRESULT hr = m_renderTarget->EndDraw();

    if (hr == D2DERR_RECREATE_TARGET)
        DiscardDeviceResources();
}

// ─── Edit Toolbar ─────────────────────────────────────────────────────────────
// Görüntü üst kısmında merkeze hizalı floating toolbar.
// Sadece statik (animasyonsuz) görsel yüklüyken çizilir.
// editToolbarAlpha: hover fade tarafından kontrol edilir; editDirty=true → her zaman tam görünür.

// Döndür ikon çizici — Direct2D path geometry kullanır (font bağımsız, her DPI'da temiz görünür).
// cx,cy: buton merkezi; clockwise: saat yönü mu; strokeW: çizgi kalınlığı.
static void DrawRotateIcon(ID2D1HwndRenderTarget* rt, ID2D1Factory* factory,
                            ID2D1SolidColorBrush* brush, float cx, float cy,
                            bool clockwise, float strokeW)
{
    constexpr float pi = 3.14159265358979f;
    constexpr float R  = 10.0f;  // yay yarıçapı

    // Saat açısı (0°=yukarı/12, saat yönünde artar).
    // P(θ) = (cx + R·sin θ,  cy − R·cos θ)  — Y aşağı olan ekran koordinatı
    auto clockPt = [&](float deg) -> D2D1_POINT_2F {
        float r = deg * pi / 180.0f;
        return { cx + R * sinf(r), cy - R * cosf(r) };
    };

    // Yay: 300° açıklık, 60°'lik boşluk üstte (330°–30° arası, 0°=12 saat)
    // CW  →  30°'den 330°'ye saat yönünde (300° yay), ok başı 330°'de (sol üst, sağ-yukarı yönde)
    // CCW → 330°'den  30°'ye saat yönü tersine (300° yay), ok başı 30°'de (sağ üst, sol-yukarı yönde)
    // Böylece iki ikon birbirinin ayna görüntüsü olur; klasik ↺/↻ görünümüyle uyumlu.
    const float arcStartDeg = clockwise ?  30.0f : 330.0f;
    const float arcEndDeg   = clockwise ? 330.0f :  30.0f;

    D2D1_POINT_2F startPt = clockPt(arcStartDeg);
    D2D1_POINT_2F endPt   = clockPt(arcEndDeg);

    ID2D1PathGeometry* arcGeo = nullptr;
    factory->CreatePathGeometry(&arcGeo);
    if (arcGeo)
    {
        ID2D1GeometrySink* sink = nullptr;
        arcGeo->Open(&sink);
        if (sink)
        {
            sink->BeginFigure(startPt, D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddArc(D2D1::ArcSegment(
                endPt,
                D2D1::SizeF(R, R),
                0.0f,
                clockwise ? D2D1_SWEEP_DIRECTION_CLOCKWISE
                          : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                D2D1_ARC_SIZE_LARGE));  // 300° > 180° → büyük yay
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            sink->Release();
        }
        rt->DrawGeometry(arcGeo, brush, strokeW);
        arcGeo->Release();
    }

    // Ok başı — yay ucundaki tanjant yönünde dolu üçgen
    // P(θ)'nin saat yönündeki tanjantı: dP/dθ = (R·cos θ, R·sin θ) → normalize → (cos θ, sin θ)
    float endRad = arcEndDeg * pi / 180.0f;
    float tx = clockwise ?  cosf(endRad) : -cosf(endRad);
    float ty = clockwise ?  sinf(endRad) : -sinf(endRad);
    float px = -ty, py = tx;  // dik yön

    constexpr float arrowLen = 5.5f, arrowHW = 3.8f;
    D2D1_POINT_2F tip   = { endPt.x + tx * arrowLen,     endPt.y + ty * arrowLen };
    D2D1_POINT_2F baseL = { endPt.x + px * arrowHW,      endPt.y + py * arrowHW };
    D2D1_POINT_2F baseR = { endPt.x - px * arrowHW,      endPt.y - py * arrowHW };

    ID2D1PathGeometry* arrowGeo = nullptr;
    factory->CreatePathGeometry(&arrowGeo);
    if (arrowGeo)
    {
        ID2D1GeometrySink* arrowSink = nullptr;
        arrowGeo->Open(&arrowSink);
        if (arrowSink)
        {
            arrowSink->BeginFigure(tip, D2D1_FIGURE_BEGIN_FILLED);
            arrowSink->AddLine(baseL);
            arrowSink->AddLine(baseR);
            arrowSink->EndFigure(D2D1_FIGURE_END_CLOSED);
            arrowSink->Close();
            arrowSink->Release();
        }
        rt->FillGeometry(arrowGeo, brush);
        arrowGeo->Release();
    }
}

// Serbest döndürme ikonu — eğik ufuk çizgisi + yatay referans.
// cx,cy: buton merkezi; strokeW: çizgi kalınlığı.
static void DrawFreeRotateIcon(ID2D1HwndRenderTarget* rt, ID2D1SolidColorBrush* brush,
                                float cx, float cy, float strokeW)
{
    // Eğik çizgi (~18°): cos(18°)=0.9511, sin(18°)=0.3090
    constexpr float cosT = 0.9511f;
    constexpr float sinT = 0.3090f;
    constexpr float len  = 8.5f;

    float lx1 = cx - len * cosT, ly1 = cy + len * sinT;  // sol uç
    float lx2 = cx + len * cosT, ly2 = cy - len * sinT;  // sağ uç
    rt->DrawLine(D2D1::Point2F(lx1, ly1), D2D1::Point2F(lx2, ly2), brush, strokeW);

    // Sağ uçta ok başı — çizgi yönü geri (sol): (-cosT, sinT)
    // Ok kanatları: geri yönün ±70° sapması
    constexpr float arrowLen = 3.8f;
    // Kanat 1: perpendicular - back mix
    float w1x = lx2 + arrowLen * (-cosT * 0.5f - sinT * 0.87f);
    float w1y = ly2 + arrowLen * ( sinT * 0.5f - cosT * 0.87f);
    float w2x = lx2 + arrowLen * (-cosT * 0.5f + sinT * 0.87f);
    float w2y = ly2 + arrowLen * ( sinT * 0.5f + cosT * 0.87f);
    rt->DrawLine(D2D1::Point2F(lx2, ly2), D2D1::Point2F(w1x, w1y), brush, strokeW);
    rt->DrawLine(D2D1::Point2F(lx2, ly2), D2D1::Point2F(w2x, w2y), brush, strokeW);

    // Yatay referans çizgisi (doğru yatay = hedefteki ufuk)
    constexpr float refY = 4.0f;
    float op = brush->GetOpacity();
    brush->SetOpacity(op * 0.55f);
    rt->DrawLine(D2D1::Point2F(cx - len, cy + refY), D2D1::Point2F(cx + len, cy + refY), brush, strokeW * 0.7f);
    brush->SetOpacity(op);
}

// Yeniden boyutlandır ikonu — 4 köşeye diyagonal oklar (köşegen ölçek sembolü).
// cx,cy: buton merkezi; strokeW: çizgi kalınlığı.
static void DrawResizeIcon(ID2D1HwndRenderTarget* rt, ID2D1SolidColorBrush* brush,
                            float cx, float cy, float strokeW)
{
    constexpr float sq2      = 0.70710678f;   // 1/√2
    constexpr float shaftEnd = 8.5f;          // ok ucunun merkezden uzaklığı
    constexpr float shaftBeg = 2.5f;          // ok gövdesinin merkezden başlangıcı
    constexpr float arrowH   = 3.2f;          // ok başı yarı genişliği

    const float dx[] = {-sq2,  sq2, -sq2, sq2};
    const float dy[] = {-sq2, -sq2,  sq2, sq2};

    for (int i = 0; i < 4; ++i)
    {
        float ddx = dx[i], ddy = dy[i];
        float tipX  = cx + ddx * shaftEnd,   tipY  = cy + ddy * shaftEnd;
        float baseX = cx + ddx * (shaftEnd - arrowH * 1.1f),
              baseY = cy + ddy * (shaftEnd - arrowH * 1.1f);
        float px = -ddy * arrowH, py = ddx * arrowH;

        // Gövde çizgisi
        rt->DrawLine({cx + ddx * shaftBeg, cy + ddy * shaftBeg}, {tipX, tipY}, brush, strokeW);

        // Ok başı
        rt->DrawLine({tipX, tipY}, {baseX + px, baseY + py}, brush, strokeW);
        rt->DrawLine({tipX, tipY}, {baseX - px, baseY - py}, brush, strokeW);
    }
}

// Kırpma ikonu: iki L-şekli köşe + çapraz kılavuz çizgisi (makas efekti)
static void DrawCropIcon(ID2D1HwndRenderTarget* rt, ID2D1SolidColorBrush* brush,
                          float cx, float cy, float strokeW)
{
    constexpr float half = 8.5f;   // ikonun yarı genişliği/yüksekliği
    constexpr float arm  = 5.5f;   // L kolunun uzunluğu
    constexpr float gap  = 2.2f;   // merkeze boşluk

    // Sol-üst L köşesi
    rt->DrawLine({cx - half, cy - half + arm}, {cx - half, cy - half}, brush, strokeW);
    rt->DrawLine({cx - half, cy - half}, {cx - half + arm, cy - half}, brush, strokeW);

    // Sağ-üst L köşesi
    rt->DrawLine({cx + half - arm, cy - half}, {cx + half, cy - half}, brush, strokeW);
    rt->DrawLine({cx + half, cy - half}, {cx + half, cy - half + arm}, brush, strokeW);

    // Sol-alt L köşesi
    rt->DrawLine({cx - half, cy + half - arm}, {cx - half, cy + half}, brush, strokeW);
    rt->DrawLine({cx - half, cy + half}, {cx - half + arm, cy + half}, brush, strokeW);

    // Sağ-alt L köşesi
    rt->DrawLine({cx + half - arm, cy + half}, {cx + half, cy + half}, brush, strokeW);
    rt->DrawLine({cx + half, cy + half}, {cx + half, cy + half - arm}, brush, strokeW);

    // Merkez çapraz kılavuz çizgileri (kırpma ikonunun tipik stilini taklit eder)
    rt->DrawLine({cx - gap, cy}, {cx - half + arm * 0.6f, cy}, brush, strokeW * 0.7f);
    rt->DrawLine({cx + gap, cy}, {cx + half - arm * 0.6f, cy}, brush, strokeW * 0.7f);
    rt->DrawLine({cx, cy - gap}, {cx, cy - half + arm * 0.6f}, brush, strokeW * 0.7f);
    rt->DrawLine({cx, cy + gap}, {cx, cy + half - arm * 0.6f}, brush, strokeW * 0.7f);
}

void Renderer::DrawEditToolbar(const ViewState& vs)
{
    m_editToolbarVisible = false;
    if (vs.editIsAnimated) return;
    if (!m_bitmap) return;

    float alpha = vs.editDirty ? 1.0f : vs.editToolbarAlpha;
    if (alpha <= 0.01f) return;

    if (!m_panelBgBrush || !m_whiteBrush || !m_activeBrush) return;

    m_editToolbarVisible = true;

    D2D1_SIZE_F sz   = m_renderTarget->GetSize();
    float availW     = sz.width - vs.panelAnimWidth;

    constexpr float kBtnSize   = 40.0f;
    constexpr float kBtnRadius = 8.0f;
    constexpr float kGap       = 8.0f;
    constexpr float kMarginTop = 16.0f;
    constexpr float kStrokeW   = 2.2f;

    // Toolbar genişliği editMoreAlpha ile interpolate: 3 btn (kapalı) → 6 btn (açık)
    const float collapsedW = kBtnSize * 3.0f + kGap * 2.0f;   // [↺][↻][···]
    const float expandedW  = kBtnSize * 6.0f + kGap * 5.0f;   // [↺][↻][···][↗][⤢][✂]
    const float totalW     = collapsedW + (expandedW - collapsedW) * vs.editMoreAlpha;
    const float startX     = (availW - totalW) * 0.5f;
    const float btnY       = kMarginTop;

    const float btn1X = startX;
    const float btn2X = btn1X + kBtnSize + kGap;
    const float btn3X = btn2X + kBtnSize + kGap;  // ···
    const float btn4X = btn3X + kBtnSize + kGap;  // ↗
    const float btn5X = btn4X + kBtnSize + kGap;  // ⤢
    const float btn6X = btn5X + kBtnSize + kGap;  // ✂

    float savedPanelOp  = m_panelBgBrush->GetOpacity();
    float savedWhiteOp  = m_whiteBrush->GetOpacity();
    float savedActiveOp = m_activeBrush->GetOpacity();
    m_panelBgBrush->SetOpacity(savedPanelOp  * alpha);
    m_whiteBrush->SetOpacity(savedWhiteOp   * alpha);
    m_activeBrush->SetOpacity(savedActiveOp * alpha);

    // [1] ↺ Döndür CCW
    m_editBtnRotLRect = D2D1::RectF(btn1X, btnY, btn1X + kBtnSize, btnY + kBtnSize);
    {
        D2D1_ROUNDED_RECT rr = { m_editBtnRotLRect, kBtnRadius, kBtnRadius };
        m_renderTarget->FillRoundedRectangle(rr,
            vs.editBtnRotLPressed ? m_activeBrush : m_panelBgBrush);
        DrawRotateIcon(m_renderTarget, m_factory, m_whiteBrush,
            btn1X + kBtnSize * 0.5f, btnY + kBtnSize * 0.5f, false, kStrokeW);
    }

    // [2] ↻ Döndür CW
    m_editBtnRotRRect = D2D1::RectF(btn2X, btnY, btn2X + kBtnSize, btnY + kBtnSize);
    {
        D2D1_ROUNDED_RECT rr = { m_editBtnRotRRect, kBtnRadius, kBtnRadius };
        m_renderTarget->FillRoundedRectangle(rr,
            vs.editBtnRotRPressed ? m_activeBrush : m_panelBgBrush);
        DrawRotateIcon(m_renderTarget, m_factory, m_whiteBrush,
            btn2X + kBtnSize * 0.5f, btnY + kBtnSize * 0.5f, true, kStrokeW);
    }

    // [3] ··· Araçlar (genişlet/daralt)
    m_editBtnMoreRect = D2D1::RectF(btn3X, btnY, btn3X + kBtnSize, btnY + kBtnSize);
    {
        D2D1_ROUNDED_RECT rr = { m_editBtnMoreRect, kBtnRadius, kBtnRadius };
        bool moreActive = vs.editBtnMorePressed || vs.editMoreExpanded;
        m_renderTarget->FillRoundedRectangle(rr, moreActive ? m_activeBrush : m_panelBgBrush);
        float cx = btn3X + kBtnSize * 0.5f;
        float cy = btnY  + kBtnSize * 0.5f;
        constexpr float kDotR  = 2.3f;
        constexpr float kDotSp = 7.5f;
        m_renderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx - kDotSp, cy), kDotR, kDotR), m_whiteBrush);
        m_renderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx,          cy), kDotR, kDotR), m_whiteBrush);
        m_renderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx + kDotSp, cy), kDotR, kDotR), m_whiteBrush);
    }

    // [4,5,6] Genişletilmiş araçlar — editMoreAlpha ile fade-in
    if (vs.editMoreAlpha > 0.01f)
    {
        float panOp = m_panelBgBrush->GetOpacity();
        float whtOp = m_whiteBrush->GetOpacity();
        float actOp = m_activeBrush->GetOpacity();
        m_panelBgBrush->SetOpacity(panOp * vs.editMoreAlpha);
        m_whiteBrush->SetOpacity(whtOp * vs.editMoreAlpha);
        m_activeBrush->SetOpacity(actOp * vs.editMoreAlpha);

        // [4] ↗ Serbest döndür
        m_editBtnRotFreeRect = D2D1::RectF(btn4X, btnY, btn4X + kBtnSize, btnY + kBtnSize);
        {
            D2D1_ROUNDED_RECT rr = { m_editBtnRotFreeRect, kBtnRadius, kBtnRadius };
            m_renderTarget->FillRoundedRectangle(rr,
                vs.editBtnRotFreePressed ? m_activeBrush : m_panelBgBrush);
            DrawFreeRotateIcon(m_renderTarget, m_whiteBrush,
                btn4X + kBtnSize * 0.5f, btnY + kBtnSize * 0.5f, kStrokeW);
        }

        // [5] ⤢ Yeniden Boyutlandır
        m_editBtnResizeRect = D2D1::RectF(btn5X, btnY, btn5X + kBtnSize, btnY + kBtnSize);
        {
            D2D1_ROUNDED_RECT rr = { m_editBtnResizeRect, kBtnRadius, kBtnRadius };
            m_renderTarget->FillRoundedRectangle(rr,
                vs.editBtnResizePressed ? m_activeBrush : m_panelBgBrush);
            DrawResizeIcon(m_renderTarget, m_whiteBrush,
                btn5X + kBtnSize * 0.5f, btnY + kBtnSize * 0.5f, kStrokeW);
        }

        // [6] ✂ Kırp
        m_editBtnCropRect = D2D1::RectF(btn6X, btnY, btn6X + kBtnSize, btnY + kBtnSize);
        {
            D2D1_ROUNDED_RECT rr = { m_editBtnCropRect, kBtnRadius, kBtnRadius };
            m_renderTarget->FillRoundedRectangle(rr,
                (vs.editBtnCropPressed || vs.showCropDialog) ? m_activeBrush : m_panelBgBrush);
            DrawCropIcon(m_renderTarget, m_whiteBrush,
                btn6X + kBtnSize * 0.5f, btnY + kBtnSize * 0.5f, kStrokeW);
        }

        m_panelBgBrush->SetOpacity(panOp);
        m_whiteBrush->SetOpacity(whtOp);
        m_activeBrush->SetOpacity(actOp);
    }
    else
    {
        m_editBtnRotFreeRect = {};
        m_editBtnResizeRect  = {};
        m_editBtnCropRect    = {};
    }

    // Tooltip — hover'daki butonun altında fade-in etiket
    if (vs.editToolbarHoverBtn >= 1 && vs.editToolbarHoverBtn <= 6 && m_btnFormat && m_separatorBrush
        && vs.editToolbarTooltipAlpha > 0.01f)
    {
        static const wchar_t* kTips[]  = { L"Sola döndür", L"Sağa döndür", L"Araçlar",
                                            L"Serbest döndür", L"Yeniden boyutlandır", L"Kırp" };
        static const float    kTipWs[] = { 110.0f, 110.0f, 80.0f, 140.0f, 180.0f, 72.0f };
        const int   tipIdx = vs.editToolbarHoverBtn - 1;
        const float tipW   = kTipWs[tipIdx];
        const float tipH   = 26.0f;
        const float tip    = vs.editToolbarTooltipAlpha;

        float btnCX = 0.0f;
        switch (vs.editToolbarHoverBtn) {
            case 1: btnCX = btn1X + kBtnSize * 0.5f; break;
            case 2: btnCX = btn2X + kBtnSize * 0.5f; break;
            case 3: btnCX = btn3X + kBtnSize * 0.5f; break;
            case 4: btnCX = btn4X + kBtnSize * 0.5f; break;
            case 5: btnCX = btn5X + kBtnSize * 0.5f; break;
            default: btnCX = btn6X + kBtnSize * 0.5f; break;
        }

        float tipLeft = btnCX - tipW * 0.5f;
        float tipTop  = btnY + kBtnSize + 6.0f;
        if (tipLeft < 4.0f) tipLeft = 4.0f;
        if (tipLeft + tipW > availW - 4.0f) tipLeft = availW - 4.0f - tipW;

        D2D1_RECT_F        tipRect = D2D1::RectF(tipLeft, tipTop, tipLeft + tipW, tipTop + tipH);
        D2D1_ROUNDED_RECT  tipRR   = { tipRect, 5.0f, 5.0f };

        float op1 = m_panelBgBrush->GetOpacity();
        float op2 = m_whiteBrush->GetOpacity();
        float op3 = m_separatorBrush->GetOpacity();
        m_panelBgBrush->SetOpacity(op1 * tip);
        m_whiteBrush->SetOpacity(op2 * tip);
        m_separatorBrush->SetOpacity(op3 * alpha * tip);

        m_renderTarget->FillRoundedRectangle(tipRR, m_panelBgBrush);
        m_renderTarget->DrawRoundedRectangle(tipRR, m_separatorBrush, 1.0f);
        m_renderTarget->DrawText(kTips[tipIdx], static_cast<UINT32>(wcslen(kTips[tipIdx])),
                                  m_btnFormat, tipRect, m_whiteBrush);

        m_panelBgBrush->SetOpacity(op1);
        m_whiteBrush->SetOpacity(op2);
        m_separatorBrush->SetOpacity(op3);
    }

    m_panelBgBrush->SetOpacity(savedPanelOp);
    m_whiteBrush->SetOpacity(savedWhiteOp);
    m_activeBrush->SetOpacity(savedActiveOp);
}

// ─── Save Bar ─────────────────────────────────────────────────────────────────
// Kaydedilmemiş değişiklik olduğunda görüntünün altında (filmstrip üstünde) çizilir.
// 3 buton: Kaydet (yeşil) | Kaydetme (nötr) | Ayrı Kaydet (nötr)

void Renderer::DrawSaveBar(const ViewState& vs)
{
    m_saveBarVisible = false;
    if (!vs.editDirty) return;
    if (!m_panelBgBrush || !m_whiteBrush || !m_separatorBrush || !m_saveBtnBrush
     || !m_btnFormat) return;

    m_saveBarVisible = true;

    bool slayer = (vs.saveBarAlpha < 0.99f) && m_dialogLayer;
    if (slayer)
    {
        D2D1_LAYER_PARAMETERS lp = D2D1::LayerParameters();
        lp.opacity = vs.saveBarAlpha;
        m_renderTarget->PushLayer(lp, m_dialogLayer);
    }

    D2D1_SIZE_F sz = m_renderTarget->GetSize();
    float availW   = sz.width - vs.panelAnimWidth;

    constexpr float kBarW  = 320.0f;
    constexpr float kBarH  = 52.0f;
    constexpr float kRadii = 10.0f;
    constexpr float kPadX  = 10.0f;
    constexpr float kBtnH  = 34.0f;
    constexpr float kGap   =  6.0f;

    const float barBottom = sz.height - vs.stripAnimHeight - 16.0f;
    const float barTop    = barBottom - kBarH;
    const float barLeft   = (availW - kBarW) * 0.5f;
    const float barRight  = barLeft + kBarW;

    // Bar arka planı
    D2D1_ROUNDED_RECT barRR = {
        D2D1::RectF(barLeft, barTop, barRight, barBottom), kRadii, kRadii
    };
    m_renderTarget->FillRoundedRectangle(barRR, m_panelBgBrush);
    m_renderTarget->DrawRoundedRectangle(barRR, m_separatorBrush, 1.0f);

    // 3 eşit buton
    const float contentW = kBarW - 2.0f * kPadX;
    const float btnW     = (contentW - 2.0f * kGap) / 3.0f;
    const float btnTop   = barTop + (kBarH - kBtnH) * 0.5f;

    float bx = barLeft + kPadX;

    int btnIdx = 0;
    auto DrawBtn = [&](ID2D1SolidColorBrush* bg, const wchar_t* label, D2D1_RECT_F& outRect)
    {
        ++btnIdx;
        outRect = D2D1::RectF(bx, btnTop, bx + btnW, btnTop + kBtnH);
        D2D1_ROUNDED_RECT rr = { outRect, 6.0f, 6.0f };
        m_renderTarget->FillRoundedRectangle(rr, bg);

        // Hover / press overlay
        if (vs.saveBarPressedBtn == btnIdx)
        {
            float savedOp = m_overlayBrush->GetOpacity();
            m_overlayBrush->SetOpacity(0.18f);
            m_renderTarget->FillRoundedRectangle(rr, m_overlayBrush);
            m_overlayBrush->SetOpacity(savedOp);
        }
        else if (vs.saveBarHover == btnIdx)
        {
            float savedOp = m_activeBrush->GetOpacity();
            m_activeBrush->SetOpacity(0.28f);
            m_renderTarget->FillRoundedRectangle(rr, m_activeBrush);
            m_activeBrush->SetOpacity(savedOp);
        }

        m_renderTarget->DrawText(label, static_cast<UINT32>(wcslen(label)),
                                  m_btnFormat, outRect, m_whiteBrush);
        bx += btnW + kGap;
    };

    DrawBtn(m_saveBtnBrush,    L"Kaydet",       m_saveBarSaveRect);
    DrawBtn(m_separatorBrush,  L"Kaydetme",     m_saveBarDiscardRect);
    DrawBtn(m_overlayBrush,    L"Ayr\u0131 Kaydet",  m_saveBarSaveAsRect);

    if (slayer)
        m_renderTarget->PopLayer();
}

// ─── Silme Onay / Uyarı Dialogu ──────────────────────────────────────────────

void Renderer::DrawDeleteConfirmDialog(const ViewState& vs, const ImageInfo* info)
{
    m_dlgVisible = false;
    if (!vs.showDeleteConfirmDialog && !vs.showUnsavedWarningDialog) return;
    if (!m_panelBgBrush || !m_separatorBrush || !m_whiteBrush || !m_grayBrush
     || !m_deleteBrush  || !m_overlayBrush   || !m_btnFormat  || !m_labelFormat) return;

    m_dlgVisible = true;

    bool dlayer = (vs.dialogAlpha < 0.99f) && m_dialogLayer;
    if (dlayer)
    {
        D2D1_LAYER_PARAMETERS lp = D2D1::LayerParameters();
        lp.opacity = vs.dialogAlpha;
        m_renderTarget->PushLayer(lp, m_dialogLayer);
    }

    D2D1_SIZE_F sz = m_renderTarget->GetSize();

    // Tam ekran yarı saydam backdrop
    float savedOp = m_overlayBrush->GetOpacity();
    m_overlayBrush->SetOpacity(0.72f);
    m_renderTarget->FillRectangle(D2D1::RectF(0, 0, sz.width, sz.height), m_overlayBrush);
    m_overlayBrush->SetOpacity(savedOp);

    const bool isWarning = vs.showUnsavedWarningDialog;

    constexpr float kDlgW    = 380.0f;
    constexpr float kDlgH    = 168.0f;
    constexpr float kRadii   = 12.0f;
    constexpr float kPadX    = 20.0f;
    constexpr float kPadY    = 20.0f;
    constexpr float kBtnH    = 38.0f;
    constexpr float kBtnGap  = 10.0f;

    const float dlgLeft   = (sz.width  - kDlgW) * 0.5f;
    const float dlgTop    = (sz.height - kDlgH) * 0.5f;
    const float dlgRight  = dlgLeft + kDlgW;
    const float dlgBottom = dlgTop  + kDlgH;

    // Dialog arka planı
    D2D1_ROUNDED_RECT dlgRR = { D2D1::RectF(dlgLeft, dlgTop, dlgRight, dlgBottom), kRadii, kRadii };
    m_renderTarget->FillRoundedRectangle(dlgRR, m_panelBgBrush);
    m_renderTarget->DrawRoundedRectangle(dlgRR, m_separatorBrush, 1.0f);

    // Başlık
    const wchar_t* title   = isWarning ? L"Silinemedi" : L"Fotoğrafı Sil";
    D2D1_RECT_F titleRect  = D2D1::RectF(dlgLeft + kPadX, dlgTop + kPadY,
                                          dlgRight - kPadX, dlgTop + kPadY + 22.0f);
    m_renderTarget->DrawText(title, static_cast<UINT32>(wcslen(title)),
                              m_valueFormat, titleRect, m_whiteBrush);

    // Mesaj metni
    const wchar_t* msg;
    std::wstring dynMsg;
    if (isWarning)
    {
        msg = L"Bu fotoğraf üzerinde kaydedilmemiş değişiklikler var.\n"
              L"Önce değişiklikleri kaydedin veya iptal edin.";
    }
    else
    {
        if (info && !info->filename.empty())
            dynMsg = L"“" + info->filename + L"” dosyasını Geri Dönüşüm Kutusu'na taşımak istediğinize emin misiniz?";
        else
            dynMsg = L"Bu fotoğrafı Geri Dönüşüm Kutusu'na taşımak istediğinize emin misiniz?";
        msg = dynMsg.c_str();
    }
    D2D1_RECT_F msgRect = D2D1::RectF(dlgLeft + kPadX, dlgTop + kPadY + 28.0f,
                                       dlgRight - kPadX, dlgBottom - kBtnH - kPadY - 8.0f);
    m_renderTarget->DrawText(msg, static_cast<UINT32>(wcslen(msg)),
                              m_labelFormat, msgRect, m_grayBrush);

    // Butonlar — alt kısım
    const float btnTop    = dlgBottom - kPadY - kBtnH;
    const float btnBottom = btnTop + kBtnH;

    auto DrawDlgBtn = [&](D2D1_RECT_F& outRect, float left, float right,
                           ID2D1SolidColorBrush* bg, const wchar_t* label, int btnId)
    {
        outRect = D2D1::RectF(left, btnTop, right, btnBottom);
        D2D1_ROUNDED_RECT rr = { outRect, 7.0f, 7.0f };
        m_renderTarget->FillRoundedRectangle(rr, bg);

        if (vs.deleteDlgPressedBtn == btnId)
        {
            float op = m_overlayBrush->GetOpacity();
            m_overlayBrush->SetOpacity(0.20f);
            m_renderTarget->FillRoundedRectangle(rr, m_overlayBrush);
            m_overlayBrush->SetOpacity(op);
        }
        else if (vs.deleteDlgHoverBtn == btnId)
        {
            float op = m_activeBrush->GetOpacity();
            m_activeBrush->SetOpacity(0.30f);
            m_renderTarget->FillRoundedRectangle(rr, m_activeBrush);
            m_activeBrush->SetOpacity(op);
        }

        m_renderTarget->DrawText(label, static_cast<UINT32>(wcslen(label)),
                                  m_btnFormat, outRect, m_whiteBrush);
    };

    if (isWarning)
    {
        // Tek buton: Tamam
        float btnLeft  = dlgLeft  + kPadX;
        float btnRight = dlgRight - kPadX;
        m_dlgCancelRect = {};
        DrawDlgBtn(m_dlgDeleteRect, btnLeft, btnRight, m_separatorBrush, L"Tamam", 2);
    }
    else
    {
        // İki buton: İptal | Sil
        float totalW  = kDlgW - 2.0f * kPadX;
        float btnW    = (totalW - kBtnGap) * 0.5f;
        float btn1L   = dlgLeft + kPadX;
        float btn1R   = btn1L + btnW;
        float btn2L   = btn1R + kBtnGap;
        float btn2R   = dlgRight - kPadX;

        DrawDlgBtn(m_dlgCancelRect, btn1L, btn1R, m_separatorBrush, L"İptal", 1);
        DrawDlgBtn(m_dlgDeleteRect, btn2L, btn2R, m_deleteBrush,    L"Sil",      2);
    }

    if (dlayer)
        m_renderTarget->PopLayer();
}

// ─── Yeniden Boyutlandır Dialogu ─────────────────────────────────────────────
// Buton ID'leri: 1=İptal, 2=Uygula, 3=px modu, 4=% modu,
//                5=W azalt, 6=W artır, 7=H azalt, 8=H artır, 9=oran kilidi

void Renderer::DrawResizeDialog(const ViewState& vs)
{
    m_resizeDlgVisible = false;
    if (!vs.showResizeDialog) return;
    if (!m_panelBgBrush || !m_separatorBrush || !m_whiteBrush || !m_grayBrush
     || !m_overlayBrush  || !m_btnFormat     || !m_labelFormat || !m_valueFormat
     || !m_activeBrush   || !m_saveBtnBrush) return;

    m_resizeDlgVisible = true;

    bool dlayer = (vs.dialogAlpha < 0.99f) && m_dialogLayer;
    if (dlayer)
    {
        D2D1_LAYER_PARAMETERS lp = D2D1::LayerParameters();
        lp.opacity = vs.dialogAlpha;
        m_renderTarget->PushLayer(lp, m_dialogLayer);
    }

    D2D1_SIZE_F sz = m_renderTarget->GetSize();

    // Tam ekran yarı saydam backdrop
    {
        float op = m_overlayBrush->GetOpacity();
        m_overlayBrush->SetOpacity(0.72f);
        m_renderTarget->FillRectangle(D2D1::RectF(0, 0, sz.width, sz.height), m_overlayBrush);
        m_overlayBrush->SetOpacity(op);
    }

    constexpr float kDlgW    = 420.0f;
    constexpr float kDlgH    = 252.0f;
    constexpr float kRadii   = 12.0f;
    constexpr float kPadX    = 20.0f;
    constexpr float kPadY    = 20.0f;
    constexpr float kBtnH    = 36.0f;
    constexpr float kBtnGap  = 10.0f;
    constexpr float kRowH    = 32.0f;
    constexpr float kSmBtnW  = 28.0f;
    constexpr float kSmBtnH  = 28.0f;

    const float dlgLeft   = (sz.width  - kDlgW) * 0.5f;
    const float dlgTop    = (sz.height - kDlgH) * 0.5f;
    const float dlgRight  = dlgLeft + kDlgW;
    const float dlgBottom = dlgTop  + kDlgH;

    // Dialog arka planı
    D2D1_ROUNDED_RECT dlgRR = { D2D1::RectF(dlgLeft, dlgTop, dlgRight, dlgBottom), kRadii, kRadii };
    m_renderTarget->FillRoundedRectangle(dlgRR, m_panelBgBrush);
    m_renderTarget->DrawRoundedRectangle(dlgRR, m_separatorBrush, 1.0f);

    // Başlık
    {
        const wchar_t* title = L"Yeniden Boyutlandır";
        D2D1_RECT_F r = D2D1::RectF(dlgLeft + kPadX, dlgTop + kPadY,
                                      dlgRight - kPadX, dlgTop + kPadY + 22.0f);
        m_renderTarget->DrawText(title, static_cast<UINT32>(wcslen(title)),
                                  m_valueFormat, r, m_whiteBrush);
    }

    // ── Mod toggle (px / %) ─────────────────────────────────────────────────
    const float toggleY  = dlgTop + 58.0f;
    const float toggleH  = 28.0f;
    const float toggleW  = 54.0f;

    auto DrawModeBtn = [&](D2D1_RECT_F& out, float x, const wchar_t* lbl, int id, bool active) {
        out = D2D1::RectF(x, toggleY, x + toggleW, toggleY + toggleH);
        D2D1_ROUNDED_RECT rr = { out, 6.0f, 6.0f };
        auto* bg = active ? m_activeBrush : m_separatorBrush;
        if (vs.resizeDlgHoverBtn == id && !active) {
            float op = m_activeBrush->GetOpacity();
            m_activeBrush->SetOpacity(0.35f);
            m_renderTarget->FillRoundedRectangle(rr, m_activeBrush);
            m_activeBrush->SetOpacity(op);
        } else {
            m_renderTarget->FillRoundedRectangle(rr, bg);
        }
        m_renderTarget->DrawText(lbl, static_cast<UINT32>(wcslen(lbl)),
                                  m_btnFormat, out, m_whiteBrush);
    };

    DrawModeBtn(m_resizeDlgModePxRect,  dlgLeft + kPadX,            L"px", 3, vs.resizeMode == 0);
    DrawModeBtn(m_resizeDlgModePctRect, dlgLeft + kPadX + toggleW + 6.0f, L"%",  4, vs.resizeMode == 1);

    // ── En-boy oranı kilidi ─────────────────────────────────────────────────
    {
        const float lockX = dlgRight - kPadX - 120.0f;
        m_resizeDlgLockRect = D2D1::RectF(lockX, toggleY, dlgRight - kPadX, toggleY + toggleH);
        D2D1_ROUNDED_RECT rr = { m_resizeDlgLockRect, 6.0f, 6.0f };
        auto* bg = vs.resizeLockAspect ? m_activeBrush : m_separatorBrush;
        if (vs.resizeDlgHoverBtn == 9 && !vs.resizeLockAspect) {
            float op = m_activeBrush->GetOpacity();
            m_activeBrush->SetOpacity(0.35f);
            m_renderTarget->FillRoundedRectangle(rr, m_activeBrush);
            m_activeBrush->SetOpacity(op);
        } else {
            m_renderTarget->FillRoundedRectangle(rr, bg);
        }
        const wchar_t* lockLbl = L"Oran koru";
        m_renderTarget->DrawText(lockLbl, static_cast<UINT32>(wcslen(lockLbl)),
                                  m_btnFormat, m_resizeDlgLockRect, m_whiteBrush);
    }

    // ── Alan satırı (W veya Yüzde / H) ─────────────────────────────────────
    const float labelX  = dlgLeft + kPadX;
    const float labelW  = 80.0f;
    const float minusX  = labelX + labelW + 4.0f;
    const float valX    = minusX + kSmBtnW + 4.0f;
    const float valW    = 100.0f;
    const float plusX   = valX + valW + 4.0f;

    auto DrawFieldRow = [&](D2D1_RECT_F& decOut, D2D1_RECT_F& incOut,
                             float rowTop, int decId, int incId,
                             const wchar_t* labelTxt, const wchar_t* valueTxt)
    {
        // Etiket
        D2D1_RECT_F labelR = D2D1::RectF(labelX, rowTop, labelX + labelW, rowTop + kRowH);
        m_renderTarget->DrawText(labelTxt, static_cast<UINT32>(wcslen(labelTxt)),
                                  m_labelFormat, labelR, m_grayBrush);

        // [−] butonu
        decOut = D2D1::RectF(minusX, rowTop + 2.0f, minusX + kSmBtnW, rowTop + 2.0f + kSmBtnH);
        {
            D2D1_ROUNDED_RECT rr = { decOut, 6.0f, 6.0f };
            bool pressed = vs.resizeDlgPressedBtn == decId;
            bool hover   = vs.resizeDlgHoverBtn == decId;
            auto* bg = pressed ? m_activeBrush : (hover ? m_separatorBrush : m_separatorBrush);
            m_renderTarget->FillRoundedRectangle(rr, bg);
            if (hover && !pressed) {
                float op = m_activeBrush->GetOpacity();
                m_activeBrush->SetOpacity(0.25f);
                m_renderTarget->FillRoundedRectangle(rr, m_activeBrush);
                m_activeBrush->SetOpacity(op);
            }
            m_renderTarget->DrawText(L"−", 1, m_btnFormat, decOut, m_whiteBrush);
        }

        // Değer göstergesi
        D2D1_RECT_F valR = D2D1::RectF(valX, rowTop, valX + valW, rowTop + kRowH);
        m_renderTarget->DrawText(valueTxt, static_cast<UINT32>(wcslen(valueTxt)),
                                  m_btnFormat, valR, m_whiteBrush);

        // [+] butonu
        incOut = D2D1::RectF(plusX, rowTop + 2.0f, plusX + kSmBtnW, rowTop + 2.0f + kSmBtnH);
        {
            D2D1_ROUNDED_RECT rr = { incOut, 6.0f, 6.0f };
            bool pressed = vs.resizeDlgPressedBtn == incId;
            bool hover   = vs.resizeDlgHoverBtn == incId;
            m_renderTarget->FillRoundedRectangle(rr, m_separatorBrush);
            if (hover && !pressed) {
                float op = m_activeBrush->GetOpacity();
                m_activeBrush->SetOpacity(0.25f);
                m_renderTarget->FillRoundedRectangle(rr, m_activeBrush);
                m_activeBrush->SetOpacity(op);
            } else if (pressed) {
                float op = m_activeBrush->GetOpacity();
                m_activeBrush->SetOpacity(0.6f);
                m_renderTarget->FillRoundedRectangle(rr, m_activeBrush);
                m_activeBrush->SetOpacity(op);
            }
            m_renderTarget->DrawText(L"+", 1, m_btnFormat, incOut, m_whiteBrush);
        }
    };

    const float row1Top = dlgTop + 100.0f;
    const float row2Top = dlgTop + 142.0f;

    if (vs.resizeMode == 0)
    {
        // Piksel modu: Genişlik + Yükseklik
        wchar_t wBuf[32], hBuf[32];
        _snwprintf_s(wBuf, 32, _TRUNCATE, L"%d px", vs.resizeW);
        _snwprintf_s(hBuf, 32, _TRUNCATE, L"%d px", vs.resizeH);
        DrawFieldRow(m_resizeDlgWDecRect, m_resizeDlgWIncRect, row1Top, 5, 6, L"Genişlik",   wBuf);
        DrawFieldRow(m_resizeDlgHDecRect, m_resizeDlgHIncRect, row2Top, 7, 8, L"Yükseklik",  hBuf);
        m_resizeDlgHDecRect = {};  // px modunda H row mevcut — sıfırlamıyoruz
    }
    else
    {
        // Yüzde modu: tek satır yüzde + sonuç bilgisi
        wchar_t pBuf[32];
        _snwprintf_s(pBuf, 32, _TRUNCATE, L"%d %%", vs.resizePct);
        DrawFieldRow(m_resizeDlgWDecRect, m_resizeDlgWIncRect, row1Top, 5, 6, L"Yüzde", pBuf);
        m_resizeDlgHDecRect = {};
        m_resizeDlgHIncRect = {};

        // Sonuç piksel boyutu
        wchar_t resBuf[64];
        _snwprintf_s(resBuf, 64, _TRUNCATE, L"→  %d × %d px", vs.resizeW, vs.resizeH);
        D2D1_RECT_F resR = D2D1::RectF(labelX, row2Top, dlgRight - kPadX, row2Top + kRowH);
        m_renderTarget->DrawText(resBuf, static_cast<UINT32>(wcslen(resBuf)),
                                  m_labelFormat, resR, m_grayBrush);
    }

    // Ayraç çizgisi
    const float sepY = dlgBottom - kBtnH - kPadY - 8.0f;
    m_renderTarget->DrawLine({dlgLeft + kPadX, sepY}, {dlgRight - kPadX, sepY},
                              m_separatorBrush, 1.0f);

    // ── Alt butonlar: İptal | Uygula ────────────────────────────────────────
    const float btnTop    = dlgBottom - kPadY - kBtnH;
    const float btnBottom = btnTop + kBtnH;
    const float totalBtnW = kDlgW - 2.0f * kPadX;
    const float btnW      = (totalBtnW - kBtnGap) * 0.5f;
    const float btn1L     = dlgLeft + kPadX;
    const float btn1R     = btn1L + btnW;
    const float btn2L     = btn1R + kBtnGap;
    const float btn2R     = dlgRight - kPadX;

    auto DrawMainBtn = [&](D2D1_RECT_F& out, float left, float right,
                            ID2D1SolidColorBrush* bg, const wchar_t* lbl, int id) {
        out = D2D1::RectF(left, btnTop, right, btnBottom);
        D2D1_ROUNDED_RECT rr = { out, 7.0f, 7.0f };
        m_renderTarget->FillRoundedRectangle(rr, bg);
        if (vs.resizeDlgPressedBtn == id) {
            float op = m_overlayBrush->GetOpacity();
            m_overlayBrush->SetOpacity(0.20f);
            m_renderTarget->FillRoundedRectangle(rr, m_overlayBrush);
            m_overlayBrush->SetOpacity(op);
        } else if (vs.resizeDlgHoverBtn == id) {
            float op = m_activeBrush->GetOpacity();
            m_activeBrush->SetOpacity(0.25f);
            m_renderTarget->FillRoundedRectangle(rr, m_activeBrush);
            m_activeBrush->SetOpacity(op);
        }
        m_renderTarget->DrawText(lbl, static_cast<UINT32>(wcslen(lbl)),
                                  m_btnFormat, out, m_whiteBrush);
    };

    DrawMainBtn(m_resizeDlgCancelRect, btn1L, btn1R, m_separatorBrush, L"İptal",  1);
    DrawMainBtn(m_resizeDlgApplyRect,  btn2L, btn2R, m_saveBtnBrush,   L"Uygula", 2);

    if (dlayer)
        m_renderTarget->PopLayer();
}

// ─── Serbest Döndürme Dialogu ────────────────────────────────────────────────
// Açı aralığı: -45°..+45°. Canlı D2D önizlemesi (piksel değişmez, Uygula'da kalıcı olur).
// Buton ID'leri: 1=−1°, 2=−0.1°, 3=+0.1°, 4=+1°, 5=Sıfırla, 6=İptal, 7=Uygula

void Renderer::DrawRotateFreeDialog(const ViewState& vs)
{
    m_rotFreeDlgVisible = false;
    if (!vs.showRotateFreeDialog) return;
    if (!m_panelBgBrush || !m_separatorBrush || !m_whiteBrush || !m_grayBrush
     || !m_overlayBrush  || !m_btnFormat     || !m_labelFormat || !m_valueFormat
     || !m_activeBrush   || !m_saveBtnBrush) return;

    m_rotFreeDlgVisible = true;

    bool dlayer = (vs.dialogAlpha < 0.99f) && m_dialogLayer;
    if (dlayer)
    {
        D2D1_LAYER_PARAMETERS lp = D2D1::LayerParameters();
        lp.opacity = vs.dialogAlpha;
        m_renderTarget->PushLayer(lp, m_dialogLayer);
    }

    D2D1_SIZE_F sz = m_renderTarget->GetSize();

    // Yarı saydam backdrop
    {
        float op = m_overlayBrush->GetOpacity();
        m_overlayBrush->SetOpacity(0.72f);
        m_renderTarget->FillRectangle(D2D1::RectF(0, 0, sz.width, sz.height), m_overlayBrush);
        m_overlayBrush->SetOpacity(op);
    }

    constexpr float kDlgW  = 340.0f;
    constexpr float kDlgH  = 200.0f;
    constexpr float kRadii = 12.0f;
    constexpr float kPadX  = 20.0f;

    const float dlgLeft   = (sz.width  - kDlgW) * 0.5f;
    const float dlgTop    = (sz.height - kDlgH) * 0.5f;
    const float dlgRight  = dlgLeft + kDlgW;
    const float dlgBottom = dlgTop  + kDlgH;

    D2D1_ROUNDED_RECT dlgRR = { D2D1::RectF(dlgLeft, dlgTop, dlgRight, dlgBottom), kRadii, kRadii };
    m_renderTarget->FillRoundedRectangle(dlgRR, m_panelBgBrush);
    m_renderTarget->DrawRoundedRectangle(dlgRR, m_separatorBrush, 1.0f);

    // Başlık
    {
        const wchar_t* title = L"Serbest Döndürme";
        D2D1_RECT_F r = D2D1::RectF(dlgLeft + kPadX, dlgTop + 16.0f,
                                      dlgRight - kPadX, dlgTop + 38.0f);
        m_renderTarget->DrawText(title, static_cast<UINT32>(wcslen(title)),
                                  m_valueFormat, r, m_whiteBrush);
    }

    // ── Slider ──────────────────────────────────────────────────────────────
    constexpr float kSliderPadX  = 30.0f;
    constexpr float kSliderY     = 62.0f;   // rail merkez y (dialog'a göre)
    constexpr float kSliderH     = 6.0f;
    constexpr float kKnobR       = 9.0f;
    constexpr float kRange       = 90.0f;   // -45..+45 = 90°

    const float railLeft  = dlgLeft + kSliderPadX;
    const float railRight = dlgRight - kSliderPadX;
    const float railW     = railRight - railLeft;
    const float railTop   = dlgTop + kSliderY - kSliderH * 0.5f;
    const float railBot   = dlgTop + kSliderY + kSliderH * 0.5f;
    const float knobCX    = railLeft + (vs.rotateFreeAngle + 45.0f) / kRange * railW;
    const float knobCY    = dlgTop + kSliderY;

    // Slider hit testi alanı (knob dahil)
    m_rotFreeDlgSliderRect = D2D1::RectF(railLeft - kKnobR, dlgTop + kSliderY - kKnobR,
                                          railRight + kKnobR, dlgTop + kSliderY + kKnobR);

    // Rail arka plan
    {
        D2D1_ROUNDED_RECT rrRail = { D2D1::RectF(railLeft, railTop, railRight, railBot), 3.0f, 3.0f };
        m_renderTarget->FillRoundedRectangle(rrRail, m_separatorBrush);
    }
    // Rail sol taraf (dolu kısım)
    {
        float fillRight = min(knobCX, railRight);
        if (fillRight > railLeft)
        {
            D2D1_ROUNDED_RECT rrFill = { D2D1::RectF(railLeft, railTop, fillRight, railBot), 3.0f, 3.0f };
            m_renderTarget->FillRoundedRectangle(rrFill, m_activeBrush);
        }
    }
    // Knob (beyaz daire)
    m_renderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knobCX, knobCY), kKnobR, kKnobR), m_whiteBrush);
    m_renderTarget->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(knobCX, knobCY), kKnobR, kKnobR),
                                 m_separatorBrush, 1.0f);

    // Slider uç etiketleri
    {
        D2D1_RECT_F rL = D2D1::RectF(railLeft - 18.0f, dlgTop + kSliderY + 14.0f,
                                       railLeft + 18.0f, dlgTop + kSliderY + 30.0f);
        D2D1_RECT_F rR = D2D1::RectF(railRight - 18.0f, dlgTop + kSliderY + 14.0f,
                                       railRight + 18.0f, dlgTop + kSliderY + 30.0f);
        m_renderTarget->DrawText(L"-45°", 5, m_toggleFormat, rL, m_grayBrush);
        m_renderTarget->DrawText(L"+45°", 5, m_toggleFormat, rR, m_grayBrush);
    }

    // ── Açı göstergesi ───────────────────────────────────────────────────────
    {
        wchar_t angleTxt[16];
        swprintf_s(angleTxt, L"%.1f°", vs.rotateFreeAngle);
        D2D1_RECT_F r = D2D1::RectF(dlgLeft, dlgTop + 86.0f, dlgRight, dlgTop + 108.0f);
        m_renderTarget->DrawText(angleTxt, static_cast<UINT32>(wcslen(angleTxt)),
                                  m_btnFormat, r, m_whiteBrush);
    }

    // ── Ayar butonları: −1° | −0.1° | +0.1° | +1° ──────────────────────────
    constexpr float kAdjBtnW  = 58.0f;
    constexpr float kAdjBtnH  = 28.0f;
    constexpr float kAdjBtnGap = 8.0f;
    constexpr float kAdjTotalW = kAdjBtnW * 4.0f + kAdjBtnGap * 3.0f;
    const float adjLeft = dlgLeft + (kDlgW - kAdjTotalW) * 0.5f;
    const float adjTop  = dlgTop + 112.0f;

    auto DrawAdjBtn = [&](D2D1_RECT_F& out, float x, const wchar_t* lbl, int id)
    {
        out = D2D1::RectF(x, adjTop, x + kAdjBtnW, adjTop + kAdjBtnH);
        D2D1_ROUNDED_RECT rr = { out, 6.0f, 6.0f };
        bool pressed = vs.rotateFreeDlgPressBtn == id;
        bool hover   = vs.rotateFreeDlgHoverBtn == id;
        m_renderTarget->FillRoundedRectangle(rr, m_separatorBrush);
        if (pressed)
        {
            float op = m_activeBrush->GetOpacity();
            m_activeBrush->SetOpacity(0.6f);
            m_renderTarget->FillRoundedRectangle(rr, m_activeBrush);
            m_activeBrush->SetOpacity(op);
        }
        else if (hover)
        {
            float op = m_activeBrush->GetOpacity();
            m_activeBrush->SetOpacity(0.28f);
            m_renderTarget->FillRoundedRectangle(rr, m_activeBrush);
            m_activeBrush->SetOpacity(op);
        }
        m_renderTarget->DrawText(lbl, static_cast<UINT32>(wcslen(lbl)), m_btnFormat, out, m_whiteBrush);
    };

    float ax = adjLeft;
    DrawAdjBtn(m_rotFreeDlgCoarseDecRect, ax,                          L"-1°",   1); ax += kAdjBtnW + kAdjBtnGap;
    DrawAdjBtn(m_rotFreeDlgFineDecRect,   ax,                          L"-0.1°", 2); ax += kAdjBtnW + kAdjBtnGap;
    DrawAdjBtn(m_rotFreeDlgFineIncRect,   ax,                          L"+0.1°", 3); ax += kAdjBtnW + kAdjBtnGap;
    DrawAdjBtn(m_rotFreeDlgCoarseIncRect, ax,                          L"+1°",   4);

    // ── Alt butonlar: Sıfırla | İptal | Uygula ───────────────────────────────
    constexpr float kBtnH   = 34.0f;
    constexpr float kBtnGap =  8.0f;
    const float btnW   = (kDlgW - 2.0f * kPadX - 2.0f * kBtnGap) / 3.0f;
    const float btnTop = dlgTop + 152.0f;

    auto DrawMainBtn = [&](D2D1_RECT_F& out, float left, ID2D1SolidColorBrush* bg,
                            const wchar_t* lbl, int id)
    {
        out = D2D1::RectF(left, btnTop, left + btnW, btnTop + kBtnH);
        D2D1_ROUNDED_RECT rr = { out, 7.0f, 7.0f };
        m_renderTarget->FillRoundedRectangle(rr, bg);
        if (vs.rotateFreeDlgPressBtn == id)
        {
            float op = m_overlayBrush->GetOpacity();
            m_overlayBrush->SetOpacity(0.20f);
            m_renderTarget->FillRoundedRectangle(rr, m_overlayBrush);
            m_overlayBrush->SetOpacity(op);
        }
        else if (vs.rotateFreeDlgHoverBtn == id)
        {
            float op = m_activeBrush->GetOpacity();
            m_activeBrush->SetOpacity(0.25f);
            m_renderTarget->FillRoundedRectangle(rr, m_activeBrush);
            m_activeBrush->SetOpacity(op);
        }
        m_renderTarget->DrawText(lbl, static_cast<UINT32>(wcslen(lbl)), m_btnFormat, out, m_whiteBrush);
    };

    const float btn1L = dlgLeft + kPadX;
    const float btn2L = btn1L + btnW + kBtnGap;
    const float btn3L = btn2L + btnW + kBtnGap;
    DrawMainBtn(m_rotFreeDlgResetRect,  btn1L, m_separatorBrush, L"Sıfırla", 5);
    DrawMainBtn(m_rotFreeDlgCancelRect, btn2L, m_separatorBrush, L"İptal",   6);
    DrawMainBtn(m_rotFreeDlgApplyRect,  btn3L, m_saveBtnBrush,   L"Uygula",  7);

    if (dlayer)
        m_renderTarget->PopLayer();
}

// ─── Kırpma Dialogu ───────────────────────────────────────────────────────────
// Görüntü üzerinde kırpma seçimi + oran butonları + İptal/Uygula

void Renderer::DrawCropDialog(const ViewState& vs)
{
    m_cropDlgVisible = false;
    if (!vs.showCropDialog) return;
    if (!m_panelBgBrush || !m_separatorBrush || !m_whiteBrush || !m_grayBrush
     || !m_overlayBrush  || !m_btnFormat     || !m_activeBrush || !m_saveBtnBrush) return;
    if (m_imageDisplayRect.right <= m_imageDisplayRect.left) return;

    m_cropDlgVisible = true;

    bool dlayer = (vs.dialogAlpha < 0.99f) && m_dialogLayer;
    if (dlayer)
    {
        D2D1_LAYER_PARAMETERS lp = D2D1::LayerParameters();
        lp.opacity = vs.dialogAlpha;
        m_renderTarget->PushLayer(lp, m_dialogLayer);
    }

    const float imgX0 = m_imageDisplayRect.left;
    const float imgY0 = m_imageDisplayRect.top;
    const float imgW  = m_imageDisplayRect.right  - m_imageDisplayRect.left;
    const float imgH  = m_imageDisplayRect.bottom - m_imageDisplayRect.top;

    // Kırpma rect'ini piksel koordinatlarına çevir
    const float cx0 = imgX0 + vs.cropX0 * imgW;
    const float cy0 = imgY0 + vs.cropY0 * imgH;
    const float cx1 = imgX0 + vs.cropX1 * imgW;
    const float cy1 = imgY0 + vs.cropY1 * imgH;

    D2D1_SIZE_F sz = m_renderTarget->GetSize();

    // ── Dış karartma (kırpma dışı alan) ─────────────────────────────────────
    {
        float op = m_overlayBrush->GetOpacity();
        m_overlayBrush->SetOpacity(0.60f);
        // Üst
        if (cy0 > 0.0f)
            m_renderTarget->FillRectangle(D2D1::RectF(0, 0, sz.width, cy0), m_overlayBrush);
        // Alt
        if (cy1 < sz.height)
            m_renderTarget->FillRectangle(D2D1::RectF(0, cy1, sz.width, sz.height), m_overlayBrush);
        // Sol (kırpma yüksekliği arasında)
        if (cx0 > 0.0f)
            m_renderTarget->FillRectangle(D2D1::RectF(0, cy0, cx0, cy1), m_overlayBrush);
        // Sağ (kırpma yüksekliği arasında)
        if (cx1 < sz.width)
            m_renderTarget->FillRectangle(D2D1::RectF(cx1, cy0, sz.width, cy1), m_overlayBrush);
        m_overlayBrush->SetOpacity(op);
    }

    // ── Kırpma sınır çizgisi ─────────────────────────────────────────────────
    m_renderTarget->DrawRectangle(D2D1::RectF(cx0, cy0, cx1, cy1), m_whiteBrush, 1.5f);

    // ── Üçte bir kılavuz çizgileri ────────────────────────────────────────────
    {
        float op = m_whiteBrush->GetOpacity();
        m_whiteBrush->SetOpacity(op * 0.28f);
        float dx = (cx1 - cx0) / 3.0f;
        float dy = (cy1 - cy0) / 3.0f;
        m_renderTarget->DrawLine({cx0 + dx, cy0}, {cx0 + dx, cy1}, m_whiteBrush, 0.8f);
        m_renderTarget->DrawLine({cx0 + 2.0f * dx, cy0}, {cx0 + 2.0f * dx, cy1}, m_whiteBrush, 0.8f);
        m_renderTarget->DrawLine({cx0, cy0 + dy}, {cx1, cy0 + dy}, m_whiteBrush, 0.8f);
        m_renderTarget->DrawLine({cx0, cy0 + 2.0f * dy}, {cx1, cy0 + 2.0f * dy}, m_whiteBrush, 0.8f);
        m_whiteBrush->SetOpacity(op);
    }

    // ── Köşe ve kenar tutamaçları ─────────────────────────────────────────────
    constexpr float kHW  = 9.0f;   // tutamaç yarı genişliği
    constexpr float kHT  = 3.0f;   // tutamaç kalınlığı
    float midX = (cx0 + cx1) * 0.5f;
    float midY = (cy0 + cy1) * 0.5f;

    auto DrawHandle = [&](float hx, float hy) {
        m_renderTarget->FillRectangle(
            D2D1::RectF(hx - kHW, hy - kHT, hx + kHW, hy + kHT), m_whiteBrush);
        m_renderTarget->FillRectangle(
            D2D1::RectF(hx - kHT, hy - kHW, hx + kHT, hy + kHW), m_whiteBrush);
    };

    // 4 köşe
    DrawHandle(cx0, cy0);
    DrawHandle(cx1, cy0);
    DrawHandle(cx0, cy1);
    DrawHandle(cx1, cy1);
    // 4 kenar ortası
    DrawHandle(midX, cy0);
    DrawHandle(midX, cy1);
    DrawHandle(cx0, midY);
    DrawHandle(cx1, midY);

    // ── Alt panel: oran butonları + İptal/Uygula ──────────────────────────────
    constexpr float kBarH    = 52.0f;
    constexpr float kBarW    = 512.0f;
    constexpr float kRadii   = 10.0f;
    constexpr float kBtnH    = 32.0f;
    constexpr float kRatioW  = 60.0f;
    constexpr float kActionW = 72.0f;
    constexpr float kGap     =  6.0f;
    constexpr float kPadX    = 12.0f;

    const float barBottom = sz.height - vs.stripAnimHeight - 16.0f;
    const float barTop    = barBottom - kBarH;
    const float barLeft   = (sz.width - vs.panelAnimWidth - kBarW) * 0.5f;
    const float barRight  = barLeft + kBarW;

    D2D1_ROUNDED_RECT barRR = { D2D1::RectF(barLeft, barTop, barRight, barBottom), kRadii, kRadii };
    m_renderTarget->FillRoundedRectangle(barRR, m_panelBgBrush);
    m_renderTarget->DrawRoundedRectangle(barRR, m_separatorBrush, 1.0f);

    const float btnTop = barTop + (kBarH - kBtnH) * 0.5f;

    // Oran butonları: Serbest | 1:1 | 4:3 | 3:2 | 16:9
    static const wchar_t* kRatioLabels[] = { L"Serbest", L"1:1", L"4:3", L"3:2", L"16:9" };
    float rx = barLeft + kPadX;
    for (int i = 0; i < 5; ++i)
    {
        m_cropDlgRatioRects[i] = D2D1::RectF(rx, btnTop, rx + kRatioW, btnTop + kBtnH);
        D2D1_ROUNDED_RECT rr = { m_cropDlgRatioRects[i], 7.0f, 7.0f };
        bool active  = (vs.cropAspectMode == i);
        bool pressed = (vs.cropDlgPressedBtn == i + 1);
        bool hover   = (vs.cropDlgHoverBtn   == i + 1);

        if (active)
        {
            float op = m_activeBrush->GetOpacity();
            m_activeBrush->SetOpacity(0.85f);
            m_renderTarget->FillRoundedRectangle(rr, m_activeBrush);
            m_activeBrush->SetOpacity(op);
        }
        else
        {
            m_renderTarget->FillRoundedRectangle(rr, m_separatorBrush);
            if (pressed)
            {
                float op = m_activeBrush->GetOpacity();
                m_activeBrush->SetOpacity(0.50f);
                m_renderTarget->FillRoundedRectangle(rr, m_activeBrush);
                m_activeBrush->SetOpacity(op);
            }
            else if (hover)
            {
                float op = m_activeBrush->GetOpacity();
                m_activeBrush->SetOpacity(0.25f);
                m_renderTarget->FillRoundedRectangle(rr, m_activeBrush);
                m_activeBrush->SetOpacity(op);
            }
        }
        m_renderTarget->DrawText(kRatioLabels[i], static_cast<UINT32>(wcslen(kRatioLabels[i])),
                                  m_btnFormat, m_cropDlgRatioRects[i], m_whiteBrush);
        rx += kRatioW + kGap;
    }

    // İptal butonu
    const float cancelLeft = barRight - kPadX - kActionW * 2.0f - kGap;
    m_cropDlgCancelRect = D2D1::RectF(cancelLeft, btnTop, cancelLeft + kActionW, btnTop + kBtnH);
    {
        D2D1_ROUNDED_RECT rr = { m_cropDlgCancelRect, 7.0f, 7.0f };
        bool pressed = (vs.cropDlgPressedBtn == 6);
        bool hover   = (vs.cropDlgHoverBtn   == 6);
        m_renderTarget->FillRoundedRectangle(rr, m_separatorBrush);
        if (pressed || hover)
        {
            float op = m_activeBrush->GetOpacity();
            m_activeBrush->SetOpacity(pressed ? 0.50f : 0.25f);
            m_renderTarget->FillRoundedRectangle(rr, m_activeBrush);
            m_activeBrush->SetOpacity(op);
        }
        m_renderTarget->DrawText(L"İptal", 5, m_btnFormat, m_cropDlgCancelRect, m_whiteBrush);
    }

    // Uygula butonu
    const float applyLeft = cancelLeft + kActionW + kGap;
    m_cropDlgApplyRect = D2D1::RectF(applyLeft, btnTop, applyLeft + kActionW, btnTop + kBtnH);
    {
        D2D1_ROUNDED_RECT rr = { m_cropDlgApplyRect, 7.0f, 7.0f };
        bool pressed = (vs.cropDlgPressedBtn == 7);
        bool hover   = (vs.cropDlgHoverBtn   == 7);
        m_renderTarget->FillRoundedRectangle(rr, m_saveBtnBrush);
        if (pressed)
        {
            float op = m_overlayBrush->GetOpacity();
            m_overlayBrush->SetOpacity(0.30f);
            m_renderTarget->FillRoundedRectangle(rr, m_overlayBrush);
            m_overlayBrush->SetOpacity(op);
        }
        else if (hover)
        {
            float op = m_whiteBrush->GetOpacity();
            m_whiteBrush->SetOpacity(0.12f);
            m_renderTarget->FillRoundedRectangle(rr, m_whiteBrush);
            m_whiteBrush->SetOpacity(op);
        }
        m_renderTarget->DrawText(L"Uygula", 6, m_btnFormat, m_cropDlgApplyRect, m_whiteBrush);
    }

    if (dlayer)
        m_renderTarget->PopLayer();
}

void Renderer::Resize(UINT width, UINT height)
{
    if (m_renderTarget)
        m_renderTarget->Resize(D2D1::SizeU(width, height));
}

// ─── Thumbnail Cache ──────────────────────────────────────────────────────────

void Renderer::LoadThumbnail(const std::wstring& path, const uint8_t* pixels, UINT w, UINT h)
{
    if (path.empty() || !pixels || w == 0 || h == 0) return;
    if (FAILED(CreateDeviceResources()) || !m_renderTarget) return;

    // Mevcut kaydı varsa önce kaldır (güncelleme)
    auto existing = m_thumbCache.find(path);
    if (existing != m_thumbCache.end())
    {
        if (existing->second) existing->second->Release();
        m_thumbCache.erase(existing);
        auto it = std::find(m_thumbOrder.begin(), m_thumbOrder.end(), path);
        if (it != m_thumbOrder.end()) m_thumbOrder.erase(it);
    }

    // LRU — önbellek doluysa en eski girdiyi çıkar
    if (static_cast<int>(m_thumbCache.size()) >= StripLayout::ThumbCacheMax)
    {
        const std::wstring& oldest = m_thumbOrder.front();
        auto it = m_thumbCache.find(oldest);
        if (it != m_thumbCache.end())
        {
            if (it->second) it->second->Release();
            m_thumbCache.erase(it);
        }
        m_thumbOrder.pop_front();
    }

    D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
    );
    ID2D1Bitmap* bmp = nullptr;
    if (SUCCEEDED(m_renderTarget->CreateBitmap(D2D1::SizeU(w, h), pixels, w * 4, props, &bmp)) && bmp)
    {
        m_thumbCache[path] = bmp;
        m_thumbOrder.push_back(path);
    }
}

bool Renderer::HasThumbnail(const std::wstring& path) const
{
    return m_thumbCache.count(path) > 0;
}

bool Renderer::ShowThumbnailAsPlaceholder(const std::wstring& path)
{
    auto it = m_thumbCache.find(path);
    if (it == m_thumbCache.end() || !it->second) return false;

    if (m_bitmap) { m_bitmap->Release(); m_bitmap = nullptr; }
    it->second->AddRef();
    m_bitmap    = it->second;
    m_imagePath = path;
    return true;
}

void Renderer::ClearThumbnails()
{
    for (auto& [path, bmp] : m_thumbCache) if (bmp) bmp->Release();
    m_thumbCache.clear();
    m_thumbOrder.clear();
}

void Renderer::SetStripSlots(const std::vector<std::wstring>& paths, int currentIdx)
{
    m_stripPaths      = paths;
    m_stripCurrentIdx = currentIdx;
    m_thumbRects.assign(paths.size(), D2D1::RectF(0, 0, 0, 0));
}

int Renderer::GetThumbClickOffset(float x, float y) const
{
    for (int i = 0; i < static_cast<int>(m_thumbRects.size()); ++i)
    {
        const D2D1_RECT_F& r = m_thumbRects[i];
        if (r.right <= r.left) continue;  // henüz çizilmedi
        if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom)
            return i - m_stripCurrentIdx;
    }
    return INT_MIN;
}

// ─── Thumbnail Strip ──────────────────────────────────────────────────────────

void Renderer::DrawThumbnailStrip(const ViewState& vs)
{
    if (!m_renderTarget || !m_overlayBrush || !m_panelBgBrush || !m_separatorBrush) return;
    if (vs.stripAnimHeight < 1.0f) return;

    D2D1_SIZE_F sz    = m_renderTarget->GetSize();
    float availW      = sz.width - vs.panelAnimWidth;
    float stripY      = sz.height - vs.stripAnimHeight;

    // Clip: strip alanının dışına çizim çıkmasını engelle
    D2D1_RECT_F stripRect = D2D1::RectF(0.0f, stripY, availW, sz.height);
    m_renderTarget->PushAxisAlignedClip(stripRect, D2D1_ANTIALIAS_MODE_ALIASED);

    // Arka plan (#1A1A1A, tam opak)
    m_renderTarget->FillRectangle(stripRect, m_panelBgBrush);

    // Üst kenar çizgisi (#333333)
    m_renderTarget->DrawLine(
        D2D1::Point2F(0.0f, stripY),
        D2D1::Point2F(availW, stripY),
        m_separatorBrush, 1.0f
    );

    int n = static_cast<int>(m_stripPaths.size());
    if (n == 0)
    {
        m_renderTarget->PopAxisAlignedClip();
        return;
    }

    // Her thumbnail için genişlik hesapla (yüklüyse oransal, değilse varsayılan 100px)
    constexpr float kThumbH = StripLayout::ThumbH;
    constexpr float kPadX   = StripLayout::PadX;
    constexpr float kDefW   = 100.0f;
    constexpr float kMaxW   = 130.0f;

    std::vector<float> widths(n, kDefW);
    for (int i = 0; i < n; ++i)
    {
        auto it = m_thumbCache.find(m_stripPaths[i]);
        if (it != m_thumbCache.end() && it->second)
        {
            D2D1_SIZE_F bmpSz = it->second->GetSize();
            if (bmpSz.height > 0.0f)
            {
                float w = kThumbH * bmpSz.width / bmpSz.height;
                widths[i] = (w > kMaxW) ? kMaxW : w;
            }
        }
    }

    float thumbY = stripY + (vs.stripAnimHeight - kThumbH) * 0.5f;

    if (static_cast<int>(m_thumbRects.size()) != n)
        m_thumbRects.assign(n, D2D1::RectF(0, 0, 0, 0));

    // Mevcut fotoğraf (m_stripCurrentIdx) her zaman strip'in yatay ortasında konumlanır.
    // Slotlar orta noktadan dışarıya doğru yerleştirilir.
    std::vector<float> positions(n, 0.0f);
    {
        float curW  = widths[m_stripCurrentIdx];
        float curX  = availW * 0.5f - curW * 0.5f;
        positions[m_stripCurrentIdx] = curX;

        // Sola doğru
        float x = curX;
        for (int i = m_stripCurrentIdx - 1; i >= 0; --i)
        {
            x -= widths[i] + kPadX;
            positions[i] = x;
        }
        // Sağa doğru
        x = curX + curW + kPadX;
        for (int i = m_stripCurrentIdx + 1; i < n; ++i)
        {
            positions[i] = x;
            x += widths[i] + kPadX;
        }
    }

    for (int i = 0; i < n; ++i)
    {
        float w = widths[i];
        D2D1_RECT_F r = D2D1::RectF(positions[i], thumbY, positions[i] + w, thumbY + kThumbH);

        if (m_stripPaths[i].empty())
        {
            // Fotoğraf yok (sınır dışı) — boş slot, hit-test devre dışı
            m_thumbRects[i] = D2D1::RectF(0, 0, 0, 0);
            // Strip arka planıyla aynı renk → görünmez
            m_renderTarget->FillRectangle(r, m_panelBgBrush);
        }
        else
        {
            m_thumbRects[i] = r;

            auto it = m_thumbCache.find(m_stripPaths[i]);
            if (it != m_thumbCache.end() && it->second)
            {
                m_renderTarget->DrawBitmap(it->second, r, 1.0f,
                    D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            }
            else
            {
                // Yükleniyor placeholder (gri)
                m_renderTarget->FillRectangle(r, m_overlayBrush);
            }

            // Aktif thumbnail: beyaz çerçeve (2px)
            if (i == m_stripCurrentIdx && m_whiteBrush)
                m_renderTarget->DrawRectangle(r, m_whiteBrush, 2.0f);
        }

    }

    m_renderTarget->PopAxisAlignedClip();
}

// ─── Strip Toggle Pill ────────────────────────────────────────────────────────

void Renderer::DrawStripToggle(const ViewState& vs)
{
    if (!m_renderTarget || !m_overlayBrush || !m_whiteBrush || !m_toggleFormat) return;
    if (vs.imageTotal <= 0) { m_stripToggleVisible = false; return; }

    D2D1_SIZE_F sz = m_renderTarget->GetSize();
    float availW   = sz.width - vs.panelAnimWidth;

    constexpr float kW      = StripLayout::ToggleW;
    constexpr float kH      = StripLayout::ToggleH;
    constexpr float kMargin = 10.0f;

    // Sol alt köşe, strip'in üstüne yapışık
    float x0 = kMargin;
    float y0 = sz.height - vs.stripAnimHeight - kH - kMargin;
    float x1 = x0 + kW;
    float y1 = y0 + kH;

    D2D1_RECT_F rect = D2D1::RectF(x0, y0, x1, y1);
    D2D1_ROUNDED_RECT rr = { rect, 5.0f, 5.0f };

    // Info butonu ile aynı stil: koyu dolgu, basılıyken açık tint, her zaman ince border
    auto* fillBrush = vs.toggleBtnPressed ? m_activeBrush : m_panelBgBrush;
    m_renderTarget->FillRoundedRectangle(rr, fillBrush);
    m_renderTarget->DrawRoundedRectangle(rr, m_separatorBrush, 1.0f);

    // ▼ = strip açık (kapat), ▲ = strip kapalı (aç)
    const wchar_t* arrow = vs.showThumbStrip ? L"\u25BC" : L"\u25B2";
    m_renderTarget->DrawText(arrow, 1, m_toggleFormat, rect, m_whiteBrush);

    m_stripToggleRect    = rect;
    m_stripToggleVisible = true;
}

// ─── OSM Harita Tile ──────────────────────────────────────────────────────────

// Ham BGRA pre-mul piksellerden tile yükle — decode background thread'de yapıldı,
// burada sadece D2D bitmap oluşturulur (çok hızlı, UI thread'i bloklamaz).
void Renderer::UploadMapTileRaw(int zoom, int x, int y,
                                const uint8_t* bgra, UINT w, UINT h)
{
    if (!m_renderTarget || !bgra || w == 0 || h == 0) return;

    MapTileKey key{ zoom, x, y };

    // Tile zaten cache'teyse tekrar yükleme — OSM verileri değişmez
    if (m_mapTileCache.count(key)) return;

    D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
    );
    ID2D1Bitmap* bmp = nullptr;
    if (SUCCEEDED(m_renderTarget->CreateBitmap(D2D1::SizeU(w, h), bgra, w * 4, props, &bmp)) && bmp)
    {
        // Cache 60 tile (~15MB) sınırını aşarsa en eski girdiyi at
        constexpr size_t kMaxTiles = 60;
        if (m_mapTileCache.size() >= kMaxTiles)
        {
            auto oldest = m_mapTileCache.begin();
            if (oldest->second) oldest->second->Release();
            m_mapTileCache.erase(oldest);
        }
        m_mapTileCache[key] = bmp;
    }
}

// ─── OSM Harita Önizlemesi ────────────────────────────────────────────────────

void Renderer::DrawMapPreview(float x0, float /*x1*/, float y, const ViewState& vs, const ImageInfo* info)
{
    if (!m_renderTarget || !info || !info->hasGpsDecimal) return;

    constexpr float kW      = 288.0f;
    constexpr float kH      = 150.0f;
    constexpr float kRadius =   6.0f;
    constexpr int   kZoom   =  14;

    const float px0 = x0;
    const float py0 = y;
    const float px1 = px0 + kW;
    const float py1 = py0 + kH;

    D2D1_RECT_F       previewRect = D2D1::RectF(px0, py0, px1, py1);
    D2D1_ROUNDED_RECT rr          = { previewRect, kRadius, kRadius };

    m_mapPreviewRect    = previewRect;
    m_mapPreviewVisible = true;

    // ── Placeholder arka planı ─────────────────────────────────────────────
    ID2D1SolidColorBrush* bgBrush = nullptr;
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0x2A2A2A), &bgBrush);
    if (bgBrush) { m_renderTarget->FillRoundedRectangle(rr, bgBrush); bgBrush->Release(); }

    // ── Tile koordinatları ─────────────────────────────────────────────────
    int cx, cy, pinX, pinY;
    LatLonToTileXY(info->gpsLatDecimal, info->gpsLonDecimal, kZoom, cx, cy);
    LatLonToPixelInTile(info->gpsLatDecimal, info->gpsLonDecimal, kZoom,
                        cx, cy, pinX, pinY);

    // 3×2 grid: sütunlar cx-1, cx, cx+1 — satırlar cy-1, cy
    // GPS mosaic koordinatı: (256 + pinX, 256 + pinY)
    // Viewport'un sol üst köşesi mosaic'te: (viewLeft, viewTop)
    const float viewLeft = 256.0f + static_cast<float>(pinX) - kW * 0.5f;
    const float viewTop  = 256.0f + static_cast<float>(pinY) - kH * 0.5f;

    // ── Tile'ları çiz (rounded clip geometry layer içinde) ─────────────────
    ID2D1RoundedRectangleGeometry* clipGeom = nullptr;
    m_factory->CreateRoundedRectangleGeometry(rr, &clipGeom);
    if (clipGeom)
    {
        m_renderTarget->PushLayer(D2D1::LayerParameters(previewRect, clipGeom), nullptr);

        for (int row = 0; row < 2; ++row)
        {
            for (int col = 0; col < 3; ++col)
            {
                MapTileKey key{ kZoom, cx - 1 + col, cy - 1 + row };
                auto it = m_mapTileCache.find(key);
                if (it == m_mapTileCache.end() || !it->second) continue;

                const float tx = px0 + (col * 256.0f - viewLeft);
                const float ty = py0 + (row * 256.0f - viewTop);
                m_renderTarget->DrawBitmap(it->second, D2D1::RectF(tx, ty, tx + 256.0f, ty + 256.0f));
            }
        }

        m_renderTarget->PopLayer();
        clipGeom->Release();
    }

    // Hiç tile yüklü değilse "Loading map..." göster
    bool anyTile = false;
    for (int r = 0; r < 2 && !anyTile; ++r)
        for (int c = 0; c < 3 && !anyTile; ++c)
            anyTile = m_mapTileCache.count({ kZoom, cx - 1 + c, cy - 1 + r }) > 0;

    if (!anyTile && m_labelFormat && m_grayBrush)
    {
        m_renderTarget->DrawText(
            L"Loading map...", 14, m_labelFormat, previewRect, m_grayBrush);
    }

    // ── Kırmızı pin — previewRect'in tam ortası ────────────────────────────
    constexpr float kPinR      = 7.0f;
    constexpr float kPinBorder = 2.0f;
    const float pinSx = px0 + kW * 0.5f;
    const float pinSy = py0 + kH * 0.5f;
    D2D1_ELLIPSE pinEllipse = D2D1::Ellipse(D2D1::Point2F(pinSx, pinSy), kPinR, kPinR);

    ID2D1SolidColorBrush* redBrush = nullptr;
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0xE53935), &redBrush);
    if (redBrush)
    {
        m_renderTarget->FillEllipse(pinEllipse, redBrush);
        redBrush->Release();
    }
    if (m_whiteBrush) m_renderTarget->DrawEllipse(pinEllipse, m_whiteBrush, kPinBorder);

    // ── Önizleme dış çerçevesi ─────────────────────────────────────────────
    if (m_separatorBrush) m_renderTarget->DrawRoundedRectangle(rr, m_separatorBrush, 1.0f);
}
