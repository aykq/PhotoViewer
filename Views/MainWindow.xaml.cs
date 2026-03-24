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
                    MainImage.RenderTransformOrigin = new Windows.Foundation.Point(0.5, 0.5);

                    ImageTransform.ScaleX = 1;
                    ImageTransform.ScaleY = 1;
                    ImageTransform.TranslateX = 0;
                    ImageTransform.TranslateY = 0;
                    ImageTransform.CenterX = 0;
                    ImageTransform.CenterY = 0;

                    MainImage.UpdateLayout();
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
            double newScale = oldScale * zoomFactor;
            if (!(newScale >= 0.1 && newScale <= 10)) { e.Handled = true; return; }

            var origin = new Windows.Foundation.Point(MainImage.ActualWidth * 0.5, MainImage.ActualHeight * 0.5);
            var p = pointerPoint.Position;

            ImageTransform.ScaleX = newScale; ImageTransform.ScaleY = newScale;

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
                _lastPointerPosition = currentPosition;
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

            if (Math.Abs(targetScale - 1.0) < 0.001)
            {
                ResetImageTransforms();
                return;
            }

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
