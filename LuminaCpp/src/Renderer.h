#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <string>
#include <cstdint>
#include <vector>
#include <map>
#include <deque>
#include "ImageDecoder.h"

// Direct2D, DirectWrite ve WIC kütüphanelerini otomatik linkle
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")

// Navigation arrow pill boyutları — hem Renderer hem main.cpp tarafından kullanılır
namespace ArrowLayout {
    constexpr float ZoneW  = 48.0f;  // tıklanabilir bölge genişliği (tam yükseklik)
    constexpr float PillH  = 80.0f;  // pill yüksekliği (sadece görsel)
    constexpr float Radius = 8.0f;   // köşe yarıçapı
}

// Info panel boyutları — hem Renderer hem main.cpp tarafından kullanılır
namespace PanelLayout {
    constexpr float Width   = 320.0f;  // panel genişliği
    constexpr float PadX    = 16.0f;   // panel içi yatay dolgu
    constexpr float HeaderH = 48.0f;   // "Image Details" başlık yüksekliği (sabit, scrolllanmaz)
}

// Info button boyutları — hem Renderer hem main.cpp tarafından kullanılır
namespace InfoButton {
    constexpr float Size   = 32.0f;  // buton genişlik/yükseklik
    constexpr float Margin = 12.0f;  // pencere kenarından mesafe
}

// OSM harita tile anahtarı — tile cache map key'i olarak kullanılır
struct MapTileKey
{
    int zoom = 0;
    int x    = 0;
    int y    = 0;

    bool operator<(const MapTileKey& o) const
    {
        if (zoom != o.zoom) return zoom < o.zoom;
        if (x    != o.x)   return x    < o.x;
        return y < o.y;
    }
};

// Thumbnail strip boyutları — hem Renderer hem main.cpp tarafından kullanılır
namespace StripLayout {
    constexpr float OpenH     = 88.0f;  // strip açık yüksekliği
    constexpr float ThumbH    = 68.0f;  // thumbnail yüksekliği
    constexpr float PadY      = 10.0f;  // üst/alt dolgu
    constexpr float PadX      =  4.0f;  // thumbnail arası boşluk
    constexpr float ToggleW   = 48.0f;  // toggle pill genişliği
    constexpr float ToggleH   = 18.0f;  // toggle pill yüksekliği
    constexpr int   HalfCount =  4;     // mevcut her iki yanındaki thumbnail sayısı
    constexpr int   ThumbCacheMax = 60; // maksimum önbellek girdi sayısı
}

// Görüntüye ait metadata (decode thread'de doldurulur, UI thread'de okunur)
struct ImageInfo
{
    std::wstring filename;
    int          width         = 0;
    int          height        = 0;
    int64_t      fileSizeBytes = 0;
    std::wstring format;           // e.g. L"JPEG"
    // EXIF alanları — mevcut değilse boş wstring
    std::wstring dateTaken;
    std::wstring cameraMake;
    std::wstring cameraModel;
    std::wstring aperture;         // e.g. L"f/2.8"
    std::wstring shutterSpeed;     // e.g. L"1/500s"
    std::wstring iso;
    // GPS — mevcut değilse boş
    std::wstring gpsLatitude;
    std::wstring gpsLongitude;
    std::wstring gpsAltitude;
    // GPS ondalık derece — OSM linki için; hasGpsDecimal false ise geçersiz
    bool         hasGpsDecimal  = false;
    double       gpsLatDecimal  = 0.0;
    double       gpsLonDecimal  = 0.0;
    // Nominatim konum adı — ör. L"Merzifon, Amasya, Türkiye"
    std::wstring gpsLocationName;
    // ICC renk profili adı — ör. L"Adobe RGB (1998)"; yoksa boş
    std::wstring iccProfileName;
    // Decode hatası — boş değilse ekranda gösterilir
    std::wstring errorMessage;
};

