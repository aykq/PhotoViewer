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
        PROPVARIANT pvO;
        PropVariantInit(&pvO);
        if (SUCCEEDED(mqr->GetMetadataByName(L"/app1/ifd/{ushort=274}", &pvO)))
        {
            if      (pvO.vt == VT_UI2) orientation = pvO.uiVal;
            else if (pvO.vt == VT_UI4) orientation = static_cast<UINT16>(pvO.ulVal);
            PropVariantClear(&pvO);
        }

        out.cameraMake   = WicQueryStr(mqr, L"/app1/ifd/{ushort=271}");
        out.cameraModel  = WicQueryStr(mqr, L"/app1/ifd/{ushort=272}");
        out.dateTaken    = WicQueryStr(mqr, L"/app1/ifd/exif/{ushort=36867}");
        out.aperture     = WicQueryRational(mqr, L"/app1/ifd/exif/{ushort=33437}", L"aperture");
        out.shutterSpeed = WicQueryRational(mqr, L"/app1/ifd/exif/{ushort=33434}", L"shutter");

        PROPVARIANT pvIso;
        PropVariantInit(&pvIso);
        if (SUCCEEDED(mqr->GetMetadataByName(L"/app1/ifd/exif/{ushort=34855}", &pvIso)))
        {
            wchar_t buf[16];
            if      (pvIso.vt == VT_UI2) { swprintf_s(buf, L"%u",  static_cast<unsigned>(pvIso.uiVal)); out.iso = buf; }
            else if (pvIso.vt == VT_UI4) { swprintf_s(buf, L"%lu", pvIso.ulVal);                         out.iso = buf; }
            PropVariantClear(&pvIso);
        }

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
