using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;

namespace AvidScript.CSharpSemantic;

public static class SemanticCommandLine
{
    private static readonly string[] RequiredOptions =
    {
        "--source",
        "--source-id",
        "--frontend",
        "--output",
    };

    public static int Run(string[] args)
    {
        try
        {
            IReadOnlyDictionary<string, string> options = ParseOptions(args, out List<string> referenceSourcePaths);
            string sourcePath = GetRequiredOption(options, "--source");
            string sourceId = GetRequiredOption(options, "--source-id");
            string frontendPath = GetRequiredOption(options, "--frontend");
            string outputPath = GetRequiredOption(options, "--output");
            if (!File.Exists(sourcePath))
            {
                throw new ArgumentException($"Source file does not exist: {sourcePath}");
            }

            if (!File.Exists(frontendPath))
            {
                throw new ArgumentException($"Frontend artifact does not exist: {frontendPath}");
            }

            string source = File.ReadAllText(sourcePath);
            string frontendSourceSha256 = ReadFrontendSourceSha256(frontendPath);
            IReadOnlyList<SemanticReferenceSource> referenceSources = LoadReferenceSources(referenceSourcePaths);
            SemanticDocument document = SemanticAnalyzer.Analyze(source, sourceId, frontendSourceSha256, referenceSources);
            SemanticArtifactWriter.WriteAtomic(outputPath, SemanticSerializer.Serialize(document));
            return document.Succeeded ? 0 : 1;
        }
        catch (ArgumentException exception)
        {
            Console.Error.WriteLine(exception.Message);
            return 2;
        }
        catch (IOException exception)
        {
            Console.Error.WriteLine(exception.Message);
            return 2;
        }
        catch (UnauthorizedAccessException exception)
        {
            Console.Error.WriteLine(exception.Message);
            return 2;
        }
        catch (JsonException exception)
        {
            Console.Error.WriteLine(exception.Message);
            return 2;
        }
    }

    private static IReadOnlyDictionary<string, string> ParseOptions(
        string[] args,
        out List<string> referenceSourcePaths)
    {
        referenceSourcePaths = new List<string>();
        if (args.Length == 0 || args.Length % 2 != 0)
        {
            throw new ArgumentException("Usage: --source <path> --source-id <id> --frontend <path> --output <path> [--reference-source <path>]...");
        }

        Dictionary<string, string> options = new(StringComparer.Ordinal);
        for (int index = 0; index < args.Length; index += 2)
        {
            string option = args[index];
            string value = args[index + 1];
            if (option == "--reference-source")
            {
                if (string.IsNullOrWhiteSpace(value))
                {
                    throw new ArgumentException("Reference source paths must be non-empty.");
                }

                referenceSourcePaths.Add(value);
                continue;
            }

            if (Array.IndexOf(RequiredOptions, option) < 0)
            {
                throw new ArgumentException($"Unknown option: {option}");
            }

            if (string.IsNullOrWhiteSpace(value) || !options.TryAdd(option, value))
            {
                throw new ArgumentException($"Option must appear once with a non-empty value: {option}");
            }
        }

        return options;
    }

    private static IReadOnlyList<SemanticReferenceSource> LoadReferenceSources(IReadOnlyList<string> referenceSourcePaths)
    {
        List<SemanticReferenceSource> referenceSources = new(referenceSourcePaths.Count);
        for (int index = 0; index < referenceSourcePaths.Count; index++)
        {
            string path = referenceSourcePaths[index];
            if (!File.Exists(path))
            {
                throw new ArgumentException($"Reference source file does not exist: {path}");
            }

            string sourceId = $"reference:{index}:{Path.GetFileName(path)}";
            referenceSources.Add(new SemanticReferenceSource(File.ReadAllText(path), sourceId));
        }

        return referenceSources;
    }

    private static string GetRequiredOption(IReadOnlyDictionary<string, string> options, string name)
    {
        if (!options.TryGetValue(name, out string? value))
        {
            throw new ArgumentException($"Missing required option: {name}");
        }

        return value;
    }

    private static string ReadFrontendSourceSha256(string frontendPath)
    {
        using JsonDocument document = JsonDocument.Parse(File.ReadAllBytes(frontendPath));
        JsonElement root = document.RootElement;
        if (!root.TryGetProperty("language", out JsonElement language) ||
            language.ValueKind != JsonValueKind.String ||
            language.GetString() != "csharp" ||
            !root.TryGetProperty("schema_version", out JsonElement schemaVersion) ||
            !schemaVersion.TryGetInt32(out int schema) ||
            schema != 1)
        {
            throw new ArgumentException("Frontend artifact has an unsupported language or schema version.");
        }

        if (!root.TryGetProperty("source", out JsonElement source) ||
            source.ValueKind != JsonValueKind.Object ||
            !source.TryGetProperty("sha256", out JsonElement sourceSha256) ||
            sourceSha256.ValueKind != JsonValueKind.String ||
            string.IsNullOrWhiteSpace(sourceSha256.GetString()))
        {
            throw new ArgumentException("Frontend artifact does not contain source.sha256.");
        }

        return sourceSha256.GetString()!;
    }
}
