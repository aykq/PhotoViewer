#include "GeoServices.h"

#include <winhttp.h>
#include <cwchar>
#include <cmath>    // std::pow, std::floor, std::tan, std::cos, std::log

#pragma comment(lib, "winhttp.lib")

#ifndef M_PI
static constexpr double M_PI = 3.14159265358979323846;
#endif

// ─── Photon (Komoot) reverse geocoding ───────────────────────────────────────
// GPS koordinatlarından konum adını döndürür: "Şehir, İl, Ülke"
// WinHTTP ile photon.komoot.io/reverse API'sine istek atar (GeoJSON, rate limit yok).

std::wstring FetchLocationName(double lat, double lon)
{
    wchar_t reqPath[128];
    swprintf_s(reqPath, L"/reverse?lat=%.6f&lon=%.6f", lat, lon);

    HINTERNET hSession = WinHttpOpen(L"Lumina/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return {};

    WinHttpSetTimeouts(hSession, 5000, 5000, 10000, 10000);

    HINTERNET hConn = WinHttpConnect(hSession,
        L"photon.komoot.io", INTERNET_DEFAULT_HTTPS_PORT, 0);
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
        L"Accept: application/json",
        static_cast<DWORD>(-1L), WINHTTP_ADDREQ_FLAG_ADD);

    bool ok = WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0) != FALSE
           && WinHttpReceiveResponse(hReq, nullptr) != FALSE;

    std::string body;
    if (ok)
    {
        constexpr size_t kMaxResponseBytes = 1 * 1024 * 1024;  // 1 MB
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(hReq, &avail) && avail > 0)
        {
            if (body.size() + avail > kMaxResponseBytes) break;
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

    // Photon GeoJSON: features[0].properties.{city,locality,county,state,country}
    auto propsPos = body.find("\"properties\":");
    auto propsEnd = propsPos != std::string::npos ? body.find('}', propsPos) : std::string::npos;
    std::string props = (propsPos != std::string::npos && propsEnd != std::string::npos)
                        ? body.substr(propsPos, propsEnd - propsPos + 1)
                        : body;

    // Şehir/kasaba/köy → il → ülke
    std::string city = extract(props, "city");
    if (city.empty()) city = extract(props, "locality");
    if (city.empty()) city = extract(props, "county");
    if (city.empty()) city = extract(props, "name");
    std::string state   = extract(props, "state");
    std::string country = extract(props, "country");

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
    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, result.c_str(), -1, nullptr, 0);
    if (len <= 1) return {};
    std::wstring wresult(len - 1, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, result.c_str(), -1, wresult.data(), len - 1))
        return {};
    return wresult;
}

// ─── OSM tile yardımcıları ────────────────────────────────────────────────────

// Web Mercator projeksiyonu: GPS koordinatını OSM tile indeksine çevirir.
// Standart formül: https://wiki.openstreetmap.org/wiki/Slippy_map_tilenames
void LatLonToTileXY(double lat, double lon, int zoom, int& tx, int& ty)
{
    const double n    = std::pow(2.0, zoom);
    const double latR = lat * M_PI / 180.0;

    tx = static_cast<int>(std::floor((lon + 180.0) / 360.0 * n));
    ty = static_cast<int>(std::floor(
             (1.0 - std::log(std::tan(latR) + 1.0 / std::cos(latR)) / M_PI) / 2.0 * n));

    // Sınır dışı değerleri clamp et
    const int maxIdx = static_cast<int>(n) - 1;
    if (tx < 0) tx = 0; else if (tx > maxIdx) tx = maxIdx;
    if (ty < 0) ty = 0; else if (ty > maxIdx) ty = maxIdx;
}

// Tile içindeki piksel ofsetini hesaplar (tile boyutu 256×256).
// Tam piksel koordinatını hesaplayıp tile başlangıcına göre ofset alır.
void LatLonToPixelInTile(double lat, double lon, int zoom,
                         int tileX, int tileY, int& px, int& py)
{
    const double n    = std::pow(2.0, zoom);
    const double latR = lat * M_PI / 180.0;

    const double fullX = (lon + 180.0) / 360.0 * n * 256.0;
    const double fullY = (1.0 - std::log(std::tan(latR) + 1.0 / std::cos(latR)) / M_PI)
                         / 2.0 * n * 256.0;

    px = static_cast<int>(fullX) - tileX * 256;
    py = static_cast<int>(fullY) - tileY * 256;

    // [0, 255] aralığına clamp et
    if (px < 0) px = 0; else if (px > 255) px = 255;
    if (py < 0) py = 0; else if (py > 255) py = 255;
}

// WinHTTP ile tile.openstreetmap.org'dan PNG tile indirir.
// tile.openstreetmap.org/{zoom}/{x}/{y}.png
std::vector<uint8_t> FetchOsmTile(int zoom, int x, int y)
{
    const wchar_t* host = L"tile.openstreetmap.org";

    wchar_t reqPath[80];
    swprintf_s(reqPath, L"/%d/%d/%d.png", zoom, x, y);

    // tile.openstreetmap.org kullanım politikası geçerli bir uygulama UA'sı gerektirir.
    HINTERNET hSession = WinHttpOpen(
        L"Lumina/1.1 (https://github.com/AykQ/Lumina)",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return {};

    // TLS 1.2/1.3 açıkça etkinleştir (CDN uyumluluğu için)
    DWORD tlsProto = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    WinHttpSetOption(hSession, WINHTTP_OPTION_SECURE_PROTOCOLS, &tlsProto, sizeof(tlsProto));

    // Yönlendirmeleri otomatik takip et
    DWORD rdPolicy = 2; // WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS
    WinHttpSetOption(hSession, WINHTTP_OPTION_REDIRECT_POLICY, &rdPolicy, sizeof(rdPolicy));

    WinHttpSetTimeouts(hSession, 8000, 8000, 15000, 15000);

    HINTERNET hConn = WinHttpConnect(hSession, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
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
        L"Accept: image/png, image/*;q=0.9, */*;q=0.8\r\n",
        static_cast<DWORD>(-1L), WINHTTP_ADDREQ_FLAG_ADD);

    bool ok = WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0) != FALSE
           && WinHttpReceiveResponse(hReq, nullptr) != FALSE;

    // Yalnızca HTTP 200 yanıtlarını oku; 429/403 gibi hata sayfaları PNG değildir
    if (ok)
    {
        DWORD statusCode = 0;
        DWORD scLen = sizeof(statusCode);
        if (!WinHttpQueryHeaders(hReq,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &scLen, nullptr)
            || statusCode != 200)
            ok = false;
    }

    std::vector<uint8_t> pngData;
    if (ok)
    {
        constexpr size_t kMaxTileBytes = 1 * 1024 * 1024;  // 1 MB
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(hReq, &avail) && avail > 0)
        {
            const size_t offset = pngData.size();
            if (offset + avail > kMaxTileBytes) break;
            pngData.resize(offset + avail);
            DWORD read = 0;
            if (!WinHttpReadData(hReq, pngData.data() + offset, avail, &read) || read == 0)
            {
                pngData.resize(offset);
                break;
            }
            pngData.resize(offset + read);
        }
    }

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConn);
    WinHttpCloseHandle(hSession);

    return pngData;
}
