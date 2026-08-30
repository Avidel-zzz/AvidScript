using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

namespace AvidScript.UeTypeGenerator;

public sealed record UeTypePublishResult(int ChangedFileCount, int CacheHitFileCount);

public static class UeTypeGenerationPublisher
{
    public static UeTypePublishResult Publish(string outputRoot, UeTypeGenerationResult result)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(outputRoot);
        ArgumentNullException.ThrowIfNull(result);
        string root = Path.GetFullPath(outputRoot);
        Directory.CreateDirectory(root);
        int changed = 0;
        int cacheHits = 0;
        IEnumerable<KeyValuePair<string, byte[]>> orderedFiles = result.Files
            .OrderBy(pair => pair.Key == UeTypeShellGenerator.ManifestPath ? 1 : 0)
            .ThenBy(pair => pair.Key, StringComparer.Ordinal);
        foreach (KeyValuePair<string, byte[]> file in orderedFiles)
        {
            string path = GetSafeOutputPath(root, file.Key);
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            if (File.Exists(path) && File.ReadAllBytes(path).AsSpan().SequenceEqual(file.Value))
            {
                ++cacheHits;
                continue;
            }

            string temporaryPath = path + ".tmp." + Guid.NewGuid().ToString("N");
            try
            {
                File.WriteAllBytes(temporaryPath, file.Value);
                File.Move(temporaryPath, path, true);
            }
            finally
            {
                if (File.Exists(temporaryPath))
                {
                    File.Delete(temporaryPath);
                }
            }
            ++changed;
        }
        return new UeTypePublishResult(changed, cacheHits);
    }

    private static string GetSafeOutputPath(string root, string relativePath)
    {
        if (Path.IsPathRooted(relativePath))
        {
            throw new InvalidOperationException($"Generated output path '{relativePath}' must be relative.");
        }
        string path = Path.GetFullPath(Path.Combine(root, relativePath.Replace('/', Path.DirectorySeparatorChar)));
        string prefix = root.EndsWith(Path.DirectorySeparatorChar)
            ? root
            : root + Path.DirectorySeparatorChar;
        if (!path.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidOperationException($"Generated output path '{relativePath}' escapes the output root.");
        }
        return path;
    }
}
