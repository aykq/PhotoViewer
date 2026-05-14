#include "ImageDecoder.h"

#include <wincodec.h>
#include <wincodecsdk.h>   // IWICMetadataBlockReader/Writer, IWICMetadataQueryWriter
#include <cwchar>
#include <cstring>  // memcpy
#include <cmath>    // std::pow, std::floor, std::tan, std::cos, std::log

// vcpkg bundled decoders
#include <webp/decode.h>
#include <webp/demux.h>   // WebPAnimDecoder (animated WebP)
#include <webp/encode.h>  // WebPEncodeRGBA (encode)
#include <webp/mux.h>     // WebPMux (EXIF chunk ekleme)
#include <libheif/heif.h>
#include <jxl/decode.h>
#include <jxl/codestream_header.h>
#include <jxl/thread_parallel_runner.h>
// jxl/encode.h intentionally excluded — JXL encoder objects in jxl.lib require
// __std_rotate/__std_unique_4 (MSVC 17.7+ STL intrinsics) incompatible with v143 toolset.
#include <avif/avif.h>

#pragma comment(lib, "libwebpdemux.lib")
#pragma comment(lib, "libwebpmux.lib")
#pragma comment(lib, "jxl_threads.lib")

#pragma warning(push)
#pragma warning(disable: 5033)  // lcms2: 'register' C++17'de kaldırıldı
#include <lcms2.h>
#pragma warning(pop)
#pragma comment(lib, "lcms2.lib")

// ─── Dosya okuma yardımcısı ───────────────────────────────────────────────────

static std::vector<uint8_t> ReadFileBytes(const std::wstring& path)
{
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
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

// RGBA → BGRA + pre-multiply tek geçişte (SwapRB + Premultiply ayrı iki passtan daha hızlı).
// ICC profili gerektirmeyen HEIC/JXL için kullanılır; cache bandwidth yarıya iner.
static void SwapRBAndPremultiply(uint8_t* pixels, UINT width, UINT height)
{
    const UINT count = width * height;
    for (UINT i = 0; i < count; i++)
    {
        uint8_t* p = pixels + i * 4;
        const uint32_t a = p[3];
        const uint8_t  r = p[0];   // RGBA'da R
        const uint8_t  b = p[2];   // RGBA'da B
        if (a < 255)
        {
            p[0] = static_cast<uint8_t>((b * a + 127) / 255);   // BGRA B = premul(B_src)
            p[1] = static_cast<uint8_t>((p[1] * a + 127) / 255);
            p[2] = static_cast<uint8_t>((r * a + 127) / 255);   // BGRA R = premul(R_src)
        }
        else
        {
            p[0] = b;   // opak: sadece swap
            p[2] = r;
        }
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
            int len = MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, pv.pszVal, -1, nullptr, 0);
            if (len > 1)
            {
                result.resize(len - 1);
                MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, pv.pszVal, -1, result.data(), len - 1);
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
// Baş kısmındaki "Exif\0\0" veya HEIC 4-byte ISO offset ön eki varsa atlanır.
static void ParseRawExif(const uint8_t* data, size_t size, DecodeOutput& out)
{
    // "Exif\0\0" ön ekini atla (JPEG APP1 payload formatı)
    if (size >= 6 && memcmp(data, "Exif\0\0", 6) == 0)
    {
        data += 6;
        size -= 6;
    }
    // HEIC/libheif EXIF item formatı (ISO 14496-12 ExifDataBlock):
    // İlk 4 byte big-endian uint32 = 4-byte alanının hemen ardından TIFF header'a olan offset.
    // Örn. iPhone HEIC: offset=6 → 4 byte + "Exif\0\0" (6 byte) + TIFF verisi.
    else if (size >= 4)
    {
        uint32_t off4 = (uint32_t(data[0]) << 24) | (uint32_t(data[1]) << 16) |
                        (uint32_t(data[2]) << 8)  |  uint32_t(data[3]);
        size_t tiffStart = static_cast<size_t>(4) + off4;
        if (tiffStart + 4 <= size)
        {
            const uint8_t* t = data + tiffStart;
            if ((t[0] == 'I' && t[1] == 'I') || (t[0] == 'M' && t[1] == 'M'))
            {
                data  = t;
                size -= tiffStart;
            }
        }
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

// ─── ICC profil adı yardımcısı ────────────────────────────────────────────────
// Ham ICC baytlarından profil açıklama adını döndürür; piksellere dokunmaz.
static std::wstring IccProfileName(const uint8_t* iccData, size_t iccSize)
{
    if (!iccData || iccSize == 0) return {};
    cmsHPROFILE prof = cmsOpenProfileFromMem(iccData, static_cast<cmsUInt32Number>(iccSize));
    if (!prof) return {};
    wchar_t descBuf[256] = {};
    if (cmsGetProfileInfo(prof, cmsInfoDescription, "tr", "TR", descBuf, 256) == 0)
        cmsGetProfileInfo(prof, cmsInfoDescription, "en", "US", descBuf, 256);
    std::wstring name = descBuf;
    while (!name.empty() && (name.back() == L' ' || name.back() == L'\0'))
        name.pop_back();
    cmsCloseProfile(prof);
    return name;
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

// ─── WIC frame metadata okuyucu ──────────────────────────────────────────────
// EXIF, GPS ve ICC profil bilgilerini frame'den okur; piksellere dokunmaz.
// orientation: EXIF yönelim değeri (1-8); iccDataOut: piksel dönüşümü için ham ICC baytları.
static void WicReadFrameMeta(IWICBitmapFrameDecode* frame, DecodeOutput& out,
                              UINT16& orientation, std::vector<BYTE>& iccDataOut)
{
    orientation = 1;

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

    // ICC renk profili — ham baytları çıkar; profil adını da set et
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
                    UINT profileSize = 0;
                    colorCtx->GetProfileBytes(0, nullptr, &profileSize);
                    if (profileSize > 0)
                    {
                        iccDataOut.resize(profileSize);
                        colorCtx->GetProfileBytes(profileSize, iccDataOut.data(), &profileSize);
                        out.iccProfileName = IccProfileName(iccDataOut.data(), profileSize);
                    }
                }
                else if (ctxType == WICColorContextExifColorSpace)
                {
                    UINT exifCS = 0;
                    if (SUCCEEDED(colorCtx->GetExifColorSpace(&exifCS)))
                    {
                        if (exifCS == 1)       out.iccProfileName = L"sRGB";
                        else if (exifCS == 65535) out.iccProfileName = L"Uncalibrated";
                    }
                }
            }
            colorCtx->Release();
        }
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

    // EXIF metadata + ICC profili
    UINT16 orientation = 1;
    std::vector<BYTE> wicIccData;
    WicReadFrameMeta(frame, out, orientation, wicIccData);

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
        // out.iccProfileName WicReadFrameMeta tarafından zaten set edildi.
        if (!wicIccData.empty())
            ApplyIccProfile(out.pixels.data(), out.width, out.height,
                            wicIccData.data(), wicIccData.size());
        PremultiplyBGRA(out.pixels.data(), out.width, out.height);
    }

    return !out.pixels.empty();
}

// ─── WebP decoder (libwebp) ───────────────────────────────────────────────────

static bool DecodeWebP(const std::wstring& path, DecodeOutput& out)
{
    auto data = ReadFileBytes(path);
    if (data.empty()) return false;

    // ICC + EXIF chunk'larını çıkar (WebPDemux ile)
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
            // EXIF chunk — kameradan gelen WebP fotoğraflarında bulunur (ör. Android/Pixel)
            if (WebPDemuxGetChunk(dmx, "EXIF", 1, &chunkIter))
            {
                ParseRawExif(chunkIter.chunk.bytes, chunkIter.chunk.size, out);
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

// ─── HEIF/HEIC decoder ───────────────────────────────────────────────────────
// WIC-first: Windows HEIF Image Extensions kuruluysa donanım hızlandırmalı
// (GPU) decode kullanır. Çoğu Windows 11 sisteminde yüklü gelir; Windows 10'da
// Microsoft Store'dan ücretsiz kurulabilir. Codec yoksa libheif'e düşülür.

static bool TryDecodeHEIFWithWIC(const std::wstring& path, DecodeOutput& out)
{
    IWICImagingFactory2* pFactory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory2, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFactory))))
        return false;

    // HEIF codec kurulu değilse burada WINCODEC_ERR_COMPONENTNOTFOUND döner
    IWICBitmapDecoder* pDecoder = nullptr;
    HRESULT hr = pFactory->CreateDecoderFromFilename(
        path.c_str(), nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnLoad, &pDecoder);
    if (FAILED(hr)) { pFactory->Release(); return false; }

    IWICBitmapFrameDecode* pFrame = nullptr;
    hr = pDecoder->GetFrame(0, &pFrame);
    if (FAILED(hr)) { pDecoder->Release(); pFactory->Release(); return false; }

    // GUID_WICPixelFormat32bppPBGRA: premultiplied BGRA — mevcut libheif
    // çıktısıyla aynı format. WIC HEIF codec, HEIF container'daki döndürme/
    // çevirme dönüşümlerini otomatik olarak uygular.
    IWICFormatConverter* pConvert = nullptr;
    pFactory->CreateFormatConverter(&pConvert);
    hr = pConvert->Initialize(pFrame, GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone, nullptr, 0.0f, WICBitmapPaletteTypeCustom);
    pFrame->Release();
    if (FAILED(hr))
    {
        pConvert->Release(); pDecoder->Release(); pFactory->Release();
        return false;
    }

    UINT w = 0, h = 0;
    pConvert->GetSize(&w, &h);
    const UINT stride  = w * 4;
    const UINT bufSize = stride * h;
    out.pixels.resize(bufSize);
    hr = pConvert->CopyPixels(nullptr, stride, bufSize, out.pixels.data());

    pConvert->Release();
    pDecoder->Release();
    pFactory->Release();

    if (FAILED(hr)) { out.pixels.clear(); return false; }

    out.width  = w;
    out.height = h;
    out.format = L"HEIC";
    return true;
}

