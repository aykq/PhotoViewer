using ImageMagick;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Media.Imaging;
using PhotoViewer.Views;
using Microsoft.Windows.AppLifecycle;
using Windows.ApplicationModel.Activation;
using System;
using WinRT.Interop;
using System.Runtime.InteropServices; // DLL Import için gerekli

namespace PhotoViewer
{
    public partial class App : Application
    {
        private Window? _window;

        // --- WIN32 API TANIMLAMALARI (Zorunlu Odak İçin) ---
        [DllImport("user32.dll")]
        private static extern bool SetForegroundWindow(IntPtr hWnd);

        [DllImport("user32.dll")]
        private static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);

        private const int SW_RESTORE = 9;

        public App()
        {
            this.InitializeComponent();
            MagickNET.SetResourceLimit(ResourceType.Memory, 512UL * 1024 * 1024); // 512 MB
            MagickNET.SetResourceLimit(ResourceType.Width, 16384);
            MagickNET.SetResourceLimit(ResourceType.Height, 16384);
            MagickNET.SetResourceLimit(ResourceType.Area, 16384UL * 16384);

            UnhandledException += (s, e) =>
            {
                Debug.WriteLine($"[CRITICAL] Unhandled: {e.Exception}");
                e.Handled = true;
            };
            TaskScheduler.UnobservedTaskException += (s, e) =>
            {
                Debug.WriteLine($"[ERROR] Unobserved task: {e.Exception}");
                e.SetObserved();
            };
        }

        protected override void OnLaunched(Microsoft.UI.Xaml.LaunchActivatedEventArgs args)
        {
            _window = new MainWindow();

            // 1. Uygulama açıkken gelecek yeni dosya açma isteklerini dinle
            AppInstance.GetCurrent().Activated += App_Activated;

            // 2. İlk açılıştaki dosyayı kontrol et
            var activatedArgs = AppInstance.GetCurrent().GetActivatedEventArgs();
            CheckForFileActivation(activatedArgs);

            // Activate the window. In some environments the visual root may not be ready
            // immediately; if Activate throws due to an internal null, defer activation
            // to the window's dispatcher to avoid crashing at startup.
            try
            {
                _window.Activate();
            }
            catch (NullReferenceException)
            {
                // Defer activation to allow the visual tree to finish initializing.
                _window?.DispatcherQueue?.TryEnqueue(() =>
                {
                    try { _window.Activate(); } catch { }
                });
            }
        }

        private void App_Activated(object? sender, AppActivationArguments e)
        {
            _window?.DispatcherQueue.TryEnqueue(() =>
            {
                CheckForFileActivation(e);
                if (_window is MainWindow mainWindow)
                {
                    var hwnd = WindowNative.GetWindowHandle(mainWindow);
                    ShowWindow(hwnd, SW_RESTORE);
                    SetForegroundWindow(hwnd);
                    mainWindow.Activate();
                }
            });
        }

        private void CheckForFileActivation(AppActivationArguments args)
        {
            if (args.Kind == ExtendedActivationKind.File)
            {
                var fileArgs = args.Data as IFileActivatedEventArgs;
                if (fileArgs != null && fileArgs.Files.Count > 0)
                {
                    var filePath = fileArgs.Files[0].Path;
                    if (_window is MainWindow mainWindow)
                    {
                        mainWindow.ViewModel.LoadPhotoAsync(filePath).ContinueWith(
                            t => Debug.WriteLine($"[ERROR] LoadPhoto failed: {t.Exception}"),
                            TaskContinuationOptions.OnlyOnFaulted);
                    }
                }
            }
        }
    }
}