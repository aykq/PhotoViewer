# Project Brief

PhotoViewer, Windows masaüstü için geliştirilmiş modern ve **hızlı** bir fotoğraf **görüntüleyici** uygulamasıdır.

## Core Requirements

- Hızlı ve performanslı fotoğraf görüntüleme (WIC / sistem codec’leri)
- EXIF/metadata görüntüleme
- Yakınlaştırma ve kaydırma (pan)
- Aynı klasörde **hızlı gezinti** (önceki/sonraki, klavye okları)
- Tekil çalışma (single-instance)

## Scope

- Windows 10/11 desktop uygulaması
- WinUI 3 kullanıcı arayüzü
- **Görüntü decode:** `Windows.Graphics.Imaging.BitmapDecoder` (Magick yok)
- MetadataExtractor ile EXIF okuma
- OpenStreetMap harita entegrasyonu (metadata paneli koordinatları için)

## Goals

- Kullanıcı dostu, modern ve **düşük gecikmeli** fotoğraf görüntüleyici
- EXIF verilerini kolayca keşfetmek
- Dizin içinde rahat albüm gezintisi

## Out of scope (şu an)

- Döndürme, kırpma, diske yazma, `.bak` yedekleme (kaldırıldı)
