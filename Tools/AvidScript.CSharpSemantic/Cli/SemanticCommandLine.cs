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
            IReadOnlyDictionary<string, string> options = ParseOptions(args);
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
            SemanticDocument document = SemanticAnalyzer.Analyze(source, sourceId, frontendSourceSha256);
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

    private static IReadOnlyDictionary<string, string> ParseOptions(string[] args)
    {
        if (args.Length == 0 || args.Length % 2 != 0)
        {
            throw new ArgumentException("Usage: --source <path> --source-id <id> --frontend <path> --output <path>");
        }

        Dictionary<string, string> options = new(StringComparer.Ordinal);
        for (int index = 0; index < args.Length; index += 2)
        {
            string option = args[index];
            string value = args[index + 1];
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
