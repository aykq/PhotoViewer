using System.Collections.Generic;
using MetadataExtractor;
using PhotoViewer.Models;

namespace PhotoViewer.Services
{
    public class MetadataService
    {
        public List<ExifData> GetExifMetadata(string filePath)
        {
            var metadataList = new List<ExifData>();

            try
            {
                // MetadataExtractor ile tüm etiketleri oku
                var directories = ImageMetadataReader.ReadMetadata(filePath);

                foreach (var directory in directories)
                {
                    foreach (var tag in directory.Tags)
                    {
                        metadataList.Add(new ExifData
                        {
                            Label = $"{directory.Name} - {tag.Name}",
                            Value = tag.Description
                        });
                    }
                }
            }
            catch
            {
                metadataList.Add(new ExifData { Label = "Hata", Value = "Metadata okunamadı." });
            }

            return metadataList;
        }
    }
}