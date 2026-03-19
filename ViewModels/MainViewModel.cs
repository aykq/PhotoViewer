using System;
using System.Linq;
using System.IO;
using System.Threading.Tasks;
using System.Diagnostics;
using Windows.Foundation;
using Microsoft.UI.Xaml.Media.Imaging;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using PhotoViewer.Services;
using ImageMagick;

namespace PhotoViewer.ViewModels
{
    public partial class MainViewModel : ObservableObject
    {
        private readonly ImageLoaderService _imageLoader;
        private readonly MetadataService _metadataService;
        private string? _currentFilePath;
        private Rect _pendingCropRectNormalized = new Rect(0, 0, 1, 1);

        [ObservableProperty]
        public partial WriteableBitmap? DisplayImage { get; set; }

        [ObservableProperty]
        public partial string? Camera { get; set; }

        [ObservableProperty]
        public partial string? Resolution { get; set; }

        [ObservableProperty]
        public partial string? FileSize { get; set; }

        [ObservableProperty]
        public partial string? Format { get; set; }

        [ObservableProperty]
        public partial string? Coordinates { get; set; }

        [ObservableProperty]
        public partial string? GpsAltitude { get; set; }

        [ObservableProperty]
        public partial bool IsCropMode { get; set; }

        public MainViewModel()
        {
            _imageLoader = new ImageLoaderService();
            _metadataService = new MetadataService();
        }

        [RelayCommand]
        public async Task LoadPhotoAsync(string filePath)
        {
            if (string.IsNullOrEmpty(filePath)) return;

            _currentFilePath = filePath;
            IsCropMode = false;
            _pendingCropRectNormalized = new Rect(0, 0, 1, 1);

            try
            {
                DisplayImage = await _imageLoader.LoadImageAsync(_currentFilePath);

                var metadata = _metadataService.GetExifMetadata(_currentFilePath);

                Camera = metadata.FirstOrDefault(m => m.Label == "Camera")?.Value ?? string.Empty;
                Resolution = metadata.FirstOrDefault(m => m.Label == "Resolution")?.Value ?? string.Empty;
                FileSize = metadata.FirstOrDefault(m => m.Label == "File Size")?.Value ?? string.Empty;
                Format = metadata.FirstOrDefault(m => m.Label == "Format")?.Value ?? string.Empty;
                Coordinates = metadata.FirstOrDefault(m => m.Label == "Coordinates")?.Value ?? string.Empty;
                GpsAltitude = metadata.FirstOrDefault(m => m.Label == "GPS Altitude")?.Value;
            }
            catch (Exception ex)
            {
                Debug.WriteLine($"LoadPhotoAsync error: {ex}");
            }
        }

        public void SetCropRectNormalized(Rect rect)
        {
            // Clamp to valid [0..1] normalized rectangle.
            var x = Math.Clamp(rect.X, 0, 1);
            var y = Math.Clamp(rect.Y, 0, 1);
            var w = Math.Clamp(rect.Width, 0, 1);
            var h = Math.Clamp(rect.Height, 0, 1);

            if (x + w > 1) w = 1 - x;
            if (y + h > 1) h = 1 - y;

            _pendingCropRectNormalized = new Rect(x, y, Math.Max(0, w), Math.Max(0, h));
        }

        // --- Phase 1 toolbar commands ---
        [RelayCommand]
        public async Task RotateLeft()
        {
            Debug.WriteLine($"RotateLeft clicked. IsCropMode={IsCropMode}");
            if (IsCropMode) return;
            await RotateByAsync(-90);
        }

        [RelayCommand]
        public async Task RotateRight()
        {
            Debug.WriteLine($"RotateRight clicked. IsCropMode={IsCropMode}");
            if (IsCropMode) return;
            await RotateByAsync(90);
        }

        [RelayCommand]
        public async Task CropToggle()
        {
            Debug.WriteLine($"CropToggle clicked. IsCropMode={IsCropMode}, pendingRect={_pendingCropRectNormalized}");
            if (string.IsNullOrEmpty(_currentFilePath) || !File.Exists(_currentFilePath))
                return;

            if (!IsCropMode)
            {
                Debug.WriteLine("Entering crop mode.");
                IsCropMode = true;
                return;
            }

            Debug.WriteLine($"Applying crop with rect={_pendingCropRectNormalized} on file={_currentFilePath}");
            await ApplyCropAsync();
            IsCropMode = false;
        }

