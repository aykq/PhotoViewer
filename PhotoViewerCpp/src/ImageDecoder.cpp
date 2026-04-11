#include "ImageDecoder.h"

#include <wincodec.h>
#include <winhttp.h>
#include <cwchar>
#include <cstring>  // memcpy
#include <cmath>    // std::pow, std::floor, std::tan, std::cos, std::log

#pragma comment(lib, "winhttp.lib")

// vcpkg bundled decoders
#include <webp/decode.h>
#include <webp/demux.h>   // WebPAnimDecoder (animated WebP)
#include <libheif/heif.h>
#include <jxl/decode.h>
#include <jxl/codestream_header.h>
#include <avif/avif.h>

#pragma comment(lib, "libwebpdemux.lib")

#pragma warning(push)
#pragma warning(disable: 5033)  // lcms2: 'register' C++17'de kaldırıldı
#include <lcms2.h>
#pragma warning(pop)
#pragma comment(lib, "lcms2.lib")

// ─── Dosya okuma yardımcısı ───────────────────────────────────────────────────

static std::vector<uint8_t> ReadFileBytes(const std::wstring& path)
{
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return {};

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(hFile, &fileSize) ||
        fileSize.QuadPart <= 0 ||
        fileSize.QuadPart > 512LL * 1024 * 1024)  // 512 MB limit
    {
        CloseHandle(hFile);
        return {};
    }

    std::vector<uint8_t> data(static_cast<size_t>(fileSize.QuadPart));
    DWORD bytesRead = 0;
    bool ok = ReadFile(hFile, data.data(), static_cast<DWORD>(data.size()), &bytesRead, nullptr);
    CloseHandle(hFile);

    if (!ok || bytesRead != static_cast<DWORD>(data.size())) return {};
    return data;
}

// ─── Piksel dönüşüm yardımcıları ─────────────────────────────────────────────

// BGRA düz alfa → BGRA pre-multiplied (Direct2D gereksinimi)
static void PremultiplyBGRA(uint8_t* pixels, UINT width, UINT height)
{
    const UINT count = width * height;
    for (UINT i = 0; i < count; i++)
    {
        uint8_t* p = pixels + i * 4;
        const uint32_t a = p[3];
        if (a < 255)
        {
            p[0] = static_cast<uint8_t>((p[0] * a + 127) / 255);
            p[1] = static_cast<uint8_t>((p[1] * a + 127) / 255);
            p[2] = static_cast<uint8_t>((p[2] * a + 127) / 255);
        }
    }
}

// RGBA ↔ BGRA: R ve B kanallarını değiştir
static void SwapRB(uint8_t* pixels, UINT width, UINT height)
{
    const UINT count = width * height;
    for (UINT i = 0; i < count; i++)
    {
        uint8_t* p = pixels + i * 4;
        const uint8_t tmp = p[0];
        p[0] = p[2];
        p[2] = tmp;
    }
}

// ─── WIC decoder: JPEG, PNG, BMP, GIF, TIFF, ICO ─────────────────────────────

static std::wstring WicQueryStr(IWICMetadataQueryReader* r, const wchar_t* q)
{
    PROPVARIANT pv;
    PropVariantInit(&pv);
    std::wstring result;

    if (SUCCEEDED(r->GetMetadataByName(q, &pv)))
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
    while (!result.empty() && (result.back() == L'\0' || result.back() == L' '))
        result.pop_back();
    return result;
}

// qPrimary denenir; boşsa qFallback — PNG/TIFF path farklılıkları için
static std::wstring WicQueryStr2(IWICMetadataQueryReader* r,
                                  const wchar_t* q1, const wchar_t* q2)
{
    auto v = WicQueryStr(r, q1);
    return v.empty() ? WicQueryStr(r, q2) : v;
}