static bool DecodeHEIF(const std::wstring& path, DecodeOutput& out)
{
    // WIC-first: donanım hızlandırmalı decode — başarısızsa libheif'e düş
    if (TryDecodeHEIFWithWIC(path, out))
    {
        // Pikseller WIC'ten geldi; EXIF/GPS alanlarını hızlı metadata
        // extract ile doldur (~10-20ms, piksel decode yok)
        DecodeOutput metaOnly;
        if (ExtractImageMeta(path, metaOnly))
        {
            out.dateTaken    = metaOnly.dateTaken;
            out.cameraMake   = metaOnly.cameraMake;
            out.cameraModel  = metaOnly.cameraModel;
            out.aperture     = metaOnly.aperture;
            out.shutterSpeed = metaOnly.shutterSpeed;
            out.iso          = metaOnly.iso;
            out.gpsLatitude  = metaOnly.gpsLatitude;
            out.gpsLongitude = metaOnly.gpsLongitude;
            out.gpsAltitude  = metaOnly.gpsAltitude;
            out.hasGpsDecimal  = metaOnly.hasGpsDecimal;
            out.gpsLatDecimal  = metaOnly.gpsLatDecimal;
            out.gpsLonDecimal  = metaOnly.gpsLonDecimal;
            out.iccProfileName = metaOnly.iccProfileName;
        }
        return true;
    }

    // Fallback: libheif + libde265 (yazılım H.265 decode)
    auto data = ReadFileBytes(path);
    if (data.empty()) return false;

    heif_context* ctx = heif_context_alloc();

    // Tüm mantıksal işlemci çekirdeklerini decode'a tahsis et
    {
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        int nThreads = static_cast<int>(si.dwNumberOfProcessors);
        if (nThreads < 1) nThreads = 1;
        heif_context_set_max_decoding_threads(ctx, nThreads);
    }

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

    // EXIF bloğu varsa parse et (libheif 4-byte ISO offset header dahil döner; ParseRawExif halleder)
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

    // libheif RGBA döner — BGRA + premultiply
    if (!heifIccData.empty())
    {
        // ICC straight alpha'ya uygulanmalı; önce swap, sonra ICC, sonra premultiply
        SwapRB(out.pixels.data(), out.width, out.height);
        out.iccProfileName = ApplyIccProfile(
            out.pixels.data(), out.width, out.height,
            heifIccData.data(), heifIccData.size());
        PremultiplyBGRA(out.pixels.data(), out.width, out.height);
    }
    else
    {
        // ICC yok: tek geçişte RGBA→BGRA + premultiply (daha az bellek bandwith)
        SwapRBAndPremultiply(out.pixels.data(), out.width, out.height);
    }
    return true;
}

// ─── JXL decoder (libjxl) ────────────────────────────────────────────────────

static bool DecodeJXL(const std::wstring& path, DecodeOutput& out)
{
    auto data = ReadFileBytes(path);
    if (data.empty()) return false;

    // Tüm mantıksal çekirdekleri decode'a tahsis et
    SYSTEM_INFO jxlSi{};
    GetSystemInfo(&jxlSi);
    const size_t nJxlThreads = jxlSi.dwNumberOfProcessors > 0
                                   ? static_cast<size_t>(jxlSi.dwNumberOfProcessors) : 1;
    void* jxlRunner = JxlThreadParallelRunnerCreate(nullptr, nJxlThreads);

    JxlDecoder* dec = JxlDecoderCreate(nullptr);
    if (!dec) { JxlThreadParallelRunnerDestroy(jxlRunner); return false; }

    JxlDecoderSetParallelRunner(dec, JxlThreadParallelRunner, jxlRunner);

    JxlDecoderSubscribeEvents(dec,
        JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING | JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE
        | JXL_DEC_BOX);  // JXL container box'larını oku (EXIF için)
    JxlDecoderSetInput(dec, data.data(), data.size());
    JxlDecoderCloseInput(dec);

    JxlBasicInfo   basicInfo{};
    JxlPixelFormat fmt = { 4, JXL_TYPE_UINT8, JXL_LITTLE_ENDIAN, 0 };  // RGBA

    bool isAnimated      = false;
    bool success         = false;
    int  currentDurMs    = 100;
    std::vector<uint8_t> frameBuf;
    std::vector<uint8_t> jxlIccData;

    // EXIF box okuma — kameradan gelen JXL dosyalarında bulunur
    std::vector<uint8_t> jxlExifBuf;
    bool inExifBox = false;

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
        else if (status == JXL_DEC_BOX)
        {
            // Önceki EXIF box tamamlandı — buffer'ı serbest bırak ve parse et
            if (inExifBox && !jxlExifBuf.empty())
            {
                size_t rem     = JxlDecoderReleaseBoxBuffer(dec);
                size_t written = jxlExifBuf.size() - rem;
                if (written > 0)
                    ParseRawExif(jxlExifBuf.data(), written, out);
                inExifBox = false;
                jxlExifBuf.clear();
            }
            // Yeni box: "Exif" ise buffer hazırla
            JxlBoxType boxType{};
            if (JxlDecoderGetBoxType(dec, boxType, JXL_FALSE) == JXL_DEC_SUCCESS
                && memcmp(boxType, "Exif", 4) == 0)
            {
                jxlExifBuf.resize(256 * 1024);  // 256 KB — kamera EXIF için yeterli
                JxlDecoderSetBoxBuffer(dec, jxlExifBuf.data(), jxlExifBuf.size());
                inExifBox = true;
            }
        }
        else if (status == JXL_DEC_ERROR)
        {
            break;
        }
    }

    // Loop bitmeden tamamlanan son EXIF box (stream aniden biterse)
    if (inExifBox && !jxlExifBuf.empty())
    {
        size_t rem     = JxlDecoderReleaseBoxBuffer(dec);
        size_t written = jxlExifBuf.size() - rem;
        if (written > 0)
            ParseRawExif(jxlExifBuf.data(), written, out);
    }

    JxlDecoderDestroy(dec);
    JxlThreadParallelRunnerDestroy(jxlRunner);

    if (!success) { out.pixels.clear(); out.frames.clear(); return false; }

    if (!isAnimated)
    {
        // Statik JXL: RGBA → BGRA + premultiply
        if (!jxlIccData.empty())
        {
            SwapRB(out.pixels.data(), out.width, out.height);
            out.iccProfileName = ApplyIccProfile(
                out.pixels.data(), out.width, out.height,
                jxlIccData.data(), jxlIccData.size());
            PremultiplyBGRA(out.pixels.data(), out.width, out.height);
        }
        else
        {
            SwapRBAndPremultiply(out.pixels.data(), out.width, out.height);
        }
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

    // Tüm mantıksal çekirdekleri kullan
    {
        SYSTEM_INFO avifSi{};
        GetSystemInfo(&avifSi);
        decoder->maxThreads = static_cast<int>(avifSi.dwNumberOfProcessors);
        if (decoder->maxThreads < 1) decoder->maxThreads = 1;
    }

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

// ─── Format-specific metadata extractor'lar (piksel decode YOK) ──────────────
// Her fonksiyon sadece container/stream'i açıp EXIF+boyut okur; piksel decode etmez.

static bool ExtractMetaWIC(const std::wstring& path, DecodeOutput& out)
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

    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);

    UINT16 orientation = 1;
    std::vector<BYTE> iccData;
    WicReadFrameMeta(frame, out, orientation, iccData);

    // 90/270 dönüşünde w ve h yer değiştirir (tam decode ile aynı sonuç)
    if (orientation == 5 || orientation == 6 || orientation == 7 || orientation == 8)
        std::swap(w, h);
    out.width  = w;
    out.height = h;

    frame->Release();
    wic->Release();
    return true;
}

