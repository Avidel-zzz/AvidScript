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
            WriteSemantic(semanticPath, CSharpGuestSemanticFixture.Create());

            int firstExitCode = GuestCommandLine.Run(new[]
            {
                "--semantic", semanticPath, "--output", outputPath,
                "--state-schema", stateSchemaPath,
            });
            byte[] first = File.ReadAllBytes(outputPath);
            byte[] firstStateSchema = File.ReadAllBytes(stateSchemaPath);
            int secondExitCode = GuestCommandLine.Run(new[]
            {
                "--state-schema", stateSchemaPath,
                "--output", outputPath,
                "--semantic", semanticPath,
            });
            byte[] second = File.ReadAllBytes(outputPath);
            byte[] secondStateSchema = File.ReadAllBytes(stateSchemaPath);
            GuestModule module = GuestIrSerializer.Deserialize(second);

            Assert(firstExitCode == 0 && secondExitCode == 0,
                "valid CLI invocation should succeed in either option order");
            Assert(first.SequenceEqual(second) && GuestModuleValidator.Validate(module).Succeeded,
                "CLI output should be deterministic and independently valid");
            Assert(firstStateSchema.SequenceEqual(secondStateSchema),
                "CLI state schema output should be deterministic");

            File.WriteAllText(outputPath, "stale");
            File.WriteAllText(stateSchemaPath, "stale");
            WriteSemantic(semanticPath, CSharpGuestSemanticFixture.Create(succeeded: false));
            int failureExitCode = GuestCommandLine.Run(new[]
            {
                "--semantic", semanticPath,
                "--output", outputPath,
                "--state-schema", stateSchemaPath,
            });

            Assert(failureExitCode == 1
                && !File.Exists(outputPath)
                && !File.Exists(stateSchemaPath),
                "failed lowering should remove stale Guest IR and state schema artifacts");
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

    private static void Assert(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
