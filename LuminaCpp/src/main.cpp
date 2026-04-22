#include <windows.h>
#include <windowsx.h>   // GET_X_LPARAM, GET_Y_LPARAM
#include <shellapi.h>   // CommandLineToArgvW
#include <mmsystem.h>   // timeBeginPeriod / timeEndPeriod
#pragma comment(lib, "winmm.lib")
#include <string>
#include <cmath>           // fabsf, expf
#include <thread>          // std::thread
#include <atomic>          // std::atomic
#include <vector>          // std::vector (DecodeResult piksel buffer'ı)
#include <unordered_map>   // Prefetch cache
#include <unordered_set>   // Prefetch desired set
#include <mutex>           // std::mutex
#include <semaphore>       // std::counting_semaphore
#include <cstdint>
#include "Renderer.h"
#include "FolderNavigator.h"
#include "ImageDecoder.h"

// --- Sabitler ---

static constexpr UINT     WM_DECODE_DONE        = WM_APP + 1;
static constexpr UINT     WM_TILE_DONE          = WM_APP + 2;
static constexpr UINT     WM_THUMB_DONE         = WM_APP + 3;
static constexpr UINT     WM_META_DONE          = WM_APP + 4;  // hızlı metadata aşaması
static constexpr UINT     WM_LOCATION_DONE      = WM_APP + 5;  // Nominatim sonucu
static constexpr UINT     WM_PREVIEW_DONE       = WM_APP + 6;  // HEIC gömülü thumbnail
static constexpr UINT_PTR kZoomIndicatorTimerID = 1;
static constexpr UINT_PTR kPanelAnimTimerID     = 2;
static constexpr UINT_PTR kAnimTimerID          = 3;
static constexpr UINT_PTR kZoomAnimTimerID      = 4;
static constexpr UINT_PTR kStripAnimTimerID     = 5;
static constexpr UINT_PTR kIndexIdleTimerID     = 6;  // 1.5s hareketsizlik → index fade başlar
static constexpr UINT_PTR kIndexFadeTimerID     = 7;  // index bar alpha animasyonu
static constexpr UINT_PTR kZoomFadeTimerID      = 8;  // zoom indicator alpha animasyonu
static constexpr UINT_PTR kCopyFeedbackTimerID  = 9;  // 1.5s kopyala feedback sıfırlama

// Animasyon timer aralığı — 7ms ≈ 143fps (timeBeginPeriod(1) ile hassas çalışır)
static constexpr UINT     kAnimIntervalMs       = 7;

// Panel ve zoom animasyon hızı — üstel sönüm sabiti (saniye⁻¹).
// lerp_per_frame = 1 - exp(-dt * speed); 60fps'de panel ≈ 0.33, zoom ≈ 0.25
static constexpr float    kPanelAnimSpeed       = 25.0f;
static constexpr float    kZoomAnimSpeed        = 30.0f;
static constexpr float    kStripAnimSpeed       = 25.0f;

// QPC frekansı ve son-tick zamanları — delta-time hesabı için
static LARGE_INTEGER      g_qpcFreq             = {};
static LARGE_INTEGER      g_panelAnimLastTime   = {};
static LARGE_INTEGER      g_zoomAnimLastTime    = {};
static LARGE_INTEGER      g_stripAnimLastTime   = {};
static LARGE_INTEGER      g_indexFadeLastTime   = {};
static LARGE_INTEGER      g_zoomFadeLastTime    = {};

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

// Hızlı metadata aşaması sonucu — piksel içermez, sadece ImageInfo alanları
struct MetaResult
{
    ImageInfo    info;
    uint64_t     generation = 0;
};

// Nominatim reverse geocoding sonucu — piksel decode'dan bağımsız aşama
struct LocationResult
{
    std::wstring locationName;
    uint64_t     generation = 0;
};

// HEIC gömülü thumbnail sonucu — tam decode bitmeden anlık önizleme için
struct PreviewResult
{
    std::vector<uint8_t> pixels;   // BGRA pre-mul
    UINT     width      = 0;
    UINT     height     = 0;
    uint64_t generation = 0;
};

static std::atomic<uint64_t> g_decodeGeneration{0};
static std::thread            g_decodeThread;

// --- Prefetch cache ---

static std::mutex                                       g_cacheMutex;
static std::unordered_map<std::wstring, DecodeResult*> g_decodeCache;
static constexpr size_t                                kCacheMaxSize   = 24;  // ~1.15GB (12MP HEIC × 24)
static constexpr int                                   kPrefetchRange  = 8;   // ±8 = 16 komşu

// Path-based prefetch iptal seti: decode tamamlandığında path hâlâ
// isteniyorsa cache'e eklenir, yoksa sonuç atılır.
static std::mutex                                        g_prefetchDesiredMutex;
static std::unordered_set<std::wstring>                  g_prefetchDesired;

// Aynı anda en fazla 4 ağır decode (CPU thrash önlemi: her biri ~100ms, 6 foto/sn için yeterli)
static std::counting_semaphore<16>                       g_prefetchSemaphore{4};

// --- Thumbnail decode cancel ---
static std::atomic<uint64_t>                           g_thumbCancel{0};

// --- Tile fetch cancel ---
static std::atomic<uint64_t>                           g_tileCancel{0};

// --- Decode yardımcıları ---

// Ortak decode mantığı — hem ana decode hem prefetch tarafından kullanılır.
// COM zaten başlatılmış olmalı. result->path önceden doldurulmuş olmalı.
// fetchLocation=false → Nominatim atlanır (StartDecode'da ayrı aşamada yapılır).
static void DoDecodeToResult(DecodeResult* result, bool fetchLocation = true)
{
    const std::wstring& path = result->path;

    // Dosya adı
    auto sep = path.rfind(L'\\');
    if (sep == std::wstring::npos) sep = path.rfind(L'/');
    result->info.filename = (sep != std::wstring::npos) ? path.substr(sep + 1) : path;

    // Dosya boyutu
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
        result->info.fileSizeBytes =
            (static_cast<int64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;

    // Decode (tüm format desteği ImageDecoder'da)
    DecodeOutput decoded;
    bool decodeOk   = DecodeImage(path, decoded);
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
        result->info.iccProfileName  = decoded.iccProfileName;

        // Nominatim reverse geocoding — fetchLocation=false ise atlanır
        if (fetchLocation && decoded.hasGpsDecimal)
            result->info.gpsLocationName =
                FetchLocationName(decoded.gpsLatDecimal, decoded.gpsLonDecimal);
    }
    else
    {
        result->info.errorMessage = L"Bu dosya açılamıyor";
    }
}

