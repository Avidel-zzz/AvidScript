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
            WriteSemantic(semanticPath, CSharpGuestSemanticFixture.Create());

            int firstExitCode = GuestCommandLine.Run(new[]
            {
                "--semantic", semanticPath, "--output", outputPath,
            });
            byte[] first = File.ReadAllBytes(outputPath);
            int secondExitCode = GuestCommandLine.Run(new[]
            {
                "--output", outputPath, "--semantic", semanticPath,
            });
            byte[] second = File.ReadAllBytes(outputPath);
            GuestModule module = GuestIrSerializer.Deserialize(second);

            Assert(firstExitCode == 0 && secondExitCode == 0,
                "valid CLI invocation should succeed in either option order");
            Assert(first.SequenceEqual(second) && GuestModuleValidator.Validate(module).Succeeded,
                "CLI output should be deterministic and independently valid");

            File.WriteAllText(outputPath, "stale");
            WriteSemantic(semanticPath, CSharpGuestSemanticFixture.Create(succeeded: false));
            int failureExitCode = GuestCommandLine.Run(new[]
            {
                "--semantic", semanticPath, "--output", outputPath,
            });

            Assert(failureExitCode == 1 && !File.Exists(outputPath),
                "failed lowering should remove a stale output artifact");
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
