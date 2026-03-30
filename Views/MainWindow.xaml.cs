using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using PhotoViewer.ViewModels;
using System;
using System.Runtime.InteropServices;
using Windows.System;

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

        private const double MinZoom = 0.1;
        private const double MaxZoom = 10;
        private const double DoubleTapZoomIn = 2.5;
        /// <summary>1.0’a yakın zoom’da tam ortala.</summary>
        private const double SnapToOneEpsilon = 0.02;

        // --- WIN32 API TANIMLAMALARI ---
        [DllImport("user32.dll")]
        public static extern bool SetForegroundWindow(IntPtr hWnd);

        [DllImport("user32.dll")]
        public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);

        public const int SW_RESTORE = 9;

        public MainWindow()
        {
            this.InitializeComponent();
            LoadPanelSettings();
        }

        private void RootGrid_Loaded(object sender, RoutedEventArgs e)
        {
            RootGrid.Focus(FocusState.Programmatic);
        }

        private void MainImage_SizeChanged(object sender, SizeChangedEventArgs e)
        {
            if (MainImage.ActualWidth <= 0 || MainImage.ActualHeight <= 0)
                return;
            // Ölçek/döndürme merkezi — RenderTransformOrigin (0,0) ile birlikte kullanılır
            ImageTransform.CenterX = MainImage.ActualWidth / 2;
            ImageTransform.CenterY = MainImage.ActualHeight / 2;
        }

        private Windows.Foundation.Point GetImageCenter()
        {
            return new Windows.Foundation.Point(MainImage.ActualWidth * 0.5, MainImage.ActualHeight * 0.5);
        }

        /// <summary>
        /// İmleç altındaki nokta sabit kalacak şekilde ölçek uygular (RenderTransformOrigin 0.5,0.5 + Center ile uyumlu).
        /// </summary>
        private void ApplyZoomScaleAtPoint(double newScale, Windows.Foundation.Point pointerInImage)
        {
            double oldScale = ImageTransform.ScaleX;
            if (oldScale <= 0) oldScale = 1;

            newScale = Math.Clamp(newScale, MinZoom, MaxZoom);

            // %100’e yakınsa tam sıfırla (ortada, kayma yok)
            if (Math.Abs(newScale - 1) < SnapToOneEpsilon)
            {
                ImageTransform.ScaleX = 1;
                ImageTransform.ScaleY = 1;
                ImageTransform.TranslateX = 0;
                ImageTransform.TranslateY = 0;
                return;
            }

            double factor = newScale / oldScale;
            var c = GetImageCenter();

            ImageTransform.ScaleX = newScale;
            ImageTransform.ScaleY = newScale;
            ImageTransform.TranslateX += (pointerInImage.X - c.X) * (1 - factor);
            ImageTransform.TranslateY += (pointerInImage.Y - c.Y) * (1 - factor);
        }

        private void RootGrid_KeyDown(object sender, KeyRoutedEventArgs e)
        {
            if (e.Key == VirtualKey.Left)
            {
                _ = ViewModel.PreviousPhotoAsync();
                e.Handled = true;
            }
            else if (e.Key == VirtualKey.Right)
            {
                _ = ViewModel.NextPhotoAsync();
                e.Handled = true;
            }
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

            var startTime = DateTime.Now;
            var duration = 200.0;

            DispatcherQueue.TryEnqueue(async () => {
                double elapsed = 0;
                while (elapsed < duration)
                {
                    elapsed = (DateTime.Now - startTime).TotalMilliseconds;
                    double progress = Math.Min(elapsed / duration, 1);
                    double easedProgress = 1 - Math.Cos((progress * Math.PI) / 2);

                    double currentWidth = open ? (easedProgress * PanelWidth) : (PanelWidth - (easedProgress * PanelWidth));
                    InfoPanelColumn.Width = new GridLength(Math.Max(0, currentWidth));

                    await System.Threading.Tasks.Task.Delay(10);
                }

                if (!open) InfoPanel.Visibility = Visibility.Collapsed;
                InfoPanelColumn.Width = new GridLength(open ? PanelWidth : 0);
            });
        }

        private void ResetImageTransforms()
        {
            DispatcherQueue.TryEnqueue(() =>
            {
                try
                {
                    MainImage.RenderTransformOrigin = new Windows.Foundation.Point(0, 0);

                    ImageTransform.ScaleX = 1;
                    ImageTransform.ScaleY = 1;
                    ImageTransform.TranslateX = 0;
                    ImageTransform.TranslateY = 0;

                    MainImage.UpdateLayout();
                    if (MainImage.ActualWidth > 0 && MainImage.ActualHeight > 0)
                    {
                        ImageTransform.CenterX = MainImage.ActualWidth / 2;
                        ImageTransform.CenterY = MainImage.ActualHeight / 2;
                    }
                }
                catch { }
            });

            HideZoomIndicator();
        }

        private void MainImage_PointerWheelChanged(object sender, PointerRoutedEventArgs e)
        {
            var pointerPoint = e.GetCurrentPoint(MainImage);
            var delta = pointerPoint.Properties.MouseWheelDelta;
            double zoomFactor = delta > 0 ? 1.1 : 0.9;
            double oldScale = ImageTransform.ScaleX;
            if (oldScale <= 0) oldScale = 1;
            double newScale = oldScale * zoomFactor;
            if (!(newScale >= MinZoom && newScale <= MaxZoom)) { e.Handled = true; return; }

            ApplyZoomScaleAtPoint(newScale, pointerPoint.Position);
            ShowZoomIndicator(ImageTransform.ScaleX);
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
            if (!_isPanning) return;

            var curPos = e.GetCurrentPoint(MainImage).Position;
            ImageTransform.TranslateX += curPos.X - _lastPointerPosition.X;
            ImageTransform.TranslateY += curPos.Y - _lastPointerPosition.Y;
            _lastPointerPosition = curPos;
        }

        private void MainImage_PointerReleased(object sender, PointerRoutedEventArgs e)
        {
            _isPanning = false;
            MainImage.ReleasePointerCapture(e.Pointer);
        }

        private void MainImage_DoubleTapped(object sender, DoubleTappedRoutedEventArgs e)
        {
            var p = e.GetPosition(MainImage);
            double oldScale = ImageTransform.ScaleX;
            if (oldScale <= 0) oldScale = 1;

            // Zoom in: imleç altı sabit; zoom out: tam ortala
            if (oldScale > 1.0 + SnapToOneEpsilon)
            {
                ResetImageTransforms();
                return;
            }

            double newScale = DoubleTapZoomIn;
            ApplyZoomScaleAtPoint(newScale, p);
            ShowZoomIndicator(ImageTransform.ScaleX);
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

                _ = System.Threading.Tasks.Task.Run(async () =>
                {
                    try
                    {
                        await System.Threading.Tasks.Task.Delay(1500, ct);
                        if (ct.IsCancellationRequested) return;
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