static void StartDecode(HWND hwnd, const std::wstring& path)
{
    uint64_t gen = ++g_decodeGeneration;

    if (g_decodeThread.joinable())
        g_decodeThread.detach();

    g_decodeThread = std::thread([hwnd, path, gen]()
    {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

        // ── Aşama 1: Hızlı metadata (piksel decode YOK) ──────────────────────
        // HEIC/AVIF gibi yavaş formatlarda ~50-100ms; info paneli hemen dolar.
        {
            DecodeOutput metaOut;
            if (ExtractImageMeta(path, metaOut))
            {
                // Jenerasyon hâlâ geçerliyse gönder (kullanıcı navigasyon yapmadıysa)
                if (g_decodeGeneration.load() == gen)
                {
                    auto* meta          = new MetaResult();
                    meta->generation    = gen;
                    meta->info.width    = static_cast<int>(metaOut.width);
                    meta->info.height   = static_cast<int>(metaOut.height);
                    meta->info.format        = metaOut.format;
                    meta->info.dateTaken     = metaOut.dateTaken;
                    meta->info.cameraMake    = metaOut.cameraMake;
                    meta->info.cameraModel   = metaOut.cameraModel;
                    meta->info.aperture      = metaOut.aperture;
                    meta->info.shutterSpeed  = metaOut.shutterSpeed;
                    meta->info.iso           = metaOut.iso;
                    meta->info.gpsLatitude   = metaOut.gpsLatitude;
                    meta->info.gpsLongitude  = metaOut.gpsLongitude;
                    meta->info.gpsAltitude   = metaOut.gpsAltitude;
                    meta->info.hasGpsDecimal = metaOut.hasGpsDecimal;
                    meta->info.gpsLatDecimal = metaOut.gpsLatDecimal;
                    meta->info.gpsLonDecimal = metaOut.gpsLonDecimal;
                    meta->info.iccProfileName = metaOut.iccProfileName;
                    PostMessage(hwnd, WM_META_DONE, 0, reinterpret_cast<LPARAM>(meta));
                }
            }
        }

        // ── Aşama 1b: HEIC gömülü thumbnail (~5-20ms, cache miss fallback) ────────
        {
            auto dot2 = path.rfind(L'.');
            if (dot2 != std::wstring::npos)
            {
                std::wstring ext2 = path.substr(dot2 + 1);
                for (auto& c : ext2) c = towupper(c);
                if ((ext2 == L"HEIC" || ext2 == L"HEIF")
                    && g_decodeGeneration.load() == gen)
                {
                    auto* prev = new PreviewResult{};
                    prev->generation = gen;
                    if (ExtractHEICEmbeddedPreview(path,
                            prev->pixels, prev->width, prev->height))
                    {
                        PostMessage(hwnd, WM_PREVIEW_DONE, 0,
                                    reinterpret_cast<LPARAM>(prev));
                    }
                    else
                    {
                        delete prev;
                    }
                }
            }
        }

        // ── Aşama 2: Piksel decode (Nominatim YOK — görsel hemen görünsün) ─────
        auto* result       = new DecodeResult();
        result->path       = path;
        result->generation = gen;

        DoDecodeToResult(result, /*fetchLocation=*/false);

        // GPS koordinatlarını Aşama 3 için kopyala (result UI thread'ine devrediliyor)
        const bool   hasGps = result->info.hasGpsDecimal;
        const double gpsLat = result->info.gpsLatDecimal;
        const double gpsLon = result->info.gpsLonDecimal;

        CoUninitialize();
        PostMessage(hwnd, WM_DECODE_DONE, 0, reinterpret_cast<LPARAM>(result));
        result = nullptr;  // sahiplik UI thread'ine geçti

        // ── Aşama 3: Nominatim reverse geocoding (görsel zaten göründü) ─────────
        if (hasGps && g_decodeGeneration.load() == gen)
        {
            auto locName = FetchLocationName(gpsLat, gpsLon);
            if (!locName.empty() && g_decodeGeneration.load() == gen)
            {
                auto* loc           = new LocationResult{};
                loc->locationName   = std::move(locName);
                loc->generation     = gen;
                PostMessage(hwnd, WM_LOCATION_DONE, 0, reinterpret_cast<LPARAM>(loc));
            }
        }
    });
}

// Prefetch: arka planda decode edip cache'e ekler, mesaj göndermez.
// cancelToken yerine g_prefetchDesired seti kullanılır: decode bitince path
// hâlâ isteniyorsa cache'e eklenir, değilse sonuç atılır (thread waste az).
static void StartPrefetch(const std::wstring& path)
{
    if (path.empty()) return;

    // Zaten cache'de var mı?
    {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        if (g_decodeCache.count(path)) return;
    }

    std::thread([path]()
    {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

        g_prefetchSemaphore.acquire();   // CPU thrash önlemi: max 4 eş zamanlı decode

        auto* result = new DecodeResult();
        result->path = path;

        DoDecodeToResult(result, /*fetchLocation=*/false);

        g_prefetchSemaphore.release();

        CoUninitialize();

        // Decode bitti — hâlâ isteniyor mu?
        {
            std::lock_guard<std::mutex> lk(g_prefetchDesiredMutex);
            if (!g_prefetchDesired.count(path))
            {
                delete result;   // artık istenmiyor, at
                return;
            }
        }

        // Cache'e ekle
        {
            std::lock_guard<std::mutex> lock(g_cacheMutex);
            // Cache doluysa en eski girdiyi at
            if (g_decodeCache.size() >= kCacheMaxSize)
            {
                auto it = g_decodeCache.begin();
                delete it->second;
                g_decodeCache.erase(it);
            }
            // Race: aynı path başka bir thread tarafından eklendi mi?
            if (!g_decodeCache.count(path))
                g_decodeCache[path] = result;
            else
                delete result;
        }
    }).detach();
}