static bool ExtractMetaWebP(const std::wstring& path, DecodeOutput& out)
{
    auto data = ReadFileBytes(path);
    if (data.empty()) return false;

    WebPData rawData = { data.data(), data.size() };
    WebPDemuxer* dmx = WebPDemux(&rawData);
    if (!dmx) return false;

    out.width  = WebPDemuxGetI(dmx, WEBP_FF_CANVAS_WIDTH);
    out.height = WebPDemuxGetI(dmx, WEBP_FF_CANVAS_HEIGHT);

    WebPChunkIterator chunkIter;
    // ICC profil adı
    if (WebPDemuxGetChunk(dmx, "ICCP", 1, &chunkIter))
    {
        out.iccProfileName = IccProfileName(chunkIter.chunk.bytes, chunkIter.chunk.size);
        WebPDemuxReleaseChunkIterator(&chunkIter);
    }
    // EXIF
    if (WebPDemuxGetChunk(dmx, "EXIF", 1, &chunkIter))
    {
        ParseRawExif(chunkIter.chunk.bytes, chunkIter.chunk.size, out);
        WebPDemuxReleaseChunkIterator(&chunkIter);
    }

    WebPDemuxDelete(dmx);
    return out.width > 0;
}

static bool ExtractMetaHEIF(const std::wstring& path, DecodeOutput& out)
{
    auto data = ReadFileBytes(path);
    if (data.empty()) return false;

    heif_context* ctx = heif_context_alloc();
    heif_error err = heif_context_read_from_memory_without_copy(
        ctx, data.data(), data.size(), nullptr);
    if (err.code != heif_error_Ok) { heif_context_free(ctx); return false; }

    heif_image_handle* handle = nullptr;
    err = heif_context_get_primary_image_handle(ctx, &handle);
    if (err.code != heif_error_Ok) { heif_context_free(ctx); return false; }

    // Boyutlar (piksel decode gerektirmez)
    out.width  = static_cast<UINT>(heif_image_handle_get_width(handle));
    out.height = static_cast<UINT>(heif_image_handle_get_height(handle));

    // ICC profil adı
    heif_color_profile_type cpType = heif_image_handle_get_color_profile_type(handle);
    if (cpType == heif_color_profile_type_rICC || cpType == heif_color_profile_type_prof)
    {
        size_t sz = heif_image_handle_get_raw_color_profile_size(handle);
        if (sz > 0)
        {
            std::vector<uint8_t> icc(sz);
            heif_image_handle_get_raw_color_profile(handle, icc.data());
            out.iccProfileName = IccProfileName(icc.data(), sz);
        }
    }

    // EXIF
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
                ParseRawExif(exifBuf.data(), exifSz, out);
        }
    }

    heif_image_handle_release(handle);
    heif_context_free(ctx);
    return out.width > 0;
}

// ─── HEIC gömülü thumbnail decoder ───────────────────────────────────────────
// HEIC/HEIF container'ındaki embedded thumbnail'i decode eder (~5-20ms).
// iPhone/kamera fotoğraflarında genellikle 512×384 veya daha küçük bir JPEG/HEVC
// thumbnail bulunur. Tam decode (~100-200ms) tamamlanana kadar anlık önizleme sağlar.

bool ExtractHEICEmbeddedPreview(const std::wstring& path,
                                 std::vector<uint8_t>& pixelsOut,
                                 UINT& widthOut, UINT& heightOut)
{
    char pathUtf8[MAX_PATH * 4]{};
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1,
                        pathUtf8, static_cast<int>(sizeof(pathUtf8)), nullptr, nullptr);

    heif_context* ctx = heif_context_alloc();
    heif_error err = heif_context_read_from_file(ctx, pathUtf8, nullptr);
    if (err.code != heif_error_Ok) { heif_context_free(ctx); return false; }

    heif_image_handle* handle = nullptr;
    err = heif_context_get_primary_image_handle(ctx, &handle);
    if (err.code != heif_error_Ok) { heif_context_free(ctx); return false; }

    int nThumbs = heif_image_handle_get_number_of_thumbnails(handle);
    if (nThumbs <= 0)
    {
        heif_image_handle_release(handle);
        heif_context_free(ctx);
        return false;
    }

    heif_item_id thumbId;
    heif_image_handle_get_list_of_thumbnail_IDs(handle, &thumbId, 1);

    heif_image_handle* thumbHandle = nullptr;
    err = heif_image_handle_get_thumbnail(handle, thumbId, &thumbHandle);
    heif_image_handle_release(handle);
    if (err.code != heif_error_Ok) { heif_context_free(ctx); return false; }

    heif_image* img = nullptr;
    // Thumbnail küçük olduğu için threading options gerekmez
    err = heif_decode_image(thumbHandle, &img,
                            heif_colorspace_RGB, heif_chroma_interleaved_RGBA, nullptr);
    heif_image_handle_release(thumbHandle);
    if (err.code != heif_error_Ok) { heif_context_free(ctx); return false; }

    int stride = 0;
    const uint8_t* src = heif_image_get_plane_readonly(
        img, heif_channel_interleaved, &stride);
    const int w = heif_image_get_width(img,  heif_channel_interleaved);
    const int h = heif_image_get_height(img, heif_channel_interleaved);

    widthOut  = static_cast<UINT>(w);
    heightOut = static_cast<UINT>(h);
    pixelsOut.resize(static_cast<size_t>(w) * h * 4);
    for (int row = 0; row < h; row++)
        memcpy(pixelsOut.data() + static_cast<size_t>(row) * w * 4,
               src              + static_cast<size_t>(row) * stride,
               static_cast<size_t>(w) * 4);

    heif_image_release(img);
    heif_context_free(ctx);

    // RGBA → BGRA + premultiply (tek geçiş)
    SwapRBAndPremultiply(pixelsOut.data(), widthOut, heightOut);
    return true;
}