static std::wstring WicQueryRational(IWICMetadataQueryReader* r,
                                     const wchar_t* q, const wchar_t* mode)
{
    PROPVARIANT pv;
    PropVariantInit(&pv);
    std::wstring result;

    if (SUCCEEDED(r->GetMetadataByName(q, &pv)))
    {
        ULONG num = 0, den = 1;
        bool valid = false;

        if (pv.vt == VT_UI8)
        {
            num   = pv.uhVal.LowPart;
            den   = pv.uhVal.HighPart;
            valid = (den > 0);
        }
        else if (pv.vt == VT_R8 && pv.dblVal > 0.0)
        {
            wchar_t buf[32];
            if (wcscmp(mode, L"aperture") == 0)
                swprintf_s(buf, L"f/%.1f", pv.dblVal);
            else
            {
                ULONG d = static_cast<ULONG>(1.0 / pv.dblVal + 0.5);
                if (d > 1) swprintf_s(buf, L"1/%lus", d);
                else       swprintf_s(buf, L"%.1fs",  pv.dblVal);
            }
            result = buf;
        }

        if (valid)
        {
            wchar_t buf[32];
            if (wcscmp(mode, L"aperture") == 0)
            {
                swprintf_s(buf, L"f/%.1f", static_cast<double>(num) / den);
            }
            else
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

static std::wstring WicQueryRational2(IWICMetadataQueryReader* r,
                                       const wchar_t* q1, const wchar_t* q2,
                                       const wchar_t* mode)
{
    auto v = WicQueryRational(r, q1, mode);
    return v.empty() ? WicQueryRational(r, q2, mode) : v;
}

// GPS koordinat rational dizisini DMS formatında oku; ondalık dereceyi de döndürür.
// Başarılıysa true, bulunamazsa false döner.
// decimal: N/E = pozitif, S/W = negatif
static bool WicReadGPSCoord(IWICMetadataQueryReader* r,
                             const wchar_t* qRef, const wchar_t* qCoord,
                             std::wstring& display, double& decimal)
{
    std::wstring ref = WicQueryStr(r, qRef);
    if (ref.empty()) return false;

    PROPVARIANT pv;
    PropVariantInit(&pv);
    bool result = false;

    if (SUCCEEDED(r->GetMetadataByName(qCoord, &pv)))
    {
        if ((pv.vt == (VT_VECTOR | VT_UI8)) && pv.cauh.cElems >= 3)
        {
            auto rat = [](ULARGE_INTEGER u) -> double {
                return u.HighPart > 0 ? static_cast<double>(u.LowPart) / u.HighPart : 0.0;
            };
            double deg = rat(pv.cauh.pElems[0]);
            double min = rat(pv.cauh.pElems[1]);
            double sec = rat(pv.cauh.pElems[2]);

            wchar_t buf[64];
            swprintf_s(buf, L"%.0f\u00b0%02.0f\u2032%05.2f\u2033%ls",
                       deg, min, sec, ref.c_str());
            display = buf;

            double absVal = deg + min / 60.0 + sec / 3600.0;
            wchar_t refCh = ref.empty() ? L'?' : ref[0];
            decimal = (refCh == L'S' || refCh == L'W') ? -absVal : absVal;
            result = true;
        }
    }
    PropVariantClear(&pv);
    return result;
}

// GPS irtifa oku (ör. "123.4 m")
static std::wstring WicQueryGPSAltitude(IWICMetadataQueryReader* r,
                                         const wchar_t* qRef, const wchar_t* qAlt)
{
    PROPVARIANT pvRef;
    PropVariantInit(&pvRef);
    bool belowSea = false;
    if (SUCCEEDED(r->GetMetadataByName(qRef, &pvRef)))
    {
        if (pvRef.vt == VT_UI1) belowSea = (pvRef.bVal == 1);
        PropVariantClear(&pvRef);
    }

    PROPVARIANT pv;
    PropVariantInit(&pv);
    std::wstring result;

    if (SUCCEEDED(r->GetMetadataByName(qAlt, &pv)) &&
        pv.vt == VT_UI8 && pv.uhVal.HighPart > 0)
    {
        double alt = static_cast<double>(pv.uhVal.LowPart) / pv.uhVal.HighPart;
        wchar_t buf[32];
        swprintf_s(buf, belowSea ? L"%.1f m (below)" : L"%.1f m", alt);
        result = buf;
    }
    PropVariantClear(&pv);
    return result;
}

// ─── ICC renk profili → sRGB dönüşümü (lcms2) ────────────────────────────────
// Piksel tamponu BGRA düz alfa (premultiply öncesi) olmalıdır.
// Profil adını (Description) döner; dönüşüm uygulanamadıysa sadece adı döner.
static std::wstring ApplyIccProfile(uint8_t* pixels, UINT w, UINT h,
                                     const uint8_t* iccData, size_t iccSize)
{
    if (!iccData || iccSize == 0 || !pixels) return {};

    cmsHPROFILE src = cmsOpenProfileFromMem(iccData, static_cast<cmsUInt32Number>(iccSize));
    if (!src) return {};

    // Profil açıklama adını oku (önce tr-TR, sonra en-US)
    wchar_t descBuf[256] = {};
    if (cmsGetProfileInfo(src, cmsInfoDescription, "tr", "TR", descBuf, 256) == 0)
        cmsGetProfileInfo(src, cmsInfoDescription, "en", "US", descBuf, 256);
    std::wstring profileName = descBuf;
    // Sonundaki boşlukları temizle
    while (!profileName.empty() && (profileName.back() == L' ' || profileName.back() == L'\0'))
        profileName.pop_back();

    cmsHPROFILE dst = cmsCreate_sRGBProfile();
    if (!dst) { cmsCloseProfile(src); return profileName; }

    // TYPE_BGRA_8: 8bpp BGRA (alpha extra channel, renk kanalları B-G-R)
    cmsHTRANSFORM xform = cmsCreateTransform(
        src, TYPE_BGRA_8,
        dst, TYPE_BGRA_8,
        INTENT_PERCEPTUAL, 0);

    cmsCloseProfile(src);
    cmsCloseProfile(dst);

    if (!xform) return profileName;

    cmsDoTransform(xform, pixels, pixels, static_cast<cmsUInt32Number>(w) * h);
    cmsDeleteTransform(xform);

    return profileName;
}

// ─── Ham EXIF (TIFF-IFD) ayrıştırıcı ─────────────────────────────────────────
// HEIC ve AVIF dosyalarından elde edilen ham EXIF byte dizisini ayrıştırır.
// Baş kısmındaki "Exif\0\0" ön eki varsa atlanır.
static void ParseRawExif(const uint8_t* data, size_t size, DecodeOutput& out)
{
    // "Exif\0\0" ön ekini atla
    if (size >= 6 && memcmp(data, "Exif\0\0", 6) == 0)
    {
        data += 6;
        size -= 6;
    }
    if (size < 8) return;

    // Byte sırası: II = little-endian, MM = big-endian
    const bool le = (data[0] == 'I' && data[1] == 'I');
    if (!le && !(data[0] == 'M' && data[1] == 'M')) return;

    auto r16 = [&](size_t o) -> uint16_t {
        if (o + 2 > size) return 0;
        return le ? static_cast<uint16_t>(uint32_t(data[o]) | uint32_t(data[o+1]) << 8)
                  : static_cast<uint16_t>(uint32_t(data[o]) << 8 | uint32_t(data[o+1]));
    };
    auto r32 = [&](size_t o) -> uint32_t {
        if (o + 4 > size) return 0;
        return le ? (uint32_t(data[o]) | uint32_t(data[o+1])<<8 | uint32_t(data[o+2])<<16 | uint32_t(data[o+3])<<24)
                  : (uint32_t(data[o])<<24 | uint32_t(data[o+1])<<16 | uint32_t(data[o+2])<<8 | uint32_t(data[o+3]));
    };

    if (r16(2) != 42) return;  // TIFF magic
    uint32_t ifd0 = r32(4);

    // TIFF veri tipi → byte boyutu (indis = tip numarası)
    static const uint8_t kTypeSz[13] = {0,1,1,2,4,8,1,1,2,4,8,4,8};

    // IFD'yi ayrıştır; her kayıt için cb(tag, type, count, resolvedDataOffset) çağrılır.
    // resolvedDataOffset: değer <= 4 byte ise inline konumu, aksi hâlde işaret ettiği konum.
    auto parseIFD = [&](uint32_t ifdOff, auto cb)
    {
        if (ifdOff + 2 > size) return;
        uint16_t n = r16(ifdOff);
        for (uint16_t i = 0; i < n; ++i)
        {
            uint32_t e = ifdOff + 2 + i * 12;
            if (e + 12 > size) break;
            uint16_t tag  = r16(e);
            uint16_t type = r16(e + 2);
            uint32_t cnt  = r32(e + 4);
            uint32_t bsz  = (type < 13) ? uint32_t(kTypeSz[type]) * cnt : cnt;
            uint32_t doff = (bsz <= 4) ? (e + 8) : r32(e + 8);
            if (doff + bsz > uint32_t(size)) continue;
            cb(tag, type, cnt, doff);
        }
    };

    // ASCII string okuyucu
    auto readAscii = [&](uint32_t off, uint32_t cnt) -> std::wstring {
        std::wstring s;
        for (uint32_t i = 0; i < cnt && off + i < size; ++i)
        {
            char c = static_cast<char>(data[off + i]);
            if (!c) break;
            s += static_cast<wchar_t>(static_cast<unsigned char>(c));
        }
        while (!s.empty() && s.back() == L' ') s.pop_back();
        return s;
    };

    // RATIONAL okuyucu (unsigned)
    auto readRat = [&](uint32_t off) -> double {
        uint32_t num = r32(off), den = r32(off + 4);
        return den > 0 ? double(num) / den : 0.0;
    };

    // GPS DMS → görüntü metni
    auto gpsDMS = [&](double absVal, wchar_t ref) -> std::wstring {
        int    deg  = int(absVal);
        double minD = (absVal - deg) * 60.0;
        int    minI = int(minD);
        double sec  = (minD - minI) * 60.0;
        wchar_t buf[64];
        swprintf_s(buf, L"%d\u00b0%02d\u2032%05.2f\u2033%c", deg, minI, sec, ref);
        return buf;
    };

    uint32_t exifIFDOff = 0, gpsIFDOff = 0;

    // ── IFD0 ─────────────────────────────────────────────────────────────────
    parseIFD(ifd0, [&](uint16_t tag, uint16_t type, uint32_t cnt, uint32_t off)
    {
        switch (tag)
        {
        case 0x010F:  // Make
            if (type == 2 && out.cameraMake.empty())
                out.cameraMake = readAscii(off, cnt);
            break;
        case 0x0110:  // Model
            if (type == 2 && out.cameraModel.empty())
                out.cameraModel = readAscii(off, cnt);
            break;
        case 0x0132:  // DateTime (modification) — DateTimeOriginal ile üzerine yazılacak
            if (type == 2 && out.dateTaken.empty())
                out.dateTaken = readAscii(off, cnt);
            break;
        case 0x8769:  // ExifIFD offset
            if (type == 4) exifIFDOff = r32(off);
            break;
        case 0x8825:  // GPS IFD offset
            if (type == 4) gpsIFDOff = r32(off);
            break;
        }
    });

    // ── ExifIFD ───────────────────────────────────────────────────────────────
    if (exifIFDOff > 0)
    {
        parseIFD(exifIFDOff, [&](uint16_t tag, uint16_t type, uint32_t cnt, uint32_t off)
        {
            switch (tag)
            {
            case 0x9003:  // DateTimeOriginal
                if (type == 2)
                    out.dateTaken = readAscii(off, cnt);
                break;
            case 0x829D:  // FNumber (diyafram)
                if (type == 5 && out.aperture.empty())
                {
                    double v = readRat(off);
                    if (v > 0.0) {
                        wchar_t buf[32];
                        swprintf_s(buf, L"f/%.1f", v);
                        out.aperture = buf;
                    }
                }
                break;
            case 0x829A:  // ExposureTime (enstantane)
                if (type == 5 && out.shutterSpeed.empty())
                {
                    uint32_t num = r32(off), den = r32(off + 4);
                    if (den > 0) {
                        wchar_t buf[32];
                        if (num > 0 && den > num)
                            swprintf_s(buf, L"1/%us",  den / num);
                        else if (num > 0)
                            swprintf_s(buf, L"%.1fs", double(num) / den);
                        else
                            buf[0] = L'\0';
                        if (buf[0]) out.shutterSpeed = buf;
                    }
                }
                break;
            case 0x8827:  // ISOSpeedRatings
                if (out.iso.empty())
                {
                    wchar_t buf[16];
                    if      (type == 3) { swprintf_s(buf, L"%u",  uint32_t(r16(off)));  out.iso = buf; }
                    else if (type == 4) { swprintf_s(buf, L"%u",  r32(off));             out.iso = buf; }
                }
                break;
            }
        });
    }

    // ── GPS IFD ───────────────────────────────────────────────────────────────
    if (gpsIFDOff > 0)
    {
        char     latRef = 0, lonRef = 0;
        uint32_t latOff = 0, lonOff = 0, latCnt = 0, lonCnt = 0;
        uint8_t  altRef = 0;
        uint32_t altOff = 0;
        bool     hasAlt = false;

        parseIFD(gpsIFDOff, [&](uint16_t tag, uint16_t type, uint32_t cnt, uint32_t off)
        {
            switch (tag)
            {
            case 0x0001:  // GPSLatitudeRef
                if (type == 2 && off < size) latRef = static_cast<char>(data[off]);
                break;
            case 0x0002:  // GPSLatitude
                if (type == 5 && cnt >= 3) { latOff = off; latCnt = cnt; }
                break;
            case 0x0003:  // GPSLongitudeRef
                if (type == 2 && off < size) lonRef = static_cast<char>(data[off]);
                break;
            case 0x0004:  // GPSLongitude
                if (type == 5 && cnt >= 3) { lonOff = off; lonCnt = cnt; }
                break;
            case 0x0005:  // GPSAltitudeRef
                if (type == 1) altRef = data[off];
                break;
            case 0x0006:  // GPSAltitude
                if (type == 5) { altOff = off; hasAlt = true; }
                break;
            }
            (void)cnt;
        });

        if (latRef && latOff > 0)
        {
            double val = readRat(latOff) + readRat(latOff+8)/60.0 + readRat(latOff+16)/3600.0;
            out.gpsLatitude  = gpsDMS(val, static_cast<wchar_t>(latRef));
            out.gpsLatDecimal = (latRef == 'S') ? -val : val;
        }
        if (lonRef && lonOff > 0)
        {
            double val = readRat(lonOff) + readRat(lonOff+8)/60.0 + readRat(lonOff+16)/3600.0;
            out.gpsLongitude = gpsDMS(val, static_cast<wchar_t>(lonRef));
            out.gpsLonDecimal = (lonRef == 'W') ? -val : val;
        }
        if (!out.gpsLatitude.empty() && !out.gpsLongitude.empty())
            out.hasGpsDecimal = true;

        if (hasAlt && altOff > 0)
        {
            double alt = readRat(altOff);
            wchar_t buf[32];
            swprintf_s(buf, (altRef == 1) ? L"%.1f m (below)" : L"%.1f m", alt);
            out.gpsAltitude = buf;
        }
    }
}

// EXIF Orientation (1-8) → WICBitmapTransformOptions
static WICBitmapTransformOptions ExifOrientationToTransform(UINT16 o)
{
    switch (o)
    {
    case 2: return WICBitmapTransformFlipHorizontal;
    case 3: return WICBitmapTransformRotate180;
    case 4: return WICBitmapTransformFlipVertical;
    case 5: return static_cast<WICBitmapTransformOptions>(
                WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal);
    case 6: return WICBitmapTransformRotate90;
    case 7: return static_cast<WICBitmapTransformOptions>(
                WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal);
    case 8: return WICBitmapTransformRotate270;
    default: return WICBitmapTransformRotate0;
    }
}

static bool DecodeWithWIC(const std::wstring& path, DecodeOutput& out)
{
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))))
        return false;

    IWICBitmapDecoder* decoder = nullptr;
    HRESULT hr = wic->CreateDecoderFromFilename(
        path.c_str(), nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr)) { wic->Release(); return false; }

    IWICBitmapFrameDecode* frame = nullptr;
    hr = decoder->GetFrame(0, &frame);
    decoder->Release();
    if (FAILED(hr)) { wic->Release(); return false; }

    // EXIF metadata + yön okuma
    UINT16 orientation = 1;
    IWICMetadataQueryReader* mqr = nullptr;
    if (SUCCEEDED(frame->GetMetadataQueryReader(&mqr)) && mqr)
    {
        // Orientation: JPEG → /app1/ifd/..., TIFF/PNG → /ifd/...
        PROPVARIANT pvO;
        PropVariantInit(&pvO);
        bool gotOri = SUCCEEDED(mqr->GetMetadataByName(L"/app1/ifd/{ushort=274}", &pvO));
        if (!gotOri)
        {
            PropVariantClear(&pvO);
            PropVariantInit(&pvO);
            gotOri = SUCCEEDED(mqr->GetMetadataByName(L"/ifd/{ushort=274}", &pvO));
        }
        if (gotOri)
        {
            if      (pvO.vt == VT_UI2) orientation = pvO.uiVal;
            else if (pvO.vt == VT_UI4) orientation = static_cast<UINT16>(pvO.ulVal);
        }
        PropVariantClear(&pvO);

        // Make/model/date: JPEG = /app1/ifd/..., TIFF = /ifd/... (PNG eXIf de aynı)
        out.cameraMake   = WicQueryStr2(mqr, L"/app1/ifd/{ushort=271}",        L"/ifd/{ushort=271}");
        out.cameraModel  = WicQueryStr2(mqr, L"/app1/ifd/{ushort=272}",        L"/ifd/{ushort=272}");
        out.dateTaken    = WicQueryStr2(mqr, L"/app1/ifd/exif/{ushort=36867}", L"/ifd/exif/{ushort=36867}");
        out.aperture     = WicQueryRational2(mqr, L"/app1/ifd/exif/{ushort=33437}", L"/ifd/exif/{ushort=33437}", L"aperture");
        out.shutterSpeed = WicQueryRational2(mqr, L"/app1/ifd/exif/{ushort=33434}", L"/ifd/exif/{ushort=33434}", L"shutter");

        // ISO
        auto readIso = [&](const wchar_t* q)
        {
            PROPVARIANT pvIso;
            PropVariantInit(&pvIso);
            if (SUCCEEDED(mqr->GetMetadataByName(q, &pvIso)))
            {
                wchar_t buf[16];
                if      (pvIso.vt == VT_UI2) { swprintf_s(buf, L"%u",  static_cast<unsigned>(pvIso.uiVal)); out.iso = buf; }
                else if (pvIso.vt == VT_UI4) { swprintf_s(buf, L"%lu", pvIso.ulVal);                         out.iso = buf; }
                PropVariantClear(&pvIso);
            }
        };
        readIso(L"/app1/ifd/exif/{ushort=34855}");
        if (out.iso.empty()) readIso(L"/ifd/exif/{ushort=34855}");

        // GPS koordinatları — JPEG: /app1/ifd/gps/..., TIFF/PNG: /ifd/gps/...
        // Her iki yol da denenir; ilk başarılı sonuç kullanılır.
        double latDec = 0.0, lonDec = 0.0;
        bool hasLat = WicReadGPSCoord(mqr,
            L"/app1/ifd/gps/{ushort=1}", L"/app1/ifd/gps/{ushort=2}",
            out.gpsLatitude, latDec);
        if (!hasLat)
            hasLat = WicReadGPSCoord(mqr,
                L"/ifd/gps/{ushort=1}", L"/ifd/gps/{ushort=2}",
                out.gpsLatitude, latDec);

        bool hasLon = WicReadGPSCoord(mqr,
            L"/app1/ifd/gps/{ushort=3}", L"/app1/ifd/gps/{ushort=4}",
            out.gpsLongitude, lonDec);
        if (!hasLon)
            hasLon = WicReadGPSCoord(mqr,
                L"/ifd/gps/{ushort=3}", L"/ifd/gps/{ushort=4}",
                out.gpsLongitude, lonDec);

        if (hasLat && hasLon)
        {
            out.hasGpsDecimal = true;
            out.gpsLatDecimal = latDec;
            out.gpsLonDecimal = lonDec;
        }

        out.gpsAltitude = WicQueryGPSAltitude(mqr,
            L"/app1/ifd/gps/{ushort=5}", L"/app1/ifd/gps/{ushort=6}");
        if (out.gpsAltitude.empty())
            out.gpsAltitude = WicQueryGPSAltitude(mqr,
                L"/ifd/gps/{ushort=5}", L"/ifd/gps/{ushort=6}");

        mqr->Release();
    }

    // ICC renk profili çıkar (ICC transform sonradan manuel uygulanacak)
    std::vector<BYTE> wicIccData;
    std::wstring       wicExifColorSpaceName;  // gömülü ICC yoksa EXIF renk alanı adı
    {
        IWICColorContext* colorCtx = nullptr;
        UINT numCtx = 0;
        if (SUCCEEDED(frame->GetColorContexts(1, &colorCtx, &numCtx)) && colorCtx && numCtx > 0)
        {
            WICColorContextType ctxType = WICColorContextUninitialized;
            if (SUCCEEDED(colorCtx->GetType(&ctxType)))
            {
                if (ctxType == WICColorContextProfile)
                {
                    // Gömülü ICC profili: ham baytları al, lcms2 ile sRGB'ye dönüştür
                    UINT profileSize = 0;
                    colorCtx->GetProfileBytes(0, nullptr, &profileSize);
                    if (profileSize > 0)
                    {
                        wicIccData.resize(profileSize);
                        colorCtx->GetProfileBytes(profileSize, wicIccData.data(), &profileSize);
                    }
                }
                else if (ctxType == WICColorContextExifColorSpace)
                {
                    // EXIF ColorSpace tag ile işaretlenmiş; gömülü ICC yok.
                    // Değer 1 = sRGB, 65535 = Uncalibrated (genellikle Adobe RGB)
                    UINT exifCS = 0;
                    if (SUCCEEDED(colorCtx->GetExifColorSpace(&exifCS)))
                    {
                        if (exifCS == 1)
                            wicExifColorSpaceName = L"sRGB";
                        else if (exifCS == 65535)
                            wicExifColorSpaceName = L"Uncalibrated";
                    }
                }
            }
            colorCtx->Release();
        }
    }

    // Format dönüştürücü: 32bppBGRA (düz alfa) — ICC uygulandıktan sonra manuel premultiply
    IWICFormatConverter* converter = nullptr;
    hr = wic->CreateFormatConverter(&converter);
    if (FAILED(hr)) { frame->Release(); wic->Release(); return false; }

    hr = converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
                               WICBitmapDitherTypeNone, nullptr, 0.0f,
                               WICBitmapPaletteTypeMedianCut);
    frame->Release();
    if (FAILED(hr)) { converter->Release(); wic->Release(); return false; }

    // EXIF yönelimini flip/rotator ile uygula
    WICBitmapTransformOptions transform = ExifOrientationToTransform(orientation);
    IWICBitmapSource*     source  = converter;
    IWICBitmapFlipRotator* rotator = nullptr;

    if (transform != WICBitmapTransformRotate0)
    {
        if (SUCCEEDED(wic->CreateBitmapFlipRotator(&rotator)))
        {
            if (SUCCEEDED(rotator->Initialize(converter, transform)))
                source = rotator;
            else
            {
                rotator->Release();
                rotator = nullptr;
            }
        }
    }

    UINT w = 0, h = 0;
    source->GetSize(&w, &h);  // 90/270 dönüşte w ve h yer değiştirir
    out.width  = w;
    out.height = h;

    const UINT stride = w * 4;
    out.pixels.resize(static_cast<size_t>(stride) * h);
    hr = source->CopyPixels(nullptr, stride,
                            static_cast<UINT>(out.pixels.size()),
                            out.pixels.data());
    if (FAILED(hr)) out.pixels.clear();

    if (rotator)  rotator->Release();
    converter->Release();
    wic->Release();

    if (!out.pixels.empty())
    {
        // ICC dönüşümü (varsa) → sRGB'ye çevir, sonra pre-multiply
        if (!wicIccData.empty())
            out.iccProfileName = ApplyIccProfile(
                out.pixels.data(), out.width, out.height,
                wicIccData.data(), wicIccData.size());
        else if (!wicExifColorSpaceName.empty())
            out.iccProfileName = wicExifColorSpaceName;  // EXIF tag'den gelen isim; piksel dönüşümü yok
        PremultiplyBGRA(out.pixels.data(), out.width, out.height);
    }

    return !out.pixels.empty();
}

