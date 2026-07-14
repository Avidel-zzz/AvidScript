using System;
using System.Collections.Generic;
using System.IO;
using System.Security.Cryptography;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

public static class GuestCommandLine
{
    public static int Run(string[] args)
    {
        try
        {
            IReadOnlyDictionary<string, string> options = ParseOptions(args);
            string semanticPath = options["--semantic"];
            string outputPath = options["--output"];
            if (!File.Exists(semanticPath))
            {
                throw new ArgumentException($"Semantic artifact does not exist: {semanticPath}");
            }

            File.Delete(outputPath);
            byte[] artifact = File.ReadAllBytes(semanticPath);
            string semanticSha256 = Convert.ToHexString(SHA256.HashData(artifact)).ToLowerInvariant();
            CSharpGuestLoweringResult result = CSharpGuestLowerer.Lower(
                SemanticArtifactReader.Deserialize(artifact),
                semanticSha256);
            if (!result.Succeeded || result.Module is null)
            {
                foreach (GuestDiagnostic diagnostic in result.Diagnostics)
                {
                    Console.Error.WriteLine($"{diagnostic.Code}: {diagnostic.Message}");
                }

                return 1;
            }

            GuestIrArtifactWriter.Write(outputPath, result.Module);
            return 0;
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
    }

    private static IReadOnlyDictionary<string, string> ParseOptions(string[] args)
    {
        if (args.Length != 4)
        {
            throw new ArgumentException("Usage: --semantic <path> --output <path>");
        }

        Dictionary<string, string> options = new(StringComparer.Ordinal);
        for (int index = 0; index < args.Length; index += 2)
        {
            string name = args[index];
            string value = args[index + 1];
            if ((name != "--semantic" && name != "--output")
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
}