// Görüntünün ekrandaki dönüşüm durumu.
// WndProc fare/klavye olaylarında günceller; Renderer sadece okur.
struct ViewState
{
    float zoomFactor        = 1.0f;   // 1.0 = fit-to-window, >1 = yakın, <1 = uzak
    float panX              = 0.0f;   // Piksel cinsinden yatay kaydırma
    float panY              = 0.0f;   // Piksel cinsinden dikey kaydırma
    // Smooth zoom animasyonu hedefleri — timer her frame'de bunlara lerp eder
    float zoomTarget        = 1.0f;
    float panXTarget        = 0.0f;
    float panYTarget        = 0.0f;
    bool  showInfoPanel     = false;  // I tuşuyla toggle — navigasyonda sıfırlanmaz
    float panelAnimWidth    = 0.0f;   // Animasyonlu panel genişliği (0.0–PanelLayout::Width)
    bool  use12HourTime     = false;  // T tuşuyla toggle — 12h/24h saat gösterimi
    int   imageIndex        = 0;      // 1-based; 0 = klasör yok
    int   imageTotal        = 0;      // 0 = klasör yok
    bool  showThumbStrip    = true;   // F tuşuyla toggle — thumbnail filmstrip
    float stripAnimHeight   = 0.0f;   // Animasyonlu strip yüksekliği (0.0–StripLayout::OpenH)
    bool  infoBtnPressed       = false;  // Info butonu basılı tutulurken geçici highlight
    bool  deleteBtnPressed     = false;  // Delete butonu basılı tutulurken highlight
    bool  toggleBtnPressed     = false;  // Filmstrip toggle pill basılı tutulurken highlight
    float indexBarAlpha        = 1.0f;   // Index göstergesi opaklığı (0=görünmez, 1=tam)
    float zoomIndicatorAlpha   = 0.0f;   // Zoom göstergesi opaklığı — alpha>0 ise çizilir
    bool  leftArrowPressed     = false;  // Sol ok basılı (press highlight)
    bool  rightArrowPressed    = false;  // Sağ ok basılı (press highlight)
    float panelScrollY         = 0.0f;  // Info paneli scroll ofseti (piksel, 0 = üst)

    // Fotoğraf düzenleme durumu
    bool  editDirty            = false;  // kaydedilmemiş değişiklik var — save bar gösterilir
    bool  editIsAnimated       = false;  // animasyonlu görsel — düzenleme toolbar gizlenir
    float editToolbarAlpha     = 0.0f;  // edit toolbar görünürlüğü (hover fade, 0=gizli 1=tam)
    bool  editBtnRotLPressed    = false;  // ↺ (CCW) butonu basılı
    bool  editBtnRotRPressed    = false;  // ↻ (CW)  butonu basılı
    bool  editBtnRotFreePressed = false;  // serbest döndür butonu basılı
    bool  editBtnResizePressed  = false;  // Resize butonu basılı (press highlight)
    int   editToolbarHoverBtn   = 0;      // 0=yok, 1=CCW, 2=CW, 3=Araçlar, 4=Free, 5=Resize, 6=Kırp
    float editToolbarTooltipAlpha = 0.0f; // tooltip fade-in opaklığı (0=gizli, 1=tam)
    bool  editBtnMorePressed   = false;  // ··· araçlar butonu basılı
    bool  editMoreExpanded     = false;  // araçlar paneli açık mı
    float editMoreAlpha        = 0.0f;   // genişleme animasyon değeri (0=kapalı, 1=açık)

    // Serbest döndürme dialog
    bool  showRotateFreeDialog  = false;
    float rotateFreeAngle       = 0.0f;  // -45..+45 derece; canlı D2D önizlemesi için
    int   rotateFreeDlgHoverBtn = 0;     // 0=yok, 1=−1°, 2=−0.1°, 3=+0.1°, 4=+1°, 5=Sıfırla, 6=İptal, 7=Uygula
    int   rotateFreeDlgPressBtn = 0;

    // Yeniden boyutlandır dialog
    bool  showResizeDialog     = false;
    int   resizeMode           = 0;     // 0=piksel, 1=yüzde
    int   resizeW              = 0;     // hedef genişlik (piksel)
    int   resizeH              = 0;     // hedef yükseklik (piksel)
    int   resizePct            = 100;   // yüzde değeri (% modunda)
    int   resizeOrigW          = 0;     // dialog açıldığında orijinal genişlik
    int   resizeOrigH          = 0;     // dialog açıldığında orijinal yükseklik
    bool  resizeLockAspect     = true;  // en-boy oranı kilidi
    int   resizeDlgHoverBtn    = 0;     // hover edilen buton ID (0=yok)
    int   resizeDlgPressedBtn  = 0;     // basılı buton ID

