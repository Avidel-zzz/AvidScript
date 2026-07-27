using System;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Serialization;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

internal static class CSharpGuestCliTests
{
    private static readonly JsonSerializerOptions SemanticOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower,
        DefaultIgnoreCondition = JsonIgnoreCondition.Never,
        WriteIndented = true,
    };

    public static int Run()
    {
        CliWritesDeterministicArtifactAndRemovesStaleOutputOnFailure();
        return 1;
    }

    private static void CliWritesDeterministicArtifactAndRemovesStaleOutputOnFailure()
    {
        string directory = Directory.CreateTempSubdirectory("AvidScript.P41.4.").FullName;
        try
        {
            string semanticPath = Path.Combine(directory, "input.semantic.json");
            string outputPath = Path.Combine(directory, "output.guestir.json");
            string stateSchemaPath = Path.Combine(directory, "output.state.json");
            string debugMapPath = Path.Combine(directory, "output.debug.json");
            string frontendArtifactSha256 = new('f', 64);
            WriteSemantic(semanticPath, CreateDebugDocument());

            int firstExitCode = GuestCommandLine.Run(new[]
            {
                "--semantic", semanticPath, "--output", outputPath,
                "--state-schema", stateSchemaPath,
                "--debug-map", debugMapPath,
                "--frontend-artifact-sha256", frontendArtifactSha256,
            });
            Assert(firstExitCode == 0, "valid CLI invocation with debug map should succeed");
            byte[] first = File.ReadAllBytes(outputPath);
            byte[] firstStateSchema = File.ReadAllBytes(stateSchemaPath);
            byte[] firstDebugMap = File.ReadAllBytes(debugMapPath);
            int secondExitCode = GuestCommandLine.Run(new[]
            {
                "--state-schema", stateSchemaPath,
                "--debug-map", debugMapPath,
                "--frontend-artifact-sha256", frontendArtifactSha256,
                "--output", outputPath,
                "--semantic", semanticPath,
                "--data-lane-fusion", "disabled",
            });
            Assert(secondExitCode == 0,
                "valid CLI options and disabled data-lane fusion should remain order independent");
            byte[] second = File.ReadAllBytes(outputPath);
            byte[] secondStateSchema = File.ReadAllBytes(stateSchemaPath);
            byte[] secondDebugMap = File.ReadAllBytes(debugMapPath);
            GuestModule module = GuestIrSerializer.Deserialize(second);

            Assert(first.SequenceEqual(second) && GuestModuleValidator.Validate(module).Succeeded,
                "CLI output should be deterministic and independently valid");
            Assert(firstStateSchema.SequenceEqual(secondStateSchema),
                "CLI state schema output should be deterministic");
            Assert(firstDebugMap.SequenceEqual(secondDebugMap),
                "CLI debug map output should be deterministic");

            int invalidFusionExitCode = GuestCommandLine.Run(new[]
            {
                "--semantic", semanticPath,
                "--output", outputPath,
                "--data-lane-fusion", "automatic",
            });
            Assert(invalidFusionExitCode == 2,
                "unknown data-lane fusion modes should be rejected by the CLI contract");

            File.WriteAllText(outputPath, "stale");
            File.WriteAllText(stateSchemaPath, "stale");
            File.WriteAllText(debugMapPath, "stale");
            WriteSemantic(semanticPath, CreateDebugDocument(succeeded: false));
            int failureExitCode = GuestCommandLine.Run(new[]
            {
                "--semantic", semanticPath,
                "--output", outputPath,
                "--state-schema", stateSchemaPath,
                "--debug-map", debugMapPath,
                "--frontend-artifact-sha256", frontendArtifactSha256,
            });

            Assert(failureExitCode == 1
                && !File.Exists(outputPath)
                && !File.Exists(stateSchemaPath)
                && !File.Exists(debugMapPath),
                "failed lowering should remove stale Guest IR, state schema, and debug map artifacts");
        }
        finally
        {
            Directory.Delete(directory, recursive: true);
        }
    }

    private static void WriteSemantic(string path, SemanticDocument document)
    {
        File.WriteAllBytes(path, JsonSerializer.SerializeToUtf8Bytes(document, SemanticOptions));
    }

    private static SemanticDocument CreateDebugDocument(bool succeeded = true)
    {
        SemanticDocument document = CSharpGuestSemanticFixture.Create(succeeded);
        SemanticSpan span = new(0, 0, 0, 0, 0, 0);
        return document with
        {
            Symbols = document.Symbols.Concat(new[]
            {
                new SemanticSymbol(
                    "symbol:type:global::Game.Script",
                    "type",
                    "Script",
                    null,
                    "type:global::Game.Script",
                    "global::Game.Script",
                    true,
                    "public",
                    span),
                new SemanticSymbol(
                    CSharpGuestSemanticFixture.MainMethodId,
                    "method",
                    "Main",
                    "symbol:type:global::Game.Script",
                    "type:void",
                    "Main():void",
                    true,
                    "public",
                    span),
            }).ToArray(),
        };
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
