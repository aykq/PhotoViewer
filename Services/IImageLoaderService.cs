using System.Threading;
using System.Threading.Tasks;
using Microsoft.UI.Xaml.Media.Imaging;

namespace PhotoViewer.Services
{
    public sealed class LoadOutcome
    {
        public WriteableBitmap? Bitmap { get; init; }
        public bool UsedMagickFallback { get; init; }
    }

    public interface IImageLoaderService
    {
        Task<LoadOutcome> LoadImageAsync(string filePath, CancellationToken cancellationToken = default);
    }
}
