using System;
using System.IO;
using System.Linq;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class CSharpGuestAsyncLanguageErrorTests
{
    public static int Run()
    {
        const string source = """
            using AvidScript;
            using System;
            using System.Runtime.InteropServices;
            using System.Threading.Tasks;
            public static class Script
            {
                static async Task<int> LoadAsync(bool fail)
                {
                    await AvidContinuations.NextTickAsync();
                    if (fail) throw new InvalidOperationException();
                    return 12;
                }
                [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                public static async void BeginPlay()
                {
                    await LoadAsync(true);
                }
            }
            """;
        const string sourceId = "Scripts/AsyncTaskLanguageError.cs";
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);
        SemanticDocument semantic = SemanticAnalyzer.Analyze(source, sourceId,
            frontend.Source.Sha256, new[]
            {
                new SemanticReferenceSource(CSharpGuestContinuationTests.ReferenceFacade,
                    "generated://AvidScript.Continuations.generated.cs", true),
            });
        Check(semantic.SchemaVersion == SemanticContract.AsyncLanguageErrorSchemaVersion
            && semantic.Diagnostics.Count(item => item.Code == "ASCS5422") == 1,
            "source must publish one async Task throw site");
        Check(!CSharpGuestLowerer.Lower(semantic, new string('a', 64)).Succeeded,
            "ordinary compilation must reject the diagnostic-only artifact");
        CSharpGuestLoweringResult lowered = CSharpGuestLowerer.Lower(semantic,
            new string('a', 64), enableAsyncLanguageErrors: true);
        Check(lowered.Succeeded && lowered.Module is not null,
            "async Task throw lowering: " + string.Join(" | ",
                lowered.Diagnostics.Select(item => item.Code + ":" + item.Message)));
        GuestModule module = lowered.Module!;
        Check(module.SchemaVersion == GuestTaskLanguageErrorValidator.AsyncSchemaVersion
            && module.IrVersion == GuestTaskLanguageErrorValidator.AsyncIrVersion
            && module.Provenance.SemanticSchemaVersion == semantic.SchemaVersion
            && module.LanguageErrorCatalog is { Types.Count: 1, Sources.Count: 1 }
            && module.Imports.Count(item => item.Name == "avid_task_fault_language_error_v1") == 1
            && module.Functions.SelectMany(function => function.Blocks)
                .SelectMany(block => block.Instructions)
                .Count(item => item.Op == "call"
                    && item.TargetId == "import:task_fault_language_error_v1") == 1,
            "IR 21 must bind one source-backed Task fault call");
        Check(GuestModuleValidator.Validate(module).Succeeded,
            "lowered IR must pass its versioned contract");
        byte[] serialized = GuestIrSerializer.Serialize(module);
        Check(serialized.SequenceEqual(GuestIrSerializer.Serialize(
            GuestIrSerializer.Deserialize(serialized))),
            "IR 21 must round-trip canonically");
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(module);
        Check(wasm.Succeeded && wasm.Bytes.Length > 8,
            "IR 21 must compile to WASM: " + string.Join(" | ",
                wasm.Diagnostics.Select(item => item.Message)));
        Check(!GuestModuleValidator.Validate(module with
        {
            SchemaVersion = GuestTaskLanguageErrorValidator.SchemaVersion,
            IrVersion = GuestTaskLanguageErrorValidator.IrVersion,
        }).Succeeded, "IR 20 must reject a Semantic 41 Task fault");

        string directory = Path.Combine(Path.GetTempPath(),
            "avidscript-async-error-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        try
        {
            string semanticPath = Path.Combine(directory, "source.semantic.json");
            string outputPath = Path.Combine(directory, "module.guest-ir.json");
            File.WriteAllBytes(semanticPath, SemanticSerializer.Serialize(semantic));
            Check(GuestCommandLine.Run(new[]
            {
                "--semantic", semanticPath, "--output", outputPath,
            }) != 0 && !File.Exists(outputPath),
                "the formal CLI must keep async language faults opt-in");
            Check(GuestCommandLine.Run(new[]
            {
                "--semantic", semanticPath, "--output", outputPath,
                "--language-errors", "bounded",
            }) == 0 && File.Exists(outputPath),
                "bounded CLI must publish an executable IR 21 module");
            GuestModule published = GuestIrSerializer.Deserialize(File.ReadAllBytes(outputPath));
            Check(published.SchemaVersion == GuestTaskLanguageErrorValidator.AsyncSchemaVersion
                && GuestModuleValidator.Validate(published).Succeeded,
                "formal CLI output must retain the IR 21 contract");
            WasmCompilationResult publishedWasm = WasmModuleCompiler.Compile(published);
            Check(publishedWasm.Succeeded && publishedWasm.Bytes.Length > 8,
                "formal CLI IR 21 must compile to WASM");
            string? fixtureDirectory = Environment.GetEnvironmentVariable(
                "AVIDSCRIPT_ASYNC_LANGUAGE_ERROR_WASM_DIR");
            if (!string.IsNullOrWhiteSpace(fixtureDirectory))
            {
                Directory.CreateDirectory(fixtureDirectory);
                string stem = Path.Combine(fixtureDirectory, "csharp-task-language-error");
                File.WriteAllBytes(stem + ".wasm", publishedWasm.Bytes);
                File.WriteAllBytes(stem + ".guest-ir.json",
                    GuestIrSerializer.Serialize(published));
            }
        }
        finally
        {
            Directory.Delete(directory, recursive: true);
        }
        return 2;
    }

    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
}