    // Save bar buton durumları (0=yok, 1=Kaydet, 2=Kaydetme, 3=Ayrı Kaydet)
    int   saveBarHover         = 0;
    int   saveBarPressedBtn    = 0;

    // Silme onay dialogu
    bool  showDeleteConfirmDialog  = false;  // "Geri Dönüşüm Kutusu'na taşı?" onay ekranı
    bool  showUnsavedWarningDialog = false;  // "Kaydedilmemiş değişiklik" uyarı ekranı
    int   deleteDlgHoverBtn        = 0;      // 0=yok, 1=İptal/Tamam, 2=Sil
    int   deleteDlgPressedBtn      = 0;      // basılı buton (press highlight)

    // Kırpma modu
    bool  showCropDialog     = false;
    int   cropAspectMode     = 0;     // 0=Serbest, 1=1:1, 2=4:3, 3=3:2, 4=16:9
    float cropX0             = 0.0f;  // image-normalized [0..1]
    float cropY0             = 0.0f;
    float cropX1             = 1.0f;
    float cropY1             = 1.0f;
    int   cropDlgHoverBtn    = 0;     // 0=yok, 1-5=oran, 6=iptal, 7=uygula
    int   cropDlgPressedBtn  = 0;
    bool  editBtnCropPressed = false;

    // Animasyon geçiş durumları
    float dialogAlpha      = 1.0f;  // modal dialog fade-in (0=gizli, 1=tam görünür)
    float saveBarAlpha     = 0.0f;  // save bar fade-in (0=gizli, 1=tam görünür)
};

// Renderer: Direct2D render target yönetimi + WIC görüntü yükleme
// Görev: pencereye GPU üzerinden çizim yapmak
class Renderer
{
public:
    explicit Renderer(HWND hwnd);
    ~Renderer();

    // Renderer kopyalanamaz (COM nesneleri paylaşılamaz)
    Renderer(const Renderer&)            = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Ana çizim fonksiyonu — WM_PAINT'te çağrılır
    void Render(const ViewState& vs, const ImageInfo* info);

    // Pencere boyutlandığında render target'ı güncelle — WM_SIZE'da çağrılır
    void Resize(UINT width, UINT height);

    // Arka plan decode sonucu: ham piksel buffer'ından GPU bitmap oluştur (UI thread'de çağrılır)
    bool LoadImageFromPixels(const uint8_t* pixels, UINT width, UINT height,
                             const std::wstring& path);

    // Mevcut bitmap'i serbest bırak (decode hatası veya gezinme öncesi temizlik)
    void ClearImage();

    // Date toggle rect — DrawInfoPanel her çizimde günceller;
    // WndProc WM_LBUTTONUP'ta tıklama kontrolü için kullanılır.
    D2D1_RECT_F GetDateToggleRect()    const { return m_dateToggleRect; }
    bool        IsDateToggleVisible()  const { return m_dateToggleVisible; }

    // GPS link rect — tıklanabilir koordinat satırı; hasGpsDecimal varsa görünür.
    D2D1_RECT_F GetGpsLinkRect()    const { return m_gpsLinkRect; }
    bool        IsGpsLinkVisible()  const { return m_gpsLinkVisible; }

    // Ham BGRA pre-mul piksellerden tile yükle — WM_TILE_DONE'da kullanılır (hızlı yol).
    // w×h: tile boyutu (genellikle 256×256); stride = w*4 kabul edilir.
    void UploadMapTileRaw(int zoom, int x, int y,
                          const uint8_t* bgra, UINT w, UINT h);

    // Harita önizleme rect — DrawInfoPanel her çizimde günceller;
    // WndProc WM_LBUTTONUP'ta tıklama kontrolü için kullanılır.
    D2D1_RECT_F GetMapPreviewRect()    const { return m_mapPreviewRect; }
    bool        IsMapPreviewVisible()  const { return m_mapPreviewVisible; }

    // Kopyala butonu rect — harita sağ üst köşesi 28×28px.
    D2D1_RECT_F GetMapCopyBtnRect()    const { return m_mapCopyBtnRect; }
    bool        IsMapCopyBtnVisible()  const { return m_mapCopyBtnVisible; }

    // Koordinat kopyalandı bildirimi — 1.5s yeşil tik gösterimi başlatır.
    void MarkCoordsCopied() { m_mapCopiedAt = GetTickCount64(); }

