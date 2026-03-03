using System.Collections.Generic;
using System.Linq;
using MetadataExtractor;
using MetadataExtractor.Formats.Exif;
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
                var directories = ImageMetadataReader.ReadMetadata(filePath);

                // Collect tags
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

                // Prioritize common fields: Camera (Make + Model), Focal Length, FNumber, DateTimeOriginal
                var prioritized = new List<ExifData>();

                var ifd0 = directories.OfType<ExifIfd0Directory>().FirstOrDefault();
                var subIfd = directories.OfType<ExifSubIfdDirectory>().FirstOrDefault();

                string make = ifd0?.GetDescription(ExifDirectoryBase.TagMake) ?? string.Empty;
                string model = ifd0?.GetDescription(ExifDirectoryBase.TagModel) ?? string.Empty;
                if (!string.IsNullOrEmpty(make) || !string.IsNullOrEmpty(model))
                {
                    prioritized.Add(new ExifData { Label = "Camera", Value = string.Join(" ", new[] { make, model }.Where(s => !string.IsNullOrEmpty(s))) });
                }

                var focal = subIfd?.GetDescription(ExifDirectoryBase.TagFocalLength);
                if (!string.IsNullOrEmpty(focal)) prioritized.Add(new ExifData { Label = "Focal Length", Value = focal });

                var fnum = subIfd?.GetDescription(ExifDirectoryBase.TagFNumber);
                if (!string.IsNullOrEmpty(fnum)) prioritized.Add(new ExifData { Label = "Aperture", Value = fnum });

                var dt = subIfd?.GetDescription(ExifDirectoryBase.TagDateTimeOriginal);
                if (!string.IsNullOrEmpty(dt)) prioritized.Add(new ExifData { Label = "Taken", Value = dt });

                // Build final list: prioritized first (if present), then remaining (avoid duplicates)
                var added = new HashSet<string>(prioritized.Select(p => p.Label));
                var result = new List<ExifData>();
                result.AddRange(prioritized);

                foreach (var item in metadataList)
                {
                    if (added.Contains(item.Label)) continue;
                    result.Add(item);
                }

                return result;
            }
            catch
            {
                metadataList.Add(new ExifData { Label = "Hata", Value = "Metadata okunamadı." });
                return metadataList;
            }
        }
    }
}
