using Microsoft.UI.Xaml;
using PhotoViewer.ViewModels;
using System;
using Windows.Storage.Pickers;
using WinRT.Interop;
using Microsoft.UI.Xaml.Input; // Pointer olayları için gerekli

namespace PhotoViewer.Views
{
    public sealed partial class MainWindow : Window
    {
        public MainViewModel ViewModel { get; } = new MainViewModel();

        // --- KAYDIRMA (PAN) İÇİN YARDIMCI DEĞİŞKENLER ---
        private bool _isPanning = false;
        private Windows.Foundation.Point _lastPointerPosition;

        public MainWindow()
        {
            this.InitializeComponent();
        }

        private async void SelectFile_Click(object sender, RoutedEventArgs e)
        {
            var picker = new FileOpenPicker();
            var hwnd = WindowNative.GetWindowHandle(this);
            InitializeWithWindow.Initialize(picker, hwnd);

            picker.ViewMode = PickerViewMode.Thumbnail;
            picker.SuggestedStartLocation = PickerLocationId.PicturesLibrary;

            picker.FileTypeFilter.Add(".jpg");
            picker.FileTypeFilter.Add(".png");
            picker.FileTypeFilter.Add(".gif");
            picker.FileTypeFilter.Add(".tga");
            picker.FileTypeFilter.Add(".webp");
            picker.FileTypeFilter.Add(".tiff");
            picker.FileTypeFilter.Add(".heic");
            picker.FileTypeFilter.Add(".heif");
            picker.FileTypeFilter.Add(".jxl");

            var file = await picker.PickSingleFileAsync();
            if (file != null)
            {
                // Yeni resim açıldığında zoom ve kaydırmayı sıfırlamak iyi bir fikirdir
                ResetImageTransforms();
                await ViewModel.LoadPhotoAsync(file.Path);
            }
        }

        private void ResetImageTransforms()
        {
            ImageTransform.ScaleX = 1;
            ImageTransform.ScaleY = 1;
            ImageTransform.TranslateX = 0;
            ImageTransform.TranslateY = 0;
        }

        // --- ZOOM MANTIĞI ---
        private void MainImage_PointerWheelChanged(object sender, PointerRoutedEventArgs e)
        {
            var pointerPoint = e.GetCurrentPoint(MainImage);
            var position = pointerPoint.Position;

            ImageTransform.CenterX = position.X;
            ImageTransform.CenterY = position.Y;

            var delta = pointerPoint.Properties.MouseWheelDelta;
            double zoomFactor = delta > 0 ? 1.1 : 0.9;

            double newScaleX = ImageTransform.ScaleX * zoomFactor;
            double newScaleY = ImageTransform.ScaleY * zoomFactor;

            if (newScaleX >= 0.1 && newScaleX <= 10)
            {
                ImageTransform.ScaleX = newScaleX;
                ImageTransform.ScaleY = newScaleY;
            }

            e.Handled = true;
        }

        // --- KAYDIRMA (PAN) MANTIĞI REVİZE ---
        private void MainImage_PointerPressed(object sender, PointerRoutedEventArgs e)
        {
            var properties = e.GetCurrentPoint(MainImage).Properties;

            // Sadece SOL TIK basılıysa kaydırmayı başlat (Orta tuş kaldırıldı)
            if (properties.IsLeftButtonPressed)
            {
                _isPanning = true;
                _lastPointerPosition = e.GetCurrentPoint(MainImage).Position;
                MainImage.CapturePointer(e.Pointer);
                e.Handled = true;
            }
        }

        private void MainImage_PointerMoved(object sender, PointerRoutedEventArgs e)
        {
            if (_isPanning)
            {
                var currentPosition = e.GetCurrentPoint(MainImage).Position;

                // Fare hareket miktarını hesapla
                double deltaX = currentPosition.X - _lastPointerPosition.X;
                double deltaY = currentPosition.Y - _lastPointerPosition.Y;

                // Resmi kaydır (Translate)
                ImageTransform.TranslateX += deltaX;
                ImageTransform.TranslateY += deltaY;

                // Not: Pozisyonu güncellemiyoruz çünkü TranslateX/Y değiştikçe 
                // koordinat sistemi de kayıyor. _lastPointerPosition'ı sabit tutmak
                // bu özel transform yapısında daha pürüzsüz sonuç verebilir.
                // Eğer zıplama olursa: _lastPointerPosition = currentPosition;
            }
        }

        private void MainImage_PointerReleased(object sender, PointerRoutedEventArgs e)
        {
            if (_isPanning)
            {
                _isPanning = false;
                MainImage.ReleasePointerCapture(e.Pointer);
                e.Handled = true;
            }
        }

        // --- ÇİFT TIKLAMA ZOOM MANTIĞI ---
        private void MainImage_DoubleTapped(object sender, DoubleTappedRoutedEventArgs e)
        {
            // Eğer şu an zoom yapılmışsa (Scale > 1), sıfırla
            if (ImageTransform.ScaleX > 1.0)
            {
                ResetImageTransforms();
            }
            else
            {
                // Değilse, imlecin olduğu noktaya %250 (2.5 kat) zoom yap
                var position = e.GetPosition(MainImage);

                ImageTransform.CenterX = position.X;
                ImageTransform.CenterY = position.Y;

                ImageTransform.ScaleX = 2.5;
                ImageTransform.ScaleY = 2.5;

                // Opsiyonel: Zoom yapınca resmi merkeze hafifçe kaydırabiliriz 
                // ama şu anki haliyle imlece odaklanması yeterli olacaktır.
            }
            e.Handled = true;
        }
    }
}