    // Tile cache'ini temizle — yeniden çekme için.
    void ClearMapTiles()
    {
        for (auto& [key, bmp] : m_mapTileCache) if (bmp) bmp->Release();
        m_mapTileCache.clear();
    }

    // Animasyon — WM_DECODE_DONE'dan sonra çağrılır
    void LoadAnimationFrames(const std::vector<AnimFrame>& frames);
    void ClearAnimation();
    bool IsAnimated()            const { return !m_animBitmaps.empty(); }
    int  AdvanceFrame();               // bir sonraki frame'e geç, yeni frame süresi (ms) döner
    int  GetCurrentFrameDuration() const;

    // Edit toolbar — düzenleme butonları
    D2D1_RECT_F GetEditBtnRotLRect()   const { return m_editBtnRotLRect; }
    D2D1_RECT_F GetEditBtnRotRRect()   const { return m_editBtnRotRRect; }
    D2D1_RECT_F GetEditBtnMoreRect()   const { return m_editBtnMoreRect; }
    bool        IsEditToolbarVisible() const { return m_editToolbarVisible; }

    // Save bar — kayıt seçenekleri (Kaydet / Kaydetme / Ayrı Kaydet)
    D2D1_RECT_F GetSaveBarSaveRect()    const { return m_saveBarSaveRect; }
    D2D1_RECT_F GetSaveBarDiscardRect() const { return m_saveBarDiscardRect; }
    D2D1_RECT_F GetSaveBarSaveAsRect()  const { return m_saveBarSaveAsRect; }
    bool        IsSaveBarVisible()      const { return m_saveBarVisible; }

    // Delete butonu rect — info butonunun solunda; WndProc tıklama testi için
    D2D1_RECT_F GetDeleteBtnRect()    const { return m_deleteBtnRect; }
    bool        IsDeleteBtnVisible()  const { return m_deleteBtnVisible; }

    // Resize butonu (edit toolbar, 3. buton) — WndProc tıklama testi için
    D2D1_RECT_F GetEditBtnResizeRect()     const { return m_editBtnResizeRect; }

    // Kırpma butonu (edit toolbar, 5. buton)
    D2D1_RECT_F GetEditBtnCropRect()       const { return m_editBtnCropRect; }

    // Kırpma dialog rect'leri — WndProc hit testi için
    bool        IsCropDlgVisible()         const { return m_cropDlgVisible; }
    D2D1_RECT_F GetCropDlgCancelRect()     const { return m_cropDlgCancelRect; }
    D2D1_RECT_F GetCropDlgApplyRect()      const { return m_cropDlgApplyRect; }
    D2D1_RECT_F GetCropDlgRatioRect(int i) const { return m_cropDlgRatioRects[i]; }  // i: 0-4

    // Son render'da hesaplanan görüntü ekran rect'i — kırpma hit testi için
    D2D1_RECT_F GetImageDisplayRect()      const { return m_imageDisplayRect; }

    // Resize dialog rect'leri — WndProc etkileşim testi için
    bool        IsResizeDialogVisible()    const { return m_resizeDlgVisible; }
    D2D1_RECT_F GetResizeDlgCancelRect()   const { return m_resizeDlgCancelRect; }
    D2D1_RECT_F GetResizeDlgApplyRect()    const { return m_resizeDlgApplyRect; }
    D2D1_RECT_F GetResizeDlgModePxRect()   const { return m_resizeDlgModePxRect; }
    D2D1_RECT_F GetResizeDlgModePctRect()  const { return m_resizeDlgModePctRect; }
    D2D1_RECT_F GetResizeDlgWDecRect()     const { return m_resizeDlgWDecRect; }
    D2D1_RECT_F GetResizeDlgWIncRect()     const { return m_resizeDlgWIncRect; }
    D2D1_RECT_F GetResizeDlgHDecRect()     const { return m_resizeDlgHDecRect; }
    D2D1_RECT_F GetResizeDlgHIncRect()     const { return m_resizeDlgHIncRect; }
    D2D1_RECT_F GetResizeDlgLockRect()     const { return m_resizeDlgLockRect; }

    // Silme onay dialogu buton rect'leri — WndProc hit testi için
    D2D1_RECT_F GetDlgCancelRect() const { return m_dlgCancelRect; }
    D2D1_RECT_F GetDlgDeleteRect() const { return m_dlgDeleteRect; }
    bool        IsDeleteDialogVisible() const { return m_dlgVisible; }

