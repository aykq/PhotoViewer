#include <windows.h>
#include <windowsx.h>   // GET_X_LPARAM, GET_Y_LPARAM
#include <shellapi.h>   // CommandLineToArgvW
#include <string>
#include <cmath>        // fabsf
#include <thread>       // std::thread
#include <atomic>       // std::atomic
#include <vector>       // std::vector (DecodeResult piksel buffer'ı)
#include <cstdint>
#include "Renderer.h"
#include "FolderNavigator.h"

// --- Sabitler ---

static constexpr UINT     WM_DECODE_DONE        = WM_APP + 1;  // Arka plan decode tamamlandı
static constexpr UINT_PTR kZoomIndicatorTimerID = 1;           // Zoom overlay otomatik gizleme

// --- Arka plan decode ---

// Arka plan thread'inden UI thread'ine taşınan decode sonucu.
// PostMessage(WM_DECODE_DONE) ile heap'te iletilir; WndProc delete eder.
struct DecodeResult
{
    std::vector<uint8_t> pixels;    // 32bppPBGRA ham piksel verisi
    UINT     width      = 0;
    UINT     height     = 0;
    std::wstring path;              // Hangi dosyadan geldiği
    uint64_t generation = 0;        // Eski sonuçları ayırt etmek için nesil numarası
};

// Her StartDecode çağrısında artırılır.
// WM_DECODE_DONE, generation == g_decodeGeneration ise geçerli kabul edilir.
static std::atomic<uint64_t> g_decodeGeneration{0};
static std::thread            g_decodeThread;

// Yeni bir arka plan decode başlatır. Önceki decode geçersiz sayılır.
static void StartDecode(HWND hwnd, const std::wstring& path)
{
    uint64_t gen = ++g_decodeGeneration;  // Bu değer eski in-flight decode'ları otomatik iptal eder

    // Önceki thread'i bırak — sonucu gelirse generation uyuşmadığından yok sayılır
    if (g_decodeThread.joinable())
        g_decodeThread.detach();

    g_decodeThread = std::thread([hwnd, path, gen]()
    {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

        auto* result       = new DecodeResult();
        result->path       = path;
        result->generation = gen;

        // Her erken çıkışta result heap'te kalır; WM_DECODE_DONE handler siler.
        // Başarısız decode'da pixels boş gelir → handler bunu kontrol eder.
        auto post = [&]() {
            CoUninitialize();
            PostMessage(hwnd, WM_DECODE_DONE, 0, reinterpret_cast<LPARAM>(result));
        };

        IWICImagingFactory* wicFactory = nullptr;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wicFactory))))
        {
            post(); return;
        }

        IWICBitmapDecoder* decoder = nullptr;
        HRESULT hr = wicFactory->CreateDecoderFromFilename(
            path.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnLoad, &decoder);
        if (FAILED(hr)) { wicFactory->Release(); post(); return; }

        IWICBitmapFrameDecode* frame = nullptr;
        hr = decoder->GetFrame(0, &frame);
        decoder->Release();
        if (FAILED(hr)) { wicFactory->Release(); post(); return; }

        IWICFormatConverter* converter = nullptr;
        hr = wicFactory->CreateFormatConverter(&converter);
        if (FAILED(hr)) { frame->Release(); wicFactory->Release(); post(); return; }

        // Tüm WIC formatlarını Direct2D'nin istediği 32bppPBGRA'ya dönüştür
        hr = converter->Initialize(
            frame, GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0f, WICBitmapPaletteTypeMedianCut);
        frame->Release();
        if (FAILED(hr)) { converter->Release(); wicFactory->Release(); post(); return; }

        UINT w = 0, h = 0;
        converter->GetSize(&w, &h);
        result->width  = w;
        result->height = h;

        UINT stride = w * 4;
        result->pixels.resize(static_cast<size_t>(stride) * h);

        // Piksel verisini CPU buffer'ına kopyala — GPU yüklemesi UI thread'inde yapılacak
        hr = converter->CopyPixels(nullptr, stride, stride * h, result->pixels.data());
        if (FAILED(hr)) result->pixels.clear();  // Boş → handler yükleme yapmaz

        converter->Release();
        wicFactory->Release();
        post();
    });
}

// --- Yardımcı ---

static void UpdateWindowTitle(HWND hwnd, const std::wstring& filePath)
{
    std::wstring title = L"PhotoViewer";
    if (!filePath.empty())
    {
        auto pos = filePath.find_last_of(L"\\/");
        std::wstring name = (pos != std::wstring::npos) ? filePath.substr(pos + 1) : filePath;
        title = name + L" \u2014 PhotoViewer";  // \u2014 = em dash (—)
    }
    SetWindowTextW(hwnd, title.c_str());
}