// --- Yardımcılar ---

// Decode sonucunu renderer'a uygular — UI thread'de çağrılmalıdır.
// result'ın sahipliğini almaz; çağıran silmekten sorumludur.
static void ApplyDecodeResult(HWND hwnd, DecodeResult* result);  // ileriye bildirim

static void UpdateWindowTitle(HWND hwnd, const std::wstring& filePath)
{
    std::wstring title = L"Lumina";
    if (!filePath.empty())
    {
        auto pos = filePath.find_last_of(L"\\/");
        std::wstring name = (pos != std::wstring::npos) ? filePath.substr(pos + 1) : filePath;
        title = name + L" \u2014 Lumina";
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
static bool  g_clickInStrip      = false;  // Strip veya toggle tıklaması


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

static constexpr wchar_t kRegKey[] = L"Software\\Lumina";

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
        val = g_viewState.showThumbStrip ? 1 : 0;
        RegSetValueExW(hKey, L"ShowThumbStrip", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&val), sizeof(DWORD));
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
        sz = sizeof(DWORD);
        if (RegQueryValueExW(hKey, L"ShowThumbStrip", nullptr, nullptr, reinterpret_cast<LPBYTE>(&val), &sz) == ERROR_SUCCESS)
            g_viewState.showThumbStrip = (val != 0);

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
    // Başlangıçta animasyon yok — doğrudan hedeflere atla
    g_viewState.panelAnimWidth  = g_viewState.showInfoPanel  ? PanelLayout::Width  : 0.0f;
    g_viewState.stripAnimHeight = g_viewState.showThumbStrip ? StripLayout::OpenH  : 0.0f;
}

// --- Animasyon başlatıcıları ---

static void StartPanelAnim(HWND hwnd)
{
    QueryPerformanceCounter(&g_panelAnimLastTime);
    KillTimer(hwnd, kPanelAnimTimerID);
    SetTimer(hwnd, kPanelAnimTimerID, kAnimIntervalMs, nullptr);
}

static void StartZoomAnim(HWND hwnd)
{
    QueryPerformanceCounter(&g_zoomAnimLastTime);
    KillTimer(hwnd, kZoomAnimTimerID);
    SetTimer(hwnd, kZoomAnimTimerID, kAnimIntervalMs, nullptr);
}

static void StartStripAnim(HWND hwnd)
{
    QueryPerformanceCounter(&g_stripAnimLastTime);
    KillTimer(hwnd, kStripAnimTimerID);
    SetTimer(hwnd, kStripAnimTimerID, kAnimIntervalMs, nullptr);
}

// Kullanıcı aktivitesinde çağrılır: alpha'yı 1'e sıfırlar,
// fade timer'larını durdurur ve 5s idle timer'ı yeniden başlatır.
static void ResetIndexIdleTimer(HWND hwnd)
{
    g_viewState.indexBarAlpha = 1.0f;
    KillTimer(hwnd, kIndexFadeTimerID);
    KillTimer(hwnd, kIndexIdleTimerID);
    SetTimer(hwnd, kIndexIdleTimerID, 1500, nullptr);
}

// --- Decode sonucu uygulama + prefetch tetikleyici ---

// Decode sonucunu renderer'a ve g_imageInfo'ya uygular (UI thread'de çağrılır).
static void ApplyDecodeResult(HWND hwnd, DecodeResult* result)
{
    KillTimer(hwnd, kAnimTimerID);
    if (g_renderer) g_renderer->ClearAnimation();

    if (g_renderer)
    {
        if (!result->frames.empty())
        {
            g_renderer->ClearImage();
            g_renderer->LoadAnimationFrames(result->frames);
            int firstDur = g_renderer->GetCurrentFrameDuration();
            SetTimer(hwnd, kAnimTimerID, static_cast<UINT>(firstDur), nullptr);
        }
        else if (!result->pixels.empty())
        {
            g_renderer->LoadImageFromPixels(
                result->pixels.data(), result->width, result->height, result->path);
        }
        else
        {
            g_renderer->ClearImage();
        }
    }
    g_imageInfo = result->info;
    ResetIndexIdleTimer(hwnd);
    InvalidateRect(hwnd, nullptr, FALSE);
}

// Mevcut konumun ±kPrefetchRange komşularını prefetch eder.
// Yakından uzağa öncelik: +1,-1,+2,-2,... sırası ile thread başlatılır.
// Semafor(4) sayesinde yakın komşular CPU slotunu önce alır.
static void TriggerPrefetch()
{
    if (!g_navigator || g_navigator->empty()) return;

    std::unordered_set<std::wstring> newDesired;
    std::vector<std::wstring>        ordered;   // yakından uzağa öncelik sırası

    for (int d = 1; d <= kPrefetchRange; ++d)
    {
        for (int sign : {+1, -1})
        {
            const std::wstring p = g_navigator->peek_at_linear(d * sign);
            if (!p.empty() && !newDesired.count(p))
            {
                newDesired.insert(p);
                ordered.push_back(p);
            }
        }
    }

    {
        std::lock_guard<std::mutex> lk(g_prefetchDesiredMutex);
        g_prefetchDesired = newDesired;
    }

    for (const auto& p : ordered)
        StartPrefetch(p);
}

// --- Thumbnail decode yardımcıları ---

struct ThumbResult
{
    std::wstring         path;
    std::vector<uint8_t> pixels;  // BGRA pre-mul, ölçeklenmiş
    UINT                 width  = 0;
    UINT                 height = 0;
    uint64_t             cancelToken = 0;
};

