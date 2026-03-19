using System;
using System.IO;
using System.Threading.Tasks;
using Microsoft.UI.Xaml.Media.Imaging;
using ImageMagick;
using System.Runtime.InteropServices.WindowsRuntime;

namespace PhotoViewer.Services
{
    public class ImageLoaderService
    {
        public async Task<WriteableBitmap> LoadImageAsync(string filePath)
        {
            // 1. Resim verisini arka planda oku (Ağır iş burası)
            var imageData = await Task.Run(() =>
            {
                using (var image = new MagickImage(filePath))
                {
                    // Keep UI consistent with what we'll later save/crop/rotate.
                    image.AutoOrient();
                    image.Format = MagickFormat.Bgra;
                    return new
                    {
                        Pixels = image.ToByteArray(MagickFormat.Bgra),
                        Width = (int)image.Width,
                        Height = (int)image.Height
                    };
                }
            });

            // 2. Bitmap nesnesini UI Thread üzerinde oluştur (Hatanın çözümü)
            var bitmap = new WriteableBitmap(imageData.Width, imageData.Height);

            using (Stream stream = bitmap.PixelBuffer.AsStream())
            {
                await stream.WriteAsync(imageData.Pixels, 0, imageData.Pixels.Length);
            }

            return bitmap;
        }
    }
}