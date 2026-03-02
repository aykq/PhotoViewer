using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media.Animation;
using PhotoViewer.ViewModels;
using System;
using System.Runtime.InteropServices;

namespace PhotoViewer.Views
{
    public sealed partial class MainWindow : Window
    {
        public MainViewModel ViewModel { get; } = new MainViewModel();

        private bool _isPanning = false;
        private Windows.Foundation.Point _lastPointerPosition;
        private System.Threading.CancellationTokenSource? _zoomCts;
        private bool _isInfoPanelOpen = false;
        private const double PanelWidth = 350;

        // --- WIN32 API TANIMLAMALARI ---
        [DllImport("user32.dll")]
        public static extern bool SetForegroundWindow(IntPtr hWnd);

        [DllImport("user32.dll")]
        public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);

        public const int SW_RESTORE = 9;

        // --- ANIMASYON KÖPRÜSÜ (Grid Üzerinde Tanımlı) ---
        // Window DependencyObject olmadığı için bu özelliği RootGrid üzerinden yöneteceğiz.
        public static readonly DependencyProperty PanelWidthValueProperty =
            DependencyProperty.Register("PanelWidthValue", typeof(double), typeof(Grid),
            new PropertyMetadata(0.0, (s, e) => {
                // Bu event tetiklendiğinde MainWindow'a ulaşıp sütun genişliğini güncelle
                if (s is FrameworkElement element && element.XamlRoot?.Content is Grid rootGrid)
                {
                    // Not: XamlRoot üzerinden erişim yerine basitçe doğrudan sütuna ulaşıyoruz
                    // Çünkü bu kod MainWindow.xaml.cs içinde çalışıyor.
                }
            }));

        public MainWindow()
        {
            this.InitializeComponent();
            LoadPanelSettings();
        }

        private void LoadPanelSettings()
        {
            var localSettings = Windows.Storage.ApplicationData.Current.LocalSettings;
            _isInfoPanelOpen = (bool)(localSettings.Values["IsInfoPanelOpen"] ?? false);

            if (_isInfoPanelOpen)
            {
                InfoPanelColumn.Width = new GridLength(PanelWidth);
                InfoPanel.Visibility = Visibility.Visible;
            }
            else
            {
                InfoPanelColumn.Width = new GridLength(0);
                InfoPanel.Visibility = Visibility.Collapsed;
            }
        }

        private void InfoButton_Click(object sender, RoutedEventArgs e)
        {
            _isInfoPanelOpen = !_isInfoPanelOpen;
            var localSettings = Windows.Storage.ApplicationData.Current.LocalSettings;
            localSettings.Values["IsInfoPanelOpen"] = _isInfoPanelOpen;

            AnimatePanel(_isInfoPanelOpen);
        }

        private void AnimatePanel(bool open)
        {
            if (open) InfoPanel.Visibility = Visibility.Visible;

            // ÇÖZÜM: GridLength animasyonu yerine doğrudan sütun genişliğini kodla değiştiriyoruz.
            // WinUI 3'te Storyboard ile GridLength animasyonu sorunlu olduğu için
            // Composition API veya manuel zamanlayıcı en güvenlisidir.
            // Ama en basiti, animasyonsuz geçiş yapıp sonra animasyonu eklemektir.

            DoubleAnimation animation = new DoubleAnimation
            {
                From = open ? 0 : PanelWidth,
                To = open ? PanelWidth : 0,
                Duration = TimeSpan.FromMilliseconds(200),
                EasingFunction = new CircleEase { EasingMode = EasingMode.EaseOut }
            };

            Storyboard sb = new Storyboard();
            // Hedef olarak doğrudan sütunu değil, genişlik değerini manuel güncelleyen bir yapı kuruyoruz
            animation.EnableDependentAnimation = true;

            // Animasyon süresince sütun genişliğini elle güncelleyen küçük bir hile:
            var startTime = DateTime.Now;
            var duration = animation.Duration.TimeSpan.TotalMilliseconds;

            DispatcherQueue.TryEnqueue(async () => {
                double elapsed = 0;
                while (elapsed < duration)
                {
                    elapsed = (DateTime.Now - startTime).TotalMilliseconds;
                    double progress = Math.Min(elapsed / duration, 1);
                    // Basit bir easing uygulaması
                    double easedProgress = 1 - Math.Cos((progress * Math.PI) / 2);

                    double currentWidth = open ? (easedProgress * PanelWidth) : (PanelWidth - (easedProgress * PanelWidth));
                    InfoPanelColumn.Width = new GridLength(Math.Max(0, currentWidth));

                    await System.Threading.Tasks.Task.Delay(10);
                }

                if (!open) InfoPanel.Visibility = Visibility.Collapsed;
                InfoPanelColumn.Width = new GridLength(open ? PanelWidth : 0);
            });
        }