static void StartThumbDecode(HWND hwnd, const std::wstring& path, uint64_t cancelToken)
{
    if (path.empty()) return;

    std::thread([hwnd, path, cancelToken]()
    {
        if (g_thumbCancel.load() != cancelToken) return;

        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

        constexpr UINT kTargetH = static_cast<UINT>(StripLayout::ThumbH);
        std::vector<uint8_t> pixels;
        UINT outW = 0, outH = 0;
        bool ok = DecodeImageForThumbnail(path, kTargetH, pixels, outW, outH);

        CoUninitialize();

        if (g_thumbCancel.load() != cancelToken) return;
        if (!ok || pixels.empty()) return;

        auto* result        = new ThumbResult();
        result->path        = path;
        result->pixels      = std::move(pixels);
        result->width       = outW;
        result->height      = outH;
        result->cancelToken = cancelToken;

        PostMessage(hwnd, WM_THUMB_DONE, 0, reinterpret_cast<LPARAM>(result));
    }).detach();
}

// --- Tile fetch yardımcıları ---

struct TileFetchResult
{
    int                  zoom        = 0;
    int                  x           = 0;
    int                  y           = 0;
    std::vector<uint8_t> pixels;     // BGRA pre-mul — PNG decode background thread'de yapılır
    UINT                 width       = 0;
    UINT                 height      = 0;
    uint64_t             cancelToken = 0;
};

// 3×2 tile grid'i (zoom 14) için 6 paralel fetch thread başlatır.
// Her thread kendi token kontrolünü yapar; geçersizse veriyi atmaz.
static void StartTileFetches(HWND hwnd, double lat, double lon)
{
    ++g_tileCancel;
    const uint64_t token = g_tileCancel.load();

    constexpr int kZoom = 14;
    int cx, cy;
    LatLonToTileXY(lat, lon, kZoom, cx, cy);

    for (int row = 0; row < 2; ++row)
    {
        for (int col = 0; col < 3; ++col)
        {
            const int tx = cx - 1 + col;
            const int ty = cy - 1 + row;

            std::thread([hwnd, tx, ty, token]()
            {
                if (g_tileCancel.load() != token) return;

                CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

                // PNG indir
                std::vector<uint8_t> png = FetchOsmTile(kZoom, tx, ty);

                // PNG'yi BGRA piksellerine decode et — UI thread yerine burada yapılır
                std::vector<uint8_t> pixels;
                UINT w = 0, h = 0;
                if (!png.empty())
                    DecodePngToPixels(png.data(), png.size(), pixels, w, h);

                CoUninitialize();

                if (g_tileCancel.load() != token || pixels.empty()) return;

                auto* result        = new TileFetchResult();
                result->zoom        = kZoom;
                result->x           = tx;
                result->y           = ty;
                result->pixels      = std::move(pixels);
                result->width       = w;
                result->height      = h;
                result->cancelToken = token;

                PostMessage(hwnd, WM_TILE_DONE, 0, reinterpret_cast<LPARAM>(result));
            }).detach();
        }
    }
}

// Strip slot'larını navigator'a göre renderer'a yükler
static void UpdateStripSlots()
{
    if (!g_renderer) return;
    if (!g_navigator || g_navigator->empty())
    {
        g_renderer->SetStripSlots({}, 0);
        return;
    }
    constexpr int kHalf = StripLayout::HalfCount;
    std::vector<std::wstring> paths;
    paths.reserve(2 * kHalf + 1);
    for (int i = -kHalf; i <= kHalf; ++i)
        paths.push_back(g_navigator->peek_at_linear(i));  // döngüsüz: sınır dışı = boş
    g_renderer->SetStripSlots(paths, kHalf);
}

// Mevcut strip slot'ları için thumbnail decode'larını başlatır (önbellekte yoksa)
static void TriggerThumbFetches(HWND hwnd)
{
    if (!g_renderer || !g_navigator || g_navigator->empty()) return;
    uint64_t token = g_thumbCancel.load();
    constexpr int kHalf = StripLayout::HalfCount;
    for (int i = -kHalf; i <= kHalf; ++i)
    {
        const std::wstring& path = g_navigator->peek_at(i);
        if (!g_renderer->HasThumbnail(path))
            StartThumbDecode(hwnd, path, token);
    }
}

// --- Yardımcı: ViewState'in UI alanlarını koruyarak navigasyon ---

