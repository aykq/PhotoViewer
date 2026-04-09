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
static constexpr UINT_PTR kPanelAnimTimerID     = 2;
static constexpr UINT_PTR kAnimTimerID          = 3;

// Panel animasyon: her frame'de kalan mesafenin bu oranı kadar hareket eder (ease-out lerp)
static constexpr float    kPanelAnimLerp        = 0.35f;

// --- Arka plan decode ---

struct DecodeResult
{
    std::vector<uint8_t> pixels;
    UINT     width      = 0;
    UINT     height     = 0;
    std::wstring path;
    uint64_t generation = 0;
    ImageInfo info;   // decode thread'inde doldurulur
    // Animasyon frame'leri — boş = statik görüntü
    std::vector<AnimFrame> frames;
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
        bool decodeOk = DecodeImage(path, decoded);
        bool hasContent = decodeOk && (!decoded.pixels.empty() || !decoded.frames.empty());

        if (hasContent)
        {
            result->pixels            = std::move(decoded.pixels);
            result->frames            = std::move(decoded.frames);
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
            result->info.gpsLatitude     = decoded.gpsLatitude;
            result->info.gpsLongitude    = decoded.gpsLongitude;
            result->info.gpsAltitude     = decoded.gpsAltitude;
            result->info.hasGpsDecimal   = decoded.hasGpsDecimal;
            result->info.gpsLatDecimal   = decoded.gpsLatDecimal;
            result->info.gpsLonDecimal   = decoded.gpsLonDecimal;
            result->info.gpsLocationName = decoded.gpsLocationName;
            result->info.iccProfileName  = decoded.iccProfileName;
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

struct SavedWindowRect { int x, y, w, h; bool valid; };
static SavedWindowRect  g_savedWindowRect = {};

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
static bool  g_clickInPanel      = false;  // Panel alanı tıklaması — drag/zoom engellenir


// --- Yardımcı: arrow zone hit-test ---

enum class ArrowZone { None, Left, Right };

// Tıklanabilir bölge: yatayda ok zone genişliği, dikeyde tam yükseklik (panel alanı hariç)
// panelW: mevcut panel genişliği (animasyonlu olabilir)
static ArrowZone HitTestArrow(HWND hwnd, float x, float y, float panelW)
{
    (void)y;  // Tam yükseklik — y kısıtlaması yok
    RECT rc;
    GetClientRect(hwnd, &rc);
    float availW = static_cast<float>(rc.right) - panelW;
    if (x < ArrowLayout::ZoneW)                         return ArrowZone::Left;
    if (x >= availW - ArrowLayout::ZoneW && x < availW) return ArrowZone::Right;
    return ArrowZone::None;
}

// --- Yardımcı: info button hit-test ---

static bool HitTestInfoButton(HWND hwnd, float x, float y, float panelW)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    float x0 = rc.right - InfoButton::Margin - InfoButton::Size - panelW;
    float y0 = InfoButton::Margin;
    float x1 = rc.right - InfoButton::Margin - panelW;
    float y1 = InfoButton::Margin + InfoButton::Size;
    return x >= x0 && x <= x1 && y >= y0 && y <= y1;
}

// --- Registry ayarları ---

static constexpr wchar_t kRegKey[] = L"Software\\PhotoViewer";

static void SaveSettings()
{
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr,
                        0, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS)
    {
        DWORD val = g_viewState.showInfoPanel ? 1 : 0;
        RegSetValueExW(hKey, L"ShowInfoPanel",  0, REG_DWORD, reinterpret_cast<const BYTE*>(&val), sizeof(DWORD));
        val = g_viewState.use12HourTime ? 1 : 0;
        RegSetValueExW(hKey, L"Use12HourTime", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&val), sizeof(DWORD));
        RegCloseKey(hKey);
    }
}

