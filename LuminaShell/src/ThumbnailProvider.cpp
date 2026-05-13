#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>       // SHChangeNotify, SHCNE_*, SHCNF_*
#include <shlwapi.h>
#include <thumbcache.h>   // IThumbnailProvider, WTS_ALPHATYPE, WTSAT_ARGB
#include <new>
#include <string>
#include <vector>

#include "ImageDecoder.h"
#include <olectl.h>   // SELFREG_E_CLASS

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "windowscodecs.lib")

// ─── CLSID: {3C7F5A2D-4B8E-4F5C-A1B2-3C4D5E6F7A8B} ─────────────────────────
static const CLSID CLSID_LuminaThumb =
    { 0x3C7F5A2D, 0x4B8E, 0x4F5C, { 0xA1, 0xB2, 0x3C, 0x4D, 0x5E, 0x6F, 0x7A, 0x8B } };

static const wchar_t k_clsid[]       = L"{3C7F5A2D-4B8E-4F5C-A1B2-3C4D5E6F7A8B}";
static const wchar_t k_desc[]        = L"Lumina Thumbnail Handler";
// IThumbnailProvider shell extension key GUID
static const wchar_t k_thumbExtGuid[] = L"{E357FCCD-A995-4576-B01F-234630154E96}";

static const wchar_t* k_extensions[] =
    { L".heic", L".heif", L".jxl", L".avif", L".webp", nullptr };

// ─── Module refcount + handle ─────────────────────────────────────────────────
static LONG    g_cRef    = 0;
static HMODULE g_hModule = nullptr;

BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_hModule = hMod;
        DisableThreadLibraryCalls(hMod);
    }
    return TRUE;
}

// ─── CThumbnailProvider ──────────────────────────────────────────────────────
class CThumbnailProvider : public IThumbnailProvider,
                           public IInitializeWithFile
{
    LONG         m_cRef = 1;
    std::wstring m_path;

public:
    CThumbnailProvider()  { InterlockedIncrement(&g_cRef); }
    ~CThumbnailProvider() { InterlockedDecrement(&g_cRef); }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IThumbnailProvider)
            *ppv = static_cast<IThumbnailProvider*>(this);
        else if (riid == IID_IInitializeWithFile)
            *ppv = static_cast<IInitializeWithFile*>(this);
        else { *ppv = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_cRef); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG r = InterlockedDecrement(&m_cRef);
        if (r == 0) delete this;
        return r;
    }

    // IInitializeWithFile
    HRESULT STDMETHODCALLTYPE Initialize(LPCWSTR pszFilePath, DWORD) override
    {
        m_path = pszFilePath ? pszFilePath : L"";
        return m_path.empty() ? E_INVALIDARG : S_OK;
    }

    // IThumbnailProvider
    HRESULT STDMETHODCALLTYPE GetThumbnail(UINT cx, HBITMAP* phbmp,
                                            WTS_ALPHATYPE* pdwAlpha) override
    {
        if (!phbmp || !pdwAlpha || m_path.empty()) return E_INVALIDARG;
        *phbmp    = nullptr;
        *pdwAlpha = WTSAT_UNKNOWN;

        // Explorer'ın STA thread'inde COM genellikle zaten başlatılmıştır;
        // yine de başlatılmamış durum için güvenlik önlemi.
        HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

        std::vector<uint8_t> pixels;
        UINT w = 0, h = 0;
        bool ok = DecodeImageForThumbnail(m_path, cx, pixels, w, h);

        if (SUCCEEDED(comHr)) CoUninitialize();

        if (!ok || w == 0 || h == 0) return E_FAIL;

        // pixels: BGRA premultiplied (bizim format) = PARGB32 (Windows format)
        // BITMAPINFO ile CreateDIBSection → 32bpp top-down DIB
        BITMAPINFO bmi        = {};
        auto&      bh         = bmi.bmiHeader;
        bh.biSize             = sizeof(BITMAPINFOHEADER);
        bh.biWidth            = static_cast<LONG>(w);
        bh.biHeight           = -static_cast<LONG>(h);  // negatif = top-down
        bh.biPlanes           = 1;
        bh.biBitCount         = 32;
        bh.biCompression      = BI_RGB;

        void*  pBits = nullptr;
        HBITMAP hbmp = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS,
                                        &pBits, nullptr, 0);
        if (!hbmp || !pBits) return E_OUTOFMEMORY;

        if (pixels.size() < static_cast<size_t>(w) * h * 4) return E_FAIL;
        memcpy(pBits, pixels.data(), static_cast<size_t>(w) * h * 4);

        *phbmp    = hbmp;
        *pdwAlpha = WTSAT_ARGB;  // BGRA premultiplied = PARGB32
        return S_OK;
    }
};