static bool ExtractMetaJXL(const std::wstring& path, DecodeOutput& out)
{
    auto data = ReadFileBytes(path);
    if (data.empty()) return false;

    JxlDecoder* dec = JxlDecoderCreate(nullptr);
    if (!dec) return false;

    // Yalnızca temel bilgi ve EXIF box — piksel olaylarına abone olmaz
    JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO | JXL_DEC_BOX);
    JxlDecoderSetInput(dec, data.data(), data.size());
    JxlDecoderCloseInput(dec);

    std::vector<uint8_t> jxlExifBuf;
    bool inExifBox = false;

    for (;;)
    {
        JxlDecoderStatus status = JxlDecoderProcessInput(dec);

        if (status == JXL_DEC_BASIC_INFO)
        {
            JxlBasicInfo basicInfo{};
            if (JxlDecoderGetBasicInfo(dec, &basicInfo) == JXL_DEC_SUCCESS)
            {
                out.width  = basicInfo.xsize;
                out.height = basicInfo.ysize;
            }
        }
        else if (status == JXL_DEC_BOX)
        {
            if (inExifBox && !jxlExifBuf.empty())
            {
                size_t rem     = JxlDecoderReleaseBoxBuffer(dec);
                size_t written = jxlExifBuf.size() - rem;
                if (written > 0)
                    ParseRawExif(jxlExifBuf.data(), written, out);
                inExifBox = false;
                jxlExifBuf.clear();
            }
            JxlBoxType boxType{};
            if (JxlDecoderGetBoxType(dec, boxType, JXL_FALSE) == JXL_DEC_SUCCESS
                && memcmp(boxType, "Exif", 4) == 0)
            {
                jxlExifBuf.resize(256 * 1024);
                JxlDecoderSetBoxBuffer(dec, jxlExifBuf.data(), jxlExifBuf.size());
                inExifBox = true;
            }
        }
        else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER
              || status == JXL_DEC_SUCCESS
              || status == JXL_DEC_ERROR)
        {
            break;  // piksel decode istemeden dur
        }
    }

    if (inExifBox && !jxlExifBuf.empty())
    {
        size_t rem     = JxlDecoderReleaseBoxBuffer(dec);
        size_t written = jxlExifBuf.size() - rem;
        if (written > 0)
            ParseRawExif(jxlExifBuf.data(), written, out);
    }

    JxlDecoderDestroy(dec);
    return out.width > 0;
}

static bool ExtractMetaAVIF(const std::wstring& path, DecodeOutput& out)
{
    auto data = ReadFileBytes(path);
    if (data.empty()) return false;

    avifDecoder* decoder = avifDecoderCreate();
    if (!decoder) return false;

    bool ok = (avifDecoderSetIOMemory(decoder, data.data(), data.size()) == AVIF_RESULT_OK
            && avifDecoderParse(decoder) == AVIF_RESULT_OK);

    if (ok && decoder->image)
    {
        out.width  = decoder->image->width;
        out.height = decoder->image->height;
        if (decoder->image->exif.size > 0)
            ParseRawExif(decoder->image->exif.data, decoder->image->exif.size, out);
        if (decoder->image->icc.size > 0)
            out.iccProfileName = IccProfileName(decoder->image->icc.data,
                                                decoder->image->icc.size);
    }

    avifDecoderDestroy(decoder);
    return ok && out.width > 0;
}

// ─── Metadata-only dispatch ────────────────────────────────────────────────────
// Piksel decode etmeden sadece EXIF/GPS/boyut bilgilerini doldurur.
// WM_META_DONE aşaması için çağrılır; tam DecodeImage'dan ~10-20× daha hızlı.
bool ExtractImageMeta(const std::wstring& path, DecodeOutput& out)
{
    auto dot = path.rfind(L'.');
    std::wstring ext;
    if (dot != std::wstring::npos)
    {
        ext = path.substr(dot + 1);
        for (auto& c : ext) c = towupper(c);
    }
    out.format = (ext == L"JPG") ? L"JPEG" : ext;

    if (ext == L"WEBP")              return ExtractMetaWebP(path, out);
    if (ext == L"HEIC" || ext == L"HEIF") return ExtractMetaHEIF(path, out);
    if (ext == L"JXL")               return ExtractMetaJXL(path, out);
    if (ext == L"AVIF")              return ExtractMetaAVIF(path, out);
    return ExtractMetaWIC(path, out);  // JPEG, PNG, BMP, GIF, TIFF, ICO
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

    return ok;
}

// ─── Thumbnail decode (WIC hızlı yolu + yedek) ────────────────────────────────

// WIC pipeline: Decoder → FormatConverter(BGRA) → BitmapScaler(Fant) → CopyPixels
// Metadata atlanır (WICDecodeMetadataCacheOnDemand), büyük buffer tahsis edilmez.
// JPEG, PNG, BMP, TIFF, GIF, ICO için kullanılır.
static bool DecodeThumbWithWIC(const std::wstring& path, UINT targetH,
                                std::vector<uint8_t>& pixelsOut,
                                UINT& widthOut, UINT& heightOut)
{
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))))
        return false;

    IWICBitmapDecoder* decoder = nullptr;
    HRESULT hr = wic->CreateDecoderFromFilename(
        path.c_str(), nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnDemand,  // metadata atlama — thumbnail için gereksiz
        &decoder);
    if (FAILED(hr)) { wic->Release(); return false; }

    IWICBitmapFrameDecode* frame = nullptr;
    hr = decoder->GetFrame(0, &frame);
    decoder->Release();
    if (FAILED(hr)) { wic->Release(); return false; }

    UINT srcW = 0, srcH = 0;
    frame->GetSize(&srcW, &srcH);
    if (srcW == 0 || srcH == 0) { frame->Release(); wic->Release(); return false; }

    // En-boy oranını koru, hedef yüksekliğe ölçekle
    UINT dstH = (srcH < targetH) ? srcH : targetH;
    UINT dstW = srcW * dstH / srcH;
    if (dstW < 1) dstW = 1;

    // Format dönüştürücü: 32bpp BGRA düz alfa
    IWICFormatConverter* converter = nullptr;
    hr = wic->CreateFormatConverter(&converter);
    if (FAILED(hr)) { frame->Release(); wic->Release(); return false; }

    hr = converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
                               WICBitmapDitherTypeNone, nullptr, 0.0f,
                               WICBitmapPaletteTypeMedianCut);
    frame->Release();
    if (FAILED(hr)) { converter->Release(); wic->Release(); return false; }

    // WIC scaler: Fant interpolation, doğrudan hedef boyuta
    // (lazy pull model — tam çözünürlük buffer tahsis edilmez)
    IWICBitmapScaler* scaler = nullptr;
    hr = wic->CreateBitmapScaler(&scaler);
    if (FAILED(hr)) { converter->Release(); wic->Release(); return false; }

    hr = scaler->Initialize(converter, dstW, dstH, WICBitmapInterpolationModeFant);
    converter->Release();
    if (FAILED(hr)) { scaler->Release(); wic->Release(); return false; }

    pixelsOut.resize(static_cast<size_t>(dstW) * dstH * 4);
    hr = scaler->CopyPixels(nullptr, dstW * 4,
                            static_cast<UINT>(pixelsOut.size()), pixelsOut.data());
    scaler->Release();
    wic->Release();

    if (FAILED(hr)) { pixelsOut.clear(); return false; }

    PremultiplyBGRA(pixelsOut.data(), dstW, dstH);
    widthOut  = dstW;
    heightOut = dstH;
    return true;
}

static void ScalePixelsNN(const std::vector<uint8_t>& src, UINT srcW, UINT srcH,
                           std::vector<uint8_t>& dst, UINT dstW, UINT dstH)
{
    dst.resize(static_cast<size_t>(dstW) * dstH * 4);
    for (UINT dy = 0; dy < dstH; ++dy) {
        UINT sy = dy * srcH / dstH;
        for (UINT dx = 0; dx < dstW; ++dx) {
            UINT sx = dx * srcW / dstW;
            const uint8_t* s = src.data() + (sy * srcW + sx) * 4;
            uint8_t* d = dst.data() + (dy * dstW + dx) * 4;
            d[0]=s[0]; d[1]=s[1]; d[2]=s[2]; d[3]=s[3];
        }
    }
}