        private async Task RotateByAsync(int degrees)
        {
            if (string.IsNullOrEmpty(_currentFilePath) || !File.Exists(_currentFilePath))
                return;

            try
            {
                Debug.WriteLine($"Rotating file by {degrees} degrees: {_currentFilePath}");
                await Task.Run(() =>
                {
                    using var image = new MagickImage(_currentFilePath);
                    image.AutoOrient();
                    image.Rotate(degrees);
                    // Keep cropped/rotated page info consistent for writing back.
                    image.ResetPage();
                    image.Write(_currentFilePath);
                });

                DisplayImage = await _imageLoader.LoadImageAsync(_currentFilePath);

                var metadata = _metadataService.GetExifMetadata(_currentFilePath);
                Camera = metadata.FirstOrDefault(m => m.Label == "Camera")?.Value ?? string.Empty;
                Resolution = metadata.FirstOrDefault(m => m.Label == "Resolution")?.Value ?? string.Empty;
                FileSize = metadata.FirstOrDefault(m => m.Label == "File Size")?.Value ?? string.Empty;
                Format = metadata.FirstOrDefault(m => m.Label == "Format")?.Value ?? string.Empty;
                Coordinates = metadata.FirstOrDefault(m => m.Label == "Coordinates")?.Value ?? string.Empty;
                GpsAltitude = metadata.FirstOrDefault(m => m.Label == "GPS Altitude")?.Value;
            }
            catch (Exception ex)
            {
                Debug.WriteLine($"RotateByAsync error: {ex}");
            }
        }

        private async Task ApplyCropAsync()
        {
            if (string.IsNullOrEmpty(_currentFilePath) || !File.Exists(_currentFilePath))
                return;

            var r = _pendingCropRectNormalized;
            if (r.Width <= 0.001 || r.Height <= 0.001) return;

            try
            {
                await Task.Run(() =>
                {
                    using var image = new MagickImage(_currentFilePath);
                    image.AutoOrient();

                    uint imgW = image.Width;
                    uint imgH = image.Height;

                    int imgWi = (int)imgW;
                    int imgHi = (int)imgH;

                    int x = (int)Math.Round(r.X * imgWi);
                    int y = (int)Math.Round(r.Y * imgHi);
                    int w = (int)Math.Round(r.Width * imgWi);
                    int h = (int)Math.Round(r.Height * imgHi);

                    x = Math.Clamp(x, 0, Math.Max(0, imgWi - 1));
                    y = Math.Clamp(y, 0, Math.Max(0, imgHi - 1));
                    w = Math.Clamp(w, 1, imgWi - x);
                    h = Math.Clamp(h, 1, imgHi - y);

                    image.Crop(new MagickGeometry(x, y, (uint)w, (uint)h));
                    image.ResetPage();
                    image.Write(_currentFilePath);
                });

                Debug.WriteLine($"Crop applied. New file loaded from {_currentFilePath}");
                DisplayImage = await _imageLoader.LoadImageAsync(_currentFilePath);

                var metadata = _metadataService.GetExifMetadata(_currentFilePath);
                Camera = metadata.FirstOrDefault(m => m.Label == "Camera")?.Value ?? string.Empty;
                Resolution = metadata.FirstOrDefault(m => m.Label == "Resolution")?.Value ?? string.Empty;
                FileSize = metadata.FirstOrDefault(m => m.Label == "File Size")?.Value ?? string.Empty;
                Format = metadata.FirstOrDefault(m => m.Label == "Format")?.Value ?? string.Empty;
                Coordinates = metadata.FirstOrDefault(m => m.Label == "Coordinates")?.Value ?? string.Empty;
                GpsAltitude = metadata.FirstOrDefault(m => m.Label == "GPS Altitude")?.Value;
            }
            catch (Exception ex)
            {
                Debug.WriteLine($"ApplyCropAsync error: {ex}");
            }
        }
    }
}