// ─── WebP decoder (libwebp) ───────────────────────────────────────────────────

static bool DecodeWebP(const std::wstring& path, DecodeOutput& out)
{
    auto data = ReadFileBytes(path);
    if (data.empty()) return false;

    // ICC profili çıkar (ICCP chunk — WebPDemux ile)
    std::vector<uint8_t> webpIccData;
    {
        WebPData rawData = { data.data(), data.size() };
        WebPDemuxer* dmx = WebPDemux(&rawData);
        if (dmx)
        {
            WebPChunkIterator chunkIter;
            if (WebPDemuxGetChunk(dmx, "ICCP", 1, &chunkIter))
            {
                webpIccData.assign(chunkIter.chunk.bytes,
                                   chunkIter.chunk.bytes + chunkIter.chunk.size);
                WebPDemuxReleaseChunkIterator(&chunkIter);
            }
            WebPDemuxDelete(dmx);
        }
    }

    // Frame sayısını hızlıca kontrol et
    WebPData webpData = { data.data(), data.size() };
    WebPAnimDecoderOptions opts;
    WebPAnimDecoderOptionsInit(&opts);
    opts.color_mode = MODE_BGRA;

    WebPAnimDecoder* dec = WebPAnimDecoderNew(&webpData, &opts);
    if (!dec) return false;

    WebPAnimInfo info;
    if (!WebPAnimDecoderGetInfo(dec, &info))
    {
        WebPAnimDecoderDelete(dec);
        return false;
    }

    if (info.frame_count <= 1)
    {
        // Statik WebP: hızlı yol
        WebPAnimDecoderDelete(dec);
        int w = 0, h = 0;
        uint8_t* decoded = WebPDecodeBGRA(data.data(), data.size(), &w, &h);
        if (!decoded) return false;
        out.width  = static_cast<UINT>(w);
        out.height = static_cast<UINT>(h);
        out.pixels.assign(decoded, decoded + static_cast<size_t>(w) * h * 4);
        WebPFree(decoded);
        if (!webpIccData.empty())
            out.iccProfileName = ApplyIccProfile(
                out.pixels.data(), out.width, out.height,
                webpIccData.data(), webpIccData.size());
        PremultiplyBGRA(out.pixels.data(), out.width, out.height);
        return true;
    }

    // Animated WebP: tüm frame'leri decode et
    int prevTimestamp = 0;
    while (WebPAnimDecoderHasMoreFrames(dec))
    {
        uint8_t* buf = nullptr;
        int timestamp = 0;
        if (!WebPAnimDecoderGetNext(dec, &buf, &timestamp)) break;

        AnimFrame frame;
        frame.width      = info.canvas_width;
        frame.height     = info.canvas_height;
        frame.durationMs = timestamp - prevTimestamp;
        if (frame.durationMs <= 0) frame.durationMs = 100;
        prevTimestamp    = timestamp;

        const size_t sz = static_cast<size_t>(info.canvas_width) * info.canvas_height * 4;
        frame.pixels.assign(buf, buf + sz);  // buf BGRA
        if (!webpIccData.empty())
        {
            auto name = ApplyIccProfile(frame.pixels.data(), frame.width, frame.height,
                                        webpIccData.data(), webpIccData.size());
            if (out.iccProfileName.empty()) out.iccProfileName = std::move(name);
        }
        PremultiplyBGRA(frame.pixels.data(), frame.width, frame.height);
        out.frames.push_back(std::move(frame));
    }

    WebPAnimDecoderDelete(dec);

    out.width  = info.canvas_width;
    out.height = info.canvas_height;
    out.format = L"WebP";
    return !out.frames.empty();
}