static void SaveWindowPlacement(HWND hwnd)
{
    RECT rc;
    if (IsIconic(hwnd))
    {
        // Küçültülmüşse normal (restored) pozisyonu al
        WINDOWPLACEMENT wp = { sizeof(WINDOWPLACEMENT) };
        if (!GetWindowPlacement(hwnd, &wp)) return;
        rc = wp.rcNormalPosition;
    }
    else
    {
        GetWindowRect(hwnd, &rc);
    }

    int w = rc.right  - rc.left;
    int h = rc.bottom - rc.top;
    if (w < 100 || h < 100) return;  // Anormal değerleri kaydetme

    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr,
                        0, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS)
    {
        DWORD dx = static_cast<DWORD>(rc.left);
        DWORD dy = static_cast<DWORD>(rc.top);
        DWORD dw = static_cast<DWORD>(w);
        DWORD dh = static_cast<DWORD>(h);
        RegSetValueExW(hKey, L"WindowX", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&dx), sizeof(DWORD));
        RegSetValueExW(hKey, L"WindowY", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&dy), sizeof(DWORD));
        RegSetValueExW(hKey, L"WindowW", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&dw), sizeof(DWORD));
        RegSetValueExW(hKey, L"WindowH", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&dh), sizeof(DWORD));
        RegCloseKey(hKey);
    }
}

static void LoadSettings()
{
    // Sistem locale'ini varsayılan olarak kullan (LOCALE_ITIME: "0"=12h, "1"=24h)
    wchar_t localeBuf[4] = {};
    GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_ITIME, localeBuf, _countof(localeBuf));
    g_viewState.use12HourTime = (localeBuf[0] != L'1');

    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegKey, 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        DWORD val = 0, sz = sizeof(DWORD);
        if (RegQueryValueExW(hKey, L"ShowInfoPanel", nullptr, nullptr, reinterpret_cast<LPBYTE>(&val), &sz) == ERROR_SUCCESS)
            g_viewState.showInfoPanel = (val != 0);
        sz = sizeof(DWORD);
        if (RegQueryValueExW(hKey, L"Use12HourTime", nullptr, nullptr, reinterpret_cast<LPBYTE>(&val), &sz) == ERROR_SUCCESS)
            g_viewState.use12HourTime = (val != 0);

        // Pencere pozisyon/boyut
        DWORD wx = 0, wy = 0, ww = 0, wh = 0;
        DWORD szD = sizeof(DWORD);
        bool hasX = RegQueryValueExW(hKey, L"WindowX", nullptr, nullptr, reinterpret_cast<LPBYTE>(&wx), &szD) == ERROR_SUCCESS; szD = sizeof(DWORD);
        bool hasY = RegQueryValueExW(hKey, L"WindowY", nullptr, nullptr, reinterpret_cast<LPBYTE>(&wy), &szD) == ERROR_SUCCESS; szD = sizeof(DWORD);
        bool hasW = RegQueryValueExW(hKey, L"WindowW", nullptr, nullptr, reinterpret_cast<LPBYTE>(&ww), &szD) == ERROR_SUCCESS; szD = sizeof(DWORD);
        bool hasH = RegQueryValueExW(hKey, L"WindowH", nullptr, nullptr, reinterpret_cast<LPBYTE>(&wh), &szD) == ERROR_SUCCESS;

        if (hasX && hasY && hasW && hasH && ww >= 100 && wh >= 100)
        {
            int ix = static_cast<int>(wx), iy = static_cast<int>(wy);
            int iw = static_cast<int>(ww), ih = static_cast<int>(wh);
            // Pencerenin en az bir monitörle kesiştiğini doğrula
            RECT rc = { ix, iy, ix + iw, iy + ih };
            if (MonitorFromRect(&rc, MONITOR_DEFAULTTONULL) != nullptr)
                g_savedWindowRect = { ix, iy, iw, ih, true };
        }

        RegCloseKey(hKey);
    }
    // Başlangıçta animasyon yok — doğrudan hedef genişliğe atla
    g_viewState.panelAnimWidth = g_viewState.showInfoPanel ? PanelLayout::Width : 0.0f;
}

// --- Panel animasyon başlatıcısı ---

static void StartPanelAnim(HWND hwnd)
{
    KillTimer(hwnd, kPanelAnimTimerID);
    SetTimer(hwnd, kPanelAnimTimerID, 16, nullptr);
}

// --- Yardımcı: ViewState'in UI alanlarını koruyarak navigasyon ---