static void NavigateTo(HWND hwnd, const std::wstring& path)
{
    KillTimer(hwnd, kAnimTimerID);
    KillTimer(hwnd, kZoomAnimTimerID);
    if (g_renderer) g_renderer->ClearAnimation();
    UpdateWindowTitle(hwnd, path);
    ++g_thumbCancel;  // Eski thumbnail decode thread'lerini iptal et
    ++g_tileCancel;   // Eski tile fetch thread'lerini iptal et
    if (g_renderer) g_renderer->ClearMapTiles();  // Önceki görüntünün tile'larını temizle

    bool  keepPanel      = g_viewState.showInfoPanel;
    float keepAnimWidth  = g_viewState.panelAnimWidth;
    bool  keep12h        = g_viewState.use12HourTime;
    bool  keepStrip      = g_viewState.showThumbStrip;
    float keepStripH     = g_viewState.stripAnimHeight;
    g_viewState = ViewState{};
    g_viewState.showInfoPanel   = keepPanel;
    g_viewState.panelAnimWidth  = keepAnimWidth;
    g_viewState.use12HourTime   = keep12h;
    g_viewState.showThumbStrip  = keepStrip;
    g_viewState.stripAnimHeight = keepStripH;
    if (g_navigator)
    {
        g_viewState.imageIndex = g_navigator->index() + 1;
        g_viewState.imageTotal = g_navigator->total();
    }

    // Info panelini hemen güncelle: dosya adını göster, EXIF temizle
    g_imageInfo = ImageInfo{};
    {
        auto sep = path.rfind(L'\\');
        if (sep == std::wstring::npos) sep = path.rfind(L'/');
        g_imageInfo.filename = (sep != std::wstring::npos) ? path.substr(sep + 1) : path;
    }

    // Prefetch cache kontrolü — hit ise anında uygula
    {
        DecodeResult* cached = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_cacheMutex);
            auto it = g_decodeCache.find(path);
            if (it != g_decodeCache.end())
            {
                cached = it->second;
                g_decodeCache.erase(it);
            }
        }  // lock burada serbest bırakılır

        if (cached)
        {
            // g_decodeGeneration'ı ilerlet — eski decode thread'i (varsa) geçersiz kılınır
            ++g_decodeGeneration;
            ApplyDecodeResult(hwnd, cached);
            delete cached;
            // Lock serbest bırakıldıktan sonra prefetch tetikle (deadlock önlemi)
            TriggerPrefetch();
            UpdateStripSlots();
            TriggerThumbFetches(hwnd);
            // GPS varsa OSM tile'larını arka planda çek (cache hit yolunda da gerekli)
            if (g_imageInfo.hasGpsDecimal)
            {
                StartTileFetches(hwnd, g_imageInfo.gpsLatDecimal, g_imageInfo.gpsLonDecimal);
                // Prefetch Nominatim yapmadı — cache hit'te arka planda çek
                if (g_imageInfo.gpsLocationName.empty())
                {
                    const uint64_t gen = g_decodeGeneration.load();
                    const double   lat = g_imageInfo.gpsLatDecimal;
                    const double   lon = g_imageInfo.gpsLonDecimal;
                    std::thread([hwnd, lat, lon, gen]()
                    {
                        auto locName = FetchLocationName(lat, lon);
                        if (!locName.empty() && g_decodeGeneration.load() == gen)
                        {
                            auto* loc         = new LocationResult{};
                            loc->locationName = std::move(locName);
                            loc->generation   = gen;
                            PostMessage(hwnd, WM_LOCATION_DONE, 0,
                                        reinterpret_cast<LPARAM>(loc));
                        }
                    }).detach();
                }
            }
            return;
        }
    }

    // Cache miss — normal decode başlat; strip slot'larını hemen güncelle
    ResetIndexIdleTimer(hwnd);
    UpdateStripSlots();
    TriggerThumbFetches(hwnd);
    StartDecode(hwnd, path);
    InvalidateRect(hwnd, nullptr, FALSE);
}

// --- Zoom yardımcısı ---

// Yeni zoom hedefini (target) hesaplar; animasyon timer smoothly yaklaşır.
// Temel olarak mevcut *animasyonlu* pozisyon (zoomFactor/panX/panY) kullanılır —
// böylece hızlı scroll'da her tick önceki animasyonun üstüne biner.
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
    float availW = (rc.right - rc.left) - g_viewState.panelAnimWidth;
    float halfW = availW * 0.5f;
    float halfH = ((rc.bottom - rc.top) - g_viewState.stripAnimHeight) * 0.5f;

    // Hedef pozisyonu temel al — hızlı scroll'da doğru birikim için
    float ratio = newZoom / g_viewState.zoomTarget;
    g_viewState.panXTarget = (cx - halfW) - (cx - halfW - g_viewState.panXTarget) * ratio;
    g_viewState.panYTarget = (cy - halfH) - (cy - halfH - g_viewState.panYTarget) * ratio;
    g_viewState.zoomTarget = newZoom;

    StartZoomAnim(hwnd);
}

