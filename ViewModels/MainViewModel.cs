using System;
using System.Collections.ObjectModel;
using System.Threading.Tasks;
using Microsoft.UI.Xaml.Media.Imaging;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using System.Collections.Generic;
using PhotoViewer.Models;
using PhotoViewer.Services;
using System.Linq;

namespace PhotoViewer.ViewModels
{
    // 'partial' anahtar kelimesi Toolkit'in kod üretmesi için şarttır.
    public partial class MainViewModel : ObservableObject
    {
        private readonly ImageLoaderService _imageLoader;
        private readonly MetadataService _metadataService;

        // private ve alt çizgili field kullanıyoruz.
        // Toolkit buradan otomatik olarak 'DisplayImage' üretecek.
        [ObservableProperty]
        private WriteableBitmap? _displayImage;

        [ObservableProperty]
        private string? _camera;

        [ObservableProperty]
        private string? _focalLength;

        [ObservableProperty]
        private string? _aperture;

        [ObservableProperty]
        private string? _taken;

        [ObservableProperty]
        private string? _mapImageUri;

        [ObservableProperty]
        private string? _resolution;

        [ObservableProperty]
        private string? _megapixels;

        [ObservableProperty]
        private string? _fileSize;

        [ObservableProperty]
        private string? _format;

        [ObservableProperty]
        private string? _coordinates;

        [ObservableProperty]
        private string? _gpsAltitude;

        public ObservableCollection<ExifData> ExifMetadata { get; } = new();

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
                // image loading

                DisplayImage = await _imageLoader.LoadImageAsync(filePath);

                ExifMetadata.Clear();
                var metadata = _metadataService.GetExifMetadata(filePath);

                // populate prioritized properties if present
                Camera = metadata.FirstOrDefault(m => m.Label == "Camera")?.Value ?? string.Empty;
                FocalLength = metadata.FirstOrDefault(m => m.Label == "Focal Length")?.Value ?? string.Empty;
                Aperture = metadata.FirstOrDefault(m => m.Label == "Aperture")?.Value ?? string.Empty;
                // Format taken date as 'gün / ay / yıl (gün adı)'
                var takenRaw = metadata.FirstOrDefault(m => m.Label == "Taken")?.Value;
                if (!string.IsNullOrEmpty(takenRaw))
                {
                    DateTime dt;
                    // EXIF date format: yyyy:MM:dd HH:mm:ss
                    string normalized = takenRaw.Replace('-', '/').Replace('.', '/');
                    if (normalized.Count(c => c == ':') >= 2 && normalized.Contains(" "))
                    {
                        // Replace only first two ':' with '/'
                        int first = normalized.IndexOf(':');
                        int second = normalized.IndexOf(':', first + 1);
                        if (first >= 0 && second > first)
                        {
                            normalized = normalized.Remove(second, 1).Insert(second, "/");
                            normalized = normalized.Remove(first, 1).Insert(first, "/");
                        }
                    }
                    if (DateTime.TryParse(normalized, out dt))
                    {
                        Taken = dt.ToString("dd/MM/yyyy dddd HH:mm:ss");
                    }
                    else
                    {
                        Taken = takenRaw;
                    }
                }
                else
                {
                    Taken = null;
                }
                Resolution = metadata.FirstOrDefault(m => m.Label == "Resolution")?.Value ?? string.Empty;
                Megapixels = metadata.FirstOrDefault(m => m.Label == "Megapixels")?.Value ?? string.Empty;
                Format = metadata.FirstOrDefault(m => m.Label == "Format")?.Value ?? string.Empty;
                FileSize = metadata.FirstOrDefault(m => m.Label == "File Size")?.Value ?? string.Empty;
                Coordinates = metadata.FirstOrDefault(m => m.Label == "Coordinates")?.Value ?? string.Empty;
                // Set map image URI if coordinates available
                if (!string.IsNullOrEmpty(Coordinates))
                {
                    var parts = Coordinates.Split(',');
                    if (parts.Length == 2 &&
                        double.TryParse(parts[0].Trim(), System.Globalization.NumberStyles.Any, System.Globalization.CultureInfo.InvariantCulture, out var lat) &&
                        double.TryParse(parts[1].Trim(), System.Globalization.NumberStyles.Any, System.Globalization.CultureInfo.InvariantCulture, out var lon))
                    {
                        MapImageUri = $"https://staticmap.openstreetmap.de/staticmap.php?center={lat},{lon}&zoom=14&size=300x180&markers={lat},{lon},red-pushpin";
                    }
                    else
                    {
                        MapImageUri = null;
                    }
                }
                else
                {
                    MapImageUri = null;
                }
                GpsAltitude = metadata.FirstOrDefault(m => m.Label == "GPS Altitude")?.Value;

                // Exclude prioritized labels from the remaining list
                var exclude = new HashSet<string>(new[] {
                    "Camera","Focal Length","Aperture","Taken","Resolution","Megapixels","Format","File Size","Coordinates","GPS Altitude"
                }, StringComparer.OrdinalIgnoreCase);

                foreach (var item in metadata)
                {
                    if (exclude.Contains(item.Label)) continue;
                    ExifMetadata.Add(item);
                }

                // completed
            }
            catch (Exception ex)
            {
                // report error via debug/log; do not expose raw exception text to UI
                System.Diagnostics.Debug.WriteLine($"LoadPhotoAsync error: {ex}");
            }
        }
    }
}