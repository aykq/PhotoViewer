#include <windows.h>
#include <windowsx.h>   // GET_X_LPARAM, GET_Y_LPARAM
#include <shellapi.h>   // CommandLineToArgvW
#include <mmsystem.h>   // timeBeginPeriod / timeEndPeriod
#pragma comment(lib, "winmm.lib")
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#include <commdlg.h>
#pragma comment(lib, "comdlg32.lib")

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#include <string>
#include <cmath>               // fabsf, expf, round
#include <thread>              // std::thread
#include <atomic>              // std::atomic
#include <vector>              // std::vector (DecodeResult piksel buffer'ı)
#include <deque>               // location prefetch queue
#include <unordered_map>       // Prefetch cache
#include <unordered_set>       // Prefetch desired set
#include <mutex>               // std::mutex
#include <condition_variable>  // location prefetch worker
#include <semaphore>           // std::counting_semaphore
#include <cstdint>
#include <cstdio>              // fopen, fwprintf (disk cache I/O)
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
static constexpr UINT     WM_SAVE_DONE          = WM_APP + 7;  // arka plan kayıt tamamlandı
static constexpr UINT     WM_FILE_CHANGED       = WM_APP + 8;  // dizin değişikliği (watcher thread'den)
static constexpr UINT_PTR kZoomIndicatorTimerID = 1;
static constexpr UINT_PTR kPanelAnimTimerID     = 2;
static constexpr UINT_PTR kAnimTimerID          = 3;
static constexpr UINT_PTR kZoomAnimTimerID      = 4;
static constexpr UINT_PTR kStripAnimTimerID     = 5;
static constexpr UINT_PTR kIndexIdleTimerID     = 6;  // 1.5s hareketsizlik → index fade başlar
static constexpr UINT_PTR kIndexFadeTimerID     = 7;  // index bar alpha animasyonu
static constexpr UINT_PTR kZoomFadeTimerID      = 8;  // zoom indicator alpha animasyonu
static constexpr UINT_PTR kCopyFeedbackTimerID  = 9;  // 1.5s kopyala feedback sıfırlama
static constexpr UINT_PTR kEditToolbarIdleTimerID = 10; // 2s hareketsizlik → edit toolbar fade başlar
static constexpr UINT_PTR kEditToolbarFadeTimerID = 11; // edit toolbar alpha animasyonu
static constexpr UINT_PTR kDirChangeTimerID       = 12; // dizin değişikliği debounce (200ms)

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
static LARGE_INTEGER      g_zoomFadeLastTime           = {};
static LARGE_INTEGER      g_editToolbarFadeLastTime    = {};

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

// Düzenleme durumu — decode sonucunun düzenlenebilir kopyası (UI thread'e özel)
struct EditState
{
    std::vector<uint8_t> pixels;    // BGRA pre-multiplied, güncel düzenlenmiş piksel
    UINT         width    = 0;
    UINT         height   = 0;
    bool         isDirty  = false;  // kaydedilmemiş değişiklik var
    std::wstring filePath;          // kayıt hedefi
    std::wstring format;            // "JPEG", "PNG" vb.
};
static EditState g_edit;
static std::atomic<bool> g_isSaving{false};  // arka plan kayıt devam ediyor mu

struct SaveDoneResult
{
    bool         success   = false;
    bool         isSaveAs  = false;
    std::wstring savedPath;    // kaydedilen yol
    std::wstring format;       // kullanılan format
    std::wstring origPath;     // SaveAs öncesi orijinal yol (cache eviction için)
};

static std::atomic<uint64_t> g_decodeGeneration{0};
static std::thread            g_decodeThread;

// --- Prefetch cache ---

static std::mutex                                       g_cacheMutex;
static std::unordered_map<std::wstring, DecodeResult*> g_decodeCache;
static constexpr size_t                                kCacheMaxSize   = 24;  // ~1.15GB (12MP HEIC × 24)
static constexpr int                                   kPrefetchRange  = 8;   // ±8 = 16 komşu

// --- GPS adres (Photon reverse geocoding) in-process + disk cache ---
// Key: ~50m hassasiyetle yuvarlanmış "%.4f,%.4f", Value: konum adı
// Disk cache: %APPDATA%\Lumina\location_cache.txt — uygulama başlangıcında yüklenir.
static std::mutex                                       g_locationCacheMutex;
static std::unordered_map<std::wstring, std::wstring>  g_locationCache;

// --- GPS location prefetch queue ---
// Tek worker thread sıralı HTTP istekleri atar; Photon rate limit gerektirmez.
static std::deque<std::pair<double,double>>  g_locationPrefetchQueue;
static std::mutex                            g_locationPrefetchMutex;
static std::condition_variable               g_locationPrefetchCV;
static std::atomic<bool>                     g_locationPrefetchStop{false};
static std::thread                           g_locationPrefetchThread;
static constexpr size_t                      kLocationQueueMaxSize = 24;

// --- Metadata in-process cache ---
// Key: dosya yolu, Value: ImageInfo (EXIF + boyut alanları, piksel yok)
// EXIF parse tekrarını önler; g_locationCache ile aynı yaşam süresi.
static std::mutex                                       g_metaCacheMutex;
static std::unordered_map<std::wstring, ImageInfo>      g_metaCache;
static constexpr size_t                                 kMetaCacheMaxSize = 256;

// Path-based prefetch iptal seti: decode tamamlandığında path hâlâ
// isteniyorsa cache'e eklenir, yoksa sonuç atılır.
static std::mutex                                        g_prefetchDesiredMutex;
static std::unordered_set<std::wstring>                  g_prefetchDesired;

// Aynı anda en fazla 4 ağır decode (CPU thrash önlemi: her biri ~100ms, 6 foto/sn için yeterli)
static std::counting_semaphore<16>                       g_prefetchSemaphore{4};

// --- Dizin değişiklik izleyici ---
static HANDLE       g_watchStopEvent = nullptr;  // manuel-reset event; set edilince thread durur
static std::thread  g_watchThread;
static std::wstring g_watchDirPath;

// --- Thumbnail decode cancel ---
static std::atomic<uint64_t>                           g_thumbCancel{0};

// --- Tile fetch cancel ---
static std::atomic<uint64_t>                           g_tileCancel{0};

// --- İleriye bildirimler ---
static void EnqueueLocationPrefetch(double lat, double lon);

// --- Decode yardımcıları ---

// Ortak decode mantığı — hem ana decode hem prefetch tarafından kullanılır.
// Cache'li FetchLocationName: aynı koordinat için tek HTTP isteği yapılır.
// Thread-safe: birden fazla thread aynı anda çağırabilir.
// ~50m hassasiyet: en yakın 0.0005 dereceye yuvarla, "%.4f,%.4f" formatında key üret.
static void MakeLocationKey(double lat, double lon, wchar_t* key, int keySize)
{
    double rLat = round(lat * 2000.0) / 2000.0;
    double rLon = round(lon * 2000.0) / 2000.0;
    swprintf_s(key, keySize, L"%.4f,%.4f", rLat, rLon);
}

static std::wstring FetchLocationCached(double lat, double lon)
{
    wchar_t key[32];
    MakeLocationKey(lat, lon, key, 32);

    {
        std::lock_guard<std::mutex> lk(g_locationCacheMutex);
        auto it = g_locationCache.find(key);
        if (it != g_locationCache.end())
            return it->second;
    }

    std::wstring name = FetchLocationName(lat, lon);

    {
        std::lock_guard<std::mutex> lk(g_locationCacheMutex);
        g_locationCache[key] = name;  // boş string de cache'lenir (başarısız istek tekrar atılmaz)
    }
    return name;
}

static void CacheMetaInfo(const std::wstring& path, const ImageInfo& info)
{
    std::lock_guard<std::mutex> lk(g_metaCacheMutex);
    if (g_metaCache.size() >= kMetaCacheMaxSize)
        g_metaCache.erase(g_metaCache.begin());
    g_metaCache[path] = info;
}

