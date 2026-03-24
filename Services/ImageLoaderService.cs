using System;
using System.IO;
using System.Runtime.InteropServices.WindowsRuntime;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.Graphics.Imaging;
using Windows.Storage;

namespace PhotoViewer.Services
{
    /// <summary>
    /// Windows Imaging Component (WIC) ile hızlı decode; büyük görsellerde uzun kenar sınırı.
    /// </summary>
    public class ImageLoaderService
    {
        private const int MaxDisplayLongEdge = 4096;

        public async Task<WriteableBitmap?> LoadImageAsync(string filePath, CancellationToken cancellationToken = default)
        {
            StorageFile file;
            try
            {
                file = await StorageFile.GetFileFromPathAsync(filePath);
            }
            catch
            {
                return null;
            }

            using var stream = await file.OpenAsync(FileAccessMode.Read);
            cancellationToken.ThrowIfCancellationRequested();

            BitmapDecoder decoder;
            try
            {
                decoder = await BitmapDecoder.CreateAsync(stream);
            }
            catch
            {
                // WIC codec yok (ör. bazı JXL ortamları) veya bozuk dosya
                return null;
            }

            cancellationToken.ThrowIfCancellationRequested();

            uint w = decoder.OrientedPixelWidth;
            uint h = decoder.OrientedPixelHeight;
            if (w == 0 || h == 0)
                return null;

            var transform = new BitmapTransform
            {
                InterpolationMode = BitmapInterpolationMode.Fant
            };

            if (Math.Max(w, h) > MaxDisplayLongEdge)
            {
                double scale = MaxDisplayLongEdge / (double)Math.Max(w, h);
                transform.ScaledWidth = (uint)Math.Max(1, Math.Round(w * scale));
                transform.ScaledHeight = (uint)Math.Max(1, Math.Round(h * scale));
            }
            else
            {
                transform.ScaledWidth = w;
                transform.ScaledHeight = h;
            }

            PixelDataProvider pixelProvider;
            try
            {
                pixelProvider = await decoder.GetPixelDataAsync(
                    BitmapPixelFormat.Bgra8,
                    BitmapAlphaMode.Premultiplied,
                    transform,
                    ExifOrientationMode.RespectExifOrientation,
                    ColorManagementMode.ColorManageToSRgb);
            }
            catch
            {
                return null;
            }

            cancellationToken.ThrowIfCancellationRequested();

            var pixels = pixelProvider.DetachPixelData();

            int outW = (int)transform.ScaledWidth;
            int outH = (int)transform.ScaledHeight;
            if (outW <= 0 || outH <= 0 || pixels.Length < outW * outH * 4)
                return null;

            var bitmap = new WriteableBitmap(outW, outH);
            using (var dest = bitmap.PixelBuffer.AsStream())
            {
                await dest.WriteAsync(pixels, 0, pixels.Length);
            }

            return bitmap;
        }
    }
}