// ─── CClassFactory ────────────────────────────────────────────────────────────
class CClassFactory : public IClassFactory
{
    LONG m_cRef = 1;

public:
    CClassFactory()  { InterlockedIncrement(&g_cRef); }
    ~CClassFactory() { InterlockedDecrement(&g_cRef); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory)
        { *ppv = static_cast<IClassFactory*>(this); AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef()  override { return InterlockedIncrement(&m_cRef); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG r = InterlockedDecrement(&m_cRef);
        if (r == 0) delete this;
        return r;
    }

    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* pUnkOuter,
                                              REFIID riid, void** ppv) override
    {
        if (pUnkOuter) return CLASS_E_NOAGGREGATION;
        auto* p = new(std::nothrow) CThumbnailProvider();
        if (!p) return E_OUTOFMEMORY;
        HRESULT hr = p->QueryInterface(riid, ppv);
        p->Release();
        return hr;
    }

    HRESULT STDMETHODCALLTYPE LockServer(BOOL fLock) override
    {
        if (fLock) InterlockedIncrement(&g_cRef);
        else       InterlockedDecrement(&g_cRef);
        return S_OK;
    }
};

// ─── Registry yardımcısı ──────────────────────────────────────────────────────
static HRESULT RegWriteStr(HKEY hRoot, const wchar_t* path,
                            const wchar_t* name, const wchar_t* value)
{
    HKEY hKey = nullptr;
    LONG r = RegCreateKeyExW(hRoot, path, 0, nullptr, 0,
                              KEY_SET_VALUE, nullptr, &hKey, nullptr);
    if (r != ERROR_SUCCESS) return HRESULT_FROM_WIN32(r);
    r = RegSetValueExW(hKey, name, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(value),
        static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t)));
    RegCloseKey(hKey);
    return HRESULT_FROM_WIN32(r);
}

static HRESULT RegWriteDword(HKEY hRoot, const wchar_t* path,
                              const wchar_t* name, DWORD value)
{
    HKEY hKey = nullptr;
    LONG r = RegCreateKeyExW(hRoot, path, 0, nullptr, 0,
                              KEY_SET_VALUE, nullptr, &hKey, nullptr);
    if (r != ERROR_SUCCESS) return HRESULT_FROM_WIN32(r);
    r = RegSetValueExW(hKey, name, 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&value), sizeof(DWORD));
    RegCloseKey(hKey);
    return HRESULT_FROM_WIN32(r);
}

// ─── DLL Exports ─────────────────────────────────────────────────────────────
extern "C"
{

HRESULT DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (rclsid != CLSID_LuminaThumb) return CLASS_E_CLASSNOTAVAILABLE;
    auto* pF = new(std::nothrow) CClassFactory();
    if (!pF) return E_OUTOFMEMORY;
    HRESULT hr = pF->QueryInterface(riid, ppv);
    pF->Release();
    return hr;
}

HRESULT DllCanUnloadNow()
{
    return g_cRef == 0 ? S_OK : S_FALSE;
}

