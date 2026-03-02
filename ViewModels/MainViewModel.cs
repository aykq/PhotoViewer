using System;
using System.Collections.ObjectModel;
using System.Threading.Tasks;
using Microsoft.UI.Xaml.Media.Imaging;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using PhotoViewer.Models;
using PhotoViewer.Services;

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
                foreach (var item in metadata)
                {
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