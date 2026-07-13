using System;
using System.IO;

namespace AvidScript.CSharpSemantic;

internal static class SemanticArtifactWriter
{
    public static void WriteAtomic(string outputPath, byte[] contents)
    {
        string fullPath = Path.GetFullPath(outputPath);
        string directory = Path.GetDirectoryName(fullPath)
            ?? throw new ArgumentException("Output path must include a directory.", nameof(outputPath));
        Directory.CreateDirectory(directory);
        string temporaryPath = Path.Combine(
            directory,
            $".{Path.GetFileName(fullPath)}.{Guid.NewGuid():N}.tmp");
        try
        {
            File.WriteAllBytes(temporaryPath, contents);
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
