#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>

// Nominatim reverse geocoding: koordinatları konum adına çevirir.
// Başarılıysa L"Şehir, İl, Ülke" formatında döner; hata durumunda boş wstring.
std::wstring FetchLocationName(double latDecimal, double lonDecimal);

// Web Mercator tile koordinatını hesaplar (zoom 0–19).
// tx, ty: OSM tile indeksleri; [0, 2^zoom) aralığında clamp edilir.
void LatLonToTileXY(double lat, double lon, int zoom, int& tx, int& ty);

// Tile içindeki piksel ofsetini hesaplar (tile boyutu 256×256 kabul edilir).
// px, py: tile'ın sol üst köşesine göre [0, 255] aralığında piksel konumu.
void LatLonToPixelInTile(double lat, double lon, int zoom, int tileX, int tileY,
                         int& px, int& py);

// WinHTTP ile tile.openstreetmap.org'dan PNG tile indirir.
// Başarısızsa boş vector döner.
std::vector<uint8_t> FetchOsmTile(int zoom, int x, int y);