    // Serbest döndürme dialog rect'leri — WndProc hit testi için
    D2D1_RECT_F GetRotFreeDlgCoarseDecRect() const { return m_rotFreeDlgCoarseDecRect; }
    D2D1_RECT_F GetRotFreeDlgFineDecRect()   const { return m_rotFreeDlgFineDecRect; }
    D2D1_RECT_F GetRotFreeDlgFineIncRect()   const { return m_rotFreeDlgFineIncRect; }
    D2D1_RECT_F GetRotFreeDlgCoarseIncRect() const { return m_rotFreeDlgCoarseIncRect; }
    D2D1_RECT_F GetRotFreeDlgResetRect()     const { return m_rotFreeDlgResetRect; }
    D2D1_RECT_F GetRotFreeDlgCancelRect()    const { return m_rotFreeDlgCancelRect; }
    D2D1_RECT_F GetRotFreeDlgApplyRect()     const { return m_rotFreeDlgApplyRect; }
    D2D1_RECT_F GetRotFreeDlgSliderRect()    const { return m_rotFreeDlgSliderRect; }
    bool        IsRotFreeDlgVisible()        const { return m_rotFreeDlgVisible; }
    D2D1_RECT_F GetEditBtnRotFreeRect()      const { return m_editBtnRotFreeRect; }

    // Thumbnail strip — main.cpp tarafından yönetilir
    void LoadThumbnail(const std::wstring& path, const uint8_t* pixels, UINT w, UINT h);
    bool HasThumbnail(const std::wstring& path) const;
    // Thumbnail önbellekteyse ana bitmap olarak geçici göster — tam decode gelince otomatik değişir
    bool ShowThumbnailAsPlaceholder(const std::wstring& path);
    void ClearThumbnails();
    // Hangi slot'ların gösterileceğini ayarla: paths[currentIdx] = mevcut görüntü
    void SetStripSlots(const std::vector<std::wstring>& paths, int currentIdx);
    // Strip toggle pill rect — her frame'de güncellenir
    D2D1_RECT_F GetStripToggleRect()    const { return m_stripToggleRect; }
    bool        IsStripToggleVisible()  const { return m_stripToggleVisible; }
    // Tıklanan thumbnail'in mevcut görüntüye göre ofseti; kapsam dışıysa INT_MIN döner
    int  GetThumbClickOffset(float x, float y) const;

private:
    // GPU cihazına bağlı kaynakları oluştur
    HRESULT CreateDeviceResources();
    // GPU cihazı kaybolunca kaynakları serbest bırak (D2DERR_RECREATE_TARGET)
    void    DiscardDeviceResources();
    // GPU cihazı kurtarma: senkron WIC yükleme — sadece D2DERR_RECREATE_TARGET sonrası çağrılır
    bool    LoadImage(const std::wstring& path);

    // Overlay çizim yardımcıları
    void DrawNavArrows(const ViewState& vs);
    void DrawIndexBar(const ViewState& vs);
    void DrawInfoPanel(const ViewState& vs, const ImageInfo* info);
    void DrawInfoButton(const ViewState& vs);
    void DrawThumbnailStrip(const ViewState& vs);
    void DrawStripToggle(const ViewState& vs);
    // Harita önizleme çizimi — DrawInfoPanel içinden çağrılır
    void DrawMapPreview(float x0, float x1, float y, const ViewState& vs, const ImageInfo* info);
    // Düzenleme toolbar ve save bar
    void DrawEditToolbar(const ViewState& vs);
    void DrawSaveBar(const ViewState& vs);
    // Silme onay / uyarı dialogu
    void DrawDeleteConfirmDialog(const ViewState& vs, const ImageInfo* info);
    // Yeniden boyutlandır dialogu
    void DrawResizeDialog(const ViewState& vs);
    // Serbest döndürme dialogu
    void DrawRotateFreeDialog(const ViewState& vs);
    // Kırpma dialogu
    void DrawCropDialog(const ViewState& vs);

    HWND                   m_hwnd         = nullptr;
    ID2D1Factory*          m_factory      = nullptr;   // Direct2D fabrikası (cihazdan bağımsız)
    ID2D1HwndRenderTarget* m_renderTarget = nullptr;   // HWND'e bağlı render target (GPU)