// --- Global durum ---

static Renderer*        g_renderer  = nullptr;
static FolderNavigator* g_navigator = nullptr;
static ViewState        g_viewState;

// Pan için fare sürükleme durumu
static bool  g_dragging        = false;
static float g_dragStartX      = 0.0f;
static float g_dragStartY      = 0.0f;
static float g_panAtDragStartX = 0.0f;
static float g_panAtDragStartY = 0.0f;

// --- Zoom yardımcısı: pan anchor ile yeni zoom uygula ---

static void ApplyZoom(HWND hwnd, float cx, float cy, float newZoom)
{
    constexpr float kMinZoom  = 0.1f;
    constexpr float kMaxZoom  = 10.0f;
    constexpr float kSnapZoom = 1.0f;
    constexpr float kSnapTol  = 0.02f;

    newZoom = max(kMinZoom, min(kMaxZoom, newZoom));
    if (fabsf(newZoom - kSnapZoom) < kSnapTol) newZoom = kSnapZoom;

    // Zoom anchor: cursor altındaki görüntü noktası sabit kalmalı.
    // Görüntü pencere merkezine hizalı olduğundan pan offseti merkeze göre hesaplanır.
    RECT rc;
    GetClientRect(hwnd, &rc);
    float halfW = (rc.right  - rc.left) * 0.5f;
    float halfH = (rc.bottom - rc.top)  * 0.5f;
    float ratio = newZoom / g_viewState.zoomFactor;

    g_viewState.panX       = (cx - halfW) - (cx - halfW - g_viewState.panX) * ratio;
    g_viewState.panY       = (cy - halfH) - (cy - halfH - g_viewState.panY) * ratio;
    g_viewState.zoomFactor = newZoom;
}

static void ShowZoomIndicator(HWND hwnd)
{
    g_viewState.showZoomIndicator = true;
    KillTimer(hwnd, kZoomIndicatorTimerID);
    SetTimer(hwnd, kZoomIndicatorTimerID, 1500, nullptr);  // 1.5 saniye sonra gizle
}