static void NavigateTo(HWND hwnd, const std::wstring& path)
{
    KillTimer(hwnd, kAnimTimerID);
    if (g_renderer) g_renderer->ClearAnimation();
    UpdateWindowTitle(hwnd, path);
    bool  keepPanel     = g_viewState.showInfoPanel;
    float keepAnimWidth = g_viewState.panelAnimWidth;
    bool  keep12h       = g_viewState.use12HourTime;
    g_viewState = ViewState{};
    g_viewState.showInfoPanel  = keepPanel;
    g_viewState.panelAnimWidth = keepAnimWidth;
    g_viewState.use12HourTime  = keep12h;
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

        float cx = static_cast<float>(cursor.x);
        float cy = static_cast<float>(cursor.y);
        float newZoom = g_viewState.zoomFactor * (delta > 0 ? 1.1f : (1.0f / 1.1f));
        ApplyZoom(hwnd, cx, cy, newZoom);
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
        g_clickIsInfoButton = HitTestInfoButton(hwnd, mx, my, g_viewState.panelAnimWidth);

        if (!g_clickIsInfoButton)
        {
            // Panel alanına tıklandığında drag/ok/zoom engellenir
            if (g_viewState.panelAnimWidth > 0.0f)
            {
                RECT rc;
                GetClientRect(hwnd, &rc);
                if (mx >= static_cast<float>(rc.right) - g_viewState.panelAnimWidth)
                {
                    g_clickInPanel = true;
                    return 0;
                }
            }

            // Ok zone kontrolü: hangi bölgeye basıldığını kaydet
            ArrowZone zone = (g_viewState.imageTotal > 0)
                             ? HitTestArrow(hwnd, mx, my, g_viewState.panelAnimWidth)
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

        // Panel alanı tıklaması — date toggle ve GPS link kontrol edilir
        if (g_clickInPanel)
        {
            g_clickInPanel = false;
            float delta = fabsf(mx - g_mouseDownX) + fabsf(my - g_mouseDownY);
            if (delta < 5.0f && g_renderer)
            {
                // Date toggle
                if (g_renderer->IsDateToggleVisible())
                {
                    D2D1_RECT_F r = g_renderer->GetDateToggleRect();
                    if (mx >= r.left && mx <= r.right && my >= r.top && my <= r.bottom)
                    {
                        float midX = (r.left + r.right) * 0.5f;
                        g_viewState.use12HourTime = (mx >= midX);
                        SaveSettings();
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }
                }
                // GPS link
                if (g_renderer->IsGpsLinkVisible() && g_imageInfo.hasGpsDecimal)
                {
                    D2D1_RECT_F r = g_renderer->GetGpsLinkRect();
                    if (mx >= r.left && mx <= r.right && my >= r.top && my <= r.bottom)
                    {
                        wchar_t url[256];
                        swprintf_s(url,
                            L"https://www.openstreetmap.org/?mlat=%.6f&mlon=%.6f&zoom=15",
                            g_imageInfo.gpsLatDecimal, g_imageInfo.gpsLonDecimal);
                        ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
                        return 0;
                    }
                }
            }
            return 0;
        }

        // Info button tıklaması
        if (g_clickIsInfoButton)
        {
            float delta = fabsf(mx - g_mouseDownX) + fabsf(my - g_mouseDownY);
            if (delta < 5.0f)
            {
                g_viewState.showInfoPanel = !g_viewState.showInfoPanel;
                SaveSettings();
                StartPanelAnim(hwnd);
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
        float cx = static_cast<float>(GET_X_LPARAM(lParam));
        float cy = static_cast<float>(GET_Y_LPARAM(lParam));

        // Ok zone'da çift tıklamayı engelle — navigasyon zaten birinci LBUTTONUP'ta gerçekleşti
        if (HitTestArrow(hwnd, cx, cy, g_viewState.panelAnimWidth) != ArrowZone::None)
            return 0;

        // Info button üzerine çift tıklandığında zoom yapma
        if (HitTestInfoButton(hwnd, cx, cy, g_viewState.panelAnimWidth))
            return 0;

        // Panel alanında çift tıklamayı engelle
        if (g_viewState.panelAnimWidth > 0.0f)
        {
            RECT rc;
            GetClientRect(hwnd, &rc);
            if (cx >= static_cast<float>(rc.right) - g_viewState.panelAnimWidth)
                return 0;
        }

        if (g_viewState.zoomFactor == 1.0f)
        {
            ApplyZoom(hwnd, cx, cy, 2.5f);
        }
        else
        {
            // Zoom/pan sıfırla, ama UI durumunu koru
            bool  keepPanel     = g_viewState.showInfoPanel;
            float keepAnimWidth = g_viewState.panelAnimWidth;
            bool  keep12h       = g_viewState.use12HourTime;
            int   keepIdx       = g_viewState.imageIndex;
            int   keepTotal     = g_viewState.imageTotal;
            g_viewState = ViewState{};
            g_viewState.showInfoPanel  = keepPanel;
            g_viewState.panelAnimWidth = keepAnimWidth;
            g_viewState.use12HourTime  = keep12h;
            g_viewState.imageIndex     = keepIdx;
            g_viewState.imageTotal     = keepTotal;
        }

        ShowZoomIndicator(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT && g_renderer && g_renderer->IsGpsLinkVisible())
        {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            float cx = static_cast<float>(pt.x);
            float cy = static_cast<float>(pt.y);
            D2D1_RECT_F r = g_renderer->GetGpsLinkRect();
            if (cx >= r.left && cx <= r.right && cy >= r.top && cy <= r.bottom)
            {
                SetCursor(LoadCursor(nullptr, IDC_HAND));
                return TRUE;
            }
        }
        break;

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
            SaveSettings();
            StartPanelAnim(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            break;

        case 'T':
            g_viewState.use12HourTime = !g_viewState.use12HourTime;
            SaveSettings();
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
        else if (wParam == kPanelAnimTimerID)
        {
            float target = g_viewState.showInfoPanel ? PanelLayout::Width : 0.0f;
            float diff   = target - g_viewState.panelAnimWidth;
            if (fabsf(diff) < 1.0f)
            {
                g_viewState.panelAnimWidth = target;
                KillTimer(hwnd, kPanelAnimTimerID);
            }
            else
            {
                g_viewState.panelAnimWidth += diff * kPanelAnimLerp;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        else if (wParam == kAnimTimerID)
        {
            KillTimer(hwnd, kAnimTimerID);
            if (g_renderer && g_renderer->IsAnimated())
            {
                int nextDur = g_renderer->AdvanceFrame();
                SetTimer(hwnd, kAnimTimerID, static_cast<UINT>(nextDur), nullptr);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;

    case WM_DECODE_DONE:
    {
        auto* result = reinterpret_cast<DecodeResult*>(lParam);

        if (result->generation == g_decodeGeneration.load() && g_renderer)
        {
            // Önceki animasyonu ve timer'ı temizle
            KillTimer(hwnd, kAnimTimerID);
            g_renderer->ClearAnimation();

            if (!result->frames.empty())
            {
                // Animated görüntü
                g_renderer->ClearImage();
                g_renderer->LoadAnimationFrames(result->frames);
                int firstDur = g_renderer->GetCurrentFrameDuration();
                SetTimer(hwnd, kAnimTimerID, static_cast<UINT>(firstDur), nullptr);
            }
            else if (!result->pixels.empty())
            {
                // Statik görüntü
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
        SaveWindowPlacement(hwnd);
        ++g_decodeGeneration;
        if (g_decodeThread.joinable())
            g_decodeThread.detach();
        KillTimer(hwnd, kZoomIndicatorTimerID);
        KillTimer(hwnd, kPanelAnimTimerID);
        KillTimer(hwnd, kAnimTimerID);
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

    LoadSettings();

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

    int winX = g_savedWindowRect.valid ? g_savedWindowRect.x : CW_USEDEFAULT;
    int winY = g_savedWindowRect.valid ? g_savedWindowRect.y : CW_USEDEFAULT;
    int winW = g_savedWindowRect.valid ? g_savedWindowRect.w : 1280;
    int winH = g_savedWindowRect.valid ? g_savedWindowRect.h : 800;

    HWND hwnd = CreateWindowEx(
        0,
        L"PhotoViewerWindow",
        title.c_str(),
        WS_OVERLAPPEDWINDOW,
        winX, winY, winW, winH,
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
