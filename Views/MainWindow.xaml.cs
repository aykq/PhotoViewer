using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using PhotoViewer.ViewModels;
using System.ComponentModel;
using System;
using System.Runtime.InteropServices;
using Windows.Foundation;

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

        private enum CropEdge
        {
            None,
            Left,
            Right,
            Top,
            Bottom
        }

        private CropEdge _cropDragEdge = CropEdge.None;
        private bool _isCropDragging = false;
        private Rect _cropRectNormalized = new Rect(0, 0, 1, 1);

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

            ViewModel.PropertyChanged += ViewModel_PropertyChanged;
            SyncCropUi();
        }

        private void ViewModel_PropertyChanged(object? sender, PropertyChangedEventArgs e)
        {
            if (e.PropertyName == nameof(ViewModel.IsCropMode))
                SyncCropUi();
        }

        private void SyncCropUi()
        {
            // Keep UI in sync with ViewModel state.
            DispatcherQueue.TryEnqueue(() =>
            {
                var isCropMode = ViewModel.IsCropMode;

                if (isCropMode)
                {
                    // Reset zoom/pan so crop mapping stays predictable.
                    ResetImageTransforms();

                    CropCanvas.Width = MainImage.ActualWidth;
                    CropCanvas.Height = MainImage.ActualHeight;
                    MainImage.IsHitTestVisible = false;
                    CropCanvas.Visibility = Visibility.Visible;
                    _cropRectNormalized = new Rect(0, 0, 1, 1);
                    ViewModel.SetCropRectNormalized(_cropRectNormalized);
                    UpdateCropOverlayLayout();

                    CropIcon.Visibility = Visibility.Collapsed;
                    CropCheckIcon.Visibility = Visibility.Visible;
                }
                else
                {
                    _cropDragEdge = CropEdge.None;
                    _isCropDragging = false;
                    MainImage.IsHitTestVisible = true;
                    CropCanvas.Visibility = Visibility.Collapsed;

                    CropIcon.Visibility = Visibility.Visible;
                    CropCheckIcon.Visibility = Visibility.Collapsed;
                }
            });
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

        private void MainImage_SizeChanged(object sender, SizeChangedEventArgs e)
        {
            // Keep overlay coordinate system aligned with the image control size.
            CropCanvas.Width = MainImage.ActualWidth;
            CropCanvas.Height = MainImage.ActualHeight;

            if (ViewModel.IsCropMode)
                UpdateCropOverlayLayout();
        }

        private void MainImage_PointerWheelChanged(object sender, PointerRoutedEventArgs e)
        {
            if (ViewModel.IsCropMode) return;

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
            if (ViewModel.IsCropMode) return;

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
            if (ViewModel.IsCropMode) return;

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
            if (ViewModel.IsCropMode) return;

            _isPanning = false;
            MainImage.ReleasePointerCapture(e.Pointer);
        }

        private void MainImage_DoubleTapped(object sender, DoubleTappedRoutedEventArgs e)
        {
            if (ViewModel.IsCropMode) return;

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

        private Rect GetImageContentBounds()
        {
            if (ViewModel.DisplayImage is null) return new Rect(0, 0, 1, 1);
            if (CropCanvas.ActualWidth <= 0 || CropCanvas.ActualHeight <= 0) return new Rect(0, 0, 1, 1);

            double cw = CropCanvas.ActualWidth;
            double ch = CropCanvas.ActualHeight;
            double iw = ViewModel.DisplayImage.PixelWidth;
            double ih = ViewModel.DisplayImage.PixelHeight;

            double scale = Math.Min(cw / iw, ch / ih);
            double renderedW = iw * scale;
            double renderedH = ih * scale;
            double offsetX = (cw - renderedW) / 2;
            double offsetY = (ch - renderedH) / 2;

            return new Rect(offsetX, offsetY, renderedW, renderedH);
        }

        private void UpdateCropOverlayLayout()
        {
            if (!ViewModel.IsCropMode || ViewModel.DisplayImage is null) return;
            if (CropCanvas.ActualWidth <= 0 || CropCanvas.ActualHeight <= 0) return;

            var content = GetImageContentBounds();
            if (content.Width <= 0.001 || content.Height <= 0.001) return;

            double left = content.X + _cropRectNormalized.X * content.Width;
            double top = content.Y + _cropRectNormalized.Y * content.Height;
            double width = _cropRectNormalized.Width * content.Width;
            double height = _cropRectNormalized.Height * content.Height;

            if (width <= 0.1 || height <= 0.1) return;

            Canvas.SetLeft(CropFrameRect, left);
            Canvas.SetTop(CropFrameRect, top);
            CropFrameRect.Width = width;
            CropFrameRect.Height = height;

            double hs = CropLeftHandle.Width; // handles are all same size
            double cx = left + width / 2;
            double cy = top + height / 2;

            // top
            Canvas.SetLeft(CropTopHandle, cx - hs / 2);
            Canvas.SetTop(CropTopHandle, top - hs / 2);

            // bottom
            Canvas.SetLeft(CropBottomHandle, cx - hs / 2);
            Canvas.SetTop(CropBottomHandle, top + height - hs / 2);

            // left
            Canvas.SetLeft(CropLeftHandle, left - hs / 2);
            Canvas.SetTop(CropLeftHandle, cy - hs / 2);

            // right
            Canvas.SetLeft(CropRightHandle, left + width - hs / 2);
            Canvas.SetTop(CropRightHandle, cy - hs / 2);
        }

        private void StartCropDrag(PointerRoutedEventArgs e, CropEdge edge)
        {
            _cropDragEdge = edge;
            _isCropDragging = true;
            CropCanvas.CapturePointer(e.Pointer);
            UpdateCropFromPointer(e);
            e.Handled = true;
        }

        private void UpdateCropFromPointer(PointerRoutedEventArgs e)
        {
            var content = GetImageContentBounds();
            if (content.Width <= 0.001 || content.Height <= 0.001) return;

            var p = e.GetCurrentPoint(CropCanvas).Position;
            double xNorm = (p.X - content.X) / content.Width;
            double yNorm = (p.Y - content.Y) / content.Height;

            xNorm = Math.Clamp(xNorm, 0, 1);
            yNorm = Math.Clamp(yNorm, 0, 1);

            // Represent rect by edges so dragging is intuitive.
            double left = _cropRectNormalized.X;
            double top = _cropRectNormalized.Y;
            double right = left + _cropRectNormalized.Width;
            double bottom = top + _cropRectNormalized.Height;

            const double minSize = 0.05; // normalized

            switch (_cropDragEdge)
            {
                case CropEdge.Left:
                    left = Math.Min(xNorm, right - minSize);
                    left = Math.Clamp(left, 0, 1 - minSize);
                    break;
                case CropEdge.Right:
                    right = Math.Max(xNorm, left + minSize);
                    right = Math.Clamp(right, minSize, 1);
                    break;
                case CropEdge.Top:
                    top = Math.Min(yNorm, bottom - minSize);
                    top = Math.Clamp(top, 0, 1 - minSize);
                    break;
                case CropEdge.Bottom:
                    bottom = Math.Max(yNorm, top + minSize);
                    bottom = Math.Clamp(bottom, minSize, 1);
                    break;
                default:
                    return;
            }

            _cropRectNormalized = new Rect(
                left,
                top,
                Math.Max(0, right - left),
                Math.Max(0, bottom - top));

            ViewModel.SetCropRectNormalized(_cropRectNormalized);
            UpdateCropOverlayLayout();
        }

        private void CropCanvas_PointerMoved(object sender, PointerRoutedEventArgs e)
        {
            if (!_isCropDragging) return;
            UpdateCropFromPointer(e);
        }

        private void CropCanvas_PointerReleased(object sender, PointerRoutedEventArgs e)
        {
            if (!_isCropDragging) return;
            _isCropDragging = false;
            _cropDragEdge = CropEdge.None;
            try { CropCanvas.ReleasePointerCapture(e.Pointer); } catch { }
        }

        private void CropTopHandle_PointerPressed(object sender, PointerRoutedEventArgs e) => StartCropDrag(e, CropEdge.Top);
        private void CropRightHandle_PointerPressed(object sender, PointerRoutedEventArgs e) => StartCropDrag(e, CropEdge.Right);
        private void CropBottomHandle_PointerPressed(object sender, PointerRoutedEventArgs e) => StartCropDrag(e, CropEdge.Bottom);
        private void CropLeftHandle_PointerPressed(object sender, PointerRoutedEventArgs e) => StartCropDrag(e, CropEdge.Left);
    }
}