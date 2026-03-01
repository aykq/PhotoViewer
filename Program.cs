using System;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.Windows.AppLifecycle;

namespace PhotoViewer
{
    public class Program
    {
        [STAThread]
        static async Task Main(string[] args)
        {
            WinRT.ComWrappersSupport.InitializeComWrappers();

            // 1. Mevcut uygulama örneğini kontrol et
            var instance = AppInstance.FindOrRegisterForKey("PhotoViewer_Unique_Key");

            if (instance.IsCurrent)
            {
                // Eğer bu ilk açılan kopyaysa, normal şekilde başlat
                Application.Start((p) =>
                {
                    var context = new DispatcherQueueSynchronizationContext(DispatcherQueue.GetForCurrentThread());
                    SynchronizationContext.SetSynchronizationContext(context);
                    new App();
                });
            }
            else
            {
                // 2. Eğer zaten açık bir kopya varsa, dosyayı ona yönlendir ve bunu kapat
                var eventArgs = AppInstance.GetCurrent().GetActivatedEventArgs();
                await instance.RedirectActivationToAsync(eventArgs);
            }
        }
    }
}