#include "FolderNavigator.h"

#include <windows.h>
#include <shlwapi.h>
#include <algorithm>

#pragma comment(lib, "shlwapi.lib")

static const wchar_t* kSupportedExts[] = {
    L".jpg", L".jpeg", L".png", L".bmp", L".gif",
    L".tiff", L".tif", L".ico", L".webp",
    L".heic", L".heif", L".jxl", L".avif"
};

static bool IsSupportedExtension(const std::wstring& name)
{
    auto dotPos = name.rfind(L'.');
    if (dotPos == std::wstring::npos) return false;
    std::wstring ext = name.substr(dotPos);
    for (auto& c : ext) c = towlower(c);
    for (auto kExt : kSupportedExts)
        if (ext == kExt) return true;
    return false;
}

static std::vector<std::wstring> ScanDirectory(const std::wstring& dir)
{
    std::vector<std::wstring> files;
    std::wstring pattern = dir + L"\\*";
    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return files;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring name = fd.cFileName;
        if (IsSupportedExtension(name))
            files.push_back(dir + L"\\" + name);
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);
    std::sort(files.begin(), files.end(),
        [](const std::wstring& a, const std::wstring& b) {
            return StrCmpLogicalW(a.c_str(), b.c_str()) < 0;
        });
    return files;
}

FolderNavigator::FolderNavigator(const std::wstring& filePath)
{
    if (filePath.empty()) return;

    auto pos = filePath.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return;

    m_directory = filePath.substr(0, pos);
    m_files = ScanDirectory(m_directory);

    for (int i = 0; i < static_cast<int>(m_files.size()); ++i)
    {
        if (_wcsicmp(m_files[i].c_str(), filePath.c_str()) == 0)
        {
            m_index = i;
            break;
        }
    }
}

std::wstring FolderNavigator::refresh()
{
    if (m_directory.empty()) return {};

    std::wstring cur = m_files.empty() ? L"" : m_files[m_index];
    m_files = ScanDirectory(m_directory);

    if (m_files.empty()) return {};

    for (int i = 0; i < static_cast<int>(m_files.size()); ++i)
    {
        if (_wcsicmp(m_files[i].c_str(), cur.c_str()) == 0)
        {
            m_index = i;
            return m_files[m_index];
        }
    }

    // Mevcut dosya silindi — eski indekse en yakın konuma git
    if (m_index >= static_cast<int>(m_files.size()))
        m_index = static_cast<int>(m_files.size()) - 1;
    if (m_index < 0) m_index = 0;
    return m_files[m_index];
}

const std::wstring& FolderNavigator::peek_next() const
{
    static const std::wstring kEmpty;
    if (m_files.empty()) return kEmpty;
    int idx = (m_index + 1) % static_cast<int>(m_files.size());
    return m_files[idx];
}

const std::wstring& FolderNavigator::peek_prev() const
{
    static const std::wstring kEmpty;
    if (m_files.empty()) return kEmpty;
    int idx = (m_index - 1 + static_cast<int>(m_files.size())) % static_cast<int>(m_files.size());
    return m_files[idx];
}

const std::wstring& FolderNavigator::peek_at(int offset) const
{
    static const std::wstring kEmpty;
    if (m_files.empty()) return kEmpty;
    int n   = static_cast<int>(m_files.size());
    int idx = ((m_index + offset) % n + n) % n;
    return m_files[idx];
}

const std::wstring& FolderNavigator::peek_at_linear(int offset) const
{
    static const std::wstring kEmpty;
    if (m_files.empty()) return kEmpty;
    int idx = m_index + offset;
    if (idx < 0 || idx >= static_cast<int>(m_files.size())) return kEmpty;
    return m_files[idx];
}

const std::wstring& FolderNavigator::jump(int offset)
{
    if (!m_files.empty())
    {
        int n   = static_cast<int>(m_files.size());
        m_index = ((m_index + offset) % n + n) % n;
    }
    return current();
}

const std::wstring& FolderNavigator::current() const
{
    static const std::wstring kEmpty;
    return m_files.empty() ? kEmpty : m_files[m_index];
}

const std::wstring& FolderNavigator::next()
{
    if (!m_files.empty())
        m_index = (m_index + 1) % static_cast<int>(m_files.size());
    return current();
}

const std::wstring& FolderNavigator::prev()
{
    if (!m_files.empty())
        m_index = (m_index - 1 + static_cast<int>(m_files.size())) % static_cast<int>(m_files.size());
    return current();
}
