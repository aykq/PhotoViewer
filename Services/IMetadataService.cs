using System.Collections.Generic;
using System.Threading.Tasks;
using PhotoViewer.Models;

namespace PhotoViewer.Services
{
    public interface IMetadataService
    {
        Task<List<ExifData>> GetExifMetadataAsync(string filePath);
    }
}