// g_locationCache'den anlık lookup — HTTP isteği yapmaz, sadece bellekten okur.
static std::wstring LookupLocationFromCache(double lat, double lon)
{
    wchar_t key[32];
    MakeLocationKey(lat, lon, key, 32);
    std::lock_guard<std::mutex> lk(g_locationCacheMutex);
    auto it = g_locationCache.find(key);
    return (it != g_locationCache.end()) ? it->second : std::wstring{};
}

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
                FetchLocationCached(decoded.gpsLatDecimal, decoded.gpsLonDecimal);
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
        // Önce meta cache'e bak; miss ise diskten oku ve cache'e yaz.
        {
            ImageInfo cachedMeta;
            bool metaHit = false;
            {
                std::lock_guard<std::mutex> lk(g_metaCacheMutex);
                auto it = g_metaCache.find(path);
                if (it != g_metaCache.end())
                {
                    cachedMeta = it->second;
                    metaHit    = true;
                }
            }

            if (metaHit)
            {
                // g_locationCache'de konum adı varsa hemen ekle (HTTP yapmaz)
                if (cachedMeta.hasGpsDecimal && cachedMeta.gpsLocationName.empty())
                    cachedMeta.gpsLocationName =
                        LookupLocationFromCache(cachedMeta.gpsLatDecimal, cachedMeta.gpsLonDecimal);

                if (g_decodeGeneration.load() == gen)
                {
                    auto* meta       = new MetaResult();
                    meta->generation = gen;
                    meta->info       = cachedMeta;
                    PostMessage(hwnd, WM_META_DONE, 0, reinterpret_cast<LPARAM>(meta));
                }
            }
            else
            {
                DecodeOutput metaOut;
                if (ExtractImageMeta(path, metaOut))
                {
                    ImageInfo mi;
                    mi.width          = static_cast<int>(metaOut.width);
                    mi.height         = static_cast<int>(metaOut.height);
                    mi.format         = metaOut.format;
                    mi.dateTaken      = metaOut.dateTaken;
                    mi.cameraMake     = metaOut.cameraMake;
                    mi.cameraModel    = metaOut.cameraModel;
                    mi.aperture       = metaOut.aperture;
                    mi.shutterSpeed   = metaOut.shutterSpeed;
                    mi.iso            = metaOut.iso;
                    mi.gpsLatitude    = metaOut.gpsLatitude;
                    mi.gpsLongitude   = metaOut.gpsLongitude;
                    mi.gpsAltitude    = metaOut.gpsAltitude;
                    mi.hasGpsDecimal  = metaOut.hasGpsDecimal;
                    mi.gpsLatDecimal  = metaOut.gpsLatDecimal;
                    mi.gpsLonDecimal  = metaOut.gpsLonDecimal;
                    mi.iccProfileName = metaOut.iccProfileName;
                    CacheMetaInfo(path, mi);

                    // g_locationCache'de konum adı varsa hemen ekle (HTTP yapmaz)
                    if (mi.hasGpsDecimal)
                        mi.gpsLocationName =
                            LookupLocationFromCache(mi.gpsLatDecimal, mi.gpsLonDecimal);

                    if (g_decodeGeneration.load() == gen)
                    {
                        auto* meta       = new MetaResult();
                        meta->generation = gen;
                        meta->info       = std::move(mi);
                        PostMessage(hwnd, WM_META_DONE, 0, reinterpret_cast<LPARAM>(meta));
                    }
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
            auto locName = FetchLocationCached(gpsLat, gpsLon);
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
        const bool   hasGps = result->info.hasGpsDecimal;
        const double gpsLat = result->info.gpsLatDecimal;
        const double gpsLon = result->info.gpsLonDecimal;
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
        // GPS varsa location prefetch queue'ya ekle
        if (hasGps) EnqueueLocationPrefetch(gpsLat, gpsLon);
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
static bool  g_clickIsDeleteBtn  = false;
static bool  g_clickInPanel      = false;  // Panel alanı tıklaması — drag/zoom engellenir
static bool  g_clickInStrip      = false;  // Strip veya toggle tıklaması
static bool  g_mouseTracking     = false;  // TrackMouseEvent kaydı aktif mi
static bool  g_clickInToolbar            = false;  // Edit toolbar butonu tıklaması
static bool  g_clickInSaveBar            = false;  // Save bar butonu tıklaması
static bool  g_rotFreeDlgSliderDragging  = false;  // Serbest döndürme slider sürükleniyor


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
    if (!result->path.empty())
        CacheMetaInfo(result->path, result->info);

    // Edit state güncelle — animasyonlu görüntüler düzenlenemez
    if (!result->frames.empty())
    {
        g_viewState.editIsAnimated = true;
        g_edit.pixels.clear();
        g_edit.width  = 0;
        g_edit.height = 0;
    }
    else if (!result->pixels.empty())
    {
        g_viewState.editIsAnimated = false;
        g_edit.pixels  = result->pixels;
        g_edit.width   = result->width;
        g_edit.height  = result->height;
    }
    else
    {
        g_viewState.editIsAnimated = false;
        g_edit.pixels.clear();
        g_edit.width  = 0;
        g_edit.height = 0;
    }
    g_edit.filePath = result->path;
    g_edit.format   = result->info.format;
    g_edit.isDirty  = false;
    g_viewState.editDirty = false;

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

        // END token kontrolü kaldırıldı: HEIC gibi yavaş formatlarda navigasyon
        // sırasında g_thumbCancel değişir ve sonuç atılır; filmstrip kalıcı siyah kalır.
        // Sonuç her zaman iletilir — WM_THUMB_DONE tarafında LRU cache sınırlar.
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

// --- GPS location disk cache ---

static std::wstring GetLocationCachePath()
{
    wchar_t appdata[MAX_PATH];
    if (!GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH)) return {};
    return std::wstring(appdata) + L"\\Lumina\\location_cache.txt";
}

static void LoadLocationCache()
{
    auto path = GetLocationCachePath();
    if (path.empty()) return;
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"r,ccs=UTF-8");
    if (!f) return;
    wchar_t line[512];
    while (fgetws(line, 512, f))
    {
        std::wstring l(line);
        while (!l.empty() && (l.back() == L'\n' || l.back() == L'\r')) l.pop_back();
        auto tab = l.find(L'\t');
        if (tab == std::wstring::npos || tab == 0) continue;
        std::wstring key = l.substr(0, tab);
        std::wstring val = l.substr(tab + 1);
        std::lock_guard<std::mutex> lk(g_locationCacheMutex);
        g_locationCache[key] = val;
    }
    fclose(f);
}

static void SaveLocationCache()
{
    auto path = GetLocationCachePath();
    if (path.empty()) return;
    auto dir = path.substr(0, path.rfind(L'\\'));
    CreateDirectoryW(dir.c_str(), nullptr);
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"w,ccs=UTF-8");
    if (!f) return;
    std::lock_guard<std::mutex> lk(g_locationCacheMutex);
    for (const auto& [key, val] : g_locationCache)
        if (!key.empty() && !val.empty())
            fwprintf(f, L"%ls\t%ls\n", key.c_str(), val.c_str());
    fclose(f);
}

// --- GPS location prefetch worker ---

static void EnqueueLocationPrefetch(double lat, double lon)
{
    wchar_t key[32];
    MakeLocationKey(lat, lon, key, 32);
    {
        std::lock_guard<std::mutex> lk(g_locationCacheMutex);
        if (g_locationCache.count(key)) return;
    }
    double rLat = round(lat * 2000.0) / 2000.0;
    double rLon = round(lon * 2000.0) / 2000.0;
    {
        std::lock_guard<std::mutex> lk(g_locationPrefetchMutex);
        for (const auto& c : g_locationPrefetchQueue)
            if (c.first == rLat && c.second == rLon) return;
        if (g_locationPrefetchQueue.size() >= kLocationQueueMaxSize)
            g_locationPrefetchQueue.pop_front();
        g_locationPrefetchQueue.emplace_back(rLat, rLon);
    }
    g_locationPrefetchCV.notify_one();
}

static void StartLocationPrefetchThread()
{
    g_locationPrefetchStop = false;
    g_locationPrefetchThread = std::thread([]()
    {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        while (true)
        {
            std::pair<double,double> coord;
            {
                std::unique_lock<std::mutex> lk(g_locationPrefetchMutex);
                g_locationPrefetchCV.wait(lk, []
                {
                    return g_locationPrefetchStop.load() || !g_locationPrefetchQueue.empty();
                });
                if (g_locationPrefetchStop && g_locationPrefetchQueue.empty()) break;
                coord = g_locationPrefetchQueue.front();
                g_locationPrefetchQueue.pop_front();
            }
            FetchLocationCached(coord.first, coord.second);
        }
        CoUninitialize();
    });
}

static void StopLocationPrefetchThread()
{
    {
        std::lock_guard<std::mutex> lk(g_locationPrefetchMutex);
        g_locationPrefetchStop = true;
        g_locationPrefetchQueue.clear();
    }
    g_locationPrefetchCV.notify_all();
    if (g_locationPrefetchThread.joinable())
        g_locationPrefetchThread.join();
}

static void TriggerLocationPrefetch()
{
    if (!g_navigator || g_navigator->empty()) return;
    for (int d = 1; d <= kPrefetchRange; ++d)
    {
        for (int sign : {+1, -1})
        {
            const std::wstring p = g_navigator->peek_at_linear(d * sign);
            if (p.empty()) continue;
            double lat = 0.0, lon = 0.0;
            bool found = false;
            {
                std::lock_guard<std::mutex> lk(g_metaCacheMutex);
                auto it = g_metaCache.find(p);
                if (it != g_metaCache.end() && it->second.hasGpsDecimal)
                    { lat = it->second.gpsLatDecimal; lon = it->second.gpsLonDecimal; found = true; }
            }
            if (!found)
            {
                std::lock_guard<std::mutex> lk(g_cacheMutex);
                auto it = g_decodeCache.find(p);
                if (it != g_decodeCache.end() && it->second->info.hasGpsDecimal)
                    { lat = it->second->info.gpsLatDecimal; lon = it->second->info.gpsLonDecimal; found = true; }
            }
            if (found) EnqueueLocationPrefetch(lat, lon);
        }
    }
}

// --- Dizin değişiklik izleyici ---

static void StopDirectoryWatcher()
{
    if (g_watchStopEvent)
    {
        SetEvent(g_watchStopEvent);
        if (g_watchThread.joinable()) g_watchThread.join();
        CloseHandle(g_watchStopEvent);
        g_watchStopEvent = nullptr;
    }
    g_watchDirPath.clear();
}

static void StartDirectoryWatcher(HWND hwnd, const std::wstring& dir)
{
    if (dir == g_watchDirPath) return;  // Aynı dizin — yeniden başlatma

    StopDirectoryWatcher();
    g_watchDirPath   = dir;
    g_watchStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_watchStopEvent) return;

    HANDLE stopEv = g_watchStopEvent;
    g_watchThread = std::thread([hwnd, dir, stopEv]()
    {
        HANDLE hDir = CreateFileW(
            dir.c_str(),
            FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
            nullptr
        );
        if (hDir == INVALID_HANDLE_VALUE) return;

        char     buf[4096];
        OVERLAPPED ov = {};
        HANDLE hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!hEvent) { CloseHandle(hDir); return; }
        ov.hEvent = hEvent;

        static const wchar_t* kImageExts[] = {
            L".jpg", L".jpeg", L".png", L".bmp", L".gif",
            L".tiff", L".tif", L".ico", L".webp",
            L".heic", L".heif", L".jxl", L".avif"
        };

        while (true)
        {
            ResetEvent(hEvent);
            BOOL ok = ReadDirectoryChangesW(
                hDir, buf, sizeof(buf), FALSE,
                FILE_NOTIFY_CHANGE_FILE_NAME,
                nullptr, &ov, nullptr
            );
            if (!ok && GetLastError() != ERROR_IO_PENDING)
                break;

            HANDLE handles[2] = { hEvent, stopEv };
            DWORD wait = WaitForMultipleObjects(2, handles, FALSE, INFINITE);

            if (wait != WAIT_OBJECT_0)
            {
                // Durdurma isteği veya hata — bekleyen I/O'yu iptal et
                CancelIo(hDir);
                WaitForSingleObject(hEvent, 2000);
                break;
            }

            DWORD bytesRet = 0;
            GetOverlappedResult(hDir, &ov, &bytesRet, FALSE);
            if (bytesRet == 0)
            {
                // Buffer taştı — değişiklik var ama detay yok; her koşulda bildir
                PostMessage(hwnd, WM_FILE_CHANGED, 0, 0);
                continue;
            }

            // Bildirim kayıtlarını tara — en az bir resim dosyası etkilendiyse bildir
            bool hasImageChange = false;
            const char* p = buf;
            while (!hasImageChange)
            {
                const auto* fni = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(p);
                std::wstring name(fni->FileName, fni->FileNameLength / sizeof(wchar_t));
                auto dotPos = name.rfind(L'.');
                if (dotPos != std::wstring::npos)
                {
                    std::wstring ext = name.substr(dotPos);
                    for (auto& c : ext) c = towlower(c);
                    for (auto kExt : kImageExts)
                        if (ext == kExt) { hasImageChange = true; break; }
                }
                if (fni->NextEntryOffset == 0) break;
                p += fni->NextEntryOffset;
            }

            if (hasImageChange)
                PostMessage(hwnd, WM_FILE_CHANGED, 0, 0);
        }

        CloseHandle(hEvent);
        CloseHandle(hDir);
    });
}

