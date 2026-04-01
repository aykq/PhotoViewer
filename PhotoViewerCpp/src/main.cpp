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
#include "ImageDecoder.h"

// --- Sabitler ---

static constexpr UINT     WM_DECODE_DONE        = WM_APP + 1;
static constexpr UINT_PTR kZoomIndicatorTimerID = 1;

// --- Arka plan decode ---

struct DecodeResult
{
    std::vector<uint8_t> pixels;
    UINT     width      = 0;
    UINT     height     = 0;
    std::wstring path;
    uint64_t generation = 0;
    ImageInfo info;   // decode thread'inde doldurulur
};

static std::atomic<uint64_t> g_decodeGeneration{0};
static std::thread            g_decodeThread;

static void StartDecode(HWND hwnd, const std::wstring& path)
{
    uint64_t gen = ++g_decodeGeneration;

    if (g_decodeThread.joinable())
        g_decodeThread.detach();

    g_decodeThread = std::thread([hwnd, path, gen]()
    {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

        auto* result       = new DecodeResult();
        result->path       = path;
        result->generation = gen;

        // ── Dosya adı (her zaman doldur — hata durumunda da gösterilir) ──────
        auto sep = path.rfind(L'\\');
        if (sep == std::wstring::npos) sep = path.rfind(L'/');
        result->info.filename = (sep != std::wstring::npos) ? path.substr(sep + 1) : path;

        // ── Dosya boyutu ─────────────────────────────────────────────────────
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
            result->info.fileSizeBytes =
                (static_cast<int64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;

        // ── Decode (tüm format desteği ImageDecoder'da) ──────────────────────
        DecodeOutput decoded;
        if (DecodeImage(path, decoded) && !decoded.pixels.empty())
        {
            result->pixels            = std::move(decoded.pixels);
            result->width             = decoded.width;
            result->height            = decoded.height;
            result->info.width        = static_cast<int>(decoded.width);
            result->info.height       = static_cast<int>(decoded.height);
            result->info.format       = decoded.format;
            result->info.dateTaken    = decoded.dateTaken;
            result->info.cameraMake   = decoded.cameraMake;
            result->info.cameraModel  = decoded.cameraModel;
            result->info.aperture     = decoded.aperture;
            result->info.shutterSpeed = decoded.shutterSpeed;
            result->info.iso          = decoded.iso;
        }
        else
        {
            result->info.errorMessage = L"Bu dosya açılamıyor";
        }

        CoUninitialize();
        PostMessage(hwnd, WM_DECODE_DONE, 0, reinterpret_cast<LPARAM>(result));
    });
}

// --- Yardımcılar ---

static void UpdateWindowTitle(HWND hwnd, const std::wstring& filePath)
{
    std::wstring title = L"PhotoViewer";
    if (!filePath.empty())
    {
        auto pos = filePath.find_last_of(L"\\/");
        std::wstring name = (pos != std::wstring::npos) ? filePath.substr(pos + 1) : filePath;
        title = name + L" \u2014 PhotoViewer";
    }
    SetWindowTextW(hwnd, title.c_str());
}

// --- Global durum ---

static Renderer*        g_renderer  = nullptr;
static FolderNavigator* g_navigator = nullptr;
static ViewState        g_viewState;
static ImageInfo        g_imageInfo;

// Pan sürükleme durumu
static bool  g_dragging        = false;
static float g_dragStartX      = 0.0f;
static float g_dragStartY      = 0.0f;
static float g_panAtDragStartX = 0.0f;
static float g_panAtDragStartY = 0.0f;

// Ok tıklama/sürükleme ayrımı
static bool  g_mouseInArrowZone  = false;
static float g_mouseDownX        = 0.0f;
static float g_mouseDownY        = 0.0f;
static bool  g_clickIsLeft       = false;
static bool  g_clickIsInfoButton = false;
static bool  g_suppressNextZoom  = false;  // info button çift tıklamasında zoom'u engelle

// --- Yardımcı: arrow zone hit-test ---

enum class ArrowZone { None, Left, Right };

// Tıklanabilir bölge: yatayda ok zone genişliği, dikeyde tam yükseklik (panel alanı hariç)
static ArrowZone HitTestArrow(HWND hwnd, float x, float y, bool panelOpen)
{
    (void)y;  // Tam yükseklik — y kısıtlaması yok
    RECT rc;
    GetClientRect(hwnd, &rc);
    float availW = static_cast<float>(rc.right) - (panelOpen ? PanelLayout::Width : 0.0f);
    if (x < ArrowLayout::ZoneW)                         return ArrowZone::Left;
    if (x >= availW - ArrowLayout::ZoneW && x < availW) return ArrowZone::Right;
    return ArrowZone::None;
}

// --- Yardımcı: info button hit-test ---
// Panel açıkken buton PanelLayout::Width kadar sola kaymış olur

static bool HitTestInfoButton(HWND hwnd, float x, float y, bool panelOpen)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    float xOffset = panelOpen ? PanelLayout::Width : 0.0f;
    float x0 = rc.right - InfoButton::Margin - InfoButton::Size - xOffset;
    float y0 = InfoButton::Margin;
    float x1 = rc.right - InfoButton::Margin - xOffset;
    float y1 = InfoButton::Margin + InfoButton::Size;
    return x >= x0 && x <= x1 && y >= y0 && y <= y1;
}