static void ShowZoomIndicator(HWND hwnd)
{
    g_viewState.zoomIndicatorAlpha = 1.0f;
    KillTimer(hwnd, kZoomFadeTimerID);
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

        RECT rc;
        GetClientRect(hwnd, &rc);
        float wndW = static_cast<float>(rc.right);
        float wndH = static_cast<float>(rc.bottom);

        // Info panel üzerindeyse → panel içeriğini kaydır (zoom/navigasyon değil)
        if (g_viewState.panelAnimWidth > 0.0f &&
            cx >= wndW - g_viewState.panelAnimWidth)
        {
            constexpr float kScrollStep = 40.0f;
            g_viewState.panelScrollY += (delta > 0 ? -kScrollStep : kScrollStep);
            // Sınırla: 0 ≤ scrollY ≤ (içerik yüksekliği − görünür alan)
            float maxScroll = 0.0f;
            if (g_renderer)
            {
                float visibleH = wndH - PanelLayout::HeaderH - g_viewState.stripAnimHeight;
                maxScroll = max(0.0f, g_renderer->GetInfoPanelContentHeight() - visibleH);
            }
            g_viewState.panelScrollY = max(0.0f, min(maxScroll, g_viewState.panelScrollY));
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        // Filmstrip üzerindeyse scroll → navigasyon (panel alanı zaten yukarıda döndü)
        if (g_viewState.stripAnimHeight > 0.0f && g_navigator && !g_navigator->empty())
        {
            if (cy >= wndH - g_viewState.stripAnimHeight)
            {
                if (delta > 0)
                    NavigateTo(hwnd, g_navigator->prev());
                else
                    NavigateTo(hwnd, g_navigator->next());
                return 0;
            }
        }

        // zoomTarget baz alınır — hızlı scroll'da her tick önceki hedef üstüne biner
        float newZoom = g_viewState.zoomTarget * (delta > 0 ? 1.15f : (1.0f / 1.15f));
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
        ResetIndexIdleTimer(hwnd);

        // Info button önce kontrol edilir (sağ ok zone ile üst üste gelebilir)
        g_clickIsInfoButton = HitTestInfoButton(hwnd, mx, my, g_viewState.panelAnimWidth);
        if (g_clickIsInfoButton)
        {
            g_viewState.infoBtnPressed = true;
            InvalidateRect(hwnd, nullptr, FALSE);
        }

        if (!g_clickIsInfoButton)
        {
            // Strip toggle pill tıklaması (strip alanının dışında olabilir)
            if (g_renderer && g_renderer->IsStripToggleVisible())
            {
                D2D1_RECT_F tr = g_renderer->GetStripToggleRect();
                if (mx >= tr.left && mx <= tr.right && my >= tr.top && my <= tr.bottom)
                {
                    g_viewState.toggleBtnPressed = true;
                    g_clickInStrip = true;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
            }

            // Strip alanına tıklama (thumbnail filmstrip)
            if (g_viewState.stripAnimHeight > 0.0f)
            {
                RECT rc;
                GetClientRect(hwnd, &rc);
                if (my >= static_cast<float>(rc.bottom) - g_viewState.stripAnimHeight)
                {
                    g_clickInStrip = true;
                    return 0;
                }
            }

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
            g_viewState.leftArrowPressed  = (zone == ArrowZone::Left);
            g_viewState.rightArrowPressed = (zone == ArrowZone::Right);
            if (g_viewState.leftArrowPressed || g_viewState.rightArrowPressed)
                InvalidateRect(hwnd, nullptr, FALSE);

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
        float mx = static_cast<float>(GET_X_LPARAM(lParam));
        float my = static_cast<float>(GET_Y_LPARAM(lParam));

        if (!g_dragging) return 0;
        float nx = g_panAtDragStartX + (mx - g_dragStartX);
        float ny = g_panAtDragStartY + (my - g_dragStartY);
        g_viewState.panX = g_viewState.panXTarget = nx;
        g_viewState.panY = g_viewState.panYTarget = ny;
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

        // Press highlight'ları her durumda temizle
        bool needRepaint = g_viewState.infoBtnPressed
                        || g_viewState.leftArrowPressed
                        || g_viewState.rightArrowPressed
                        || g_viewState.toggleBtnPressed;
        g_viewState.infoBtnPressed    = false;
        g_viewState.leftArrowPressed  = false;
        g_viewState.rightArrowPressed = false;
        g_viewState.toggleBtnPressed  = false;
        if (needRepaint)
            InvalidateRect(hwnd, nullptr, FALSE);

        // Strip / toggle tıklaması
        if (g_clickInStrip)
        {
            g_clickInStrip = false;
            float delta = fabsf(mx - g_mouseDownX) + fabsf(my - g_mouseDownY);
            if (delta < 5.0f && g_renderer)
            {
                // Toggle pill tıklaması
                if (g_renderer->IsStripToggleVisible())
                {
                    D2D1_RECT_F tr = g_renderer->GetStripToggleRect();
                    if (mx >= tr.left && mx <= tr.right && my >= tr.top && my <= tr.bottom)
                    {
                        g_viewState.showThumbStrip = !g_viewState.showThumbStrip;
                        SaveSettings();
                        StartStripAnim(hwnd);
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }
                }

                // Thumbnail tıklaması — offset kadar navigasyon
                int offset = g_renderer->GetThumbClickOffset(mx, my);
                if (offset != INT_MIN && offset != 0 && g_navigator && !g_navigator->empty())
                {
                    const std::wstring& path = g_navigator->jump(offset);
                    NavigateTo(hwnd, path);
                }
            }
            return 0;
        }

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
                            L"https://maps.google.com/?q=%.6f,%.6f",
                            g_imageInfo.gpsLatDecimal, g_imageInfo.gpsLonDecimal);
                        ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
                        return 0;
                    }
                }
                // Kopyala butonu — koordinatları panoya kopyala
                if (g_renderer->IsMapCopyBtnVisible() && g_imageInfo.hasGpsDecimal)
                {
                    D2D1_RECT_F r = g_renderer->GetMapCopyBtnRect();
                    if (mx >= r.left && mx <= r.right && my >= r.top && my <= r.bottom)
                    {
                        wchar_t coordStr[64];
                        swprintf_s(coordStr, L"%.6f, %.6f",
                                   g_imageInfo.gpsLatDecimal, g_imageInfo.gpsLonDecimal);
                        if (OpenClipboard(hwnd))
                        {
                            EmptyClipboard();
                            const size_t bytes = (wcslen(coordStr) + 1) * sizeof(wchar_t);
                            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
                            if (hMem)
                            {
                                memcpy(GlobalLock(hMem), coordStr, bytes);
                                GlobalUnlock(hMem);
                                SetClipboardData(CF_UNICODETEXT, hMem);
                            }
                            CloseClipboard();
                        }
                        g_renderer->MarkCoordsCopied();
                        SetTimer(hwnd, kCopyFeedbackTimerID, 1500, nullptr);
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }
                }
                // Harita önizleme tıklaması — OSM haritasını tarayıcıda aç
                if (g_renderer->IsMapPreviewVisible() && g_imageInfo.hasGpsDecimal)
                {
                    D2D1_RECT_F r = g_renderer->GetMapPreviewRect();
                    if (mx >= r.left && mx <= r.right && my >= r.top && my <= r.bottom)
                    {
                        wchar_t url[256];
                        swprintf_s(url,
                            L"https://maps.google.com/?q=%.6f,%.6f",
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

        // Strip alanında çift tıklamayı engelle
        if (g_viewState.stripAnimHeight > 0.0f)
        {
            RECT rc;
            GetClientRect(hwnd, &rc);
            if (cy >= static_cast<float>(rc.bottom) - g_viewState.stripAnimHeight)
                return 0;
        }

        // Toggle pill üzerinde çift tıklamayı engelle
        if (g_renderer && g_renderer->IsStripToggleVisible())
        {
            D2D1_RECT_F tr = g_renderer->GetStripToggleRect();
            if (cx >= tr.left && cx <= tr.right && cy >= tr.top && cy <= tr.bottom)
                return 0;
        }

        if (fabsf(g_viewState.zoomFactor - 1.0f) < 0.02f &&
            fabsf(g_viewState.zoomTarget - 1.0f) < 0.02f)
        {
            ApplyZoom(hwnd, cx, cy, 2.5f);
        }
        else
        {
            // Zoom/pan sıfırla — animasyonlu olarak fit-to-window'a dön
            g_viewState.zoomTarget = 1.0f;
            g_viewState.panXTarget = 0.0f;
            g_viewState.panYTarget = 0.0f;
            StartZoomAnim(hwnd);
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
        ResetIndexIdleTimer(hwnd);
        switch (wParam)
        {
        case VK_ESCAPE:
            DestroyWindow(hwnd);
            break;

        case 'W':
            if (GetKeyState(VK_CONTROL) & 0x8000)
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

        case 'F':
            g_viewState.showThumbStrip = !g_viewState.showThumbStrip;
            SaveSettings();
            StartStripAnim(hwnd);
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
            // 1.5s doldu — fade animasyonunu başlat
            KillTimer(hwnd, kZoomIndicatorTimerID);
            QueryPerformanceCounter(&g_zoomFadeLastTime);
            SetTimer(hwnd, kZoomFadeTimerID, kAnimIntervalMs, nullptr);
        }
        else if (wParam == kZoomFadeTimerID)
        {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            float dt = static_cast<float>(now.QuadPart - g_zoomFadeLastTime.QuadPart)
                       / static_cast<float>(g_qpcFreq.QuadPart);
            g_zoomFadeLastTime = now;
            dt = min(dt, 0.1f);

            constexpr float kFadeSpeed = 1.5f;
            g_viewState.zoomIndicatorAlpha -= dt * kFadeSpeed;
            if (g_viewState.zoomIndicatorAlpha <= 0.0f)
            {
                g_viewState.zoomIndicatorAlpha = 0.0f;
                KillTimer(hwnd, kZoomFadeTimerID);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        else if (wParam == kPanelAnimTimerID)
        {
            // Delta-time hesabı (saniye cinsinden) — frame hızından bağımsız animasyon
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            float dt = static_cast<float>(now.QuadPart - g_panelAnimLastTime.QuadPart)
                       / static_cast<float>(g_qpcFreq.QuadPart);
            g_panelAnimLastTime = now;
            dt = min(dt, 0.1f);  // Uygulama duraksamalarında animasyonun sıçramasını önle

            float lerp   = 1.0f - expf(-dt * kPanelAnimSpeed);
            float target = g_viewState.showInfoPanel ? PanelLayout::Width : 0.0f;
            float diff   = target - g_viewState.panelAnimWidth;
            if (fabsf(diff) < 0.5f)
            {
                g_viewState.panelAnimWidth = target;
                KillTimer(hwnd, kPanelAnimTimerID);
            }
            else
            {
                g_viewState.panelAnimWidth += diff * lerp;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        else if (wParam == kZoomAnimTimerID)
        {
            // Delta-time tabanlı zoom lerp
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            float dt = static_cast<float>(now.QuadPart - g_zoomAnimLastTime.QuadPart)
                       / static_cast<float>(g_qpcFreq.QuadPart);
            g_zoomAnimLastTime = now;
            dt = min(dt, 0.1f);

            float lerp     = 1.0f - expf(-dt * kZoomAnimSpeed);
            float zoomDiff = g_viewState.zoomTarget - g_viewState.zoomFactor;
            float panXDiff = g_viewState.panXTarget - g_viewState.panX;
            float panYDiff = g_viewState.panYTarget - g_viewState.panY;

            if (fabsf(zoomDiff) < 0.0005f && fabsf(panXDiff) < 0.3f && fabsf(panYDiff) < 0.3f)
            {
                g_viewState.zoomFactor = g_viewState.zoomTarget;
                g_viewState.panX       = g_viewState.panXTarget;
                g_viewState.panY       = g_viewState.panYTarget;
                KillTimer(hwnd, kZoomAnimTimerID);
            }
            else
            {
                g_viewState.zoomFactor += zoomDiff * lerp;
                g_viewState.panX       += panXDiff * lerp;
                g_viewState.panY       += panYDiff * lerp;
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
        else if (wParam == kIndexIdleTimerID)
        {
            // 5s hareketsizlik doldu — fade animasyonunu başlat
            KillTimer(hwnd, kIndexIdleTimerID);
            QueryPerformanceCounter(&g_indexFadeLastTime);
            SetTimer(hwnd, kIndexFadeTimerID, kAnimIntervalMs, nullptr);
        }
        else if (wParam == kIndexFadeTimerID)
        {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            float dt = static_cast<float>(now.QuadPart - g_indexFadeLastTime.QuadPart)
                       / static_cast<float>(g_qpcFreq.QuadPart);
            g_indexFadeLastTime = now;
            dt = min(dt, 0.1f);

            constexpr float kFadeSpeed = 1.5f;  // saniyede tam opaklık kaybolur
            g_viewState.indexBarAlpha -= dt * kFadeSpeed;
            if (g_viewState.indexBarAlpha <= 0.0f)
            {
                g_viewState.indexBarAlpha = 0.0f;
                KillTimer(hwnd, kIndexFadeTimerID);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        else if (wParam == kStripAnimTimerID)
        {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            float dt = static_cast<float>(now.QuadPart - g_stripAnimLastTime.QuadPart)
                       / static_cast<float>(g_qpcFreq.QuadPart);
            g_stripAnimLastTime = now;
            dt = min(dt, 0.1f);

            float lerp   = 1.0f - expf(-dt * kStripAnimSpeed);
            float target = g_viewState.showThumbStrip ? StripLayout::OpenH : 0.0f;
            float diff   = target - g_viewState.stripAnimHeight;
            if (fabsf(diff) < 0.5f)
            {
                g_viewState.stripAnimHeight = target;
                KillTimer(hwnd, kStripAnimTimerID);
            }
            else
            {
                g_viewState.stripAnimHeight += diff * lerp;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        else if (wParam == kCopyFeedbackTimerID)
        {
            KillTimer(hwnd, kCopyFeedbackTimerID);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_DECODE_DONE:
    {
        auto* result = reinterpret_cast<DecodeResult*>(lParam);

        if (result->generation == g_decodeGeneration.load() && g_renderer)
        {
            // WM_META_DONE GPS'i zaten set ettiyse tile fetch başlatıldı — tekrarlama
            const bool tilesAlreadyStarted = g_imageInfo.hasGpsDecimal;

            ApplyDecodeResult(hwnd, result);
            // Decode tamamlandı — komşuları arka planda yükle
            TriggerPrefetch();
            // Strip slot'larını güncelle (ilk açılışta NavigateTo çağrılmaz, burada yapılır)
            UpdateStripSlots();
            TriggerThumbFetches(hwnd);
            // GPS varsa OSM tile'larını arka planda çek (META_DONE başlatmadıysa)
            if (g_imageInfo.hasGpsDecimal && !tilesAlreadyStarted)
                StartTileFetches(hwnd, g_imageInfo.gpsLatDecimal, g_imageInfo.gpsLonDecimal);
        }

        delete result;
        return 0;
    }

    case WM_META_DONE:
    {
        auto* meta = reinterpret_cast<MetaResult*>(lParam);
        if (meta->generation == g_decodeGeneration.load())
        {
            // filename ve fileSizeBytes NavigateTo tarafından zaten set edildi; koru.
            meta->info.filename      = g_imageInfo.filename;
            meta->info.fileSizeBytes = g_imageInfo.fileSizeBytes;
            g_imageInfo = std::move(meta->info);
            // GPS varsa OSM tile'larını hemen başlat — piksel decode bekleme
            if (g_imageInfo.hasGpsDecimal)
                StartTileFetches(hwnd, g_imageInfo.gpsLatDecimal, g_imageInfo.gpsLonDecimal);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        delete meta;
        return 0;
    }

    case WM_LOCATION_DONE:
    {
        auto* loc = reinterpret_cast<LocationResult*>(lParam);
        if (loc->generation == g_decodeGeneration.load())
        {
            g_imageInfo.gpsLocationName = std::move(loc->locationName);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        delete loc;
        return 0;
    }

    case WM_PREVIEW_DONE:
    {
        auto* prev = reinterpret_cast<PreviewResult*>(lParam);
        if (prev->generation == g_decodeGeneration.load()
            && g_renderer && !prev->pixels.empty())
        {
            // Gömülü HEIC thumbnail'i geçici olarak göster; WM_DECODE_DONE tam görseli yazar
            g_renderer->LoadImageFromPixels(
                prev->pixels.data(), prev->width, prev->height, L"");
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        delete prev;
        return 0;
    }

    case WM_THUMB_DONE:
    {
        auto* result = reinterpret_cast<ThumbResult*>(lParam);
        // Geçersiz token → başka bir navigasyon iptal etti, atla
        if (result->cancelToken == g_thumbCancel.load() && g_renderer && !result->pixels.empty())
        {
            g_renderer->LoadThumbnail(result->path,
                                      result->pixels.data(),
                                      result->width,
                                      result->height);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        delete result;
        return 0;
    }

    case WM_TILE_DONE:
    {
        auto* result = reinterpret_cast<TileFetchResult*>(lParam);
        if (result->cancelToken == g_tileCancel.load() && g_renderer && !result->pixels.empty())
        {
            g_renderer->UploadMapTileRaw(result->zoom, result->x, result->y,
                                         result->pixels.data(), result->width, result->height);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        delete result;
        return 0;
    }

    case WM_DESTROY:
        SaveWindowPlacement(hwnd);
        ++g_decodeGeneration;
        // Prefetch thread'leri g_prefetchDesired üzerinden kontrol edilir
        { std::lock_guard<std::mutex> lk(g_prefetchDesiredMutex); g_prefetchDesired.clear(); }
        ++g_thumbCancel;     // Tüm thumbnail decode thread'lerini durdur
        ++g_tileCancel;      // Tüm tile fetch thread'lerini durdur
        if (g_decodeThread.joinable())
            g_decodeThread.detach();
        KillTimer(hwnd, kZoomIndicatorTimerID);
        KillTimer(hwnd, kPanelAnimTimerID);
        KillTimer(hwnd, kAnimTimerID);
        KillTimer(hwnd, kZoomAnimTimerID);
        KillTimer(hwnd, kStripAnimTimerID);
        KillTimer(hwnd, kIndexIdleTimerID);
        KillTimer(hwnd, kIndexFadeTimerID);
        KillTimer(hwnd, kZoomFadeTimerID);
        KillTimer(hwnd, kCopyFeedbackTimerID);
        timeEndPeriod(1);
        // Prefetch cache'ini temizle
        {
            std::lock_guard<std::mutex> lock(g_cacheMutex);
            for (auto& [path, result] : g_decodeCache)
                delete result;
            g_decodeCache.clear();
        }
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

    // Yüksek çözünürlüklü timer — 7ms animasyon intervalinin doğru çalışması için
    timeBeginPeriod(1);
    QueryPerformanceFrequency(&g_qpcFreq);

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
    wc.lpszClassName = L"LuminaWindow";
    wc.hIcon         = LoadIcon(hInstance, MAKEINTRESOURCE(101));
    wc.hIconSm       = (HICON)LoadImage(hInstance, MAKEINTRESOURCE(101), IMAGE_ICON, 16, 16, 0);
    RegisterClassEx(&wc);

    std::wstring title = L"Lumina";
    if (!filePath.empty())
    {
        auto pos = filePath.find_last_of(L"\\/");
        std::wstring name = (pos != std::wstring::npos) ? filePath.substr(pos + 1) : filePath;
        title = name + L" \u2014 Lumina";
    }

    int winX = g_savedWindowRect.valid ? g_savedWindowRect.x : CW_USEDEFAULT;
    int winY = g_savedWindowRect.valid ? g_savedWindowRect.y : CW_USEDEFAULT;
    int winW = g_savedWindowRect.valid ? g_savedWindowRect.w : 1280;
    int winH = g_savedWindowRect.valid ? g_savedWindowRect.h : 800;

    HWND hwnd = CreateWindowEx(
        0,
        L"LuminaWindow",
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

        // Thumbnail decode'larını ana decode ile paralel başlat
        // (WM_DECODE_DONE beklenmez — her ikisi de eş zamanlı çalışır)
        UpdateStripSlots();
        TriggerThumbFetches(hwnd);
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
