#include "Renderer.h"

Renderer::Renderer(HWND hwnd) : m_hwnd(hwnd)
{
    // D2D1Factory: Direct2D'nin giriş noktası.
    // SINGLE_THREADED: render işlemleri tek iş parçacığından yapılacak (UI thread).
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &m_factory);
}

Renderer::~Renderer()
{
    DiscardDeviceResources();

    if (m_wicFactory)
    {
        m_wicFactory->Release();
        m_wicFactory = nullptr;
    }
    if (m_factory)
    {
        m_factory->Release();
        m_factory = nullptr;
    }
}

HRESULT Renderer::CreateDeviceResources()
{
    // Render target zaten varsa tekrar oluşturma
    if (m_renderTarget) return S_OK;

    RECT rc;
    GetClientRect(m_hwnd, &rc);
    D2D1_SIZE_U size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);

    // HwndRenderTarget: doğrudan Win32 HWND üzerine GPU ile çizer.
    // Zoom/pan transform'ları bu render target üzerinde uygulanacak.
    return m_factory->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(m_hwnd, size),
        &m_renderTarget
    );
}

void Renderer::DiscardDeviceResources()
{
    // ID2D1Bitmap render target'a bağlı bir GPU kaynağıdır;
    // render target yok edilince bitmap da geçersiz olur.
    if (m_bitmap)
    {
        m_bitmap->Release();
        m_bitmap = nullptr;
    }
    if (m_renderTarget)
    {
        m_renderTarget->Release();
        m_renderTarget = nullptr;
    }
}

bool Renderer::LoadImage(const std::wstring& path)
{
    // Önceki bitmap'i temizle
    if (m_bitmap)
    {
        m_bitmap->Release();
        m_bitmap = nullptr;
    }

    // WIC fabrikasını ilk çağrıda oluştur (CoInitialize WinMain'de yapılmış olmalı)
    if (!m_wicFactory)
    {
        HRESULT hr = CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&m_wicFactory)
        );
        if (FAILED(hr)) return false;
    }

    // Dosyadan decoder oluştur — WIC desteklenen tüm formatları otomatik tanır
    IWICBitmapDecoder* decoder = nullptr;
    HRESULT hr = m_wicFactory->CreateDecoderFromFilename(
        path.c_str(),
        nullptr,
        GENERIC_READ,
        WICDecodeMetadataCacheOnLoad,  // Metadata decode sırasında okunur (Phase 5'te kullanılacak)
        &decoder
    );
    if (FAILED(hr)) return false;

    // İlk frame'i al (JPEG gibi tek-frame formatlar için frame 0)
    IWICBitmapFrameDecode* frame = nullptr;
    hr = decoder->GetFrame(0, &frame);
    decoder->Release();
    if (FAILED(hr)) return false;

    // Format converter: piksel formatını D2D'nin beklediği 32bppPBGRA'ya çevir
    // P = pre-multiplied alpha (Direct2D bu formatı ister)
    IWICFormatConverter* converter = nullptr;
    hr = m_wicFactory->CreateFormatConverter(&converter);
    if (FAILED(hr)) { frame->Release(); return false; }

    hr = converter->Initialize(
        frame,
        GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone,
        nullptr,
        0.0f,
        WICBitmapPaletteTypeMedianCut
    );
    frame->Release();
    if (FAILED(hr)) { converter->Release(); return false; }

    // Render target hazır olmalı (bitmap ona bağlı bir GPU kaynağı)
    if (FAILED(CreateDeviceResources())) { converter->Release(); return false; }

    // WIC bitmap → GPU'ya yükle (ID2D1Bitmap)
    hr = m_renderTarget->CreateBitmapFromWicBitmap(converter, nullptr, &m_bitmap);
    converter->Release();

    if (SUCCEEDED(hr))
    {
        m_imagePath = path;  // D2DERR_RECREATE_TARGET'ta yeniden yüklemek için sakla
        return true;
    }
    return false;
}

void Renderer::Render(const ViewState& vs)
{
    if (FAILED(CreateDeviceResources())) return;

    // D2DERR_RECREATE_TARGET sonrası render target yeniden oluşturuldu ama bitmap gitti —
    // kaydedilen yoldan yeniden yükle
    if (!m_bitmap && !m_imagePath.empty())
        LoadImage(m_imagePath);

    m_renderTarget->BeginDraw();

    // Arka plan: koyu gri (#1E1E1E — VS Code dark teması ile aynı ton)
    m_renderTarget->Clear(D2D1::ColorF(0.118f, 0.118f, 0.118f));

    if (m_bitmap)
    {
        D2D1_SIZE_F windowSize = m_renderTarget->GetSize();
        D2D1_SIZE_F imageSize  = m_bitmap->GetSize();

        // Aspect ratio korumalı "fit" ölçeği (zoomFactor=1 → tam ekrana sığdır)
        float scaleX  = windowSize.width  / imageSize.width;
        float scaleY  = windowSize.height / imageSize.height;
        float fitScale = min(scaleX, scaleY);

        // Nihai ölçek = fit ölçeği × kullanıcı zoom faktörü
        float finalScale = fitScale * vs.zoomFactor;

        float destW = imageSize.width  * finalScale;
        float destH = imageSize.height * finalScale;

        // Fit konumu (pencere ortası) + pan ofseti
        float destX = (windowSize.width  - destW) * 0.5f + vs.panX;
        float destY = (windowSize.height - destH) * 0.5f + vs.panY;

        m_renderTarget->DrawBitmap(
            m_bitmap,
            D2D1::RectF(destX, destY, destX + destW, destY + destH)
        );
    }

    // Phase 5: EXIF paneli (DirectWrite metin)

    HRESULT hr = m_renderTarget->EndDraw();

    if (hr == D2DERR_RECREATE_TARGET)
    {
        // GPU cihazı kayboldu (örn. ekran kartı sürücüsü sıfırlandı).
        // Render target ve bitmap'i serbest bırak.
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
