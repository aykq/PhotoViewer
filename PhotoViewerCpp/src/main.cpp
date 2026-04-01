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

static constexpr UINT     WM_DECODE_DONE        = WM_APP + 1;
static constexpr UINT_PTR kZoomIndicatorTimerID = 1;

// --- WIC Metadata Yardımcıları ---

// PROPVARIANT içindeki string değerini wstring olarak döner; başarısız olunca boş döner.
static std::wstring WicQueryString(IWICMetadataQueryReader* reader, const wchar_t* query)
{
    PROPVARIANT pv;
    PropVariantInit(&pv);
    std::wstring result;

    if (SUCCEEDED(reader->GetMetadataByName(query, &pv)))
    {
        if (pv.vt == VT_LPWSTR && pv.pwszVal)
        {
            result = pv.pwszVal;
        }
        else if (pv.vt == VT_LPSTR && pv.pszVal)
        {
            int len = MultiByteToWideChar(CP_ACP, 0, pv.pszVal, -1, nullptr, 0);
            if (len > 1)
            {
                result.resize(len - 1);
                MultiByteToWideChar(CP_ACP, 0, pv.pszVal, -1, result.data(), len);
            }
        }
    }

    PropVariantClear(&pv);
    // Trailing null veya boşlukları temizle
    while (!result.empty() && (result.back() == L'\0' || result.back() == L' '))
        result.pop_back();
    return result;
}

