using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

namespace PhotoViewer.Services
{
    /// <summary>
    /// Aynı klasördeki görüntü dosyalarını listeler (sıralı, hızlı gezinti için).
    /// </summary>
    public static class PhotoFolderService
    {
        private static readonly HashSet<string> ImageExtensions = new(StringComparer.OrdinalIgnoreCase)
        {
            ".jpg", ".jpeg", ".jfif", ".png", ".gif", ".bmp", ".webp",
            ".tif", ".tiff", ".heic", ".heif", ".jxl", ".avif", ".ico"
        };

        public static bool IsSupportedImageExtension(string path)
        {
            var ext = Path.GetExtension(path);
            return !string.IsNullOrEmpty(ext) && ImageExtensions.Contains(ext);
        }

        /// <summary>
        /// Verilen dosyanın bulunduğu klasördeki tüm görüntüleri alfabetik sıralar.
        /// </summary>
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

            list.Sort(StringComparer.OrdinalIgnoreCase);

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
