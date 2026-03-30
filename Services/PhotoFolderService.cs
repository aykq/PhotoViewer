using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;

namespace PhotoViewer.Services
{
    /// <summary>
    /// Aynı klasördeki görüntü dosyalarını listeler.
    /// Sıra: Windows Gezgini "Ad" sütunu ile uyumlu mantıksal sıralama (StrCmpLogicalW).
    /// </summary>
    public static class PhotoFolderService
    {
        private static readonly HashSet<string> ImageExtensions = new(StringComparer.OrdinalIgnoreCase)
        {
            ".jpg", ".jpeg", ".jfif", ".png", ".gif", ".bmp", ".webp",
            ".tif", ".tiff", ".heic", ".heif", ".jxl", ".avif", ".ico"
        };

        [DllImport("shlwapi.dll", CharSet = CharSet.Unicode)]
        private static extern int StrCmpLogicalW(string? psz1, string? psz2);

        /// <summary>Gezgin ile aynı dosya adı sıralaması (IMG2 &lt; IMG10).</summary>
        public static int CompareExplorerFileName(string pathA, string pathB)
        {
            var nameA = Path.GetFileName(pathA) ?? "";
            var nameB = Path.GetFileName(pathB) ?? "";
            int cmp = StrCmpLogicalW(nameA, nameB);
            if (cmp != 0)
                return cmp;
            return StringComparer.OrdinalIgnoreCase.Compare(pathA, pathB);
        }

        public static bool IsSupportedImageExtension(string path)
        {
            var ext = Path.GetExtension(path);
            return !string.IsNullOrEmpty(ext) && ImageExtensions.Contains(ext);
        }

        public static IReadOnlyList<string> GetSortedImagesInSameFolder(string filePath)
        {
            var full = Path.GetFullPath(filePath);
            var dir = Path.GetDirectoryName(full);
            if (string.IsNullOrEmpty(dir) || !Directory.Exists(dir))
                return new[] { full };

            var list = new List<string>();
            foreach (var f in Directory.EnumerateFiles(dir))
            {
                if (IsSupportedImageExtension(f))
                    list.Add(Path.GetFullPath(f));
            }

            list.Sort(CompareExplorerFileName);

            if (list.Count == 0)
                return new[] { full };

            return list;
        }

        public static int IndexOf(IReadOnlyList<string> files, string filePath)
        {
            var full = Path.GetFullPath(filePath);
            for (int i = 0; i < files.Count; i++)
            {
                if (string.Equals(files[i], full, StringComparison.OrdinalIgnoreCase))
                    return i;
            }
            return -1;
        }
    }
}
