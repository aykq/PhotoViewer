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
        private readonly IImageLoaderService _imageLoader;
        private readonly IMetadataService _metadataService;

        public MainViewModel(IImageLoaderService imageLoader, IMetadataService metadataService)
        {
            _imageLoader = imageLoader;
            _metadataService = metadataService;
        }
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

        /// <summary>Pencere başlığı: dosya adı + uygulama adı.</summary>
        [ObservableProperty]
        public partial string WindowTitle { get; set; } = "Photo Viewer";

        /// <summary>Sadece dosya adı ve uzantısı (bilgi paneli).</summary>
        [ObservableProperty]
        public partial string FileDisplayName { get; set; } = string.Empty;

        /// <summary>WIC yok / ImageMagick vb.</summary>
        [ObservableProperty]
        public partial string? StatusMessage { get; set; }

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
            var fileName = Path.GetFileName(fullPath);
            FileDisplayName = fileName;
            WindowTitle = $"{fileName} — Photo Viewer";

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
                var outcome = await _imageLoader.LoadImageAsync(fullPath, ct);
                ct.ThrowIfCancellationRequested();

                if (outcome.Bitmap is null)
                {
                    StatusMessage = "Görüntü açılamadı (Windows codec ve ImageMagick bu dosyayı okuyamadı).";
                    return;
                }

                DisplayImage = outcome.Bitmap;

                if (outcome.UsedMagickFallback)
                    StatusMessage = "Windows bu dosyayı doğrudan açamadı; ImageMagick ile gösteriliyor.";
                else
                    StatusMessage = null;

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
                StatusMessage = "Görüntü yüklenemedi.";
            }
        }

        [RelayCommand]
        public async Task NextPhotoAsync()
        {
            if (_folderFiles.Count < 2 || _currentIndex < 0) return;
            int next = _currentIndex >= _folderFiles.Count - 1 ? 0 : _currentIndex + 1;
            await LoadPhotoAsync(_folderFiles[next]);
        }

        [RelayCommand]
        public async Task PreviousPhotoAsync()
        {
            if (_folderFiles.Count < 2 || _currentIndex < 0) return;
            int prev = _currentIndex <= 0 ? _folderFiles.Count - 1 : _currentIndex - 1;
            await LoadPhotoAsync(_folderFiles[prev]);
        }
    }
}