    // DirectWrite — cihazdan bağımsız
    IDWriteFactory*        m_dwriteFactory = nullptr;
    IDWriteTextFormat*     m_textFormat    = nullptr;  // 16 DIP Semi-Bold, center — zoom indicator + ok glipleri
    IDWriteTextFormat*     m_labelFormat   = nullptr;  // 13 DIP Regular, left — panel etiketler
    IDWriteTextFormat*     m_valueFormat   = nullptr;  // 13 DIP Semi-Bold, left — panel değerler
    IDWriteTextFormat*     m_indexFormat   = nullptr;  // 14 DIP Semi-Bold, center — index bar
    IDWriteTextFormat*     m_toggleFormat  = nullptr;  // 10 DIP Regular,    center — time toggle pill
    IDWriteTextFormat*     m_btnFormat     = nullptr;  // 13 DIP SemiBold,   center — save bar butonları

    // Fırçalar (cihaza bağlı, render target ile birlikte oluşturulur/yok edilir)
    ID2D1SolidColorBrush*  m_whiteBrush       = nullptr;   // Metin için
    ID2D1SolidColorBrush*  m_overlayBrush     = nullptr;   // Yarı saydam siyah arka plan (ok, index)
    ID2D1SolidColorBrush*  m_activeBrush      = nullptr;   // Info button aktif durumu
    ID2D1SolidColorBrush*  m_toggleFillBrush  = nullptr;   // Segmented toggle aktif segment dolgusu
    ID2D1SolidColorBrush*  m_grayBrush        = nullptr;   // #AAAAAA — etiket metni
    ID2D1SolidColorBrush*  m_panelBgBrush     = nullptr;   // #1A1A1A — panel arka planı
    ID2D1SolidColorBrush*  m_separatorBrush   = nullptr;   // #333333 — section ayraçlar + panel kenar
    ID2D1SolidColorBrush*  m_saveBtnBrush     = nullptr;   // #4CAF50 — "Kaydet" butonu accent
    ID2D1SolidColorBrush*  m_deleteBrush      = nullptr;   // #C62828 — "Sil" butonu tehlike rengi

    ID2D1Bitmap*           m_bitmap       = nullptr;   // GPU'ya yüklenmiş görüntü
    std::wstring           m_imagePath;                // D2DERR_RECREATE_TARGET'ta yeniden yüklemek için

    // Date toggle badge — DrawInfoPanel tarafından doldurulur
    D2D1_RECT_F            m_dateToggleRect    = {};
    bool                   m_dateToggleVisible = false;

    // GPS link rect — DrawInfoPanel tarafından doldurulur
    D2D1_RECT_F            m_gpsLinkRect    = {};
    bool                   m_gpsLinkVisible = false;

    // Animasyon bitmap dizisi (cihaza bağlı) + frame süresi
    std::vector<ID2D1Bitmap*> m_animBitmaps;
    std::vector<int>          m_animDurations;  // ms, m_animBitmaps ile 1:1
    int                       m_animFrameIdx = 0;

    // Thumbnail cache (cihaza bağlı) — LRU, en fazla StripLayout::ThumbCacheMax girdi
    std::map<std::wstring, ID2D1Bitmap*> m_thumbCache;
    std::deque<std::wstring>             m_thumbOrder;  // LRU sırası (en eskisi önde)

    // Strip slot bilgisi — SetStripSlots tarafından güncellenir
    std::vector<std::wstring> m_stripPaths;      // gösterilecek sıralı yol listesi
    int                       m_stripCurrentIdx = 0;  // m_stripPaths içindeki mevcut indeks

    // Strip toggle + thumb hit-test rect'leri — her Render'da güncellenir
    D2D1_RECT_F              m_stripToggleRect    = {};
    bool                     m_stripToggleVisible = false;
    std::vector<D2D1_RECT_F> m_thumbRects;   // m_stripPaths ile 1:1 hizalı

    // OSM harita tile cache (cihaza bağlı) — UploadMapTileRaw ile doldurulur
    std::map<MapTileKey, ID2D1Bitmap*> m_mapTileCache;

    // Harita önizleme alanı — DrawMapPreview tarafından doldurulur
    D2D1_RECT_F m_mapPreviewRect    = {};
    bool        m_mapPreviewVisible = false;

