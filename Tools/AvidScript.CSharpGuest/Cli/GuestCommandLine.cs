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
        string? debugMapPath = null;
        try
        {
            if (args.Length == 4
                && args[0] == "--finalize-debug-map"
                && args[2] == "--offset-map"
                && !string.IsNullOrWhiteSpace(args[1])
                && !string.IsNullOrWhiteSpace(args[3]))
            {
                CSharpGuestDebugMapFinalizer.FinalizeFile(args[1], args[3]);
                return 0;
            }

            IReadOnlyDictionary<string, string> options = ParseOptions(args);
            string semanticPath = options["--semantic"];
            outputPath = options["--output"];
            options.TryGetValue("--state-schema", out stateSchemaPath);
            options.TryGetValue("--debug-map", out debugMapPath);
            options.TryGetValue("--frontend-artifact-sha256", out string? frontendArtifactSha256);
            bool dataLaneFusionEnabled = ParseDataLaneFusion(options);
            if ((debugMapPath is null) != (frontendArtifactSha256 is null))
            {
                throw new ArgumentException(
                    "--debug-map and --frontend-artifact-sha256 must be provided together.");
            }
            if (!File.Exists(semanticPath))
            {
                throw new ArgumentException($"Semantic artifact does not exist: {semanticPath}");
            }

            File.Delete(outputPath);
            if (stateSchemaPath is not null)
            {
                File.Delete(stateSchemaPath);
            }
            if (debugMapPath is not null)
            {
                File.Delete(debugMapPath);
            }
            byte[] artifact = File.ReadAllBytes(semanticPath);
            string semanticSha256 = Convert.ToHexString(SHA256.HashData(artifact)).ToLowerInvariant();
            SemanticDocument document = SemanticArtifactReader.Deserialize(artifact);
            CSharpGuestLoweringResult result = CSharpGuestLowerer.Lower(
                document,
                semanticSha256,
                enableDataLaneFusion: dataLaneFusionEnabled);
            if (!result.Succeeded || result.Module is null)
            {
                DeletePublishedArtifacts(outputPath, stateSchemaPath, debugMapPath);
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
            if (debugMapPath is not null)
            {
                string guestIrSha256 = Convert.ToHexString(
                    SHA256.HashData(File.ReadAllBytes(outputPath))).ToLowerInvariant();
                CSharpGuestDebugMap debugMap = CSharpGuestDebugMapProjector.Project(
                    document,
                    result.Module,
                    guestIrSha256,
                    frontendArtifactSha256!);
                CSharpGuestDebugMapSerializer.Write(debugMapPath, debugMap);
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
            DeletePublishedArtifacts(outputPath, stateSchemaPath, debugMapPath);
            Console.Error.WriteLine(exception.Message);
            return 2;
        }
        catch (IOException exception)
        {
            DeletePublishedArtifacts(outputPath, stateSchemaPath, debugMapPath);
            Console.Error.WriteLine(exception.Message);
            return 2;
        }
        catch (UnauthorizedAccessException exception)
        {
            DeletePublishedArtifacts(outputPath, stateSchemaPath, debugMapPath);
            Console.Error.WriteLine(exception.Message);
            return 2;
        }
    }

    private static IReadOnlyDictionary<string, string> ParseOptions(string[] args)
    {
        if (args.Length != 4
            && args.Length != 6
            && args.Length != 8
            && args.Length != 10
            && args.Length != 12)
        {
            throw new ArgumentException(
                "Usage: --semantic <path> --output <path> [--state-schema <path>] [--debug-map <path> --frontend-artifact-sha256 <sha256>] [--data-lane-fusion enabled|disabled] | --finalize-debug-map <path> --offset-map <path>");
        }

        Dictionary<string, string> options = new(StringComparer.Ordinal);
        for (int index = 0; index < args.Length; index += 2)
        {
            string name = args[index];
            string value = args[index + 1];
            if ((name != "--semantic"
                    && name != "--output"
                    && name != "--state-schema"
                    && name != "--debug-map"
                    && name != "--frontend-artifact-sha256"
                    && name != "--data-lane-fusion")
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

    private static bool ParseDataLaneFusion(IReadOnlyDictionary<string, string> options)
    {
        if (!options.TryGetValue("--data-lane-fusion", out string? value)
            || value.Equals("enabled", StringComparison.OrdinalIgnoreCase))
        {
            return true;
        }
        if (value.Equals("disabled", StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }

        throw new ArgumentException("--data-lane-fusion must be enabled or disabled.");
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
