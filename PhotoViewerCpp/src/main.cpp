#include <windows.h>
#include <windowsx.h>   // GET_X_LPARAM, GET_Y_LPARAM
#include <shellapi.h>   // CommandLineToArgvW
#include <string>
#include <cmath>        // fabsf
#include "Renderer.h"

// Global renderer — pencere mesajlarından erişilebilmesi için
// (Sonraki phase'lerde bu App sınıfına taşınacak)
static Renderer*  g_renderer  = nullptr;
static ViewState  g_viewState;

// Pan için fare sürükleme durumu
static bool  g_dragging   = false;
static float g_dragStartX = 0.0f;  // Sürükleme başladığında farenin penceredeki konumu
static float g_dragStartY = 0.0f;
static float g_panAtDragStartX = 0.0f;  // Sürükleme başladığında g_viewState.panX
static float g_panAtDragStartY = 0.0f;

// WndProc: Win32 pencere mesaj işleyicisi
// Her klavye/fare/sistem olayı buraya gelir
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
        // Pencere oluşturuldu — renderer'ı başlat
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
        // Pencere boyutlandı — render target'ı güncelle
        UINT width  = LOWORD(lParam);
        UINT height = HIWORD(lParam);
        if (g_renderer) g_renderer->Resize(width, height);
        return 0;
    }

    case WM_MOUSEWHEEL:
    {
        // Fare tekerleği → zoom ±10%, cursor pozisyonuna sabitlenmiş
        // WHEEL_DELTA = 120 (bir çentik); normalize edip her çentik %10 zoom
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);

        // Cursor'ın pencere koordinatlarını al
        POINT cursor;
        cursor.x = GET_X_LPARAM(lParam);
        cursor.y = GET_Y_LPARAM(lParam);
        ScreenToClient(hwnd, &cursor);
        float cx = static_cast<float>(cursor.x);
        float cy = static_cast<float>(cursor.y);

        float oldZoom = g_viewState.zoomFactor;
        float newZoom = oldZoom * (delta > 0 ? 1.1f : (1.0f / 1.1f));

        // %10–%1000 aralığına sıkıştır; %100'e yakın snap (±2%)
        constexpr float kMinZoom  = 0.1f;
        constexpr float kMaxZoom  = 10.0f;
        constexpr float kSnapZoom = 1.0f;
        constexpr float kSnapTol  = 0.02f;
        newZoom = max(kMinZoom, min(kMaxZoom, newZoom));
        if (fabsf(newZoom - kSnapZoom) < kSnapTol) newZoom = kSnapZoom;

        // Zoom anchor: cursor altındaki görüntü noktası sabit kalmalı.
        // Pan ofseti, zoom farkını telafi edecek şekilde güncellenir.
        // Formül: panNew = cursor - (cursor - panOld) * (newZoom / oldZoom)
        // Zoom anchor: cursor altındaki görüntü noktası sabit kalmalı.
        // Görüntü pencere merkezine hizalı olduğundan pan offseti merkeze göre hesaplanır.
        // Formül: panNew = (cx - halfW) - (cx - halfW - panOld) * ratio
        RECT rc;
        GetClientRect(hwnd, &rc);
        float halfW = (rc.right  - rc.left) * 0.5f;
        float halfH = (rc.bottom - rc.top)  * 0.5f;

        float ratio = newZoom / oldZoom;
        g_viewState.panX = (cx - halfW) - (cx - halfW - g_viewState.panX) * ratio;
        g_viewState.panY = (cy - halfH) - (cy - halfH - g_viewState.panY) * ratio;
        g_viewState.zoomFactor = newZoom;

        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_LBUTTONDOWN:
    {
        // Sürükleme başlat
        SetCapture(hwnd);  // Fare pencere dışına çıksa da mesajları almaya devam et
        g_dragging          = true;
        g_dragStartX        = static_cast<float>(GET_X_LPARAM(lParam));
        g_dragStartY        = static_cast<float>(GET_Y_LPARAM(lParam));
        g_panAtDragStartX   = g_viewState.panX;
        g_panAtDragStartY   = g_viewState.panY;
        return 0;
    }

    case WM_MOUSEMOVE:
    {
        if (!g_dragging) return 0;
        float mx = static_cast<float>(GET_X_LPARAM(lParam));
        float my = static_cast<float>(GET_Y_LPARAM(lParam));
        g_viewState.panX = g_panAtDragStartX + (mx - g_dragStartX);
        g_viewState.panY = g_panAtDragStartY + (my - g_dragStartY);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_LBUTTONUP:
        // Sürükleme bitti
        g_dragging = false;
        ReleaseCapture();
        return 0;

    case WM_LBUTTONDBLCLK:
    {
        // Çift tıklama: zoomFactor=1 (fit) ise 2.5x, değilse fit'e dön
        if (g_viewState.zoomFactor == 1.0f)
        {
            // 2.5x zoom, tıklama noktasına sabitle (scroll zoom ile aynı formül)
            float cx = static_cast<float>(GET_X_LPARAM(lParam));
            float cy = static_cast<float>(GET_Y_LPARAM(lParam));
            RECT rc;
            GetClientRect(hwnd, &rc);
            float halfW = (rc.right  - rc.left) * 0.5f;
            float halfH = (rc.bottom - rc.top)  * 0.5f;
            float ratio = 2.5f;  // zoomFactor başlangıçta 1.0 olduğundan ratio = 2.5/1.0
            g_viewState.panX = (cx - halfW) - (cx - halfW - g_viewState.panX) * ratio;
            g_viewState.panY = (cy - halfH) - (cy - halfH - g_viewState.panY) * ratio;
            g_viewState.zoomFactor = 2.5f;
        }
        else
        {
            // Fit'e sıfırla (pan da sıfırlanır)
            g_viewState = ViewState{};
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_ERASEBKGND:
        // Direct2D her frame'de tüm arka planı çiziyor.
        // GDI'ın arka planı silmesine izin verme — zoom/pan sırasında flicker önler.
        return 1;

    case WM_KEYDOWN:
        // Phase 3'te: VK_LEFT / VK_RIGHT navigasyon buraya eklenecek
        if (wParam == VK_ESCAPE)
            DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        delete g_renderer;
        g_renderer = nullptr;
        PostQuitMessage(0); // Mesaj döngüsünü sonlandır
        return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(
    _In_     HINSTANCE hInstance,
    _In_opt_ HINSTANCE,   // hPrevInstance — artık kullanılmıyor
    _In_     LPSTR,       // lpCmdLine — ANSI, Unicode için GetCommandLineW kullanıyoruz
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
    LocalFree(argv);  // CommandLineToArgvW heap allocate eder, biz serbest bırakıyoruz

    // Pencere sınıfını kaydet
    // CS_DBLCLKS: çift tıklama mesajlarını etkinleştirir (Phase 2: zoom reset)
    // CS_HREDRAW | CS_VREDRAW: pencere boyutlandığında yeniden çiz
    WNDCLASSEX wc    = {};
    wc.cbSize        = sizeof(WNDCLASSEX);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr; // Direct2D çiziyor, GDI arka planı gereksiz
    wc.lpszClassName = L"PhotoViewerWindow";
    RegisterClassEx(&wc);

    // Pencere başlığını belirle: dosya adı varsa "dosya.jpg — PhotoViewer"
    std::wstring title = L"PhotoViewer";
    if (!filePath.empty())
    {
        // Yol ayırıcıdan sonraki kısım = dosya adı (\ veya / her ikisini destekle)
        auto pos = filePath.find_last_of(L"\\/");
        std::wstring fileName = (pos != std::wstring::npos) ? filePath.substr(pos + 1) : filePath;
        title = fileName + L" \u2014 PhotoViewer";  // \u2014 = em dash (—)
    }

    // Pencereyi oluştur (WM_CREATE tetiklenir → g_renderer hazır olur)
    HWND hwnd = CreateWindowEx(
        0,                      // Genişletilmiş stil yok
        L"PhotoViewerWindow",   // Pencere sınıfı adı (yukarıda kayıtlı)
        title.c_str(),          // Başlık: "dosya.jpg — PhotoViewer" veya "PhotoViewer"
        WS_OVERLAPPEDWINDOW,    // Standart başlık + min/max/kapat butonları
        CW_USEDEFAULT, CW_USEDEFAULT, // Başlangıç konumu (OS seçer)
        1280, 800,              // Başlangıç boyutu
        nullptr,                // Üst pencere yok
        nullptr,                // Menü yok
        hInstance,
        nullptr                 // WM_CREATE'e geçirilecek ek veri yok
    );

    if (!hwnd)
    {
        CoUninitialize();
        return -1;
    }

    // Dosya yolunu yükle — CreateWindowEx içinde WM_CREATE tetiklendiğinden
    // g_renderer bu noktada hazır
    if (!filePath.empty() && g_renderer)
        g_renderer->LoadImage(filePath);

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd); // İlk WM_PAINT'i hemen gönder

    // Mesaj döngüsü: uygulama kapatılana kadar çalışır
    MSG msg = {};
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg); // Klavye mesajlarını WM_CHAR'a çevirir
        DispatchMessage(&msg);  // WndProc'a yönlendirir
    }

    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