// PROPVARIANT içindeki RATIONAL değerini biçimlendirilmiş string olarak döner.
// mode: L"aperture" → "f/2.8" | L"shutter" → "1/500s"
static std::wstring WicQueryRational(IWICMetadataQueryReader* reader, const wchar_t* query,
                                     const wchar_t* mode)
{
    PROPVARIANT pv;
    PropVariantInit(&pv);
    std::wstring result;

    if (SUCCEEDED(reader->GetMetadataByName(query, &pv)))
    {
        ULONG num = 0, den = 1;
        bool valid = false;

        if (pv.vt == VT_UI8)
        {
            // WIC packs RATIONAL as UI8: LowPart=numerator, HighPart=denominator
            num   = pv.uhVal.LowPart;
            den   = pv.uhVal.HighPart;
            valid = (den > 0);
        }
        else if (pv.vt == VT_R8)
        {
            // Bazı codec'ler double olarak döner
            double val = pv.dblVal;
            if (val > 0.0)
            {
                if (wcscmp(mode, L"aperture") == 0)
                {
                    wchar_t buf[32];
                    swprintf_s(buf, L"f/%.1f", val);
                    result = buf;
                }
                else
                {
                    wchar_t buf[32];
                    ULONG denom = static_cast<ULONG>(1.0 / val + 0.5);
                    if (denom > 1)
                        swprintf_s(buf, L"1/%lus", denom);
                    else
                        swprintf_s(buf, L"%.1fs", val);
                    result = buf;
                }
            }
        }

        if (valid)
        {
            wchar_t buf[32];
            if (wcscmp(mode, L"aperture") == 0)
            {
                swprintf_s(buf, L"f/%.1f", static_cast<double>(num) / den);
            }
            else  // shutter
            {
                if (num > 0 && den > num)
                    swprintf_s(buf, L"1/%lus", den / num);
                else if (num > 0)
                    swprintf_s(buf, L"%.1fs", static_cast<double>(num) / den);
                else
                    buf[0] = L'\0';
            }
            result = buf;
        }
    }

    PropVariantClear(&pv);
    return result;
}

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

        // converter->Initialize çağrısından sonra frame'i tutmaya devam et (metadata için)
        hr = converter->Initialize(
            frame, GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0f, WICBitmapPaletteTypeMedianCut);
        if (FAILED(hr))
        {
            frame->Release(); converter->Release(); wicFactory->Release();
            post(); return;
        }

        UINT w = 0, h = 0;
        converter->GetSize(&w, &h);
        result->width  = w;
        result->height = h;

        UINT stride = w * 4;
        result->pixels.resize(static_cast<size_t>(stride) * h);

        hr = converter->CopyPixels(nullptr, stride, stride * h, result->pixels.data());
        if (FAILED(hr)) result->pixels.clear();

        converter->Release();

        // ── Temel ImageInfo ──────────────────────────────────────────────────
        result->info.width  = static_cast<int>(w);
        result->info.height = static_cast<int>(h);

        // Dosya adı
        auto sep = path.rfind(L'\\');
        if (sep == std::wstring::npos) sep = path.rfind(L'/');
        result->info.filename = (sep != std::wstring::npos) ? path.substr(sep + 1) : path;

        // Format (uzantıdan)
        auto dotPos = path.rfind(L'.');
        if (dotPos != std::wstring::npos)
        {
            std::wstring ext = path.substr(dotPos + 1);
            for (auto& c : ext) c = towupper(c);
            if (ext == L"JPG") ext = L"JPEG";
            result->info.format = ext;
        }

        // Dosya boyutu
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
            result->info.fileSizeBytes =
                (static_cast<int64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;

        // ── EXIF Metadata (frame hâlâ geçerli) ──────────────────────────────
        IWICMetadataQueryReader* mqr = nullptr;
        if (SUCCEEDED(frame->GetMetadataQueryReader(&mqr)) && mqr)
        {
            result->info.cameraMake   = WicQueryString(mqr, L"/app1/ifd/{ushort=271}");
            result->info.cameraModel  = WicQueryString(mqr, L"/app1/ifd/{ushort=272}");
            result->info.dateTaken    = WicQueryString(mqr, L"/app1/ifd/exif/{ushort=36867}");
            result->info.aperture     = WicQueryRational(mqr, L"/app1/ifd/exif/{ushort=33437}", L"aperture");
            result->info.shutterSpeed = WicQueryRational(mqr, L"/app1/ifd/exif/{ushort=33434}", L"shutter");

            // ISO — VT_UI2
            PROPVARIANT pvIso;
            PropVariantInit(&pvIso);
            if (SUCCEEDED(mqr->GetMetadataByName(L"/app1/ifd/exif/{ushort=34855}", &pvIso)))
            {
                if (pvIso.vt == VT_UI2)
                {
                    wchar_t buf[16];
                    swprintf_s(buf, L"%u", static_cast<unsigned>(pvIso.uiVal));
                    result->info.iso = buf;
                }
                else if (pvIso.vt == VT_UI4)
                {
                    wchar_t buf[16];
                    swprintf_s(buf, L"%lu", pvIso.ulVal);
                    result->info.iso = buf;
                }
                PropVariantClear(&pvIso);
            }

            mqr->Release();
        }

        // Frame artık serbest bırakılabilir (metadata okunduktan sonra)
        frame->Release();

        wicFactory->Release();
        post();
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
static bool  g_mouseInArrowZone = false;
static float g_mouseDownX       = 0.0f;
static float g_mouseDownY       = 0.0f;
static bool  g_clickIsLeft      = false;

// --- Yardımcı: arrow zone hit-test ---

enum class ArrowZone { None, Left, Right };

static ArrowZone HitTestArrow(HWND hwnd, float x, float y)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    float midY  = rc.bottom * 0.5f;
    float halfH = ArrowLayout::PillH * 0.5f;

    if (y < midY - halfH || y > midY + halfH) return ArrowZone::None;
    if (x < ArrowLayout::ZoneW)               return ArrowZone::Left;
    if (x > rc.right - ArrowLayout::ZoneW)    return ArrowZone::Right;
    return ArrowZone::None;
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

        // Ok zone kontrolü: hangi bölgeye basıldığını kaydet
        g_mouseDownX = mx;
        g_mouseDownY = my;
        ArrowZone zone = (g_viewState.imageTotal > 0)
                         ? HitTestArrow(hwnd, mx, my)
                         : ArrowZone::None;
        g_mouseInArrowZone = (zone != ArrowZone::None);
        g_clickIsLeft      = (zone == ArrowZone::Left);

        // Pan sürüklemeyi her durumda başlat (ok üzerinde sürükleme de pan yapar)
        SetCapture(hwnd);
        g_dragging        = true;
        g_dragStartX      = mx;
        g_dragStartY      = my;
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
    {
        float mx = static_cast<float>(GET_X_LPARAM(lParam));
        float my = static_cast<float>(GET_Y_LPARAM(lParam));

        bool wasDragging = g_dragging;
        g_dragging = false;
        ReleaseCapture();

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

        if (result->generation == g_decodeGeneration.load() &&
            !result->pixels.empty() && g_renderer)
        {
            g_renderer->LoadImageFromPixels(
                result->pixels.data(), result->width, result->height, result->path);
            g_imageInfo = result->info;  // Metadata'yı global'e aktar
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