// --- WndProc ---

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
        g_renderer = new Renderer(hwnd);
        return 0;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        if (g_renderer) g_renderer->Render(g_viewState);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_SIZE:
    {
        UINT w = LOWORD(lParam), h = HIWORD(lParam);
        if (g_renderer) g_renderer->Resize(w, h);
        return 0;
    }

    case WM_MOUSEWHEEL:
    {
        // Fare tekerleği → zoom ±10%, cursor pozisyonuna sabitlenmiş
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);

        POINT cursor;
        cursor.x = GET_X_LPARAM(lParam);
        cursor.y = GET_Y_LPARAM(lParam);
        ScreenToClient(hwnd, &cursor);

        float newZoom = g_viewState.zoomFactor * (delta > 0 ? 1.1f : (1.0f / 1.1f));
        ApplyZoom(hwnd, static_cast<float>(cursor.x), static_cast<float>(cursor.y), newZoom);
        ShowZoomIndicator(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_LBUTTONDOWN:
    {
        SetCapture(hwnd);  // Fare pencere dışına çıksa da mesajları almaya devam et
        g_dragging        = true;
        g_dragStartX      = static_cast<float>(GET_X_LPARAM(lParam));
        g_dragStartY      = static_cast<float>(GET_Y_LPARAM(lParam));
        g_panAtDragStartX = g_viewState.panX;
        g_panAtDragStartY = g_viewState.panY;
        return 0;
    }

    case WM_MOUSEMOVE:
    {
        if (!g_dragging) return 0;
        g_viewState.panX = g_panAtDragStartX + (static_cast<float>(GET_X_LPARAM(lParam)) - g_dragStartX);
        g_viewState.panY = g_panAtDragStartY + (static_cast<float>(GET_Y_LPARAM(lParam)) - g_dragStartY);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_LBUTTONUP:
        g_dragging = false;
        ReleaseCapture();
        return 0;

    case WM_LBUTTONDBLCLK:
    {
        // Çift tıklama: fit ise 2.5x zoom, değilse fit'e sıfırla
        float cx = static_cast<float>(GET_X_LPARAM(lParam));
        float cy = static_cast<float>(GET_Y_LPARAM(lParam));

        if (g_viewState.zoomFactor == 1.0f)
        {
            ApplyZoom(hwnd, cx, cy, 2.5f);
        }
        else
        {
            g_viewState = ViewState{};  // Pan ve zoom sıfırla
        }

        ShowZoomIndicator(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_ERASEBKGND:
        // Direct2D her frame'de tüm arka planı çiziyor.
        // GDI'ın arka planı silmesine izin verme — zoom/pan sırasında flicker önler.
        return 1;

    case WM_KEYDOWN:
        switch (wParam)
        {
        case VK_ESCAPE:
            DestroyWindow(hwnd);
            break;

        case VK_LEFT:
            if (g_navigator && !g_navigator->empty())
            {
                const std::wstring& path = g_navigator->prev();
                UpdateWindowTitle(hwnd, path);
                g_viewState = ViewState{};  // Yeni görüntü için zoom/pan sıfırla
                StartDecode(hwnd, path);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;

        case VK_RIGHT:
            if (g_navigator && !g_navigator->empty())
            {
                const std::wstring& path = g_navigator->next();
                UpdateWindowTitle(hwnd, path);
                g_viewState = ViewState{};
                StartDecode(hwnd, path);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        }
        return 0;

    case WM_TIMER:
        if (wParam == kZoomIndicatorTimerID)
        {
            KillTimer(hwnd, kZoomIndicatorTimerID);
            g_viewState.showZoomIndicator = false;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_DECODE_DONE:
    {
        // Arka plan thread'inden gelen decode sonucu
        auto* result = reinterpret_cast<DecodeResult*>(lParam);

        if (result->generation == g_decodeGeneration.load() &&
            !result->pixels.empty() && g_renderer)
        {
            // GPU'ya yükle — bu çağrı UI thread'inde, render target hazır
            g_renderer->LoadImageFromPixels(
                result->pixels.data(), result->width, result->height, result->path);
            InvalidateRect(hwnd, nullptr, FALSE);
        }

        delete result;  // Her durumda heap'ten sil
        return 0;
    }

    case WM_DESTROY:
        ++g_decodeGeneration;                    // Pending decode'ları geçersiz say
        if (g_decodeThread.joinable())
            g_decodeThread.detach();             // Thread'i beklemeden bırak
        KillTimer(hwnd, kZoomIndicatorTimerID);
        delete g_navigator; g_navigator = nullptr;
        delete g_renderer;  g_renderer  = nullptr;
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// --- WinMain ---

int WINAPI WinMain(
    _In_     HINSTANCE hInstance,
    _In_opt_ HINSTANCE,   // hPrevInstance — artık kullanılmıyor
    _In_     LPSTR,       // lpCmdLine — ANSI; Unicode için GetCommandLineW kullanıyoruz
    _In_     int       nCmdShow)
{
    // COM başlat — WIC (IWICImagingFactory) için gerekli
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // Komut satırı argümanlarını Unicode olarak oku
    // Explorer'dan çift tıklamada: argv[1] = tam dosya yolu (boşluklu yollar dahil)
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring filePath;
    if (argc >= 2) filePath = argv[1];
    LocalFree(argv);

    // Pencere sınıfını kaydet
    // CS_DBLCLKS: çift tıklama mesajlarını etkinleştirir (zoom reset)
    WNDCLASSEX wc    = {};
    wc.cbSize        = sizeof(WNDCLASSEX);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;  // Direct2D çiziyor, GDI arka planı gereksiz
    wc.lpszClassName = L"PhotoViewerWindow";
    RegisterClassEx(&wc);

    // Pencere başlığını belirle
    std::wstring title = L"PhotoViewer";
    if (!filePath.empty())
    {
        auto pos = filePath.find_last_of(L"\\/");
        std::wstring name = (pos != std::wstring::npos) ? filePath.substr(pos + 1) : filePath;
        title = name + L" \u2014 PhotoViewer";
    }

    // Pencereyi oluştur (WM_CREATE tetiklenir → g_renderer hazır olur)
    HWND hwnd = CreateWindowEx(
        0,
        L"PhotoViewerWindow",
        title.c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        1280, 800,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!hwnd)
    {
        CoUninitialize();
        return -1;
    }

    if (!filePath.empty())
    {
        // Klasör navigatörünü oluştur — tüm görüntüler taranır ve sıralanır
        g_navigator = new FolderNavigator(filePath);

        // İlk görüntüyü arka planda decode et
        StartDecode(hwnd, filePath);
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // Mesaj döngüsü: uygulama kapatılana kadar çalışır
    MSG msg = {};
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