bool DecodeImageForThumbnail(const std::wstring& path, UINT targetH,
                              std::vector<uint8_t>& pixelsOut,
                              UINT& widthOut, UINT& heightOut)
{
    // WIC hızlı yolu (JPEG, PNG, BMP, TIFF, GIF, ICO) — HEIC/HEIF için atlanır:
    // sistem WIC HEIC codec'i ICC renk dönüşümü yapmaz, EXIF yönelimi uygulamaz;
    // sonuç siyah veya renk bozuk piksel olabilir.
    {
        bool skipWic = false;
        size_t dot = path.rfind(L'.');
        if (dot != std::wstring::npos) {
            std::wstring ext = path.substr(dot);
            for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
            skipWic = (ext == L".heic" || ext == L".heif");
        }
        if (!skipWic && DecodeThumbWithWIC(path, targetH, pixelsOut, widthOut, heightOut))
            return true;
    }

    // HEIC/HEIF: gömülü thumbnail (~5-20ms), tam decode yerine kullan (~100-200ms)
    // Explorer hover'da yeni GetThumbnail çağrısı yapar; yavaş decode timeout'a girip
    // cache'deki thumbnail'i siler. Embedded thumb bunu önler.
    {
        size_t dot = path.rfind(L'.');
        if (dot != std::wstring::npos) {
            std::wstring ext = path.substr(dot);
            for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
            if (ext == L".heic" || ext == L".heif") {
                std::vector<uint8_t> embPixels;
                UINT embW = 0, embH = 0;
                if (ExtractHEICEmbeddedPreview(path, embPixels, embW, embH) &&
                    embW > 0 && embH > 0)
                {
                    if (embH <= targetH) {
                        pixelsOut = std::move(embPixels);
                        widthOut = embW; heightOut = embH;
                        return true;
                    }
                    UINT dstH = targetH;
                    UINT dstW = embW * dstH / embH;
                    if (dstW < 1) dstW = 1;
                    ScalePixelsNN(embPixels, embW, embH, pixelsOut, dstW, dstH);
                    widthOut = dstW; heightOut = dstH;
                    return true;
                }
            }
        }
    }

    // Yedek: tam decode + nearest-neighbor ölçekleme (WebP, JXL, AVIF, gömülü thumb'sız HEIC)
    DecodeOutput decoded;
    if (!DecodeImage(path, decoded)) return false;

    const uint8_t* srcPixels = nullptr;
    UINT srcW = 0, srcH = 0;
    if (!decoded.pixels.empty())
    {
        srcPixels = decoded.pixels.data();
        srcW = decoded.width;
        srcH = decoded.height;
    }
    else if (!decoded.frames.empty())
    {
        srcPixels = decoded.frames[0].pixels.data();
        srcW = decoded.frames[0].width;
        srcH = decoded.frames[0].height;
    }
    if (!srcPixels || srcW == 0 || srcH == 0) return false;

    UINT dstH = (srcH < targetH) ? srcH : targetH;
    UINT dstW = srcW * dstH / srcH;
    if (dstW < 1) dstW = 1;

    std::vector<uint8_t> srcVec(srcPixels, srcPixels + static_cast<size_t>(srcW) * srcH * 4);
    ScalePixelsNN(srcVec, srcW, srcH, pixelsOut, dstW, dstH);

    widthOut  = dstW;
    heightOut = dstH;
    return true;
}

// ─── PNG → BGRA pre-mul piksel (background thread decode) ────────────────────

bool DecodePngToPixels(const uint8_t* pngBytes, size_t byteCount,
                       std::vector<uint8_t>& outPixels, UINT& outW, UINT& outH)
{
    if (!pngBytes || byteCount == 0) return false;

    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IWICImagingFactory, reinterpret_cast<void**>(&wic))) || !wic)
        return false;

    IWICStream* stream = nullptr;
    wic->CreateStream(&stream);
    if (!stream) { wic->Release(); return false; }
    stream->InitializeFromMemory(const_cast<BYTE*>(pngBytes), static_cast<DWORD>(byteCount));

    IWICBitmapDecoder* decoder = nullptr;
    wic->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
    stream->Release();
    if (!decoder) { wic->Release(); return false; }

    IWICBitmapFrameDecode* frame = nullptr;
    decoder->GetFrame(0, &frame);
    decoder->Release();
    if (!frame) { wic->Release(); return false; }

    IWICFormatConverter* conv = nullptr;
    wic->CreateFormatConverter(&conv);
    if (!conv) { frame->Release(); wic->Release(); return false; }

    conv->Initialize(frame, GUID_WICPixelFormat32bppPBGRA,
                     WICBitmapDitherTypeNone, nullptr, 0.0f, WICBitmapPaletteTypeMedianCut);
    frame->Release();

    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    if (w == 0 || h == 0) { conv->Release(); wic->Release(); return false; }

    outPixels.resize(static_cast<size_t>(w) * h * 4);
    conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(outPixels.size()), outPixels.data());
    conv->Release();
    wic->Release();

    outW = w;
    outH = h;
    return true;
}

// ─── Kayıt (encode) yardımcıları ─────────────────────────────────────────────

// BGRA pre-multiplied → BGRA düz alfa (kayıt öncesi ön hazırlık)
static void UnpremultiplyInPlace(uint8_t* pixels, UINT count)
{
    for (UINT i = 0; i < count; ++i) {
        uint8_t* p = pixels + i * 4;
        const uint32_t a = p[3];
        if (a == 0 || a == 255) continue;
        p[0] = static_cast<uint8_t>(min(255u, (p[0] * 255u + a / 2) / a));
        p[1] = static_cast<uint8_t>(min(255u, (p[1] * 255u + a / 2) / a));
        p[2] = static_cast<uint8_t>(min(255u, (p[2] * 255u + a / 2) / a));
    }
}

static bool WriteAllBytes(const std::wstring& path, const void* data, DWORD size)
{
    HANDLE hf = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(hf, data, size, &written, nullptr) && written == size;
    CloseHandle(hf);
    return ok;
}

// Ham EXIF byte dizisinde Orientation tag'ini (274) newVal olarak yazar.
// HEIC 4-byte offset header'ı veya "Exif\0\0" prefix'i varsa atlanır.
static void PatchExifOrientation(std::vector<uint8_t>& exif, uint16_t newVal)
{
    const uint8_t* p   = exif.data();
    size_t         sz  = exif.size();
    size_t         off = 0;

    // "Exif\0\0" prefix
    if (sz >= 6 && memcmp(p, "Exif\0\0", 6) == 0) { off = 6; }
    // HEIC ISO 14496-12 ExifDataBlock: ilk 4 byte big-endian offset
    else if (sz >= 4)
    {
        uint32_t iso = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                       (uint32_t(p[2]) << 8)  |  uint32_t(p[3]);
        size_t tiff = 4 + iso;
        if (tiff + 4 <= sz &&
            ((p[tiff] == 'I' && p[tiff+1] == 'I') || (p[tiff] == 'M' && p[tiff+1] == 'M')))
            off = tiff;
    }
    if (off + 8 > sz) return;

    const uint8_t* t  = p + off;
    size_t         tsz = sz - off;
    bool le = (t[0] == 'I' && t[1] == 'I');
    if (!le && !(t[0] == 'M' && t[1] == 'M')) return;

    auto r16 = [&](size_t o) -> uint16_t {
        if (o + 2 > tsz) return 0;
        return le ? uint16_t(t[o] | (t[o+1] << 8))
                  : uint16_t((t[o] << 8) | t[o+1]);
    };
    auto r32 = [&](size_t o) -> uint32_t {
        if (o + 4 > tsz) return 0;
        return le ? (uint32_t(t[o]) | uint32_t(t[o+1])<<8 | uint32_t(t[o+2])<<16 | uint32_t(t[o+3])<<24)
                  : (uint32_t(t[o])<<24 | uint32_t(t[o+1])<<16 | uint32_t(t[o+2])<<8 | uint32_t(t[o+3]));
    };
    auto w16 = [&](size_t o, uint16_t v) {
        if (o + 2 > tsz) return;
        uint8_t* w = exif.data() + off + o;
        if (le) { w[0] = v & 0xFF; w[1] = v >> 8; }
        else    { w[0] = v >> 8;   w[1] = v & 0xFF; }
    };

    if (r16(2) != 42) return;  // TIFF magic
    uint32_t ifd0 = r32(4);
    if (ifd0 + 2 > tsz) return;
    uint16_t n = r16(ifd0);
    for (uint16_t i = 0; i < n; ++i)
    {
        size_t e = ifd0 + 2 + static_cast<size_t>(i) * 12;
        if (e + 12 > tsz) break;
        if (r16(e) == 274)  // Orientation tag
        {
            // SHORT (type=3), count=1 → value at e+8 (little-endian SHORT)
            w16(e + 8, newVal);
            return;
        }
    }
}