// ─── HEIF/HEIC decoder (libheif) ─────────────────────────────────────────────

static bool DecodeHEIF(const std::wstring& path, DecodeOutput& out)
{
    auto data = ReadFileBytes(path);
    if (data.empty()) return false;

    heif_context* ctx = heif_context_alloc();

    // Bellek üzerinden oku (Windows yolu dönüşümü gerekmez)
    heif_error err = heif_context_read_from_memory_without_copy(
        ctx, data.data(), data.size(), nullptr);
    if (err.code != heif_error_Ok)
    {
        heif_context_free(ctx);
        return false;
    }

    heif_image_handle* handle = nullptr;
    err = heif_context_get_primary_image_handle(ctx, &handle);
    if (err.code != heif_error_Ok)
    {
        heif_context_free(ctx);
        return false;
    }

    // ICC profili çıkar
    std::vector<uint8_t> heifIccData;
    {
        heif_color_profile_type cpType = heif_image_handle_get_color_profile_type(handle);
        if (cpType == heif_color_profile_type_rICC || cpType == heif_color_profile_type_prof)
        {
            size_t sz = heif_image_handle_get_raw_color_profile_size(handle);
            if (sz > 0)
            {
                heifIccData.resize(sz);
                heif_image_handle_get_raw_color_profile(handle, heifIccData.data());
            }
        }
    }

    // EXIF bloğu varsa parse et
    {
        int nMeta = heif_image_handle_get_number_of_metadata_blocks(handle, "Exif");
        if (nMeta > 0)
        {
            heif_item_id metaId;
            heif_image_handle_get_list_of_metadata_block_IDs(handle, "Exif", &metaId, 1);
            size_t exifSz = heif_image_handle_get_metadata_size(handle, metaId);
            if (exifSz > 0)
            {
                std::vector<uint8_t> exifBuf(exifSz);
                if (heif_image_handle_get_metadata(handle, metaId, exifBuf.data()).code
                    == heif_error_Ok)
                {
                    ParseRawExif(exifBuf.data(), exifSz, out);
                }
            }
        }
    }

    heif_image* img = nullptr;
    // libheif transformasyonları (EXIF yönelimi dahil) otomatik uygular
    err = heif_decode_image(handle, &img,
                            heif_colorspace_RGB, heif_chroma_interleaved_RGBA, nullptr);
    if (err.code != heif_error_Ok)
    {
        heif_image_handle_release(handle);
        heif_context_free(ctx);
        return false;
    }

    int stride = 0;
    const uint8_t* src = heif_image_get_plane_readonly(
        img, heif_channel_interleaved, &stride);

    const int w = heif_image_get_width(img,  heif_channel_interleaved);
    const int h = heif_image_get_height(img, heif_channel_interleaved);
    out.width  = static_cast<UINT>(w);
    out.height = static_cast<UINT>(h);
    out.pixels.resize(static_cast<size_t>(w) * h * 4);

    // Stride farklıysa satır satır kopyala
    for (int row = 0; row < h; row++)
        memcpy(out.pixels.data() + static_cast<size_t>(row) * w * 4,
               src              + static_cast<size_t>(row) * stride,
               static_cast<size_t>(w) * 4);

    heif_image_release(img);
    heif_image_handle_release(handle);
    heif_context_free(ctx);

    // libheif RGBA döner — BGRA'ya çevir, ICC uygula, sonra pre-multiply
    SwapRB(out.pixels.data(), out.width, out.height);
    if (!heifIccData.empty())
        out.iccProfileName = ApplyIccProfile(
            out.pixels.data(), out.width, out.height,
            heifIccData.data(), heifIccData.size());
    PremultiplyBGRA(out.pixels.data(), out.width, out.height);
    return true;
}

