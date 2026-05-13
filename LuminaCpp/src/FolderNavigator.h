#pragma once

#include <string>
#include <vector>

// Açık dosyanın bulunduğu klasördeki görüntü dosyalarını yönetir.
// Önceki/sonraki navigasyon, Explorer-uyumlu sıralama ve döngüsel gezinme sağlar.
class FolderNavigator
{
public:
    explicit FolderNavigator(const std::wstring& filePath);

    bool                empty()     const { return m_files.empty(); }
    int                 total()     const { return static_cast<int>(m_files.size()); }
    int                 index()     const { return m_index; }
    const std::wstring& directory() const { return m_directory; }

    // Dizini yeniden tara; mevcut konumu korur, silinmişse en yakın dosyaya geçer.
    std::wstring refresh();

    const std::wstring& peek_next()             const;
    const std::wstring& peek_prev()             const;
    const std::wstring& peek_at(int offset)     const;
    const std::wstring& peek_at_linear(int offset) const;

    const std::wstring& jump(int offset);
    const std::wstring& current() const;
    const std::wstring& next();
    const std::wstring& prev();

private:
    std::vector<std::wstring> m_files;
    int                       m_index = 0;
    std::wstring              m_directory;
};