// WIC encoder: JPEG, PNG, BMP, TIFF
// sourcePath: dolu ise kaynak dosyanın EXIF/metadata blokları kopyalanır ve
//             Orientation etiketi 1 (normal) olarak sıfırlanır (pikseller zaten döndürülmüş).
// Aynı yola yazarken temp dosya kullanılır (dosya kilidi önleme).
static bool SaveImageWIC(const std::wstring& path, const GUID& fmt,
                          const uint8_t* bgraStraight, UINT width, UINT height,
                          const std::wstring& sourcePath = L"")
{
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))))
        return false;

    bool isJpeg = (fmt == GUID_ContainerFormatJpeg);
    bool isTiff = (fmt == GUID_ContainerFormatTiff);

    // Aynı yola yazıyorsak (overwrite) temp dosya kullan:
    // kaynak decoder dosyayı kilitler; write stream aynı yolu açamaz.
    // WICDecodeMetadataCacheOnLoad bellekte tutulur ama COM objesi dosyayı bırakmaz.
    bool useTemp = (!sourcePath.empty() && (isJpeg || isTiff) &&
                    _wcsicmp(path.c_str(), sourcePath.c_str()) == 0);
    std::wstring writePath = useTemp ? (path + L".lumtmp") : path;

    IWICBitmapDecoder*   srcDecoder  = nullptr;
    IWICBitmapFrameDecode* srcFrame  = nullptr;
    IWICMetadataBlockReader* blkRdr  = nullptr;

    if (!sourcePath.empty() && (isJpeg || isTiff))
    {
        if (SUCCEEDED(wic->CreateDecoderFromFilename(
                sourcePath.c_str(), nullptr, GENERIC_READ,
                WICDecodeMetadataCacheOnLoad, &srcDecoder)))
        {
            if (SUCCEEDED(srcDecoder->GetFrame(0, &srcFrame)))
                srcFrame->QueryInterface(IID_IWICMetadataBlockReader,
                                         reinterpret_cast<void**>(&blkRdr));
        }
    }

    IWICStream* stream = nullptr;
    HRESULT hr = wic->CreateStream(&stream);
    if (FAILED(hr)) goto cleanup_early;
    hr = stream->InitializeFromFilename(writePath.c_str(), GENERIC_WRITE);
    if (FAILED(hr)) { stream->Release(); stream = nullptr; goto cleanup_early; }

    {
        IWICBitmapEncoder* encoder = nullptr;
        hr = wic->CreateEncoder(fmt, nullptr, &encoder);
        if (FAILED(hr)) { stream->Release(); goto cleanup_early; }
        hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
        if (FAILED(hr)) { encoder->Release(); stream->Release(); goto cleanup_early; }

        IWICBitmapFrameEncode* frame = nullptr;
        IPropertyBag2* props = nullptr;
        hr = encoder->CreateNewFrame(&frame, &props);
        if (FAILED(hr)) { encoder->Release(); stream->Release(); goto cleanup_early; }

        if (isJpeg && props) {
            PROPBAG2 opt{};
            opt.pstrName = const_cast<OLECHAR*>(L"ImageQuality");
            VARIANT var{};
            var.vt     = VT_R4;
            var.fltVal = 0.92f;
            props->Write(1, &opt, &var);
        }
        frame->Initialize(props);  // props (or nullptr) geçirilmeli; Release'den önce
        if (props) props->Release();
        frame->SetSize(width, height);
        WICPixelFormatGUID pixFmt = isJpeg ? GUID_WICPixelFormat24bppBGR
                                           : GUID_WICPixelFormat32bppBGRA;
        frame->SetPixelFormat(&pixFmt);

        // Metadata kopyala (JPEG / TIFF)
        if (blkRdr)
        {
            IWICMetadataBlockWriter* blkWr = nullptr;
            if (SUCCEEDED(frame->QueryInterface(IID_IWICMetadataBlockWriter,
                                                reinterpret_cast<void**>(&blkWr))))
            {
                blkWr->InitializeFromBlockReader(blkRdr);  // hata varsa yoksay, metadata olmadan devam
                blkWr->Release();
            }

            // Orientation = 1 (Normal): pikseller zaten fiziksel olarak döndürülmüş
            IWICMetadataQueryWriter* qWr = nullptr;
            if (SUCCEEDED(frame->GetMetadataQueryWriter(&qWr)))
            {
                PROPVARIANT pv;
                PropVariantInit(&pv);
                pv.vt    = VT_UI2;
                pv.uiVal = 1;
                if (isJpeg) qWr->SetMetadataByName(L"/app1/ifd/{ushort=274}", &pv);
                if (isTiff) qWr->SetMetadataByName(L"/ifd/{ushort=274}",      &pv);
                PropVariantClear(&pv);
                qWr->Release();
            }
        }

        bool ok = false;
        if (isJpeg) {
            std::vector<uint8_t> bgr(static_cast<size_t>(width) * height * 3);
            for (UINT i = 0; i < width * height; ++i) {
                bgr[i * 3 + 0] = bgraStraight[i * 4 + 0];
                bgr[i * 3 + 1] = bgraStraight[i * 4 + 1];
                bgr[i * 3 + 2] = bgraStraight[i * 4 + 2];
            }
            ok = SUCCEEDED(frame->WritePixels(height, width * 3,
                                              static_cast<UINT>(bgr.size()), bgr.data()));
        } else {
            ok = SUCCEEDED(frame->WritePixels(height, width * 4, width * height * 4,
                                              const_cast<uint8_t*>(bgraStraight)));
        }
        ok = ok && SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());

        frame->Release();
        encoder->Release();
        stream->Release();

        if (blkRdr)   blkRdr->Release();
        if (srcFrame) srcFrame->Release();
        if (srcDecoder) srcDecoder->Release();
        wic->Release();

        // Temp dosyayı hedef yere taşı (kaynak decoder artık kapalı)
        if (ok && useTemp)
            ok = (MoveFileExW(writePath.c_str(), path.c_str(),
                              MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0);
        if (!ok && useTemp)
            DeleteFileW(writePath.c_str());
        return ok;
    }

cleanup_early:
    if (blkRdr)    blkRdr->Release();
    if (srcFrame)  srcFrame->Release();
    if (srcDecoder) srcDecoder->Release();
    wic->Release();
    if (useTemp) DeleteFileW(writePath.c_str());
    return false;
}

