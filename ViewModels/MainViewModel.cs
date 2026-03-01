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
        // Toolkit buradan otomatik olarak 'DisplayImage' ve 'StatusMessage' üretecek.
        [ObservableProperty]
        private WriteableBitmap? _displayImage;

        [ObservableProperty]
        private string _statusMessage = string.Empty;

        public ObservableCollection<ExifData> ExifMetadata { get; } = new();

        public MainViewModel()
        {
            _imageLoader = new ImageLoaderService();
            _metadataService = new MetadataService();
            _statusMessage = "Lütfen bir resim dosyası seçin.";
        }

        [RelayCommand]
        public async Task LoadPhotoAsync(string filePath)
        {
            if (string.IsNullOrEmpty(filePath)) return;

            try
            {
                // UI'daki TextBlock 'StatusMessage' (Büyük harf) özelliğine bağlıdır.
                StatusMessage = "Yükleniyor...";

                DisplayImage = await _imageLoader.LoadImageAsync(filePath);

                ExifMetadata.Clear();
                var metadata = _metadataService.GetExifMetadata(filePath);
                foreach (var item in metadata)
                {
                    ExifMetadata.Add(item);
                }

                StatusMessage = "Tamamlandı.";
            }
            catch (Exception ex)
            {
                StatusMessage = $"Hata: {ex.Message}";
            }
        }
    }
}