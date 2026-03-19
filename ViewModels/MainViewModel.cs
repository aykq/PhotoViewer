using System;
using System.Linq;
using System.Threading.Tasks;
using Microsoft.UI.Xaml.Media.Imaging;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using PhotoViewer.Services;

namespace PhotoViewer.ViewModels
{
    public partial class MainViewModel : ObservableObject
    {
        private readonly ImageLoaderService _imageLoader;
        private readonly MetadataService _metadataService;

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

        public MainViewModel()
        {
            _imageLoader = new ImageLoaderService();
            _metadataService = new MetadataService();
        }

        [RelayCommand]
        public async Task LoadPhotoAsync(string filePath)
        {
            if (string.IsNullOrEmpty(filePath)) return;

            try
            {
                DisplayImage = await _imageLoader.LoadImageAsync(filePath);

                var metadata = _metadataService.GetExifMetadata(filePath);

                Camera = metadata.FirstOrDefault(m => m.Label == "Camera")?.Value ?? string.Empty;
                Resolution = metadata.FirstOrDefault(m => m.Label == "Resolution")?.Value ?? string.Empty;
                FileSize = metadata.FirstOrDefault(m => m.Label == "File Size")?.Value ?? string.Empty;
                Format = metadata.FirstOrDefault(m => m.Label == "Format")?.Value ?? string.Empty;
                Coordinates = metadata.FirstOrDefault(m => m.Label == "Coordinates")?.Value ?? string.Empty;
                GpsAltitude = metadata.FirstOrDefault(m => m.Label == "GPS Altitude")?.Value;
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"LoadPhotoAsync error: {ex}");
            }
        }
    }
}