// ─── JXL decoder (libjxl) ────────────────────────────────────────────────────

static bool DecodeJXL(const std::wstring& path, DecodeOutput& out)
{
    auto data = ReadFileBytes(path);
    if (data.empty()) return false;

    JxlDecoder* dec = JxlDecoderCreate(nullptr);
    if (!dec) return false;

    JxlDecoderSubscribeEvents(dec,
        JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING | JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE);
    JxlDecoderSetInput(dec, data.data(), data.size());
    JxlDecoderCloseInput(dec);

    JxlBasicInfo   basicInfo{};
    JxlPixelFormat fmt = { 4, JXL_TYPE_UINT8, JXL_LITTLE_ENDIAN, 0 };  // RGBA

    bool isAnimated      = false;
    bool success         = false;
    int  currentDurMs    = 100;
    std::vector<uint8_t> frameBuf;
    std::vector<uint8_t> jxlIccData;

    for (;;)
    {
        JxlDecoderStatus status = JxlDecoderProcessInput(dec);

        if (status == JXL_DEC_BASIC_INFO)
        {
            if (JxlDecoderGetBasicInfo(dec, &basicInfo) != JXL_DEC_SUCCESS) break;
            isAnimated = (basicInfo.have_animation == JXL_TRUE
                          && basicInfo.animation.tps_numerator > 0);
        }
        else if (status == JXL_DEC_COLOR_ENCODING)
        {
            size_t iccSize = 0;
            if (JxlDecoderGetICCProfileSize(dec, JXL_COLOR_PROFILE_TARGET_DATA, &iccSize)
                    == JXL_DEC_SUCCESS && iccSize > 0)
            {
                jxlIccData.resize(iccSize);
                if (JxlDecoderGetColorAsICCProfile(dec, JXL_COLOR_PROFILE_TARGET_DATA,
                        jxlIccData.data(), iccSize) != JXL_DEC_SUCCESS)
                    jxlIccData.clear();
            }
        }
        else if (status == JXL_DEC_FRAME)
        {
            if (isAnimated)
            {
                JxlFrameHeader fh{};
                if (JxlDecoderGetFrameHeader(dec, &fh) == JXL_DEC_SUCCESS && fh.duration > 0)
                {
                    currentDurMs = static_cast<int>(
                        1000.0 * fh.duration
                        * basicInfo.animation.tps_denominator
                        / basicInfo.animation.tps_numerator);
                    if (currentDurMs <= 0) currentDurMs = 100;
                }
                else
                {
                    currentDurMs = 100;
                }
            }
        }
        else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER)
        {
            size_t bufSize = 0;
            if (JxlDecoderImageOutBufferSize(dec, &fmt, &bufSize) != JXL_DEC_SUCCESS) break;
            frameBuf.resize(bufSize);
            if (JxlDecoderSetImageOutBuffer(dec, &fmt, frameBuf.data(), bufSize)
                != JXL_DEC_SUCCESS) break;
        }
        else if (status == JXL_DEC_FULL_IMAGE)
        {
            if (isAnimated)
            {
                AnimFrame frame;
                frame.width      = basicInfo.xsize;
                frame.height     = basicInfo.ysize;
                frame.durationMs = currentDurMs;
                frame.pixels     = frameBuf;  // copy — frameBuf reused for next frame
                SwapRB(frame.pixels.data(), frame.width, frame.height);
                if (!jxlIccData.empty())
                {
                    auto name = ApplyIccProfile(frame.pixels.data(), frame.width, frame.height,
                                                jxlIccData.data(), jxlIccData.size());
                    if (out.iccProfileName.empty()) out.iccProfileName = std::move(name);
                }
                PremultiplyBGRA(frame.pixels.data(), frame.width, frame.height);
                out.frames.push_back(std::move(frame));
            }
            else
            {
                out.width  = basicInfo.xsize;
                out.height = basicInfo.ysize;
                out.pixels = std::move(frameBuf);
                success    = true;
                break;
            }
        }
        else if (status == JXL_DEC_SUCCESS)
        {
            if (isAnimated && !out.frames.empty())
            {
                out.width  = basicInfo.xsize;
                out.height = basicInfo.ysize;
                success    = true;
            }
            break;
        }
        else if (status == JXL_DEC_ERROR)
        {
            break;
        }
    }

    JxlDecoderDestroy(dec);

    if (!success) { out.pixels.clear(); out.frames.clear(); return false; }

    if (!isAnimated)
    {
        // Statik JXL: RGBA → BGRA, ICC uygula, pre-multiply
        SwapRB(out.pixels.data(), out.width, out.height);
        if (!jxlIccData.empty())
            out.iccProfileName = ApplyIccProfile(
                out.pixels.data(), out.width, out.height,
                jxlIccData.data(), jxlIccData.size());
        PremultiplyBGRA(out.pixels.data(), out.width, out.height);
    }
    return true;
}

