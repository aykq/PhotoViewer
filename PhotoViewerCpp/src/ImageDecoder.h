#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>

// Decode sonucu: BGRA pre-multiplied pikseller + EXIF metadata
struct DecodeOutput
{
    std::vector<uint8_t> pixels;   // BGRA pre-multiplied, satır sırası, width * height * 4 byte
    UINT         width      = 0;
    UINT         height     = 0;
    std::wstring format;           // ör. L"JPEG", L"WebP", L"HEIC"

    // EXIF metadata — yoksa boş
    std::wstring dateTaken;
    std::wstring cameraMake;
    std::wstring cameraModel;
    std::wstring aperture;         // ör. L"f/2.8"
    std::wstring shutterSpeed;     // ör. L"1/500s"
    std::wstring iso;
};

// Desteklenen tüm formatları 32bpp BGRA pre-multiplied piksellere decode eder.
// EXIF yönelimi (Orientation tag) otomatik uygulanır.
// Başarılıysa true döner ve out.pixels dolu olur.
bool DecodeImage(const std::wstring& path, DecodeOutput& out);
