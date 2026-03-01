using Microsoft.UI.Xaml;
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
        }

        protected override void OnLaunched(Microsoft.UI.Xaml.LaunchActivatedEventArgs args)
        {
            _window = new MainWindow();

            // 1. Uygulama açıkken gelecek yeni dosya açma isteklerini dinle
            AppInstance.GetCurrent().Activated += App_Activated;

            // 2. İlk açılıştaki dosyayı kontrol et
            var activatedArgs = AppInstance.GetCurrent().GetActivatedEventArgs();
            CheckForFileActivation(activatedArgs);

            _window.Activate();
        }

        private void App_Activated(object sender, AppActivationArguments e)
        {
            // UI güncellemelerini ana iş parçacığına gönder
            _window?.DispatcherQueue.TryEnqueue(() =>
            {
                CheckForFileActivation(e);

                if (_window != null)
                {
                    var hwnd = WindowNative.GetWindowHandle(_window);

                    // Önce pencereyi eski boyutuna getir (minimize ise), sonra en öne çek
                    ShowWindow(hwnd, SW_RESTORE);
                    SetForegroundWindow(hwnd);

                    _window.Activate();
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
                        _ = mainWindow.ViewModel.LoadPhotoAsync(filePath);
                    }
                }
            }
        }
    }
}