#pragma once

#include <windows.h>
#include <shlwapi.h>   // StrCmpLogicalW — Explorer sıralaması
#include <string>
#include <vector>
#include <algorithm>

#pragma comment(lib, "shlwapi.lib")

// Açık dosyanın bulunduğu klasördeki görüntü dosyalarını yönetir.
// Önceki/sonraki navigasyon, Explorer-uyumlu sıralama ve döngüsel gezinme sağlar.
class FolderNavigator
{
public:
    // filePath: başlangıç dosyasının tam yolu.
    // Yapıcı, aynı klasördeki tüm desteklenen görüntüleri tarar ve sıralar.
    explicit FolderNavigator(const std::wstring& filePath)
    {
        if (filePath.empty()) return;

        auto pos = filePath.find_last_of(L"\\/");
        if (pos == std::wstring::npos) return;

        std::wstring dir = filePath.substr(0, pos);

        // Desteklenen uzantılar (küçük harf — karşılaştırma towlower sonrası yapılır)
        static const wchar_t* kExts[] = {
            L".jpg", L".jpeg", L".png", L".bmp", L".gif",
            L".tiff", L".tif", L".ico", L".webp",
            L".heic", L".heif", L".jxl", L".avif"
        };

        // Klasördeki tüm dosyaları tara
        std::wstring pattern = dir + L"\\*";
        WIN32_FIND_DATAW fd{};
        HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
        if (hFind == INVALID_HANDLE_VALUE) return;

        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

            std::wstring name = fd.cFileName;
            auto dotPos = name.rfind(L'.');
            if (dotPos == std::wstring::npos) continue;

            // Uzantıyı küçük harfe çevir
            std::wstring ext = name.substr(dotPos);
            for (auto& c : ext) c = towlower(c);

            for (auto kExt : kExts)
            {
                if (ext == kExt)
                {
                    m_files.push_back(dir + L"\\" + name);
                    break;
                }
            }
        } while (FindNextFileW(hFind, &fd));

        FindClose(hFind);

        // Explorer-uyumlu sıralama: "image2.jpg" < "image10.jpg"
        std::sort(m_files.begin(), m_files.end(),
            [](const std::wstring& a, const std::wstring& b) {
                return StrCmpLogicalW(a.c_str(), b.c_str()) < 0;
            });

        // Başlangıç dosyasının sıralı listedeki indeksini bul
        for (int i = 0; i < static_cast<int>(m_files.size()); ++i)
        {
            if (_wcsicmp(m_files[i].c_str(), filePath.c_str()) == 0)
            {
                m_index = i;
                break;
            }
        }
    }

    bool empty() const { return m_files.empty(); }
    int  total() const { return static_cast<int>(m_files.size()); }
    int  index() const { return m_index; }

    // Geçerli dosyanın tam yolunu döner
    const std::wstring& current() const
    {
        static const std::wstring kEmpty;
        return m_files.empty() ? kEmpty : m_files[m_index];
    }

    // Sonraki dosyaya geç (son → ilk döngüsel)
    const std::wstring& next()
    {
        if (!m_files.empty())
            m_index = (m_index + 1) % static_cast<int>(m_files.size());
        return current();
    }

    // Önceki dosyaya geç (ilk → son döngüsel)
    const std::wstring& prev()
    {
        if (!m_files.empty())
            m_index = (m_index - 1 + static_cast<int>(m_files.size())) % static_cast<int>(m_files.size());
        return current();
    }

private:
    std::vector<std::wstring> m_files;
    int                       m_index = 0;
};
