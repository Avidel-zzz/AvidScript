using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
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
            bool debugInstrumentationEnabled = ParseDebugInstrumentation(options);
            bool boundedLanguageErrors = ParseLanguageErrors(options);
            options.TryGetValue("--module-id", out string? requestedModuleId);
            int implicitFunctionImportCount =
                ParseImplicitFunctionImportCount(options);
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
            GuestModule? module;
            bool asyncLanguageErrors = document.SchemaVersion
                    == SemanticContract.AsyncLanguageErrorSchemaVersion
                    && document.AsyncMethods.Any(method => method.ErrorPlan is not null)
                || document.SchemaVersion
                    is (SemanticContract.AsyncExceptionFlowSchemaVersion
                        or SemanticContract.DirectAwaitCleanupSchemaVersion
                        or SemanticContract.AsyncCancellationFlowSchemaVersion
                        or SemanticContract.TaskLocalLifetimeSchemaVersion)
                    && document.AsyncMethods.Any(method => method.ExceptionPlan is not null);
            if (boundedLanguageErrors && (document.ExceptionFlows is { Count: > 0 }
                    || asyncLanguageErrors)
                && (!dataLaneFusionEnabled || debugInstrumentationEnabled))
                throw new ArgumentException(
                    "Bounded language errors require data-lane fusion enabled and debug instrumentation disabled.");
            if (boundedLanguageErrors && document.ExceptionFlows is { Count: > 0 }
                && !asyncLanguageErrors)
            {
                if (!CSharpLanguageErrorCompiler.TryLower(document, semanticSha256,
                        out CSharpLanguageErrorCompilation? compilation, out string? error)
                    || compilation is null)
                {
                    DeletePublishedArtifacts(outputPath, stateSchemaPath, debugMapPath);
                    Console.Error.WriteLine($"ASCG1004: {error ?? "Bounded language error lowering failed."}");
                    return 1;
                }
                module = compilation.Module;
            }
            else
            {
                CSharpGuestLoweringResult result = CSharpGuestLowerer.Lower(
                    document,
                    semanticSha256,
                    enableDataLaneFusion: dataLaneFusionEnabled,
                    enableDebugInstrumentation: debugInstrumentationEnabled,
                    enableAsyncLanguageErrors: boundedLanguageErrors && asyncLanguageErrors);
                if (!result.Succeeded || result.Module is null)
                {
                    DeletePublishedArtifacts(outputPath, stateSchemaPath, debugMapPath);
                    foreach (GuestDiagnostic diagnostic in result.Diagnostics)
                        Console.Error.WriteLine($"{diagnostic.Code}: {diagnostic.Message}");
                    return 1;
                }
                module = result.Module;
            }

            if (requestedModuleId is not null)
            {
                if (requestedModuleId.Length > 1024 || requestedModuleId.Any(char.IsControl))
                    throw new ArgumentException(
                        "--module-id must be at most 1024 characters and contain no control characters.");
                module = module with { ModuleId = requestedModuleId };
                GuestValidationResult validation = GuestModuleValidator.Validate(module);
                if (!validation.Succeeded)
                    throw new InvalidDataException(
                        "ASCG1005: Requested module identity does not satisfy the Guest IR contract.");
            }

            CSharpGuestStateSchema? stateSchema = stateSchemaPath is null
                ? null
                : CSharpGuestStateSchemaProjector.Project(document, module);
            GuestIrArtifactWriter.Write(outputPath, module);
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
                    module,
                    guestIrSha256,
                    frontendArtifactSha256!,
                    implicitFunctionImportCount,
                    requestedModuleId);
                CSharpGuestDebugMapSerializer.Write(debugMapPath, debugMap);
            }
            return 0;
        }
        catch (ArgumentException exception)
        {
            DeletePublishedArtifacts(outputPath, stateSchemaPath, debugMapPath);
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
        if (args.Length is < 4 or > 20 || args.Length % 2 != 0)
        {
            throw new ArgumentException(
                "Usage: --semantic <path> --output <path> [--state-schema <path>] [--debug-map <path> --frontend-artifact-sha256 <sha256>] [--data-lane-fusion enabled|disabled] [--debug-instrumentation enabled|disabled] [--implicit-function-import-count <0..16>] [--language-errors disabled|bounded] [--module-id <id>] | --finalize-debug-map <path> --offset-map <path>");
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
                    && name != "--data-lane-fusion"
                    && name != "--debug-instrumentation"
                    && name != "--implicit-function-import-count"
                    && name != "--language-errors"
                    && name != "--module-id")
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

    private static bool ParseDebugInstrumentation(IReadOnlyDictionary<string, string> options)
    {
        if (!options.TryGetValue("--debug-instrumentation", out string? value)
            || value.Equals("disabled", StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }
        if (value.Equals("enabled", StringComparison.OrdinalIgnoreCase))
        {
            return true;
        }

        throw new ArgumentException("--debug-instrumentation must be enabled or disabled.");
    }

    private static bool ParseLanguageErrors(IReadOnlyDictionary<string, string> options)
    {
        if (!options.TryGetValue("--language-errors", out string? value)
            || value.Equals("disabled", StringComparison.OrdinalIgnoreCase))
            return false;
        if (value.Equals("bounded", StringComparison.OrdinalIgnoreCase))
            return true;
        throw new ArgumentException("--language-errors must be disabled or bounded.");
    }

    private static int ParseImplicitFunctionImportCount(
        IReadOnlyDictionary<string, string> options)
    {
        if (!options.TryGetValue(
                "--implicit-function-import-count",
                out string? value))
        {
            return 0;
        }
        if (int.TryParse(value, out int count) && count is >= 0 and <= 16)
        {
            return count;
        }
        throw new ArgumentException(
            "--implicit-function-import-count must be in the range 0..16.");
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
