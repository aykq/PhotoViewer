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
    }
}

Renderer::~Renderer()
{
    DiscardDeviceResources();

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

    if (m_bitmap)           { m_bitmap->Release();           m_bitmap = nullptr; }
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

            // Konum adı (Nominatim reverse geocoding)
            DrawRow(L"Location", info->gpsLocationName);
            DrawRow(L"Altitude", info->gpsAltitude);

            // OSM harita önizlemesi — decimal koordinat varsa
            if (info->hasGpsDecimal)
            {
                y += 8.0f;
                DrawMapPreview(x0, x1, y, vs, info);
                y += 150.0f + 8.0f;
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

        m_renderTarget->DrawBitmap(
            activeBitmap,
            D2D1::RectF(destX, destY, destX + destW, destY + destH)
        );
    }

    // Overlaylar (arka plandan öne doğru)
    DrawNavArrows(vs);
    if (vs.stripAnimHeight > 0.0f) DrawThumbnailStrip(vs);
    DrawIndexBar(vs);
    DrawStripToggle(vs);
    if (vs.panelAnimWidth > 0.0f) DrawInfoPanel(vs, info);
    DrawInfoButton(vs);  // Info panelinin üstünde çizilir

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

    auto it = m_mapTileCache.find(key);
    if (it != m_mapTileCache.end())
    {
        if (it->second) it->second->Release();
        m_mapTileCache.erase(it);
    }

    D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
    );
    ID2D1Bitmap* bmp = nullptr;
    if (SUCCEEDED(m_renderTarget->CreateBitmap(D2D1::SizeU(w, h), bgra, w * 4, props, &bmp)) && bmp)
        m_mapTileCache[key] = bmp;
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