// ─── AVIF decoder (libavif) ───────────────────────────────────────────────────

// Yardımcı: avifImage BGRA dönüşümü + DecodeOutput veya AnimFrame'e yaz
static bool AvifFrameToPixels(avifDecoder* decoder, std::vector<uint8_t>& outPixels,
                               UINT& outW, UINT& outH)
{
    avifImage* img = decoder->image;
    avifRGBImage rgb{};
    avifRGBImageSetDefaults(&rgb, img);
    rgb.format = AVIF_RGB_FORMAT_BGRA;
    avifRGBImageAllocatePixels(&rgb);

    bool ok = (avifImageYUVToRGB(img, &rgb) == AVIF_RESULT_OK);
    if (ok)
    {
        outW = rgb.width;
        outH = rgb.height;
        outPixels.resize(static_cast<size_t>(outW) * outH * 4);
        for (UINT row = 0; row < outH; row++)
            memcpy(outPixels.data() + static_cast<size_t>(row) * outW * 4,
                   rgb.pixels       + static_cast<size_t>(row) * rgb.rowBytes,
                   static_cast<size_t>(outW) * 4);
    }
    avifRGBImageFreePixels(&rgb);
    return ok;
}

static bool DecodeAVIF(const std::wstring& path, DecodeOutput& out)
{
    auto data = ReadFileBytes(path);
    if (data.empty()) return false;

    avifDecoder* decoder = avifDecoderCreate();
    if (!decoder) return false;

    if (avifDecoderSetIOMemory(decoder, data.data(), data.size()) != AVIF_RESULT_OK
        || avifDecoderParse(decoder) != AVIF_RESULT_OK)
    {
        avifDecoderDestroy(decoder);
        return false;
    }

    // EXIF + ICC: parse'dan sonra decoder->image'da mevcut
    if (decoder->image && decoder->image->exif.size > 0)
        ParseRawExif(decoder->image->exif.data, decoder->image->exif.size, out);

    // ICC profili yakala
    std::vector<uint8_t> avifIccData;
    if (decoder->image && decoder->image->icc.size > 0)
        avifIccData.assign(decoder->image->icc.data,
                           decoder->image->icc.data + decoder->image->icc.size);

    if (decoder->imageCount > 1)
    {
        // Animated AVIF
        while (avifDecoderNextImage(decoder) == AVIF_RESULT_OK)
        {
            AnimFrame frame;
            if (!AvifFrameToPixels(decoder, frame.pixels, frame.width, frame.height))
                continue;

            int durMs = static_cast<int>(decoder->imageTiming.duration * 1000.0);
            if (durMs <= 0) durMs = 100;
            frame.durationMs = durMs;

            if (!avifIccData.empty())
            {
                auto name = ApplyIccProfile(frame.pixels.data(), frame.width, frame.height,
                                            avifIccData.data(), avifIccData.size());
                if (out.iccProfileName.empty()) out.iccProfileName = std::move(name);
            }
            PremultiplyBGRA(frame.pixels.data(), frame.width, frame.height);
            out.frames.push_back(std::move(frame));
        }
        out.width  = decoder->image ? decoder->image->width  : 0;
        out.height = decoder->image ? decoder->image->height : 0;
        out.format = L"AVIF";
        avifDecoderDestroy(decoder);
        return !out.frames.empty();
    }
    else
    {
        // Statik AVIF
        bool ok = (avifDecoderNextImage(decoder) == AVIF_RESULT_OK);
        if (ok) ok = AvifFrameToPixels(decoder, out.pixels, out.width, out.height);
        avifDecoderDestroy(decoder);

        if (!ok) { out.pixels.clear(); return false; }
        if (!avifIccData.empty())
            out.iccProfileName = ApplyIccProfile(
                out.pixels.data(), out.width, out.height,
                avifIccData.data(), avifIccData.size());
        PremultiplyBGRA(out.pixels.data(), out.width, out.height);
        return true;
    }
}