    // Kopyala butonu alanı — DrawMapPreview tarafından doldurulur
    D2D1_RECT_F m_mapCopyBtnRect    = {};
    bool        m_mapCopyBtnVisible = false;

    // Koordinat kopyalama geri bildirimi — MarkCoordsCopied() ayarlar; 0 = pasif
    ULONGLONG   m_mapCopiedAt = 0;

    // Info paneli içerik yüksekliği — DrawInfoPanel her çizimde hesaplar
    float       m_infoPanelContentH = 0.0f;

    // Edit toolbar (döndür + resize butonları) — her Render'da güncellenir
    D2D1_RECT_F m_editBtnRotLRect    = {};
    D2D1_RECT_F m_editBtnRotRRect    = {};
    D2D1_RECT_F m_editBtnMoreRect    = {};
    D2D1_RECT_F m_editBtnResizeRect  = {};
    D2D1_RECT_F m_editBtnCropRect    = {};
    bool        m_editToolbarVisible = false;

    // Delete butonu — her Render'da güncellenir (info butonunun solunda)
    D2D1_RECT_F m_deleteBtnRect    = {};
    bool        m_deleteBtnVisible = false;

    // Resize dialog rect'leri — DrawResizeDialog her çizimde günceller
    D2D1_RECT_F m_resizeDlgCancelRect   = {};
    D2D1_RECT_F m_resizeDlgApplyRect    = {};
    D2D1_RECT_F m_resizeDlgModePxRect   = {};
    D2D1_RECT_F m_resizeDlgModePctRect  = {};
    D2D1_RECT_F m_resizeDlgWDecRect     = {};
    D2D1_RECT_F m_resizeDlgWIncRect     = {};
    D2D1_RECT_F m_resizeDlgHDecRect     = {};
    D2D1_RECT_F m_resizeDlgHIncRect     = {};
    D2D1_RECT_F m_resizeDlgLockRect     = {};
    bool        m_resizeDlgVisible      = false;

    // Serbest döndürme dialog rect'leri — DrawRotateFreeDialog her çizimde günceller
    D2D1_RECT_F m_rotFreeDlgCoarseDecRect = {};  // −1°
    D2D1_RECT_F m_rotFreeDlgFineDecRect   = {};  // −0.1°
    D2D1_RECT_F m_rotFreeDlgFineIncRect   = {};  // +0.1°
    D2D1_RECT_F m_rotFreeDlgCoarseIncRect = {};  // +1°
    D2D1_RECT_F m_rotFreeDlgResetRect     = {};
    D2D1_RECT_F m_rotFreeDlgCancelRect    = {};
    D2D1_RECT_F m_rotFreeDlgApplyRect     = {};
    D2D1_RECT_F m_rotFreeDlgSliderRect    = {};  // slider rail alanı
    D2D1_RECT_F m_editBtnRotFreeRect      = {};  // toolbar: serbest döndür butonu
    bool        m_rotFreeDlgVisible       = false;

    // Save bar (kayıt seçenekleri) — her Render'da güncellenir
    D2D1_RECT_F m_saveBarSaveRect    = {};
    D2D1_RECT_F m_saveBarDiscardRect = {};
    D2D1_RECT_F m_saveBarSaveAsRect  = {};
    bool        m_saveBarVisible     = false;

    // Kırpma dialog rect'leri — DrawCropDialog her çizimde günceller
    D2D1_RECT_F m_cropDlgCancelRect     = {};
    D2D1_RECT_F m_cropDlgApplyRect      = {};
    D2D1_RECT_F m_cropDlgRatioRects[5]  = {};  // Serbest | 1:1 | 4:3 | 3:2 | 16:9
    bool        m_cropDlgVisible        = false;

    // Son render'daki görüntü ekran rect'i — kırpma koordinat dönüşümü için
    D2D1_RECT_F m_imageDisplayRect = {};

    // Dialog ve save bar fade için paylaşılan D2D1 layer
    ID2D1Layer*   m_dialogLayer     = nullptr;

    // Silme onay dialogu buton rect'leri — her Render'da güncellenir
    D2D1_RECT_F m_dlgCancelRect = {};
    D2D1_RECT_F m_dlgDeleteRect = {};
    bool        m_dlgVisible    = false;

public:
    // Kaydırma sınırlaması için maksimum scroll hesabında kullanılır
    float GetInfoPanelContentHeight() const { return m_infoPanelContentH; }
};