// WebP encoder: libwebp (sourcePath varsa EXIF chunk kopyalanır)
static bool SaveImageWebP(const std::wstring& path, const uint8_t* bgraStraight,
                           UINT width, UINT height,
                           const std::wstring& sourcePath = L"")
{
    // BGRA → RGBA
    std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
    for (UINT i = 0; i < width * height; ++i) {
        rgba[i * 4 + 0] = bgraStraight[i * 4 + 2];
        rgba[i * 4 + 1] = bgraStraight[i * 4 + 1];
        rgba[i * 4 + 2] = bgraStraight[i * 4 + 0];
        rgba[i * 4 + 3] = bgraStraight[i * 4 + 3];
    }
    uint8_t* output = nullptr;
    size_t outputSize = WebPEncodeRGBA(rgba.data(), static_cast<int>(width),
                                       static_cast<int>(height),
                                       static_cast<int>(width * 4), 90.0f, &output);
    if (!output || outputSize == 0) { WebPFree(output); return false; }

    // Kaynak WebP'den EXIF chunk'ı çıkar
    std::vector<uint8_t> exifChunk;
    if (!sourcePath.empty())
    {
        auto srcData = ReadFileBytes(sourcePath);
        if (!srcData.empty())
        {
            WebPData wd = { srcData.data(), srcData.size() };
            WebPDemuxer* dmx = WebPDemux(&wd);
            if (dmx)
            {
                WebPChunkIterator ci{};
                if (WebPDemuxGetChunk(dmx, "EXIF", 1, &ci))
                {
                    exifChunk.assign(ci.chunk.bytes, ci.chunk.bytes + ci.chunk.size);
                    WebPDemuxReleaseChunkIterator(&ci);
                }
                WebPDemuxDelete(dmx);
            }
        }
    }

    bool ok = false;
    if (exifChunk.empty())
    {
        // EXIF yoksa doğrudan yaz
        ok = WriteAllBytes(path, output, static_cast<DWORD>(outputSize));
    }
    else
    {
        // Orientation'ı 1'e patch'le, WebPMux ile EXIF ekle
        PatchExifOrientation(exifChunk, 1);

        WebPData encodedWebP = { output, outputSize };
        WebPMux* mux = WebPMuxCreate(&encodedWebP, 0);
        if (mux)
        {
            WebPData exifData = { exifChunk.data(), exifChunk.size() };
            WebPMuxSetChunk(mux, "EXIF", &exifData, 0);

            WebPData assembled = { nullptr, 0 };
            if (WebPMuxAssemble(mux, &assembled) == WEBP_MUX_OK)
            {
                ok = WriteAllBytes(path, assembled.bytes, static_cast<DWORD>(assembled.size));
                WebPDataClear(&assembled);
            }
            WebPMuxDelete(mux);
        }
        if (!ok)
            ok = WriteAllBytes(path, output, static_cast<DWORD>(outputSize));  // fallback: EXIF'siz yaz
    }
    WebPFree(output);
    return ok;
}

// HEIC encoder: libheif (HEVC; AV1 fallback)
static bool SaveImageHEIC(const std::wstring& path, const uint8_t* bgraStraight,
                           UINT width, UINT height,
                           const std::wstring& sourcePath = L"")
{
    heif_context* ctx = heif_context_alloc();
    if (!ctx) return false;

    heif_encoder* enc = nullptr;
    heif_error err = heif_context_get_encoder_for_format(ctx, heif_compression_HEVC, &enc);
    if (err.code != heif_error_Ok)
        err = heif_context_get_encoder_for_format(ctx, heif_compression_AV1, &enc);
    if (err.code != heif_error_Ok) { heif_context_free(ctx); return false; }

    heif_encoder_set_lossy_quality(enc, 85);

    bool hasAlpha = false;
    for (UINT i = 0; i < width * height; ++i)
        if (bgraStraight[i * 4 + 3] < 255) { hasAlpha = true; break; }

    heif_image* img = nullptr;
    heif_chroma chroma = hasAlpha ? heif_chroma_interleaved_RGBA : heif_chroma_interleaved_RGB;
    err = heif_image_create(static_cast<int>(width), static_cast<int>(height),
                            heif_colorspace_RGB, chroma, &img);
    if (err.code != heif_error_Ok) { heif_encoder_release(enc); heif_context_free(ctx); return false; }

    err = heif_image_add_plane(img, heif_channel_interleaved,
                               static_cast<int>(width), static_cast<int>(height),
                               hasAlpha ? 32 : 24);
    if (err.code != heif_error_Ok) {
        heif_image_release(img); heif_encoder_release(enc); heif_context_free(ctx); return false;
    }

    int stride = 0;
    uint8_t* plane = heif_image_get_plane(img, heif_channel_interleaved, &stride);

    for (UINT y = 0; y < height; ++y)
        for (UINT x = 0; x < width; ++x) {
            const uint8_t* s = bgraStraight + (y * width + x) * 4;
            uint8_t* d = plane + y * stride + x * (hasAlpha ? 4 : 3);
            d[0] = s[2]; d[1] = s[1]; d[2] = s[0];
            if (hasAlpha) d[3] = s[3];
        }

    heif_encoding_options* opts = heif_encoding_options_alloc();
    heif_image_handle* outHandle = nullptr;
    err = heif_context_encode_image(ctx, img, enc, opts, &outHandle);
    heif_encoding_options_free(opts);

    // Filmstrip ve hızlı önizleme için gömülü thumbnail ekle.
    // bgraStraight üzerinden önceden ölçeklenerek fresh encoder ile encode edilir;
    // ana encode'da kullanılan enc'yi yeniden kullanmak libheif'in thumbnail encode
    // etmemesine yol açabilir ve kullanıcı tam decode (~1-3s) bitene kadar boş ekran görür.
    if (err.code == heif_error_Ok && outHandle) {
        // 512px sınırına ölçekle, en-boy oranını koru
        UINT tW, tH;
        if (width >= height) { tW = (width  < 512u ? width  : 512u); tH = max(1u, height * tW / width); }
        else                  { tH = (height < 512u ? height : 512u); tW = max(1u, width  * tH / height); }

        // bgraStraight → RGB(/RGBA) nearest-neighbor küçük resim
        const UINT bpp = hasAlpha ? 4u : 3u;
        std::vector<uint8_t> thumbRgb(static_cast<size_t>(tW) * tH * bpp);
        for (UINT ty = 0; ty < tH; ++ty) {
            const UINT sy = ty * height / tH;
            for (UINT tx = 0; tx < tW; ++tx) {
                const UINT sx = tx * width / tW;
                const uint8_t* s = bgraStraight + (sy * width + sx) * 4;
                uint8_t* d = thumbRgb.data() + (ty * tW + tx) * bpp;
                d[0] = s[2]; d[1] = s[1]; d[2] = s[0];   // BGR → RGB
                if (hasAlpha) d[3] = s[3];
            }
        }

        heif_image* tImg = nullptr;
        heif_chroma tChroma = hasAlpha ? heif_chroma_interleaved_RGBA : heif_chroma_interleaved_RGB;
        if (heif_image_create(static_cast<int>(tW), static_cast<int>(tH),
                              heif_colorspace_RGB, tChroma, &tImg).code == heif_error_Ok) {
            if (heif_image_add_plane(tImg, heif_channel_interleaved,
                                     static_cast<int>(tW), static_cast<int>(tH),
                                     hasAlpha ? 32 : 24).code == heif_error_Ok) {
                int tStride = 0;
                uint8_t* tPlane = heif_image_get_plane(tImg, heif_channel_interleaved, &tStride);
                for (UINT ty = 0; ty < tH; ++ty)
                    memcpy(tPlane + static_cast<ptrdiff_t>(ty) * tStride,
                           thumbRgb.data() + static_cast<size_t>(ty) * tW * bpp,
                           static_cast<size_t>(tW) * bpp);

                heif_encoder* tEnc = nullptr;
                heif_context_get_encoder_for_format(ctx, heif_compression_HEVC, &tEnc);
                if (!tEnc)
                    heif_context_get_encoder_for_format(ctx, heif_compression_AV1, &tEnc);
                if (tEnc) {
                    heif_encoder_set_lossy_quality(tEnc, 60);
                    heif_encoding_options* tOpts   = heif_encoding_options_alloc();
                    heif_image_handle*     tHandle = nullptr;
                    heif_context_encode_thumbnail(ctx, tImg, outHandle, tEnc, tOpts, 512, &tHandle);
                    heif_encoding_options_free(tOpts);
                    heif_encoder_release(tEnc);
                    if (tHandle) heif_image_handle_release(tHandle);
                }
            }
            heif_image_release(tImg);
        }
    }

    heif_image_release(img);
    heif_encoder_release(enc);

    // EXIF: kaynak HEIC'ten al, Orientation'ı 1'e patch'le, yeni dosyaya ekle
    if (err.code == heif_error_Ok && outHandle && !sourcePath.empty())
    {
        heif_context* srcCtx = heif_context_alloc();
        if (srcCtx)
        {
            char srcUtf8[MAX_PATH * 4]{};
            WideCharToMultiByte(CP_UTF8, 0, sourcePath.c_str(), -1,
                                srcUtf8, static_cast<int>(sizeof(srcUtf8)), nullptr, nullptr);
            if (heif_context_read_from_file(srcCtx, srcUtf8, nullptr).code == heif_error_Ok)
            {
                heif_image_handle* srcHandle = nullptr;
                if (heif_context_get_primary_image_handle(srcCtx, &srcHandle).code == heif_error_Ok)
                {
                    int nMeta = heif_image_handle_get_number_of_metadata_blocks(srcHandle, "Exif");
                    if (nMeta > 0)
                    {
                        heif_item_id metaId = 0;
                        heif_image_handle_get_list_of_metadata_block_IDs(srcHandle, "Exif", &metaId, 1);
                        size_t exifSz = heif_image_handle_get_metadata_size(srcHandle, metaId);
                        if (exifSz > 0)
                        {
                            std::vector<uint8_t> exifBuf(exifSz);
                            if (heif_image_handle_get_metadata(srcHandle, metaId, exifBuf.data()).code
                                == heif_error_Ok)
                            {
                                PatchExifOrientation(exifBuf, 1);
                                heif_context_add_exif_metadata(ctx, outHandle,
                                                               exifBuf.data(),
                                                               static_cast<int>(exifSz));
                            }
                        }
                    }
                    heif_image_handle_release(srcHandle);
                }
            }
            heif_context_free(srcCtx);
        }
    }

    bool ok = false;
    if (err.code == heif_error_Ok) {
        char pathUtf8[MAX_PATH * 4]{};
        WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1,
                            pathUtf8, static_cast<int>(sizeof(pathUtf8)), nullptr, nullptr);
        ok = (heif_context_write_to_file(ctx, pathUtf8).code == heif_error_Ok);
    }
    if (outHandle) heif_image_handle_release(outHandle);
    heif_context_free(ctx);
    return ok;
}

