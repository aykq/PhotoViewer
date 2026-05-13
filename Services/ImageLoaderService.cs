using System;
using System.IO;
using System.Runtime.InteropServices.WindowsRuntime;
using System.Threading;
using System.Threading.Tasks;
using ImageMagick;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.Graphics.Imaging;
using Windows.Storage;

namespace PhotoViewer.Services
{
    /// <summary>
    /// Önce WIC (hızlı); olmazsa ImageMagick (geniş format desteği).
    /// </summary>
    public class ImageLoaderService : IImageLoaderService
    {
        private const int MaxDisplayLongEdge = 4096;

        public async Task<LoadOutcome> LoadImageAsync(string filePath, CancellationToken cancellationToken = default)
        {
            var wic = await TryLoadWithWicAsync(filePath, cancellationToken).ConfigureAwait(true);
            if (wic is not null)
                return new LoadOutcome { Bitmap = wic, UsedMagickFallback = false };

            cancellationToken.ThrowIfCancellationRequested();
            return await TryLoadWithMagickAsync(filePath, cancellationToken).ConfigureAwait(true);
        }

        private static async Task<WriteableBitmap?> TryLoadWithWicAsync(string filePath, CancellationToken cancellationToken)
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

        /// <summary>Magick decode arka planda; WriteableBitmap UI iş parçacığında oluşturulur.</summary>
        private static async Task<LoadOutcome> TryLoadWithMagickAsync(string filePath, CancellationToken cancellationToken)
        {
            byte[]? pixels;
            int outW;
            int outH;

            try
            {
                (pixels, outW, outH) = await Task.Run(
                    () => DecodeWithMagickPixels(filePath, cancellationToken),
                    cancellationToken).ConfigureAwait(true);
            }
            catch (OperationCanceledException)
            {
                throw;
            }

            cancellationToken.ThrowIfCancellationRequested();

            if (pixels is null || outW <= 0 || outH <= 0)
                return new LoadOutcome { Bitmap = null, UsedMagickFallback = false };

            if (pixels.Length < outW * outH * 4)
                return new LoadOutcome { Bitmap = null, UsedMagickFallback = false };

            var bitmap = new WriteableBitmap(outW, outH);
            using (var dest = bitmap.PixelBuffer.AsStream())
            {
                await dest.WriteAsync(pixels, 0, pixels.Length).ConfigureAwait(true);
            }

            return new LoadOutcome { Bitmap = bitmap, UsedMagickFallback = true };
        }

        private static (byte[]? Pixels, int Width, int Height) DecodeWithMagickPixels(string filePath, CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();

            if (!File.Exists(filePath))
                return (null, 0, 0);

            try
            {
                var info = new MagickImageInfo(filePath);
                if (info.Width > 16384 || info.Height > 16384)
                    return (null, 0, 0);

                var readSettings = CreateMagickReadSettings(filePath);
                using var fs = new FileStream(filePath, FileMode.Open, FileAccess.Read, FileShare.Read);
                using var image = new MagickImage(fs, readSettings);
                image.AutoOrient();

                if (Math.Max(image.Width, image.Height) > MaxDisplayLongEdge)
                {
                    var geo = new MagickGeometry(MaxDisplayLongEdge, MaxDisplayLongEdge)
                    {
                        IgnoreAspectRatio = false,
                        Greater = true
                    };
                    image.Resize(geo);
                }

                image.ResetPage();
                image.Format = MagickFormat.Bgra;
                var pixels = image.ToByteArray(MagickFormat.Bgra);
                int outW = (int)image.Width;
                int outH = (int)image.Height;

                cancellationToken.ThrowIfCancellationRequested();

                return (pixels, outW, outH);
            }
            catch (OperationCanceledException)
            {
                throw;
            }
            catch
            {
                return (null, 0, 0);
            }
        }

        private static MagickReadSettings CreateMagickReadSettings(string filePath)
        {
            var settings = new MagickReadSettings();
            var ext = Path.GetExtension(filePath).ToLowerInvariant();
            switch (ext)
            {
                case ".heic":
                case ".heif":
                    settings.Format = MagickFormat.Heic;
                    break;
                case ".jxl":
                    settings.Format = MagickFormat.Jxl;
                    break;
                case ".webp":
                    settings.Format = MagickFormat.WebP;
                    break;
            }

            return settings;
        }
    }
}