// Pencere genişliğine göre strip yarı-slot sayısını hesaplar
static int ComputeStripHalfCount(HWND hwnd)
{
    RECT rc = {};
    GetClientRect(hwnd, &rc);
    float winW = static_cast<float>(rc.right - rc.left);
    // Ortalama slot genişliği: ThumbH * 1.3 oran (yatay) + PadX ≈ 92px
    constexpr float kAvgSlotW = StripLayout::ThumbH * 1.3f + StripLayout::PadX;
    int half = static_cast<int>(winW / 2.0f / kAvgSlotW) + 1;
    return (half < StripLayout::HalfCount) ? StripLayout::HalfCount : half;
}

// Strip slot'larını navigator'a göre renderer'a yükler
static void UpdateStripSlots(HWND hwnd)
{
    if (!g_renderer) return;
    if (!g_navigator || g_navigator->empty())
    {
        g_renderer->SetStripSlots({}, 0);
        return;
    }
    int kHalf = ComputeStripHalfCount(hwnd);
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
    int kHalf = ComputeStripHalfCount(hwnd);
    for (int i = -kHalf; i <= kHalf; ++i)
    {
        const std::wstring& path = g_navigator->peek_at_linear(i);
        if (!path.empty() && !g_renderer->HasThumbnail(path))
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

    // Edit durumunu temizle — navigasyonda kaydedilmemiş değişiklikler atılır
    g_edit.isDirty             = false;
    g_edit.pixels.clear();
    g_viewState.editDirty      = false;
    g_viewState.editIsAnimated = false;
    g_viewState.editToolbarAlpha = 0.0f;
    KillTimer(hwnd, kEditToolbarIdleTimerID);
    KillTimer(hwnd, kEditToolbarFadeTimerID);

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

    // Info panelini hemen güncelle: dosya adı + boyutu anında göster, EXIF temizle
    g_imageInfo = ImageInfo{};
    {
        auto sep = path.rfind(L'\\');
        if (sep == std::wstring::npos) sep = path.rfind(L'/');
        g_imageInfo.filename = (sep != std::wstring::npos) ? path.substr(sep + 1) : path;
    }
    {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
            g_imageInfo.fileSizeBytes =
                (static_cast<int64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
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
            TriggerLocationPrefetch();
            UpdateStripSlots(hwnd);
            TriggerThumbFetches(hwnd);
            // GPS varsa OSM tile'larını arka planda çek (cache hit yolunda da gerekli)
            if (g_imageInfo.hasGpsDecimal)
            {
                StartTileFetches(hwnd, g_imageInfo.gpsLatDecimal, g_imageInfo.gpsLonDecimal);
                // Önce g_locationCache'den anlık lookup — HTTP gerektirmez
                if (g_imageInfo.gpsLocationName.empty())
                    g_imageInfo.gpsLocationName =
                        LookupLocationFromCache(g_imageInfo.gpsLatDecimal, g_imageInfo.gpsLonDecimal);
                // Hâlâ boşsa (cache miss) arka planda Nominatim çek
                if (g_imageInfo.gpsLocationName.empty())
                {
                    const uint64_t gen = g_decodeGeneration.load();
                    const double   lat = g_imageInfo.gpsLatDecimal;
                    const double   lon = g_imageInfo.gpsLonDecimal;
                    std::thread([hwnd, lat, lon, gen]()
                    {
                        auto locName = FetchLocationCached(lat, lon);
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
    UpdateStripSlots(hwnd);
    TriggerThumbFetches(hwnd);
    StartDecode(hwnd, path);
    // Filmstrip thumbnail varsa geçici önizleme olarak göster — tam decode gelince değişir.
    // HEIC istisnası kaldırıldı: gömülü thumbnail'i olmayan eski Lumina-düzenli HEIC'lerde
    // önizleme gösterilmezdi; ekranda 2 saniye önceki fotoğraf kalıyordu.
    if (g_renderer && g_renderer->HasThumbnail(path))
        g_renderer->ShowThumbnailAsPlaceholder(path);
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

// --- Piksel döndürme ---

// 90° saat yönünde: new(nx,ny) = old(col=ny, row=H-1-nx); yeni W=eskiH, yeni H=eskiW
static void RotatePixels90CW(std::vector<uint8_t>& pixels, UINT& width, UINT& height)
{
    const UINT newW = height, newH = width;
    std::vector<uint8_t> dst(static_cast<size_t>(newW) * newH * 4);
    for (UINT ny = 0; ny < newH; ++ny)
        for (UINT nx = 0; nx < newW; ++nx)
        {
            const uint8_t* src = &pixels[((height - 1 - nx) * width + ny) * 4];
            uint8_t* d = &dst[(ny * newW + nx) * 4];
            d[0] = src[0]; d[1] = src[1]; d[2] = src[2]; d[3] = src[3];
        }
    pixels = std::move(dst);
    width  = newW;
    height = newH;
}

// 90° saat yönü tersine: new(nx,ny) = old(col=W-1-ny, row=nx); yeni W=eskiH, yeni H=eskiW
static void RotatePixels90CCW(std::vector<uint8_t>& pixels, UINT& width, UINT& height)
{
    const UINT newW = height, newH = width;
    std::vector<uint8_t> dst(static_cast<size_t>(newW) * newH * 4);
    for (UINT ny = 0; ny < newH; ++ny)
        for (UINT nx = 0; nx < newW; ++nx)
        {
            const uint8_t* src = &pixels[(nx * width + (width - 1 - ny)) * 4];
            uint8_t* d = &dst[(ny * newW + nx) * 4];
            d[0] = src[0]; d[1] = src[1]; d[2] = src[2]; d[3] = src[3];
        }
    pixels = std::move(dst);
    width  = newW;
    height = newH;
}

// Serbest açı döndürme (bilineer): ters haritalama ile kaynak piksel örneklenir.
// Çıkış boyutu girişle aynı; kapsam dışı pikseller siyah (0) bırakılır.
static void RotatePixelsFree(std::vector<uint8_t>& pixels, UINT width, UINT height, float angleDeg)
{
    if (fabsf(angleDeg) < 0.001f) return;

    const float rad  = -angleDeg * 3.14159265f / 180.0f;  // negatif: ters dönüşüm için
    const float cosA = cosf(rad);
    const float sinA = sinf(rad);
    const float cx   = (width  - 1) * 0.5f;
    const float cy   = (height - 1) * 0.5f;

    std::vector<uint8_t> dst(static_cast<size_t>(width) * height * 4, 0);

    for (UINT dy = 0; dy < height; ++dy)
    {
        const float fy = dy - cy;
        for (UINT dx = 0; dx < width; ++dx)
        {
            const float fx = dx - cx;
            // Ters döndürme: hedef pikselin kaynaktaki konumunu bul
            const float sx = cosA * fx - sinA * fy + cx;
            const float sy = sinA * fx + cosA * fy + cy;

            if (sx >= 0.0f && sx < static_cast<float>(width  - 1) &&
                sy >= 0.0f && sy < static_cast<float>(height - 1))
            {
                const int   x0 = static_cast<int>(sx);
                const int   y0 = static_cast<int>(sy);
                const float tx = sx - x0;
                const float ty = sy - y0;

                const uint8_t* p00 = &pixels[(static_cast<size_t>(y0)     * width + x0)     * 4];
                const uint8_t* p10 = &pixels[(static_cast<size_t>(y0)     * width + x0 + 1) * 4];
                const uint8_t* p01 = &pixels[(static_cast<size_t>(y0 + 1) * width + x0)     * 4];
                const uint8_t* p11 = &pixels[(static_cast<size_t>(y0 + 1) * width + x0 + 1) * 4];
                uint8_t* d = &dst[(static_cast<size_t>(dy) * width + dx) * 4];

                for (int c = 0; c < 4; ++c)
                {
                    float v = p00[c] * (1.0f - tx) * (1.0f - ty)
                            + p10[c] *          tx  * (1.0f - ty)
                            + p01[c] * (1.0f - tx) *          ty
                            + p11[c] *          tx  *          ty;
                    d[c] = static_cast<uint8_t>(v + 0.5f);
                }
            }
        }
    }

    pixels = std::move(dst);
    // Genişlik ve yükseklik değişmez
}

// --- Dosya yardımcıları ---

static std::wstring GetFormatFromPath(const std::wstring& path)
{
    auto dot = path.rfind(L'.');
    if (dot == std::wstring::npos) return L"JPEG";
    std::wstring ext = path.substr(dot + 1);
    for (auto& c : ext) c = towupper(c);
    if (ext == L"JPG" || ext == L"JPEG") return L"JPEG";
    if (ext == L"PNG")                   return L"PNG";
    if (ext == L"BMP")                   return L"BMP";
    if (ext == L"TIFF" || ext == L"TIF") return L"TIFF";
    if (ext == L"WEBP")                  return L"WebP";
    if (ext == L"HEIC" || ext == L"HEIF") return L"HEIC";
    if (ext == L"JXL")                   return L"JXL";
    if (ext == L"AVIF")                  return L"AVIF";
    return L"JPEG";
}

static std::wstring ShowSaveAsDialog(HWND hwnd, const std::wstring& currentPath)
{
    // Filter sırası — nFilterIndex (1 tabanlı) ile eşleşmeli
    static const wchar_t* kExts[] = {
        L"jpg", L"png", L"webp", L"bmp", L"tiff", L"heic", L"jxl", L"avif"
    };

    wchar_t buf[MAX_PATH] = {};
    if (!currentPath.empty())
        wcsncpy_s(buf, currentPath.c_str(), MAX_PATH - 1);

    // Mevcut dosyanın uzantısına göre başlangıç filter'ı seç
    DWORD initIdx = 1; // varsayılan: JPEG
    {
        auto dot = currentPath.rfind(L'.');
        if (dot != std::wstring::npos) {
            std::wstring ext = currentPath.substr(dot + 1);
            for (auto& c : ext) c = towupper(c);
            if      (ext == L"PNG")                     initIdx = 2;
            else if (ext == L"WEBP")                    initIdx = 3;
            else if (ext == L"BMP")                     initIdx = 4;
            else if (ext == L"TIFF" || ext == L"TIF")  initIdx = 5;
            else if (ext == L"HEIC" || ext == L"HEIF") initIdx = 6;
            else if (ext == L"JXL")                     initIdx = 7;
            else if (ext == L"AVIF")                    initIdx = 8;
        }
    }

    OPENFILENAMEW ofn = {};
    ofn.lStructSize   = sizeof(ofn);
    ofn.hwndOwner     = hwnd;
    ofn.lpstrFile     = buf;
    ofn.nMaxFile      = MAX_PATH;
    ofn.lpstrFilter   = L"JPEG\0*.jpg;*.jpeg\0PNG\0*.png\0WebP\0*.webp\0"
                        L"BMP\0*.bmp\0TIFF\0*.tiff;*.tif\0HEIC\0*.heic;*.heif\0"
                        L"JXL\0*.jxl\0AVIF\0*.avif\0\0";
    ofn.nFilterIndex  = initIdx;
    ofn.Flags         = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;

    if (!GetSaveFileNameW(&ofn)) return L"";

    // Seçilen filter'a göre uzantıyı zorla — kullanıcı JPEG seçtiyse
    // dosya adı .heic kalsa bile .jpg uzantılı yola döndür.
    std::wstring result = buf;
    DWORD idx = ofn.nFilterIndex - 1; // 0 tabanlı
    if (idx < ARRAYSIZE(kExts)) {
        auto dot = result.rfind(L'.');
        std::wstring stem = (dot != std::wstring::npos) ? result.substr(0, dot) : result;
        std::wstring corrected = stem + L'.' + kExts[idx];

        // Uzantı değiştiyse ve hedef dosya mevcutsa overwrite sor
        if (corrected != result &&
            GetFileAttributesW(corrected.c_str()) != INVALID_FILE_ATTRIBUTES) {
            std::wstring fname = corrected.substr(corrected.find_last_of(L"\\/") + 1);
            std::wstring msg   = fname + L" zaten mevcut.\nUzerine yazilsin mi?";
            if (MessageBoxW(hwnd, msg.c_str(), L"Uzerine Yaz",
                            MB_YESNO | MB_ICONWARNING) != IDYES)
                return L"";
        }
        result = corrected;
    }
    return result;
}

// --- Edit eylem yardımcıları ---

// Ortak: düzenlenmiş piksel buffer'ını renderer'a yükler, zoom/pan sıfırlar
static void DoApplyEdit(HWND hwnd)
{
    if (g_renderer)
        g_renderer->LoadImageFromPixels(
            g_edit.pixels.data(), g_edit.width, g_edit.height, g_edit.filePath);
    g_viewState.zoomFactor = g_viewState.zoomTarget = 1.0f;
    g_viewState.panX = g_viewState.panXTarget = 0.0f;
    g_viewState.panY = g_viewState.panYTarget = 0.0f;
    KillTimer(hwnd, kZoomAnimTimerID);
}

static void DoRotateCW(HWND hwnd)
{
    if (g_edit.pixels.empty() || g_viewState.editIsAnimated) return;
    if (g_isSaving.load()) return;  // kayıt devam ederken döndürme engellenir
    RotatePixels90CW(g_edit.pixels, g_edit.width, g_edit.height);
    g_edit.isDirty         = true;
    g_viewState.editDirty  = true;
    g_viewState.editToolbarAlpha = 1.0f;
    KillTimer(hwnd, kEditToolbarFadeTimerID);
    KillTimer(hwnd, kEditToolbarIdleTimerID);
    DoApplyEdit(hwnd);
    InvalidateRect(hwnd, nullptr, FALSE);
}

static void DoRotateCCW(HWND hwnd)
{
    if (g_edit.pixels.empty() || g_viewState.editIsAnimated) return;
    if (g_isSaving.load()) return;  // kayıt devam ederken döndürme engellenir
    RotatePixels90CCW(g_edit.pixels, g_edit.width, g_edit.height);
    g_edit.isDirty         = true;
    g_viewState.editDirty  = true;
    g_viewState.editToolbarAlpha = 1.0f;
    KillTimer(hwnd, kEditToolbarFadeTimerID);
    KillTimer(hwnd, kEditToolbarIdleTimerID);
    DoApplyEdit(hwnd);
    InvalidateRect(hwnd, nullptr, FALSE);
}

// Piksel tamponunu merkezden kırp — boyut küçülür, orijinal buffer'ın orta bölgesi alınır.
static void CropPixelsCenter(std::vector<uint8_t>& pixels, UINT& width, UINT& height,
                              UINT newW, UINT newH)
{
    if (newW == 0 || newH == 0 || (newW == width && newH == height)) return;
    const UINT x0 = (width  - newW) / 2;
    const UINT y0 = (height - newH) / 2;
    std::vector<uint8_t> dst(static_cast<size_t>(newW) * newH * 4);
    for (UINT y = 0; y < newH; ++y)
    {
        const uint8_t* src = &pixels[((y0 + y) * width + x0) * 4];
        uint8_t*       d   = &dst[y * newW * 4];
        memcpy(d, src, static_cast<size_t>(newW) * 4);
    }
    pixels = std::move(dst);
    width  = newW;
    height = newH;
}

static void DoOpenRotateFreeDialog(HWND hwnd)
{
    if (g_edit.pixels.empty() || g_viewState.editIsAnimated) return;
    if (g_isSaving.load()) return;
    g_viewState.rotateFreeAngle        = 0.0f;
    g_viewState.rotateFreeDlgHoverBtn  = 0;
    g_viewState.rotateFreeDlgPressBtn  = 0;
    g_viewState.showRotateFreeDialog   = true;
    InvalidateRect(hwnd, nullptr, FALSE);
}

static void DoApplyRotateFree(HWND hwnd)
{
    if (g_edit.pixels.empty()) return;
    float angle = g_viewState.rotateFreeAngle;
    g_viewState.showRotateFreeDialog  = false;
    g_viewState.rotateFreeDlgHoverBtn = 0;
    g_viewState.rotateFreeDlgPressBtn = 0;
    if (fabsf(angle) < 0.001f)
    {
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }
    RotatePixelsFree(g_edit.pixels, g_edit.width, g_edit.height, angle);

    // Otomatik kırpma: siyah köşeleri gider (Lightroom/ACR davranışı)
    // crop = (W·cosθ − H·sinθ) / cos2θ,  (H·cosθ − W·sinθ) / cos2θ
    {
        const float rad  = fabsf(angle) * 3.14159265f / 180.0f;
        const float cosA = cosf(rad);
        const float sinA = sinf(rad);
        const float cos2 = cosf(2.0f * rad);
        if (cos2 > 0.001f)
        {
            const float W = static_cast<float>(g_edit.width);
            const float H = static_cast<float>(g_edit.height);
            const auto  cropW = static_cast<UINT>((W * cosA - H * sinA) / cos2);
            const auto  cropH = static_cast<UINT>((H * cosA - W * sinA) / cos2);
            if (cropW > 0 && cropH > 0 && cropW < g_edit.width && cropH < g_edit.height)
                CropPixelsCenter(g_edit.pixels, g_edit.width, g_edit.height, cropW, cropH);
        }
    }

    g_edit.isDirty        = true;
    g_viewState.editDirty = true;
    g_viewState.editToolbarAlpha = 1.0f;
    KillTimer(hwnd, kEditToolbarFadeTimerID);
    KillTimer(hwnd, kEditToolbarIdleTimerID);
    DoApplyEdit(hwnd);
    InvalidateRect(hwnd, nullptr, FALSE);
}

// ─── Piksel boyutlandırma ─────────────────────────────────────────────────────

static void ResizePixels(std::vector<uint8_t>& pixels, UINT& w, UINT& h, UINT newW, UINT newH)
{
    if (newW == 0 || newH == 0 || (newW == w && newH == h)) return;

    std::vector<uint8_t> dst(static_cast<size_t>(newW) * newH * 4);
    const float scaleX = float(w) / float(newW);
    const float scaleY = float(h) / float(newH);

    for (UINT y = 0; y < newH; ++y)
    {
        for (UINT x = 0; x < newW; ++x)
        {
            float srcX = (x + 0.5f) * scaleX - 0.5f;
            float srcY = (y + 0.5f) * scaleY - 0.5f;
            int x0 = max(0, int(srcX));
            int y0 = max(0, int(srcY));
            int x1 = min(int(w) - 1, x0 + 1);
            int y1 = min(int(h) - 1, y0 + 1);
            float fx = srcX - float(x0), fy = srcY - float(y0);
            for (int c = 0; c < 4; ++c)
            {
                float p00 = pixels[(y0 * w + x0) * 4 + c];
                float p10 = pixels[(y0 * w + x1) * 4 + c];
                float p01 = pixels[(y1 * w + x0) * 4 + c];
                float p11 = pixels[(y1 * w + x1) * 4 + c];
                float val = p00 * (1-fx) * (1-fy) + p10 * fx * (1-fy)
                          + p01 * (1-fx) * fy     + p11 * fx * fy;
                dst[(y * newW + x) * 4 + c] = static_cast<uint8_t>(val + 0.5f);
            }
        }
    }
    pixels = std::move(dst);
    w = newW;
    h = newH;
}

// Resize dialog'u aç — mevcut g_edit boyutlarını başlangıç değerleri olarak kullan
static void DoOpenResizeDialog(HWND hwnd)
{
    if (g_edit.pixels.empty() || g_viewState.editIsAnimated) return;
    if (g_isSaving.load()) return;

    g_viewState.resizeOrigW    = int(g_edit.width);
    g_viewState.resizeOrigH    = int(g_edit.height);
    g_viewState.resizeW        = int(g_edit.width);
    g_viewState.resizeH        = int(g_edit.height);
    g_viewState.resizePct      = 100;
    g_viewState.resizeMode     = 0;
    g_viewState.resizeLockAspect = true;
    g_viewState.resizeDlgHoverBtn   = 0;
    g_viewState.resizeDlgPressedBtn = 0;
    g_viewState.showResizeDialog    = true;
    InvalidateRect(hwnd, nullptr, FALSE);
}

// Resize onaylandı — pikselleri boyutlandır, düzenlemeyi uygula
static void DoResizeConfirmed(HWND hwnd)
{
    UINT tw = static_cast<UINT>(max(1, g_viewState.resizeW));
    UINT th = static_cast<UINT>(max(1, g_viewState.resizeH));
    ResizePixels(g_edit.pixels, g_edit.width, g_edit.height, tw, th);
    g_edit.isDirty        = true;
    g_viewState.editDirty = true;
    g_viewState.editToolbarAlpha = 1.0f;
    KillTimer(hwnd, kEditToolbarFadeTimerID);
    KillTimer(hwnd, kEditToolbarIdleTimerID);
    g_viewState.showResizeDialog = false;
    DoApplyEdit(hwnd);
    InvalidateRect(hwnd, nullptr, FALSE);
}

// Resize dialog buton ID'sine göre değer güncelleme
static void ResizeDlgAdjust(HWND /*hwnd*/, int btnId)
{
    const int origW = g_viewState.resizeOrigW;
    const int origH = g_viewState.resizeOrigH;
    const bool lock = g_viewState.resizeLockAspect;

    if (g_viewState.resizeMode == 0)   // piksel modu
    {
        constexpr int kStep = 10;
        if (btnId == 5)   // W azalt
        {
            g_viewState.resizeW = max(1, g_viewState.resizeW - kStep);
            if (lock && origW > 0)
                g_viewState.resizeH = max(1, int(g_viewState.resizeW * float(origH) / float(origW) + 0.5f));
        }
        else if (btnId == 6)  // W artır
        {
            g_viewState.resizeW = g_viewState.resizeW + kStep;
            if (lock && origW > 0)
                g_viewState.resizeH = max(1, int(g_viewState.resizeW * float(origH) / float(origW) + 0.5f));
        }
        else if (btnId == 7)  // H azalt
        {
            g_viewState.resizeH = max(1, g_viewState.resizeH - kStep);
            if (lock && origH > 0)
                g_viewState.resizeW = max(1, int(g_viewState.resizeH * float(origW) / float(origH) + 0.5f));
        }
        else if (btnId == 8)  // H artır
        {
            g_viewState.resizeH = g_viewState.resizeH + kStep;
            if (lock && origH > 0)
                g_viewState.resizeW = max(1, int(g_viewState.resizeH * float(origW) / float(origH) + 0.5f));
        }
    }
    else  // yüzde modu
    {
        constexpr int kStep = 5;
        if (btnId == 5)
            g_viewState.resizePct = max(1, g_viewState.resizePct - kStep);
        else if (btnId == 6)
            g_viewState.resizePct = min(1000, g_viewState.resizePct + kStep);

        g_viewState.resizeW = max(1, int(origW * g_viewState.resizePct / 100.0f + 0.5f));
        g_viewState.resizeH = max(1, int(origH * g_viewState.resizePct / 100.0f + 0.5f));
    }
}

static void DoDeleteConfirmed(HWND hwnd)
{
    std::wstring pathDblNull = g_edit.filePath + L'\0' + L'\0';
    SHFILEOPSTRUCTW fos = {};
    fos.hwnd   = hwnd;
    fos.wFunc  = FO_DELETE;
    fos.pFrom  = pathDblNull.c_str();
    fos.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;

    if (SHFileOperationW(&fos) == 0 && !fos.fAnyOperationsAborted)
    {
        {
            std::lock_guard<std::mutex> lk(g_cacheMutex);
            g_decodeCache.erase(g_edit.filePath);
        }

        std::wstring nextPath = g_navigator->refresh();

        if (g_navigator->empty())
        {
            ++g_decodeGeneration;
            if (g_renderer) { g_renderer->ClearImage(); g_renderer->SetStripSlots({}, 0); }
            g_viewState   = ViewState{};
            g_imageInfo   = ImageInfo{};
            g_edit        = EditState{};
            UpdateWindowTitle(hwnd, L"");
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        else
        {
            NavigateTo(hwnd, nextPath);
        }
    }
}

static void DoDelete(HWND hwnd)
{
    if (g_edit.filePath.empty()) return;
    if (!g_navigator || g_navigator->empty()) return;

    if (g_edit.isDirty)
    {
        g_viewState.showUnsavedWarningDialog = true;
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }

    g_viewState.showDeleteConfirmDialog = true;
    g_viewState.deleteDlgHoverBtn       = 0;
    g_viewState.deleteDlgPressedBtn     = 0;
    InvalidateRect(hwnd, nullptr, FALSE);
}

static void DoDiscard(HWND hwnd)
{
    if (!g_edit.isDirty) return;
    g_edit.isDirty         = false;
    g_viewState.editDirty  = false;
    g_viewState.editToolbarAlpha = 1.0f;
    KillTimer(hwnd, kEditToolbarFadeTimerID);
    KillTimer(hwnd, kEditToolbarIdleTimerID);
    SetTimer(hwnd, kEditToolbarIdleTimerID, 2000, nullptr);
    // Orijinali yeniden decode et; WM_DECODE_DONE g_edit'i temizlenmiş pikselle yeniler
    g_viewState.zoomFactor = g_viewState.zoomTarget = 1.0f;
    g_viewState.panX = g_viewState.panXTarget = 0.0f;
    g_viewState.panY = g_viewState.panYTarget = 0.0f;
    KillTimer(hwnd, kZoomAnimTimerID);
    StartDecode(hwnd, g_edit.filePath);
    InvalidateRect(hwnd, nullptr, FALSE);
}

static void DoSave(HWND hwnd)
{
    if (!g_edit.isDirty || g_edit.pixels.empty()) return;
    if (g_isSaving.load()) return;

    g_isSaving.store(true);
    auto* r    = new SaveDoneResult();
    r->isSaveAs  = false;
    r->savedPath = g_edit.filePath;
    r->origPath  = g_edit.filePath;
    r->format    = g_edit.format;

    // Arka plan kayıt: pikselleri kopyala, UI thread bloklanmaz
    auto pixels = g_edit.pixels;
    UINT w = g_edit.width, h = g_edit.height;
    std::thread([hwnd, r, pixels = std::move(pixels), w, h]() mutable {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        r->success = SaveImage(r->savedPath, r->format, pixels.data(), w, h, r->origPath);
        CoUninitialize();
        PostMessage(hwnd, WM_SAVE_DONE, 0, reinterpret_cast<LPARAM>(r));
    }).detach();
}

static void DoSaveAs(HWND hwnd)
{
    if (!g_edit.isDirty || g_edit.pixels.empty()) return;
    if (g_isSaving.load()) return;

    std::wstring newPath = ShowSaveAsDialog(hwnd, g_edit.filePath);
    if (newPath.empty()) return;
    std::wstring fmt = GetFormatFromPath(newPath);

    g_isSaving.store(true);
    auto* r    = new SaveDoneResult();
    r->isSaveAs  = true;
    r->savedPath = newPath;
    r->origPath  = g_edit.filePath;
    r->format    = fmt;

    auto pixels   = g_edit.pixels;
    UINT w = g_edit.width, h = g_edit.height;
    std::wstring srcPath = g_edit.filePath;
    std::thread([hwnd, r, pixels = std::move(pixels), w, h, srcPath]() mutable {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        r->success = SaveImage(r->savedPath, r->format, pixels.data(), w, h, srcPath);
        CoUninitialize();
        PostMessage(hwnd, WM_SAVE_DONE, 0, reinterpret_cast<LPARAM>(r));
    }).detach();
}

// --- Tema yardımcıları ---

static bool IsSystemDarkMode()
{
    DWORD value = 1; // varsayılan: açık tema
    DWORD size  = sizeof(value);
    RegGetValueW(
        HKEY_CURRENT_USER,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme",
        RRF_RT_REG_DWORD, nullptr, &value, &size);
    return value == 0; // 0 = koyu tema
}

static void ApplyTitleBarTheme(HWND hwnd)
{
    BOOL dark = IsSystemDarkMode() ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
}

// --- WndProc ---

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
        g_renderer = new Renderer(hwnd);
        ApplyTitleBarTheme(hwnd);
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
                    NavigateTo(hwnd, g_navigator->next());
                else
                    NavigateTo(hwnd, g_navigator->prev());
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

        // Serbest döndürme dialogu açıksa — slider/buton etkileşimi, diğerini engelle
        if (g_viewState.showRotateFreeDialog)
        {
            if (g_renderer && g_renderer->IsRotFreeDlgVisible())
            {
                auto inR = [](float x, float y, const D2D1_RECT_F& r) {
                    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
                };
                if (inR(mx, my, g_renderer->GetRotFreeDlgSliderRect()))
                {
                    g_rotFreeDlgSliderDragging = true;
                    SetCapture(hwnd);
                    // Tıklanan x pozisyonundan açıyı hesapla
                    D2D1_RECT_F sr = g_renderer->GetRotFreeDlgSliderRect();
                    float railLeft  = sr.left  + 9.0f;
                    float railRight = sr.right  - 9.0f;
                    float t = (mx - railLeft) / (railRight - railLeft);
                    float angle = t * 90.0f - 45.0f;
                    if (angle < -45.0f) angle = -45.0f;
                    if (angle >  45.0f) angle =  45.0f;
                    g_viewState.rotateFreeAngle = angle;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                else
                {
                    int hit = 0;
                    if      (inR(mx, my, g_renderer->GetRotFreeDlgCoarseDecRect())) hit = 1;
                    else if (inR(mx, my, g_renderer->GetRotFreeDlgFineDecRect()))   hit = 2;
                    else if (inR(mx, my, g_renderer->GetRotFreeDlgFineIncRect()))   hit = 3;
                    else if (inR(mx, my, g_renderer->GetRotFreeDlgCoarseIncRect())) hit = 4;
                    else if (inR(mx, my, g_renderer->GetRotFreeDlgResetRect()))     hit = 5;
                    else if (inR(mx, my, g_renderer->GetRotFreeDlgCancelRect()))    hit = 6;
                    else if (inR(mx, my, g_renderer->GetRotFreeDlgApplyRect()))     hit = 7;
                    g_viewState.rotateFreeDlgPressBtn = hit;
                    SetCapture(hwnd);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return 0;
        }

        // Resize dialogu açıksa — buton press state güncelle, diğer etkileşimi engelle
        if (g_viewState.showResizeDialog)
        {
            if (g_renderer && g_renderer->IsResizeDialogVisible())
            {
                auto inR = [](float x, float y, const D2D1_RECT_F& r) {
                    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
                };
                int hit = 0;
                if      (inR(mx, my, g_renderer->GetResizeDlgCancelRect()))  hit = 1;
                else if (inR(mx, my, g_renderer->GetResizeDlgApplyRect()))   hit = 2;
                else if (inR(mx, my, g_renderer->GetResizeDlgModePxRect()))  hit = 3;
                else if (inR(mx, my, g_renderer->GetResizeDlgModePctRect())) hit = 4;
                else if (inR(mx, my, g_renderer->GetResizeDlgWDecRect()))    hit = 5;
                else if (inR(mx, my, g_renderer->GetResizeDlgWIncRect()))    hit = 6;
                else if (inR(mx, my, g_renderer->GetResizeDlgHDecRect()))    hit = 7;
                else if (inR(mx, my, g_renderer->GetResizeDlgHIncRect()))    hit = 8;
                else if (inR(mx, my, g_renderer->GetResizeDlgLockRect()))    hit = 9;
                g_viewState.resizeDlgPressedBtn = hit;
                SetCapture(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        // Silme dialogu açıksa — buton press state güncelle, diğer etkileşimi engelle
        if (g_viewState.showDeleteConfirmDialog || g_viewState.showUnsavedWarningDialog)
        {
            if (g_renderer && g_renderer->IsDeleteDialogVisible())
            {
                auto inRect = [](float x, float y, const D2D1_RECT_F& r) {
                    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
                };
                if (!g_viewState.showUnsavedWarningDialog &&
                    inRect(mx, my, g_renderer->GetDlgCancelRect()))
                    g_viewState.deleteDlgPressedBtn = 1;
                else if (inRect(mx, my, g_renderer->GetDlgDeleteRect()))
                    g_viewState.deleteDlgPressedBtn = 2;
                else
                    g_viewState.deleteDlgPressedBtn = 0;
                SetCapture(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        g_mouseDownX = mx;
        g_mouseDownY = my;
        SetCapture(hwnd);
        ResetIndexIdleTimer(hwnd);

        // Info button ve Delete button kontrolü
        g_clickIsInfoButton = HitTestInfoButton(hwnd, mx, my, g_viewState.panelAnimWidth);
        g_clickIsDeleteBtn  = false;
        if (!g_clickIsInfoButton && g_renderer && g_renderer->IsDeleteBtnVisible())
        {
            D2D1_RECT_F dr = g_renderer->GetDeleteBtnRect();
            g_clickIsDeleteBtn = (mx >= dr.left && mx <= dr.right && my >= dr.top && my <= dr.bottom);
        }
        if (g_clickIsInfoButton)
        {
            g_viewState.infoBtnPressed = true;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        else if (g_clickIsDeleteBtn)
        {
            g_viewState.deleteBtnPressed = true;
            InvalidateRect(hwnd, nullptr, FALSE);
        }

        if (!g_clickIsInfoButton && !g_clickIsDeleteBtn)
        {
            // Edit toolbar butonu tıklaması
            if (g_renderer && g_renderer->IsEditToolbarVisible() && !g_viewState.editIsAnimated)
            {
                D2D1_RECT_F rL   = g_renderer->GetEditBtnRotLRect();
                D2D1_RECT_F rR   = g_renderer->GetEditBtnRotRRect();
                D2D1_RECT_F rFr  = g_renderer->GetEditBtnRotFreeRect();
                D2D1_RECT_F rRz  = g_renderer->GetEditBtnResizeRect();
                bool hitL  = (mx >= rL.left  && mx <= rL.right  && my >= rL.top  && my <= rL.bottom);
                bool hitR  = (mx >= rR.left  && mx <= rR.right  && my >= rR.top  && my <= rR.bottom);
                bool hitFr = (mx >= rFr.left && mx <= rFr.right && my >= rFr.top && my <= rFr.bottom);
                bool hitRz = (mx >= rRz.left && mx <= rRz.right && my >= rRz.top && my <= rRz.bottom);
                if (hitL || hitR || hitFr || hitRz)
                {
                    g_clickInToolbar = true;
                    g_viewState.editBtnRotLPressed    = hitL;
                    g_viewState.editBtnRotRPressed    = hitR;
                    g_viewState.editBtnRotFreePressed = hitFr;
                    g_viewState.editBtnResizePressed  = hitRz;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
            }

            // Save bar butonu tıklaması
            if (g_renderer && g_renderer->IsSaveBarVisible() && g_viewState.editDirty)
            {
                D2D1_RECT_F rSave    = g_renderer->GetSaveBarSaveRect();
                D2D1_RECT_F rDiscard = g_renderer->GetSaveBarDiscardRect();
                D2D1_RECT_F rSaveAs  = g_renderer->GetSaveBarSaveAsRect();
                bool hitSave    = (mx >= rSave.left    && mx <= rSave.right    && my >= rSave.top    && my <= rSave.bottom);
                bool hitDiscard = (mx >= rDiscard.left && mx <= rDiscard.right && my >= rDiscard.top && my <= rDiscard.bottom);
                bool hitSaveAs  = (mx >= rSaveAs.left  && mx <= rSaveAs.right  && my >= rSaveAs.top  && my <= rSaveAs.bottom);
                if (hitSave || hitDiscard || hitSaveAs)
                {
                    g_clickInSaveBar = true;
                    g_viewState.saveBarPressedBtn = hitSave ? 1 : (hitDiscard ? 2 : 3);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
            }

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

        // Serbest döndürme dialogu açıksa — slider sürükleme + hover güncelle
        if (g_viewState.showRotateFreeDialog)
        {
            if (g_rotFreeDlgSliderDragging && g_renderer && g_renderer->IsRotFreeDlgVisible())
            {
                D2D1_RECT_F sr = g_renderer->GetRotFreeDlgSliderRect();
                float railLeft  = sr.left  + 9.0f;
                float railRight = sr.right  - 9.0f;
                float t = (mx - railLeft) / (railRight - railLeft);
                float angle = t * 90.0f - 45.0f;
                if (angle < -45.0f) angle = -45.0f;
                if (angle >  45.0f) angle =  45.0f;
                if (fabsf(angle - g_viewState.rotateFreeAngle) > 0.05f)
                {
                    g_viewState.rotateFreeAngle = angle;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            else if (g_renderer && g_renderer->IsRotFreeDlgVisible())
            {
                auto inR = [](float x, float y, const D2D1_RECT_F& r) {
                    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
                };
                int newHover = 0;
                if      (inR(mx, my, g_renderer->GetRotFreeDlgCoarseDecRect())) newHover = 1;
                else if (inR(mx, my, g_renderer->GetRotFreeDlgFineDecRect()))   newHover = 2;
                else if (inR(mx, my, g_renderer->GetRotFreeDlgFineIncRect()))   newHover = 3;
                else if (inR(mx, my, g_renderer->GetRotFreeDlgCoarseIncRect())) newHover = 4;
                else if (inR(mx, my, g_renderer->GetRotFreeDlgResetRect()))     newHover = 5;
                else if (inR(mx, my, g_renderer->GetRotFreeDlgCancelRect()))    newHover = 6;
                else if (inR(mx, my, g_renderer->GetRotFreeDlgApplyRect()))     newHover = 7;
                if (newHover != g_viewState.rotateFreeDlgHoverBtn)
                {
                    g_viewState.rotateFreeDlgHoverBtn = newHover;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return 0;
        }

        // Resize dialogu açıksa — hover güncelle
        if (g_viewState.showResizeDialog)
        {
            if (g_renderer && g_renderer->IsResizeDialogVisible())
            {
                auto inR = [](float x, float y, const D2D1_RECT_F& r) {
                    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
                };
                int newHover = 0;
                if      (inR(mx, my, g_renderer->GetResizeDlgCancelRect()))  newHover = 1;
                else if (inR(mx, my, g_renderer->GetResizeDlgApplyRect()))   newHover = 2;
                else if (inR(mx, my, g_renderer->GetResizeDlgModePxRect()))  newHover = 3;
                else if (inR(mx, my, g_renderer->GetResizeDlgModePctRect())) newHover = 4;
                else if (inR(mx, my, g_renderer->GetResizeDlgWDecRect()))    newHover = 5;
                else if (inR(mx, my, g_renderer->GetResizeDlgWIncRect()))    newHover = 6;
                else if (inR(mx, my, g_renderer->GetResizeDlgHDecRect()))    newHover = 7;
                else if (inR(mx, my, g_renderer->GetResizeDlgHIncRect()))    newHover = 8;
                else if (inR(mx, my, g_renderer->GetResizeDlgLockRect()))    newHover = 9;
                if (newHover != g_viewState.resizeDlgHoverBtn)
                {
                    g_viewState.resizeDlgHoverBtn = newHover;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return 0;
        }

        // Silme dialogu açıksa — hover güncelle
        if (g_viewState.showDeleteConfirmDialog || g_viewState.showUnsavedWarningDialog)
        {
            if (g_renderer && g_renderer->IsDeleteDialogVisible())
            {
                auto inRect = [](float x, float y, const D2D1_RECT_F& r) {
                    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
                };
                int newHover = 0;
                if (!g_viewState.showUnsavedWarningDialog &&
                    inRect(mx, my, g_renderer->GetDlgCancelRect()))
                    newHover = 1;
                else if (inRect(mx, my, g_renderer->GetDlgDeleteRect()))
                    newHover = 2;
                if (newHover != g_viewState.deleteDlgHoverBtn)
                {
                    g_viewState.deleteDlgHoverBtn = newHover;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return 0;
        }

        // Edit toolbar hover — düzenlenebilir statik görüntü varsa toolbar göster
        if (!g_viewState.editIsAnimated && !g_edit.pixels.empty())
        {
            if (!g_mouseTracking)
            {
                TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&tme);
                g_mouseTracking = true;
            }
            if (!g_viewState.editDirty)
            {
                KillTimer(hwnd, kEditToolbarFadeTimerID);
                KillTimer(hwnd, kEditToolbarIdleTimerID);
                SetTimer(hwnd, kEditToolbarIdleTimerID, 2000, nullptr);
            }
            if (g_viewState.editToolbarAlpha < 1.0f)
            {
                g_viewState.editToolbarAlpha = 1.0f;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }

        // Save bar hover — hangi butonun üzerinde olduğunu güncelle
        if (g_renderer && g_renderer->IsSaveBarVisible())
        {
            if (!g_mouseTracking)
            {
                TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&tme);
                g_mouseTracking = true;
            }
            auto inRect = [](float x, float y, const D2D1_RECT_F& r) {
                return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
            };
            int newHover = 0;
            if      (inRect(mx, my, g_renderer->GetSaveBarSaveRect()))    newHover = 1;
            else if (inRect(mx, my, g_renderer->GetSaveBarDiscardRect())) newHover = 2;
            else if (inRect(mx, my, g_renderer->GetSaveBarSaveAsRect()))  newHover = 3;
            if (newHover != g_viewState.saveBarHover)
            {
                g_viewState.saveBarHover = newHover;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        else if (g_viewState.saveBarHover != 0)
        {
            g_viewState.saveBarHover = 0;
            InvalidateRect(hwnd, nullptr, FALSE);
        }

        if (!g_dragging) return 0;
        float nx = g_panAtDragStartX + (mx - g_dragStartX);
        float ny = g_panAtDragStartY + (my - g_dragStartY);
        g_viewState.panX = g_viewState.panXTarget = nx;
        g_viewState.panY = g_viewState.panYTarget = ny;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_MOUSELEAVE:
        g_mouseTracking = false;
        if (!g_viewState.editDirty && g_viewState.editToolbarAlpha > 0.0f)
        {
            KillTimer(hwnd, kEditToolbarIdleTimerID);
            QueryPerformanceCounter(&g_editToolbarFadeLastTime);
            SetTimer(hwnd, kEditToolbarFadeTimerID, kAnimIntervalMs, nullptr);
        }
        if (g_viewState.saveBarHover != 0 || g_viewState.saveBarPressedBtn != 0)
        {
            g_viewState.saveBarHover      = 0;
            g_viewState.saveBarPressedBtn = 0;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_LBUTTONUP:
    {
        float mx = static_cast<float>(GET_X_LPARAM(lParam));
        float my = static_cast<float>(GET_Y_LPARAM(lParam));

        // Serbest döndürme dialogu açıksa — slider bırakma / buton eylemi
        if (g_viewState.showRotateFreeDialog)
        {
            if (g_rotFreeDlgSliderDragging)
            {
                g_rotFreeDlgSliderDragging = false;
                ReleaseCapture();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            int pressed = g_viewState.rotateFreeDlgPressBtn;
            g_viewState.rotateFreeDlgPressBtn = 0;
            ReleaseCapture();

            if (g_renderer && g_renderer->IsRotFreeDlgVisible() && pressed != 0)
            {
                auto inR = [](float x, float y, const D2D1_RECT_F& r) {
                    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
                };
                auto clampAngle = [](float a) {
                    return a < -45.0f ? -45.0f : (a > 45.0f ? 45.0f : a);
                };
                switch (pressed)
                {
                case 1:  // −1°
                    if (inR(mx, my, g_renderer->GetRotFreeDlgCoarseDecRect()))
                        g_viewState.rotateFreeAngle = clampAngle(g_viewState.rotateFreeAngle - 1.0f);
                    break;
                case 2:  // −0.1°
                    if (inR(mx, my, g_renderer->GetRotFreeDlgFineDecRect()))
                        g_viewState.rotateFreeAngle = clampAngle(g_viewState.rotateFreeAngle - 0.1f);
                    break;
                case 3:  // +0.1°
                    if (inR(mx, my, g_renderer->GetRotFreeDlgFineIncRect()))
                        g_viewState.rotateFreeAngle = clampAngle(g_viewState.rotateFreeAngle + 0.1f);
                    break;
                case 4:  // +1°
                    if (inR(mx, my, g_renderer->GetRotFreeDlgCoarseIncRect()))
                        g_viewState.rotateFreeAngle = clampAngle(g_viewState.rotateFreeAngle + 1.0f);
                    break;
                case 5:  // Sıfırla
                    if (inR(mx, my, g_renderer->GetRotFreeDlgResetRect()))
                        g_viewState.rotateFreeAngle = 0.0f;
                    break;
                case 6:  // İptal
                    g_viewState.showRotateFreeDialog  = false;
                    g_viewState.rotateFreeDlgHoverBtn = 0;
                    break;
                case 7:  // Uygula
                    if (inR(mx, my, g_renderer->GetRotFreeDlgApplyRect()))
                        DoApplyRotateFree(hwnd);
                    break;
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            else
                InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        // Resize dialogu açıksa — buton bırakma işlemi
        if (g_viewState.showResizeDialog)
        {
            int pressed = g_viewState.resizeDlgPressedBtn;
            g_viewState.resizeDlgPressedBtn = 0;
            ReleaseCapture();

            if (g_renderer && g_renderer->IsResizeDialogVisible() && pressed != 0)
            {
                auto inR = [](float x, float y, const D2D1_RECT_F& r) {
                    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
                };
                switch (pressed)
                {
                case 1:  // İptal
                    g_viewState.showResizeDialog   = false;
                    g_viewState.resizeDlgHoverBtn  = 0;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    break;
                case 2:  // Uygula
                    if (inR(mx, my, g_renderer->GetResizeDlgApplyRect()))
                        DoResizeConfirmed(hwnd);
                    else
                        InvalidateRect(hwnd, nullptr, FALSE);
                    break;
                case 3:  // px modu
                    if (inR(mx, my, g_renderer->GetResizeDlgModePxRect()))
                    {
                        if (g_viewState.resizeMode != 0)
                        {
                            g_viewState.resizeMode = 0;
                            g_viewState.resizeW = g_viewState.resizeOrigW;
                            g_viewState.resizeH = g_viewState.resizeOrigH;
                        }
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                    break;
                case 4:  // % modu
                    if (inR(mx, my, g_renderer->GetResizeDlgModePctRect()))
                    {
                        if (g_viewState.resizeMode != 1)
                        {
                            g_viewState.resizeMode = 1;
                            g_viewState.resizePct  = 100;
                            g_viewState.resizeW    = g_viewState.resizeOrigW;
                            g_viewState.resizeH    = g_viewState.resizeOrigH;
                        }
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                    break;
                case 9:  // Oran kilidi toggle
                    if (inR(mx, my, g_renderer->GetResizeDlgLockRect()))
                    {
                        g_viewState.resizeLockAspect = !g_viewState.resizeLockAspect;
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                    break;
                default:  // 5=W−, 6=W+, 7=H−, 8=H+
                {
                    // Dec/Inc doğrulama (mouse aynı buton üzerinde mi?)
                    bool stillOn = false;
                    if      (pressed == 5 && inR(mx, my, g_renderer->GetResizeDlgWDecRect())) stillOn = true;
                    else if (pressed == 6 && inR(mx, my, g_renderer->GetResizeDlgWIncRect())) stillOn = true;
                    else if (pressed == 7 && inR(mx, my, g_renderer->GetResizeDlgHDecRect())) stillOn = true;
                    else if (pressed == 8 && inR(mx, my, g_renderer->GetResizeDlgHIncRect())) stillOn = true;
                    if (stillOn)
                        ResizeDlgAdjust(hwnd, pressed);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    break;
                }
                }
            }
            else
                InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        // Silme dialogu açıksa — buton bırakma işlemi
        if (g_viewState.showDeleteConfirmDialog || g_viewState.showUnsavedWarningDialog)
        {
            int pressed = g_viewState.deleteDlgPressedBtn;
            g_viewState.deleteDlgPressedBtn = 0;
            ReleaseCapture();

            if (g_renderer && g_renderer->IsDeleteDialogVisible() && pressed != 0)
            {
                auto inRect = [](float x, float y, const D2D1_RECT_F& r) {
                    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
                };
                bool hitCancel = !g_viewState.showUnsavedWarningDialog &&
                                  inRect(mx, my, g_renderer->GetDlgCancelRect());
                bool hitDelete = inRect(mx, my, g_renderer->GetDlgDeleteRect());

                if (g_viewState.showUnsavedWarningDialog)
                {
                    if (hitDelete) // "Tamam" butonu
                    {
                        g_viewState.showUnsavedWarningDialog = false;
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                }
                else
                {
                    if (hitCancel && pressed == 1)
                    {
                        g_viewState.showDeleteConfirmDialog = false;
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                    else if (hitDelete && pressed == 2)
                    {
                        g_viewState.showDeleteConfirmDialog = false;
                        g_viewState.deleteDlgHoverBtn = 0;
                        DoDeleteConfirmed(hwnd);
                    }
                    else
                        InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            else
                InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        bool wasDragging = g_dragging;
        g_dragging = false;
        ReleaseCapture();

        // Edit toolbar butonu tıklaması
        if (g_clickInToolbar)
        {
            bool wasL   = g_viewState.editBtnRotLPressed;
            bool wasR   = g_viewState.editBtnRotRPressed;
            bool wasFr  = g_viewState.editBtnRotFreePressed;
            bool wasRz  = g_viewState.editBtnResizePressed;
            g_viewState.editBtnRotLPressed    = false;
            g_viewState.editBtnRotRPressed    = false;
            g_viewState.editBtnRotFreePressed = false;
            g_viewState.editBtnResizePressed  = false;
            g_clickInToolbar = false;
            float delta = fabsf(mx - g_mouseDownX) + fabsf(my - g_mouseDownY);
            if (delta < 5.0f)
            {
                if (wasL)        DoRotateCCW(hwnd);
                else if (wasR)   DoRotateCW(hwnd);
                else if (wasFr)  DoOpenRotateFreeDialog(hwnd);
                else if (wasRz)  DoOpenResizeDialog(hwnd);
            }
            else
                InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        // Save bar butonu tıklaması
        if (g_clickInSaveBar)
        {
            g_clickInSaveBar = false;
            g_viewState.saveBarPressedBtn = 0;
            float delta = fabsf(mx - g_mouseDownX) + fabsf(my - g_mouseDownY);
            if (delta < 5.0f && g_renderer)
            {
                D2D1_RECT_F rSave    = g_renderer->GetSaveBarSaveRect();
                D2D1_RECT_F rDiscard = g_renderer->GetSaveBarDiscardRect();
                D2D1_RECT_F rSaveAs  = g_renderer->GetSaveBarSaveAsRect();
                if (mx >= rSave.left && mx <= rSave.right && my >= rSave.top && my <= rSave.bottom)
                    DoSave(hwnd);
                else if (mx >= rDiscard.left && mx <= rDiscard.right && my >= rDiscard.top && my <= rDiscard.bottom)
                    DoDiscard(hwnd);
                else if (mx >= rSaveAs.left && mx <= rSaveAs.right && my >= rSaveAs.top && my <= rSaveAs.bottom)
                    DoSaveAs(hwnd);
            }
            else
                InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

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

        // Delete button tıklaması
        if (g_clickIsDeleteBtn)
        {
            g_viewState.deleteBtnPressed = false;
            float delta = fabsf(mx - g_mouseDownX) + fabsf(my - g_mouseDownY);
            if (delta < 5.0f)
                DoDelete(hwnd);
            else
                InvalidateRect(hwnd, nullptr, FALSE);
            g_clickIsDeleteBtn = false;
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

        // Yeniden boyutlandır / serbest döndürme dialogu açıkken çift tıklama ile zoom yapma
        if (g_viewState.showResizeDialog || g_viewState.showRotateFreeDialog) return 0;

        // Edit toolbar butonlarında çift tıklamayı engelle — hızlı ardışık döndürme desteği
        if (g_renderer && g_renderer->IsEditToolbarVisible() && !g_viewState.editIsAnimated)
        {
            D2D1_RECT_F rL = g_renderer->GetEditBtnRotLRect();
            D2D1_RECT_F rR = g_renderer->GetEditBtnRotRRect();
            if ((cx >= rL.left && cx <= rL.right && cy >= rL.top && cy <= rL.bottom) ||
                (cx >= rR.left && cx <= rR.right && cy >= rR.top && cy <= rR.bottom))
            {
                // İkinci tık: LBUTTONDOWN gibi işle — hemen döndür
                if (cx >= rL.left && cx <= rL.right && cy >= rL.top && cy <= rL.bottom)
                    DoRotateCCW(hwnd);
                else
                    DoRotateCW(hwnd);
                return 0;
            }
        }

        // Save bar butonlarında çift tıklamayı engelle
        if (g_renderer && g_renderer->IsSaveBarVisible() && g_viewState.editDirty)
        {
            D2D1_RECT_F rSave    = g_renderer->GetSaveBarSaveRect();
            D2D1_RECT_F rDiscard = g_renderer->GetSaveBarDiscardRect();
            D2D1_RECT_F rSaveAs  = g_renderer->GetSaveBarSaveAsRect();
            if ((cx >= rSave.left    && cx <= rSave.right    && cy >= rSave.top    && cy <= rSave.bottom)    ||
                (cx >= rDiscard.left && cx <= rDiscard.right && cy >= rDiscard.top && cy <= rDiscard.bottom) ||
                (cx >= rSaveAs.left  && cx <= rSaveAs.right  && cy >= rSaveAs.top  && cy <= rSaveAs.bottom))
                return 0;
        }

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

        // Serbest döndürme dialogu açıksa — Escape=kapat, Enter=uygula, ←/→=ince ayar
        if (g_viewState.showRotateFreeDialog)
        {
            auto clamp45 = [](float a) { return a < -45.0f ? -45.0f : (a > 45.0f ? 45.0f : a); };
            if (wParam == VK_ESCAPE)
            {
                g_viewState.showRotateFreeDialog  = false;
                g_viewState.rotateFreeDlgHoverBtn = 0;
                g_viewState.rotateFreeDlgPressBtn = 0;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            else if (wParam == VK_RETURN)
                DoApplyRotateFree(hwnd);
            else if (wParam == VK_LEFT)
            {
                float step = (GetKeyState(VK_SHIFT) & 0x8000) ? 1.0f : 0.1f;
                g_viewState.rotateFreeAngle = clamp45(g_viewState.rotateFreeAngle - step);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            else if (wParam == VK_RIGHT)
            {
                float step = (GetKeyState(VK_SHIFT) & 0x8000) ? 1.0f : 0.1f;
                g_viewState.rotateFreeAngle = clamp45(g_viewState.rotateFreeAngle + step);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        // Resize dialogu açıksa — Escape ile kapat, Enter ile uygula
        if (g_viewState.showResizeDialog)
        {
            if (wParam == VK_ESCAPE)
            {
                g_viewState.showResizeDialog   = false;
                g_viewState.resizeDlgHoverBtn  = 0;
                g_viewState.resizeDlgPressedBtn = 0;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            else if (wParam == VK_RETURN)
                DoResizeConfirmed(hwnd);
            return 0;
        }

        // Silme dialogu açıksa — klavye ile de kontrol edilebilir
        if (g_viewState.showDeleteConfirmDialog || g_viewState.showUnsavedWarningDialog)
        {
            if (wParam == VK_ESCAPE)
            {
                g_viewState.showDeleteConfirmDialog  = false;
                g_viewState.showUnsavedWarningDialog = false;
                g_viewState.deleteDlgHoverBtn        = 0;
                g_viewState.deleteDlgPressedBtn      = 0;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            else if (wParam == VK_RETURN)
            {
                bool isWarning = g_viewState.showUnsavedWarningDialog;
                g_viewState.showDeleteConfirmDialog  = false;
                g_viewState.showUnsavedWarningDialog = false;
                g_viewState.deleteDlgHoverBtn        = 0;
                g_viewState.deleteDlgPressedBtn      = 0;
                if (!isWarning)
                    DoDeleteConfirmed(hwnd);
                else
                    InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

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
            if (GetKeyState(VK_CONTROL) & 0x8000)
                DoRotateCCW(hwnd);
            else if (g_navigator && !g_navigator->empty())
                NavigateTo(hwnd, g_navigator->prev());
            break;

        case VK_RIGHT:
            if (GetKeyState(VK_CONTROL) & 0x8000)
                DoRotateCW(hwnd);
            else if (g_navigator && !g_navigator->empty())
                NavigateTo(hwnd, g_navigator->next());
            break;

        case VK_OEM_4:  // '[' — 90° sola döndür
            DoRotateCCW(hwnd);
            break;

        case VK_OEM_6:  // ']' — 90° sağa döndür
            DoRotateCW(hwnd);
            break;

        case VK_DELETE:  // Sil — Geri Dönüşüm Kutusu'na taşı
            DoDelete(hwnd);
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
        else if (wParam == kEditToolbarIdleTimerID)
        {
            // 2s hareketsizlik doldu — fade animasyonunu başlat (sadece dirty değilse)
            KillTimer(hwnd, kEditToolbarIdleTimerID);
            if (!g_viewState.editDirty)
            {
                QueryPerformanceCounter(&g_editToolbarFadeLastTime);
                SetTimer(hwnd, kEditToolbarFadeTimerID, kAnimIntervalMs, nullptr);
            }
        }
        else if (wParam == kEditToolbarFadeTimerID)
        {
            if (g_viewState.editDirty)
            {
                // Dirty modda toolbar kalıcı görünür — fade iptal
                KillTimer(hwnd, kEditToolbarFadeTimerID);
                return 0;
            }
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            float dt = static_cast<float>(now.QuadPart - g_editToolbarFadeLastTime.QuadPart)
                       / static_cast<float>(g_qpcFreq.QuadPart);
            g_editToolbarFadeLastTime = now;
            dt = min(dt, 0.1f);

            constexpr float kFadeSpeed = 2.0f;
            g_viewState.editToolbarAlpha -= dt * kFadeSpeed;
            if (g_viewState.editToolbarAlpha <= 0.0f)
            {
                g_viewState.editToolbarAlpha = 0.0f;
                KillTimer(hwnd, kEditToolbarFadeTimerID);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        else if (wParam == kDirChangeTimerID)
        {
            KillTimer(hwnd, kDirChangeTimerID);
            if (g_navigator && !g_navigator->empty())
            {
                std::wstring currentPath = g_navigator->current();
                bool currentExists =
                    GetFileAttributesW(currentPath.c_str()) != INVALID_FILE_ATTRIBUTES;

                g_navigator->refresh();

                if (g_navigator->empty())
                {
                    // Klasördeki tüm resimler silindi
                    if (g_renderer) { g_renderer->ClearImage(); g_renderer->SetStripSlots({}, 0); }
                    g_viewState = ViewState{};
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                else
                {
                    g_viewState.imageIndex = g_navigator->index() + 1;
                    g_viewState.imageTotal = g_navigator->total();

                    if (!currentExists && !g_viewState.editDirty)
                    {
                        // Geçerli dosya silindi, kaydedilmemiş değişiklik yok → komşuya geç
                        NavigateTo(hwnd, g_navigator->current());
                    }
                    else
                    {
                        // Listeye dosya eklendi/silindi ama geçerli dosya hâlâ mevcut
                        ++g_thumbCancel;
                        UpdateStripSlots(hwnd);
                        TriggerThumbFetches(hwnd);
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                }
            }
        }
        return 0;

    case WM_FILE_CHANGED:
        // Watcher thread'den gelen bildirim — debounce: 200ms sonra işle
        SetTimer(hwnd, kDirChangeTimerID, 200, nullptr);
        return 0;

    case WM_SAVE_DONE:
    {
        auto* r = reinterpret_cast<SaveDoneResult*>(lParam);
        g_isSaving.store(false);

        if (r->success)
        {
            if (r->isSaveAs) {
                g_edit.filePath = r->savedPath;
                g_edit.format   = r->format;
            }
            g_edit.isDirty        = false;
            g_viewState.editDirty = false;
            g_viewState.editToolbarAlpha = 1.0f;
            KillTimer(hwnd, kEditToolbarFadeTimerID);
            KillTimer(hwnd, kEditToolbarIdleTimerID);
            SetTimer(hwnd, kEditToolbarIdleTimerID, 2000, nullptr);

            // Meta cache: eski girdi temizle, güncel bilgiyi yaz
            {
                std::lock_guard<std::mutex> lk(g_metaCacheMutex);
                g_metaCache.erase(r->savedPath);
                if (r->isSaveAs && r->origPath != r->savedPath)
                    g_metaCache.erase(r->origPath);
                ImageInfo updatedMeta = g_imageInfo;
                updatedMeta.format    = r->format;
                g_metaCache[r->savedPath] = std::move(updatedMeta);
            }

            // Prefetch cache'i güncelle: eski girdi temizle, düzenlenmiş pikselleri ekle
            {
                std::lock_guard<std::mutex> lk(g_cacheMutex);
                g_decodeCache.erase(r->savedPath);
                if (r->isSaveAs && r->origPath != r->savedPath)
                    g_decodeCache.erase(r->origPath);

                // Kaydedilen pikselleri cache'e ekle — geri navigasyonda anında yüklenir
                if (!g_edit.pixels.empty() && g_edit.width > 0 && g_edit.height > 0)
                {
                    if (g_decodeCache.size() >= kCacheMaxSize)
                    {
                        auto oldest = g_decodeCache.begin();
                        delete oldest->second;
                        g_decodeCache.erase(oldest);
                    }
                    auto* cached        = new DecodeResult();
                    cached->path        = r->savedPath;
                    cached->pixels      = g_edit.pixels;  // BGRA pre-multiplied
                    cached->width       = g_edit.width;
                    cached->height      = g_edit.height;
                    cached->info        = g_imageInfo;
                    cached->info.format = r->format;
                    g_decodeCache[r->savedPath] = cached;
                }
            }

            // Filmstrip thumbnail'ini güncel (döndürülmüş) piksellerden oluştur
            if (g_renderer && !g_edit.pixels.empty() && g_edit.width > 0 && g_edit.height > 0)
            {
                constexpr UINT kTargetH = static_cast<UINT>(StripLayout::ThumbH);
                const float aspect = static_cast<float>(g_edit.width) / static_cast<float>(g_edit.height);
                const UINT  thumbH = kTargetH;
                const UINT  thumbW = max(1u, static_cast<UINT>(thumbH * aspect));
                std::vector<uint8_t> tp(static_cast<size_t>(thumbW) * thumbH * 4);
                for (UINT dy = 0; dy < thumbH; ++dy)
                    for (UINT dx = 0; dx < thumbW; ++dx)
                    {
                        UINT sy = dy * g_edit.height / thumbH;
                        UINT sx = dx * g_edit.width  / thumbW;
                        const uint8_t* s = &g_edit.pixels[(static_cast<size_t>(sy) * g_edit.width + sx) * 4];
                        uint8_t*       d = &tp[(static_cast<size_t>(dy) * thumbW + dx) * 4];
                        d[0]=s[0]; d[1]=s[1]; d[2]=s[2]; d[3]=s[3];
                    }
                g_renderer->LoadThumbnail(r->savedPath, tp.data(), thumbW, thumbH);
            }
        }
        delete r;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

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
            TriggerLocationPrefetch();
            // Strip slot'larını güncelle (ilk açılışta NavigateTo çağrılmaz, burada yapılır)
            UpdateStripSlots(hwnd);
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
        // Token kontrolü yok: geç gelen yavaş HEIC decode sonuçları da cache'e alınır.
        // LRU eviction (ThumbCacheMax=20) belleği sınırlı tutar.
        if (g_renderer && !result->pixels.empty())
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

    case WM_SETTINGCHANGE:
        if (lParam && lstrcmpW(reinterpret_cast<LPCWSTR>(lParam), L"ImmersiveColorSet") == 0)
            ApplyTitleBarTheme(hwnd);
        return 0;

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
        KillTimer(hwnd, kEditToolbarIdleTimerID);
        KillTimer(hwnd, kEditToolbarFadeTimerID);
        KillTimer(hwnd, kDirChangeTimerID);
        StopLocationPrefetchThread();
        SaveLocationCache();
        StopDirectoryWatcher();
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
    LoadLocationCache();
    StartLocationPrefetchThread();

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
        UpdateStripSlots(hwnd);
        TriggerThumbFetches(hwnd);

        // Dizin değişikliklerini izle (eklenen/silinen resimler → filmstrip güncellenir)
        if (!g_navigator->directory().empty())
            StartDirectoryWatcher(hwnd, g_navigator->directory());
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
