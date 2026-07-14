using System;
using System.IO;
using System.Linq;

namespace AvidScript.GuestIr;

public static class GuestIrArtifactWriter
{
    public static void Write(string path, GuestModule module)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        ArgumentNullException.ThrowIfNull(module);

        GuestValidationResult validation = GuestModuleValidator.Validate(module);
        if (!validation.Succeeded)
        {
            string codes = string.Join(
                ", ",
                validation.Diagnostics.Select(diagnostic => diagnostic.Code).Distinct(StringComparer.Ordinal));
            throw new InvalidDataException($"Guest IR validation failed: {codes}.");
        }

        string fullPath = Path.GetFullPath(path);
        string directory = Path.GetDirectoryName(fullPath) ?? Directory.GetCurrentDirectory();
        Directory.CreateDirectory(directory);

        byte[] artifact = GuestIrSerializer.Serialize(module);
        string temporaryPath = Path.Combine(
            directory,
            $".{Path.GetFileName(fullPath)}.{Guid.NewGuid():N}.tmp");
        try
        {
            using (FileStream stream = new(
                temporaryPath,
                FileMode.CreateNew,
                FileAccess.Write,
                FileShare.None,
                4096,
                FileOptions.WriteThrough))
            {
                stream.Write(artifact);
                stream.Flush(flushToDisk: true);
            }

            File.Move(temporaryPath, fullPath, overwrite: true);
        }
        finally
        {
            if (File.Exists(temporaryPath))
            {
                File.Delete(temporaryPath);
            }
        }
    }
}