// ─── Nominatim reverse geocoding ─────────────────────────────────────────────
// GPS koordinatlarından konum adını döndürür: "Şehir, İl, Ülke"
// WinHTTP ile nominatim.openstreetmap.org/reverse API'sine istek atar.

std::wstring FetchLocationName(double lat, double lon)
{
    // /reverse?lat=X&lon=Y&format=json  →  JSON cevabından adres bilgisi çıkar
    wchar_t reqPath[128];
    swprintf_s(reqPath, L"/reverse?lat=%.6f&lon=%.6f&format=json", lat, lon);

    HINTERNET hSession = WinHttpOpen(L"PhotoViewer/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return {};

    WinHttpSetTimeouts(hSession, 5000, 5000, 10000, 10000);

    HINTERNET hConn = WinHttpConnect(hSession,
        L"nominatim.openstreetmap.org", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConn) { WinHttpCloseHandle(hSession); return {}; }

    HINTERNET hReq = WinHttpOpenRequest(hConn, L"GET", reqPath,
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hReq)
    {
        WinHttpCloseHandle(hConn);
        WinHttpCloseHandle(hSession);
        return {};
    }

    WinHttpAddRequestHeaders(hReq,
        L"Accept: application/json\r\nAccept-Language: tr,en",
        static_cast<DWORD>(-1L), WINHTTP_ADDREQ_FLAG_ADD);

    bool ok = WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0) != FALSE
           && WinHttpReceiveResponse(hReq, nullptr) != FALSE;

    std::string body;
    if (ok)
    {
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(hReq, &avail) && avail > 0)
        {
            std::vector<char> buf(avail);
            DWORD read = 0;
            if (WinHttpReadData(hReq, buf.data(), avail, &read) && read > 0)
                body.append(buf.data(), read);
        }
    }

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConn);
    WinHttpCloseHandle(hSession);

    if (body.empty()) return {};

    // Basit JSON alan çıkarıcı — "key":"value" formatını arar
    auto extract = [&](const std::string& src, const char* key) -> std::string
    {
        std::string needle = "\"";
        needle += key;
        needle += "\":\"";
        auto p = src.find(needle);
        if (p == std::string::npos) return {};
        p += needle.size();
        auto e = src.find('"', p);
        return e != std::string::npos ? src.substr(p, e - p) : std::string{};
    };

    // "address":{...} bloğunu bul; yoksa tüm body'de ara
    auto addrPos = body.find("\"address\":");
    auto addrEnd = addrPos != std::string::npos ? body.find('}', addrPos) : std::string::npos;
    std::string addr = (addrPos != std::string::npos && addrEnd != std::string::npos)
                       ? body.substr(addrPos, addrEnd - addrPos + 1)
                       : body;

    // Şehir/kasaba/köy → il → ülke
    std::string city = extract(addr, "city");
    if (city.empty()) city = extract(addr, "town");
    if (city.empty()) city = extract(addr, "village");
    if (city.empty()) city = extract(addr, "municipality");
    if (city.empty()) city = extract(addr, "county");
    std::string state   = extract(addr, "state");
    std::string country = extract(addr, "country");

    std::string result;
    auto append = [&](const std::string& s)
    {
        if (s.empty()) return;
        if (!result.empty()) result += ", ";
        result += s;
    };
    append(city);
    append(state);
    append(country);

    if (result.empty()) return {};

    // UTF-8 → wide string
    int len = MultiByteToWideChar(CP_UTF8, 0, result.c_str(), -1, nullptr, 0);
    if (len <= 1) return {};
    std::wstring wresult(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, result.c_str(), -1, wresult.data(), len);
    return wresult;
}

// ─── Ana dispatch ─────────────────────────────────────────────────────────────

bool DecodeImage(const std::wstring& path, DecodeOutput& out)
{
    // Uzantıyı büyük harfe çevirerek belirle
    auto dot = path.rfind(L'.');
    std::wstring ext;
    if (dot != std::wstring::npos)
    {
        ext = path.substr(dot + 1);
        for (auto& c : ext) c = towupper(c);
    }

    // Format etiketi
    out.format = (ext == L"JPG") ? L"JPEG" : ext;

    bool ok;
    if (ext == L"WEBP")
        ok = DecodeWebP(path, out);
    else if (ext == L"HEIC" || ext == L"HEIF")
        ok = DecodeHEIF(path, out);
    else if (ext == L"JXL")
        ok = DecodeJXL(path, out);
    else if (ext == L"AVIF")
        ok = DecodeAVIF(path, out);
    else
        ok = DecodeWithWIC(path, out);  // JPEG, PNG, BMP, GIF, TIFF, ICO

    // GPS koordinatları varsa konum adını çek (Nominatim reverse geocoding)
    if (ok && out.hasGpsDecimal)
        out.gpsLocationName = FetchLocationName(out.gpsLatDecimal, out.gpsLonDecimal);

    return ok;
}
