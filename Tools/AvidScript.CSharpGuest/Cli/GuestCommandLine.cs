using System;
using System.Collections.Generic;
using System.IO;
using System.Security.Cryptography;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

public static class GuestCommandLine
{
    public static int Run(string[] args)
    {
        string? outputPath = null;
        string? stateSchemaPath = null;
        try
        {
            IReadOnlyDictionary<string, string> options = ParseOptions(args);
            string semanticPath = options["--semantic"];
            outputPath = options["--output"];
            options.TryGetValue("--state-schema", out stateSchemaPath);
            if (!File.Exists(semanticPath))
            {
                throw new ArgumentException($"Semantic artifact does not exist: {semanticPath}");
            }

            File.Delete(outputPath);
            if (stateSchemaPath is not null)
            {
                File.Delete(stateSchemaPath);
            }
            byte[] artifact = File.ReadAllBytes(semanticPath);
            string semanticSha256 = Convert.ToHexString(SHA256.HashData(artifact)).ToLowerInvariant();
            SemanticDocument document = SemanticArtifactReader.Deserialize(artifact);
            CSharpGuestLoweringResult result = CSharpGuestLowerer.Lower(
                document,
                semanticSha256);
            if (!result.Succeeded || result.Module is null)
            {
                foreach (GuestDiagnostic diagnostic in result.Diagnostics)
                {
                    Console.Error.WriteLine($"{diagnostic.Code}: {diagnostic.Message}");
                }

                return 1;
            }

            CSharpGuestStateSchema? stateSchema = stateSchemaPath is null
                ? null
                : CSharpGuestStateSchemaProjector.Project(document, result.Module);
            GuestIrArtifactWriter.Write(outputPath, result.Module);
            if (stateSchemaPath is not null && stateSchema is not null)
            {
                CSharpGuestStateSchemaSerializer.Write(stateSchemaPath, stateSchema);
            }
            return 0;
        }
        catch (ArgumentException exception)
        {
            Console.Error.WriteLine(exception.Message);
            return 2;
        }
        catch (InvalidDataException exception)
        {
            DeletePublishedArtifacts(outputPath, stateSchemaPath);
            Console.Error.WriteLine(exception.Message);
            return 2;
        }
        catch (IOException exception)
        {
            DeletePublishedArtifacts(outputPath, stateSchemaPath);
            Console.Error.WriteLine(exception.Message);
            return 2;
        }
        catch (UnauthorizedAccessException exception)
        {
            DeletePublishedArtifacts(outputPath, stateSchemaPath);
            Console.Error.WriteLine(exception.Message);
            return 2;
        }
    }

    private static IReadOnlyDictionary<string, string> ParseOptions(string[] args)
    {
        if (args.Length != 4 && args.Length != 6)
        {
            throw new ArgumentException(
                "Usage: --semantic <path> --output <path> [--state-schema <path>]");
        }

        Dictionary<string, string> options = new(StringComparer.Ordinal);
        for (int index = 0; index < args.Length; index += 2)
        {
            string name = args[index];
            string value = args[index + 1];
            if ((name != "--semantic" && name != "--output" && name != "--state-schema")
                || string.IsNullOrWhiteSpace(value)
                || !options.TryAdd(name, value))
            {
                throw new ArgumentException($"Unknown, duplicate, or empty option: {name}");
            }
        }

        if (!options.ContainsKey("--semantic") || !options.ContainsKey("--output"))
        {
            throw new ArgumentException("Both --semantic and --output are required.");
        }

        return options;
    }

    private static void DeletePublishedArtifacts(params string?[] paths)
    {
        foreach (string? path in paths)
        {
            if (!string.IsNullOrWhiteSpace(path))
            {
                File.Delete(path);
            }
        }
    }
}