// JXL encoder disabled — libjxl encoder objects require __std_rotate/__std_unique_4
// (MSVC 17.7+ STL intrinsics) that are incompatible with the v143 toolset.
// JXL decode (viewing) continues to work; only JXL save is unavailable.
static bool SaveImageJXL(const std::wstring&, const uint8_t*, UINT, UINT)
{
    return false;
}

// AVIF encoder: libavif (sourcePath varsa EXIF kopyalanır)
static bool SaveImageAVIF(const std::wstring& path, const uint8_t* bgraStraight,
                           UINT width, UINT height,
                           const std::wstring& sourcePath = L"")
{
    bool hasAlpha = false;
    for (UINT i = 0; i < width * height; ++i)
        if (bgraStraight[i * 4 + 3] < 255) { hasAlpha = true; break; }

    avifImage* img = avifImageCreate(width, height, 8, AVIF_PIXEL_FORMAT_YUV420);
    if (!img) return false;

    img->colorPrimaries          = AVIF_COLOR_PRIMARIES_BT709;
    img->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_SRGB;
    img->matrixCoefficients      = AVIF_MATRIX_COEFFICIENTS_BT601;
    img->yuvRange                = AVIF_RANGE_FULL;

    // Kaynak AVIF'ten EXIF al, Orientation'ı 1'e patch'le
    if (!sourcePath.empty())
    {
        auto srcData = ReadFileBytes(sourcePath);
        if (!srcData.empty())
        {
            avifDecoder* dec = avifDecoderCreate();
            dec->ignoreExif = AVIF_FALSE;
            avifResult r = avifDecoderSetIOMemory(dec, srcData.data(), srcData.size());
            if (r == AVIF_RESULT_OK) r = avifDecoderParse(dec);
            if (r == AVIF_RESULT_OK && dec->image && dec->image->exif.size > 0)
            {
                std::vector<uint8_t> exifBuf(dec->image->exif.data,
                                             dec->image->exif.data + dec->image->exif.size);
                PatchExifOrientation(exifBuf, 1);
                // avifImageSetMetadataExif: irot/imir flag'lerini de ayarlar;
                // orientation=1 → dönüşüm yok, pikseller fiziksel olarak doğru.
                avifImageSetMetadataExif(img, exifBuf.data(), exifBuf.size());
            }
            avifDecoderDestroy(dec);
        }
    }

    avifRGBImage rgb{};
    avifRGBImageSetDefaults(&rgb, img);
    rgb.format = AVIF_RGB_FORMAT_BGRA;
    rgb.depth  = 8;

    if (avifRGBImageAllocatePixels(&rgb) != AVIF_RESULT_OK) {
        avifImageDestroy(img); return false;
    }
    memcpy(rgb.pixels, bgraStraight, static_cast<size_t>(width) * height * 4);

    avifResult res = avifImageRGBToYUV(img, &rgb);
    avifRGBImageFreePixels(&rgb);
    if (res != AVIF_RESULT_OK) { avifImageDestroy(img); return false; }

    avifEncoder* encoder = avifEncoderCreate();
    if (!encoder) { avifImageDestroy(img); return false; }
    encoder->quality      = 80;
    encoder->qualityAlpha = 100;  // AVIF_QUALITY_LOSSLESS
    encoder->speed        = AVIF_SPEED_DEFAULT;

    avifRWData output = AVIF_DATA_EMPTY;
    res = avifEncoderWrite(encoder, img, &output);
    avifEncoderDestroy(encoder);
    avifImageDestroy(img);

    bool ok = (res == AVIF_RESULT_OK) && output.size > 0
           && WriteAllBytes(path, output.data, static_cast<DWORD>(output.size));
    avifRWDataFree(&output);
    return ok;
}

// ─── Ana kayıt dispatcher ──────────────────────────────────────────────────────

bool SaveImage(const std::wstring& path, const std::wstring& format,
               const uint8_t* bgraPreMul, UINT width, UINT height,
               const std::wstring& sourcePath)
{
    if (!bgraPreMul || width == 0 || height == 0) return false;

    // Pre-multiplied → düz alfa kopyası (orijinal tampon değişmez)
    const UINT pixCount = width * height;
    std::vector<uint8_t> straight(bgraPreMul, bgraPreMul + static_cast<size_t>(pixCount) * 4);
    UnpremultiplyInPlace(straight.data(), pixCount);

    const uint8_t* px = straight.data();
    if (format == L"JPEG") return SaveImageWIC(path, GUID_ContainerFormatJpeg, px, width, height, sourcePath);
    if (format == L"PNG")  return SaveImageWIC(path, GUID_ContainerFormatPng,  px, width, height);
    if (format == L"BMP")  return SaveImageWIC(path, GUID_ContainerFormatBmp,  px, width, height);
    if (format == L"TIFF" || format == L"TIF")
                           return SaveImageWIC(path, GUID_ContainerFormatTiff, px, width, height, sourcePath);
    if (format == L"WebP" || format == L"WEBP")
                           return SaveImageWebP(path, px, width, height, sourcePath);
    if (format == L"HEIC" || format == L"HEIF")
                           return SaveImageHEIC(path, px, width, height, sourcePath);
    if (format == L"JXL")  return SaveImageJXL(path,  px, width, height);
    if (format == L"AVIF") return SaveImageAVIF(path, px, width, height, sourcePath);

    // Bilinmeyen format → PNG olarak kaydet
    return SaveImageWIC(path, GUID_ContainerFormatPng, px, width, height);
}