HRESULT DllRegisterServer()
{
    wchar_t dllPath[32768];
    DWORD pathLen = GetModuleFileNameW(g_hModule, dllPath, 32768);
    if (pathLen == 0 || pathLen >= 32768) return SELFREG_E_CLASS;

    wchar_t clsidBase[128], inproc[160];
    swprintf_s(clsidBase, L"SOFTWARE\\Classes\\CLSID\\%s",                k_clsid);
    swprintf_s(inproc,    L"SOFTWARE\\Classes\\CLSID\\%s\\InprocServer32", k_clsid);

    HKEY hRoot = HKEY_LOCAL_MACHINE;
    if (RegWriteStr(hRoot, clsidBase, nullptr, k_desc) != S_OK)
        return SELFREG_E_CLASS;

    RegWriteStr(hRoot, inproc,    nullptr, dllPath);
    RegWriteStr(hRoot, inproc,    L"ThreadingModel", L"Apartment");
    // Windows 11'de izolasyon modunda IThumbnailProvider→HBITMAP cross-process
    // marshal edilemiyor; handler hiç çağrılmıyor. FIND-008 kabul edilmiş risk.
    RegWriteDword(hRoot, clsidBase, L"DisableProcessIsolation", 1);

    // Shell Extensions Approved (her ikisine de yaz)
    RegWriteStr(HKEY_CURRENT_USER,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved",
        k_clsid, k_desc);
    RegWriteStr(hRoot,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved",
        k_clsid, k_desc);

    // Her uzantı için thumbnail handler + PerceivedType=image (Details view için)
    for (int i = 0; k_extensions[i]; ++i)
    {
        wchar_t extBase[64], extPath[160];
        swprintf_s(extBase, L"SOFTWARE\\Classes\\%s", k_extensions[i]);
        swprintf_s(extPath, L"SOFTWARE\\Classes\\%s\\shellex\\%s",
                   k_extensions[i], k_thumbExtGuid);

        // PerceivedType=image → Explorer, Details view'da da thumbnail kullanır
        RegWriteStr(HKEY_LOCAL_MACHINE, extBase, L"PerceivedType", L"image");
        RegWriteStr(HKEY_CURRENT_USER,  extBase, L"PerceivedType", L"image");

        RegWriteStr(HKEY_CURRENT_USER,  extPath, nullptr, k_clsid);
        RegWriteStr(HKEY_LOCAL_MACHINE, extPath, nullptr, k_clsid);
    }

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

HRESULT DllUnregisterServer()
{
    for (int i = 0; k_extensions[i]; ++i)
    {
        wchar_t extPath[160];
        swprintf_s(extPath, L"SOFTWARE\\Classes\\%s\\shellex\\%s",
                   k_extensions[i], k_thumbExtGuid);
        RegDeleteKeyW(HKEY_CURRENT_USER,  extPath);
        RegDeleteKeyW(HKEY_LOCAL_MACHINE, extPath);
    }

    wchar_t inproc[160], clsidBase[128];
    swprintf_s(inproc,    L"SOFTWARE\\Classes\\CLSID\\%s\\InprocServer32", k_clsid);
    swprintf_s(clsidBase, L"SOFTWARE\\Classes\\CLSID\\%s",                 k_clsid);

    for (HKEY hRoot : { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE })
    {
        HKEY hClsid;
        if (RegOpenKeyExW(hRoot, clsidBase, 0, KEY_SET_VALUE, &hClsid) == ERROR_SUCCESS)
        {
            RegDeleteValueW(hClsid, L"DisableProcessIsolation");
            RegCloseKey(hClsid);
        }
        RegDeleteKeyW(hRoot, inproc);
        RegDeleteKeyW(hRoot, clsidBase);
        HKEY hApproved;
        if (RegOpenKeyExW(hRoot,
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved",
            0, KEY_SET_VALUE, &hApproved) == ERROR_SUCCESS)
        {
            RegDeleteValueW(hApproved, k_clsid);
            RegCloseKey(hApproved);
        }
    }

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

} // extern "C"
