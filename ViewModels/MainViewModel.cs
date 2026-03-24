using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using System.Diagnostics;
using Microsoft.UI.Xaml.Media.Imaging;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using PhotoViewer.Services;

namespace PhotoViewer.ViewModels
{
    public partial class MainViewModel : ObservableObject
    {
        private readonly ImageLoaderService _imageLoader = new();
        private readonly MetadataService _metadataService = new();
        private CancellationTokenSource? _loadCts;
        private IReadOnlyList<string> _folderFiles = Array.Empty<string>();
        private int _currentIndex = -1;

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

        /// <summary>Yükleme / codec mesajı (toolbar altında).</summary>
        [ObservableProperty]
        public partial string? StatusMessage { get; set; }

        /// <summary>Örn. "3 / 42"</summary>
        [ObservableProperty]
        public partial string NavigationInfo { get; set; } = string.Empty;

        [RelayCommand]
        public async Task LoadPhotoAsync(string filePath)
        {
            if (string.IsNullOrEmpty(filePath)) return;

            _loadCts?.Cancel();
            _loadCts?.Dispose();
            _loadCts = new CancellationTokenSource();
            var ct = _loadCts.Token;

            StatusMessage = null;
            DisplayImage = null;

            var fullPath = Path.GetFullPath(filePath);
            _folderFiles = PhotoFolderService.GetSortedImagesInSameFolder(fullPath);
            _currentIndex = PhotoFolderService.IndexOf(_folderFiles, fullPath);
            if (_currentIndex < 0)
            {
                _folderFiles = new[] { fullPath };
                _currentIndex = 0;
            }

            NavigationInfo = _folderFiles.Count > 1
                ? $"{_currentIndex + 1} / {_folderFiles.Count}"
                : string.Empty;

            try
            {
                var bitmap = await _imageLoader.LoadImageAsync(fullPath, ct);
                ct.ThrowIfCancellationRequested();

                if (bitmap is null)
                {
                    StatusMessage = "Görüntü açılamadı (Windows görüntü codec’i veya dosya formatı desteklenmiyor olabilir).";
                    return;
                }

                DisplayImage = bitmap;

                var metadata = await _metadataService.GetExifMetadataAsync(fullPath);

                Camera = metadata.FirstOrDefault(m => m.Label == "Camera")?.Value ?? string.Empty;
                Resolution = metadata.FirstOrDefault(m => m.Label == "Resolution")?.Value ?? string.Empty;
                FileSize = metadata.FirstOrDefault(m => m.Label == "File Size")?.Value ?? string.Empty;
                Format = metadata.FirstOrDefault(m => m.Label == "Format")?.Value ?? string.Empty;
                Coordinates = metadata.FirstOrDefault(m => m.Label == "Coordinates")?.Value ?? string.Empty;
                GpsAltitude = metadata.FirstOrDefault(m => m.Label == "GPS Altitude")?.Value;
            }
            catch (OperationCanceledException)
            {
                // hızlı gezinti — önceki yükleme iptal
            }
            catch (Exception ex)
            {
                Debug.WriteLine($"LoadPhotoAsync error: {ex}");
                StatusMessage = $"Yükleme hatası: {ex.Message}";
            }
        }

        [RelayCommand]
        public async Task NextPhotoAsync()
        {
            if (_folderFiles.Count < 2 || _currentIndex < 0 || _currentIndex >= _folderFiles.Count - 1)
                return;
            await LoadPhotoAsync(_folderFiles[_currentIndex + 1]);
        }

        [RelayCommand]
        public async Task PreviousPhotoAsync()
        {
            if (_folderFiles.Count < 2 || _currentIndex <= 0)
                return;
            await LoadPhotoAsync(_folderFiles[_currentIndex - 1]);
        }
    }
}