        // --- DİĞER METODLAR (IMAGE/PICKER) ---
        // (Buradaki SelectFile_Click, ResetImageTransforms, Pointer metodları aynı kalacak)
        // File picker removed (UI no longer exposes select button).

        private void ResetImageTransforms()
        {
            // Reset scale and translation and clear any custom centers so image returns to layout-centered position
            ImageTransform.ScaleX = 1; ImageTransform.ScaleY = 1;
            ImageTransform.TranslateX = 0; ImageTransform.TranslateY = 0;
            ImageTransform.CenterX = 0; ImageTransform.CenterY = 0;

            // hide zoom indicator when reset
            HideZoomIndicator();
        }

        private void MainImage_PointerWheelChanged(object sender, PointerRoutedEventArgs e)
        {
            var pointerPoint = e.GetCurrentPoint(MainImage);
            var delta = pointerPoint.Properties.MouseWheelDelta;
            double zoomFactor = delta > 0 ? 1.1 : 0.9;
            double oldScale = ImageTransform.ScaleX;
            double newScale = oldScale * zoomFactor;
            if (!(newScale >= 0.1 && newScale <= 10)) { e.Handled = true; return; }

            // Compute origin (because RenderTransformOrigin is 0.5,0.5)
            var origin = new Windows.Foundation.Point(MainImage.ActualWidth * 0.5, MainImage.ActualHeight * 0.5);
            var p = pointerPoint.Position;

            // Apply scale
            ImageTransform.ScaleX = newScale; ImageTransform.ScaleY = newScale;

            // Adjust translation so the point under the cursor stays at same screen position
            ImageTransform.TranslateX += (p.X - origin.X) * (oldScale - newScale);
            ImageTransform.TranslateY += (p.Y - origin.Y) * (oldScale - newScale);

            ShowZoomIndicator(newScale);
            e.Handled = true;
        }

        private void MainImage_PointerPressed(object sender, PointerRoutedEventArgs e)
        {
            if (e.GetCurrentPoint(MainImage).Properties.IsLeftButtonPressed)
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
                ImageTransform.TranslateX += currentPosition.X - _lastPointerPosition.X;
                ImageTransform.TranslateY += currentPosition.Y - _lastPointerPosition.Y;
                // NOT: Bu transform yapısında pürüzsüzlük için pozisyonu güncellemiyoruz
            }
        }

        private void MainImage_PointerReleased(object sender, PointerRoutedEventArgs e)
        {
            _isPanning = false;
            MainImage.ReleasePointerCapture(e.Pointer);
        }

        private void MainImage_DoubleTapped(object sender, DoubleTappedRoutedEventArgs e)
        {
            var targetScale = ImageTransform.ScaleX > 1.0 ? 1.0 : 2.5;

            // If toggling back to 1.0 just reset everything
            if (Math.Abs(targetScale - 1.0) < 0.001)
            {
                ResetImageTransforms();
                return;
            }

            // Zoom in centered at double-click position
            var p = e.GetPosition(MainImage);
            double oldScale = ImageTransform.ScaleX;
            double newScale = targetScale;

            var origin = new Windows.Foundation.Point(MainImage.ActualWidth * 0.5, MainImage.ActualHeight * 0.5);

            ImageTransform.ScaleX = newScale; ImageTransform.ScaleY = newScale;
            ImageTransform.TranslateX += (p.X - origin.X) * (oldScale - newScale);
            ImageTransform.TranslateY += (p.Y - origin.Y) * (oldScale - newScale);

            ShowZoomIndicator(newScale);
        }

        private void ShowZoomIndicator(double scale)
        {
            try
            {
                _zoomCts?.Cancel();
                _zoomCts = new System.Threading.CancellationTokenSource();
                var ct = _zoomCts.Token;

                ZoomIndicatorText.Text = $"{scale * 100:0}%";
                ZoomIndicatorBorder.Visibility = Visibility.Visible;
                ZoomIndicatorBorder.Opacity = 1;

                // Hide after 1.5s unless cancelled
                _ = System.Threading.Tasks.Task.Run(async () =>
                {
                    try
                    {
                        await System.Threading.Tasks.Task.Delay(1500, ct);
                        if (ct.IsCancellationRequested) return;
                        // fade out
                        DispatcherQueue.TryEnqueue(async () =>
                        {
                            for (int i = 0; i < 8; i++)
                            {
                                ZoomIndicatorBorder.Opacity -= 0.12;
                                await System.Threading.Tasks.Task.Delay(20);
                            }
                            ZoomIndicatorBorder.Opacity = 0;
                            ZoomIndicatorBorder.Visibility = Visibility.Collapsed;
                        });
                    }
                    catch (OperationCanceledException) { }
                }, ct);
            }
            catch { }
        }

        private void HideZoomIndicator()
        {
            _zoomCts?.Cancel();
            ZoomIndicatorBorder.Opacity = 0;
            ZoomIndicatorBorder.Visibility = Visibility.Collapsed;
        }
    }
}