// --- Yardımcı: ViewState'in UI alanlarını koruyarak navigasyon ---

static void NavigateTo(HWND hwnd, const std::wstring& path)
{
    UpdateWindowTitle(hwnd, path);
    bool keepPanel = g_viewState.showInfoPanel;
    g_viewState = ViewState{};
    g_viewState.showInfoPanel = keepPanel;
    if (g_navigator)
    {
        g_viewState.imageIndex = g_navigator->index() + 1;
        g_viewState.imageTotal = g_navigator->total();
    }
    StartDecode(hwnd, path);
    InvalidateRect(hwnd, nullptr, FALSE);
}

// --- Zoom yardımcısı ---

static void ApplyZoom(HWND hwnd, float cx, float cy, float newZoom)
{
    constexpr float kMinZoom  = 0.1f;
    constexpr float kMaxZoom  = 10.0f;
    constexpr float kSnapZoom = 1.0f;
    constexpr float kSnapTol  = 0.02f;

    newZoom = max(kMinZoom, min(kMaxZoom, newZoom));
    if (fabsf(newZoom - kSnapZoom) < kSnapTol) newZoom = kSnapZoom;

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
    SetTimer(hwnd, kZoomIndicatorTimerID, 1500, nullptr);
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
        if (g_renderer) g_renderer->Render(g_viewState, &g_imageInfo);
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
        float mx = static_cast<float>(GET_X_LPARAM(lParam));
        float my = static_cast<float>(GET_Y_LPARAM(lParam));

        g_mouseDownX = mx;
        g_mouseDownY = my;
        SetCapture(hwnd);

        // Info button önce kontrol edilir (sağ ok zone ile üst üste gelebilir)
        g_clickIsInfoButton = HitTestInfoButton(hwnd, mx, my, g_viewState.showInfoPanel);

        if (!g_clickIsInfoButton)
        {
            // Ok zone kontrolü: hangi bölgeye basıldığını kaydet
            ArrowZone zone = (g_viewState.imageTotal > 0)
                             ? HitTestArrow(hwnd, mx, my, g_viewState.showInfoPanel)
                             : ArrowZone::None;
            g_mouseInArrowZone = (zone != ArrowZone::None);
            g_clickIsLeft      = (zone == ArrowZone::Left);

            // Pan sürüklemeyi başlat
            g_dragging        = true;
            g_dragStartX      = mx;
            g_dragStartY      = my;
            g_panAtDragStartX = g_viewState.panX;
            g_panAtDragStartY = g_viewState.panY;
        }
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
    {
        float mx = static_cast<float>(GET_X_LPARAM(lParam));
        float my = static_cast<float>(GET_Y_LPARAM(lParam));

        bool wasDragging = g_dragging;
        g_dragging = false;
        ReleaseCapture();

        // Info button tıklaması
        if (g_clickIsInfoButton)
        {
            float delta = fabsf(mx - g_mouseDownX) + fabsf(my - g_mouseDownY);
            if (delta < 5.0f)
            {
                g_viewState.showInfoPanel = !g_viewState.showInfoPanel;
                g_suppressNextZoom = true;  // Çift tıklamada zoom'u engelle
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            g_clickIsInfoButton = false;
            return 0;
        }

        if (g_mouseInArrowZone && wasDragging && g_navigator && !g_navigator->empty())
        {
            // Manhattan mesafesi < 5px → gerçek tıklama (sürükleme değil)
            float delta = fabsf(mx - g_mouseDownX) + fabsf(my - g_mouseDownY);
            if (delta < 5.0f)
            {
                // Pan birikimini geri al (navigate olacak, pan değil)
                g_viewState.panX = g_panAtDragStartX;
                g_viewState.panY = g_panAtDragStartY;

                const std::wstring& path = g_clickIsLeft
                    ? g_navigator->prev()
                    : g_navigator->next();
                NavigateTo(hwnd, path);
            }
        }

        g_mouseInArrowZone = false;
        return 0;
    }

    case WM_LBUTTONDBLCLK:
    {
        // Info button tıklamasından gelen çift tıklamada zoom yapma
        if (g_suppressNextZoom)
        {
            g_suppressNextZoom = false;
            return 0;
        }

        float cx = static_cast<float>(GET_X_LPARAM(lParam));
        float cy = static_cast<float>(GET_Y_LPARAM(lParam));

        if (g_viewState.zoomFactor == 1.0f)
        {
            ApplyZoom(hwnd, cx, cy, 2.5f);
        }
        else
        {
            // Zoom/pan sıfırla, ama UI durumunu koru
            bool keepPanel = g_viewState.showInfoPanel;
            int  keepIdx   = g_viewState.imageIndex;
            int  keepTotal = g_viewState.imageTotal;
            g_viewState = ViewState{};
            g_viewState.showInfoPanel = keepPanel;
            g_viewState.imageIndex    = keepIdx;
            g_viewState.imageTotal    = keepTotal;
        }

        ShowZoomIndicator(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_KEYDOWN:
        switch (wParam)
        {
        case VK_ESCAPE:
            DestroyWindow(hwnd);
            break;

        case 'I':
            g_viewState.showInfoPanel = !g_viewState.showInfoPanel;
            InvalidateRect(hwnd, nullptr, FALSE);
            break;

        case VK_LEFT:
            if (g_navigator && !g_navigator->empty())
                NavigateTo(hwnd, g_navigator->prev());
            break;

        case VK_RIGHT:
            if (g_navigator && !g_navigator->empty())
                NavigateTo(hwnd, g_navigator->next());
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
        auto* result = reinterpret_cast<DecodeResult*>(lParam);

        if (result->generation == g_decodeGeneration.load() && g_renderer)
        {
            if (!result->pixels.empty())
            {
                g_renderer->LoadImageFromPixels(
                    result->pixels.data(), result->width, result->height, result->path);
            }
            else
            {
                // Decode başarısız — eski bitmap'i temizle, hata mesajı göster
                g_renderer->ClearImage();
            }
            g_imageInfo = result->info;
            InvalidateRect(hwnd, nullptr, FALSE);
        }

        delete result;
        return 0;
    }

    case WM_DESTROY:
        ++g_decodeGeneration;
        if (g_decodeThread.joinable())
            g_decodeThread.detach();
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
    _In_opt_ HINSTANCE,
    _In_     LPSTR,
    _In_     int nCmdShow)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring filePath;
    if (argc >= 2) filePath = argv[1];
    LocalFree(argv);

    WNDCLASSEX wc    = {};
    wc.cbSize        = sizeof(WNDCLASSEX);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"PhotoViewerWindow";
    RegisterClassEx(&wc);

    std::wstring title = L"PhotoViewer";
    if (!filePath.empty())
    {
        auto pos = filePath.find_last_of(L"\\/");
        std::wstring name = (pos != std::wstring::npos) ? filePath.substr(pos + 1) : filePath;
        title = name + L" \u2014 PhotoViewer";
    }

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
        g_navigator = new FolderNavigator(filePath);

        // Başlangıç index/total'ı hemen ayarla (ilk frame'den önce göster)
        if (!g_navigator->empty())
        {
            g_viewState.imageIndex = g_navigator->index() + 1;
            g_viewState.imageTotal = g_navigator->total();
        }

        StartDecode(hwnd, filePath);
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg = {};
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
