#include "ImageDecoder.h"

#include <wincodec.h>
#include <cwchar>
#include <cstring>  // memcpy

// vcpkg bundled decoders
#include <webp/decode.h>
#include <libheif/heif.h>
#include <jxl/decode.h>
#include <avif/avif.h>

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

    // Format dönüştürücü: her WIC formatını 32bppPBGRA (pre-multiplied BGRA) yapar
    IWICFormatConverter* converter = nullptr;
    hr = wic->CreateFormatConverter(&converter);
    if (FAILED(hr)) { frame->Release(); wic->Release(); return false; }

    hr = converter->Initialize(frame, GUID_WICPixelFormat32bppPBGRA,
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

    return !out.pixels.empty();
}

// ─── WebP decoder (libwebp) ───────────────────────────────────────────────────

static bool DecodeWebP(const std::wstring& path, DecodeOutput& out)
{
    auto data = ReadFileBytes(path);
    if (data.empty()) return false;

    int w = 0, h = 0;
    // WebPDecodeBGRA: BGRA düz alfa döner
    uint8_t* decoded = WebPDecodeBGRA(data.data(), data.size(), &w, &h);
    if (!decoded) return false;

    out.width  = static_cast<UINT>(w);
    out.height = static_cast<UINT>(h);
    out.pixels.assign(decoded, decoded + static_cast<size_t>(w) * h * 4);
    WebPFree(decoded);

    PremultiplyBGRA(out.pixels.data(), out.width, out.height);
    return true;
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

    // libheif RGBA döner — Direct2D için BGRA'ya çevir, sonra pre-multiply
    SwapRB(out.pixels.data(), out.width, out.height);
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
        JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE);
    JxlDecoderSetInput(dec, data.data(), data.size());
    JxlDecoderCloseInput(dec);

    JxlBasicInfo   info{};
    JxlPixelFormat fmt = { 4, JXL_TYPE_UINT8, JXL_LITTLE_ENDIAN, 0 };  // RGBA
    bool success = false;

    for (;;)
    {
        JxlDecoderStatus status = JxlDecoderProcessInput(dec);

        if (status == JXL_DEC_BASIC_INFO)
        {
            if (JxlDecoderGetBasicInfo(dec, &info) != JXL_DEC_SUCCESS) break;
        }
        else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER)
        {
            size_t bufSize = 0;
            if (JxlDecoderImageOutBufferSize(dec, &fmt, &bufSize) != JXL_DEC_SUCCESS) break;
            out.pixels.resize(bufSize);
            if (JxlDecoderSetImageOutBuffer(dec, &fmt, out.pixels.data(), bufSize)
                != JXL_DEC_SUCCESS) break;
        }
        else if (status == JXL_DEC_FULL_IMAGE)
        {
            out.width  = info.xsize;
            out.height = info.ysize;
            success    = true;
            break;
        }
        else if (status == JXL_DEC_SUCCESS || status == JXL_DEC_ERROR)
        {
            break;
        }
    }

    JxlDecoderDestroy(dec);

    if (!success || out.pixels.empty()) { out.pixels.clear(); return false; }

    // libjxl RGBA döner — Direct2D için BGRA'ya çevir, sonra pre-multiply
    SwapRB(out.pixels.data(), out.width, out.height);
    PremultiplyBGRA(out.pixels.data(), out.width, out.height);
    return true;
}

// ─── AVIF decoder (libavif) ───────────────────────────────────────────────────

static bool DecodeAVIF(const std::wstring& path, DecodeOutput& out)
{
    auto data = ReadFileBytes(path);
    if (data.empty()) return false;

    avifDecoder* decoder = avifDecoderCreate();
    if (!decoder) return false;

    // libavif 1.x: avifDecoderReadMemory(decoder, image, data, size)
    avifImage* image = avifImageCreateEmpty();
    if (!image) { avifDecoderDestroy(decoder); return false; }

    avifResult res = avifDecoderReadMemory(decoder, image, data.data(), data.size());
    if (res != AVIF_RESULT_OK)
    {
        avifImageDestroy(image);
        avifDecoderDestroy(decoder);
        return false;
    }

    // EXIF bloğu varsa parse et
    if (image->exif.size > 0)
        ParseRawExif(image->exif.data, image->exif.size, out);

    avifRGBImage rgb{};
    avifRGBImageSetDefaults(&rgb, image);
    rgb.format = AVIF_RGB_FORMAT_BGRA;   // Direct2D uyumlu format
    avifRGBImageAllocatePixels(&rgb);

    res = avifImageYUVToRGB(image, &rgb);
    bool success = (res == AVIF_RESULT_OK);

    if (success)
    {
        out.width  = rgb.width;
        out.height = rgb.height;
        out.pixels.resize(static_cast<size_t>(out.width) * out.height * 4);

        // Stride farklıysa satır satır kopyala
        for (UINT row = 0; row < out.height; row++)
            memcpy(out.pixels.data() + static_cast<size_t>(row) * out.width * 4,
                   rgb.pixels        + static_cast<size_t>(row) * rgb.rowBytes,
                   static_cast<size_t>(out.width) * 4);
    }

    avifRGBImageFreePixels(&rgb);
    avifImageDestroy(image);
    avifDecoderDestroy(decoder);

    if (!success || out.pixels.empty()) { out.pixels.clear(); return false; }

    // libavif BGRA düz alfa döner — pre-multiply gerekli
    PremultiplyBGRA(out.pixels.data(), out.width, out.height);
    return true;
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

    if (ext == L"WEBP")
        return DecodeWebP(path, out);

    if (ext == L"HEIC" || ext == L"HEIF")
        return DecodeHEIF(path, out);

    if (ext == L"JXL")
        return DecodeJXL(path, out);

    if (ext == L"AVIF")
        return DecodeAVIF(path, out);

    // JPEG, PNG, BMP, GIF, TIFF, ICO → WIC
    return DecodeWithWIC(path, out);
}
