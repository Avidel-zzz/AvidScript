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
        const string handlerSource = """
            using AvidScript;
            using System;
            using System.Threading.Tasks;
            public static class Script
            {
                static async Task<int> LoadAsync()
                {
                    await AvidContinuations.NextTickAsync();
                    return 12;
                }
                static async Task<int> RunAsync()
                {
                    try { int value = await LoadAsync(); return value; }
                    catch (InvalidOperationException) { return 7; }
                }
            }
            """;
        FrontendDocument handlerFrontend = FrontendAnalyzer.Analyze(handlerSource,
            "Scripts/AsyncHandlerVersion.cs");
        SemanticDocument handlerSemantic = SemanticAnalyzer.Analyze(handlerSource,
            "Scripts/AsyncHandlerVersion.cs", handlerFrontend.Source.Sha256,
            new[] { new SemanticReferenceSource(
                CSharpGuestContinuationTests.ReferenceFacade,
                "generated://AvidScript.Continuations.generated.cs", true) },
            new SemanticCompilerWorkspace(), enableAsyncExceptionFlow: true);
        Check(handlerSemantic.Succeeded
            && handlerSemantic.SchemaVersion == SemanticContract.AsyncExceptionFlowSchemaVersion
            && handlerSemantic.AsyncMethods.Any(method => method.ExceptionPlan is not null)
            && !CSharpGuestLowerer.Lower(handlerSemantic, new string('a', 64)).Succeeded,
            "default Guest compilation must reject an opt-in Semantic 42 handler plan");
        SemanticDocument cancellationSemantic = SemanticAnalyzer.Analyze(handlerSource,
            "Scripts/AsyncHandlerVersion.cs", handlerFrontend.Source.Sha256,
            new[] { new SemanticReferenceSource(CSharpGuestContinuationTests.ReferenceFacade,
                "generated://AvidScript.Continuations.generated.cs", true) },
            new SemanticCompilerWorkspace(), enableAsyncExceptionFlow: true,
            enableAsyncCancellationFlow: true);
        Check(cancellationSemantic.Succeeded && cancellationSemantic.SchemaVersion == 44
            && SemanticAsyncInvocationValidator.IsValid(cancellationSemantic),
            "Task-only cancellation projection must have a valid Semantic 44 identity");
        CSharpGuestLoweringResult preview = CSharpGuestLowerer.Lower(cancellationSemantic,
            new string('a', 64));
        Check(!preview.Succeeded && preview.Module is null
            && preview.Diagnostics.Any(item => item.Code == "ASCG1004"),
            "Semantic 44 still requires explicit bounded language-error compilation");
        string exceptionSource = File.ReadAllText(Path.Combine(
            Directory.GetCurrentDirectory(), "Fixtures", "Phase66", "AsyncExceptionFlow.cs"));
        const string exceptionSourceId = "Fixtures/Phase66/AsyncExceptionFlow.cs";
        FrontendDocument exceptionFrontend = FrontendAnalyzer.Analyze(
            exceptionSource, exceptionSourceId);
        SemanticDocument exceptionSemantic = SemanticAnalyzer.Analyze(
            exceptionSource, exceptionSourceId, exceptionFrontend.Source.Sha256,
            new[] { new SemanticReferenceSource(
                CSharpGuestContinuationTests.ReferenceFacade,
                "generated://AvidScript.Continuations.generated.cs", true) },
            new SemanticCompilerWorkspace(), enableAsyncExceptionFlow: true);
        CSharpGuestLoweringResult exceptionLowered = CSharpGuestLowerer.Lower(
            exceptionSemantic, new string('a', 64), enableAsyncLanguageErrors: true);
        Check(exceptionLowered.Succeeded && exceptionLowered.Module is not null,
            "Semantic 42 async exception fixture must lower to IR 22: "
                + string.Join(" | ", exceptionLowered.Diagnostics.Select(item =>
                    item.Code + ":" + item.Message)));
        GuestModule exceptionModule = exceptionLowered.Module!;
        Check(exceptionModule.SchemaVersion
                == GuestTaskLanguageErrorValidator.ExceptionFlowSchemaVersion
            && exceptionModule.IrVersion
                == GuestTaskLanguageErrorValidator.ExceptionFlowIrVersion
            && GuestModuleValidator.Validate(exceptionModule).Succeeded,
            "IR 22 must retain paired Semantic 42 provenance and pass validation");
        GuestAsyncExceptionRoute[] routes = exceptionModule.AsyncExceptionRoutes?.ToArray()
            ?? Array.Empty<GuestAsyncExceptionRoute>();
        Check(routes.Length == 2
            && routes.All(route => route.CallbackId > 0),
            "IR 22 must name each protected Task await");
        GuestAsyncExceptionRoute firstRoute = routes[0];
        Check(!GuestModuleValidator.Validate(exceptionModule with
        {
            AsyncExceptionRoutes = null,
        }).Succeeded, "IR 22 must reject missing await routes");
        Check(GuestModuleValidator.Validate(exceptionModule with
        {
            AsyncExceptionRoutes = routes.Take(1).ToArray(),
        }).Diagnostics.Any(item => item.Code == "ASIR1030"),
            "IR 22 must reject an omitted protected await");
        Check(!GuestModuleValidator.Validate(exceptionModule with
        {
            AsyncExceptionRoutes = routes.Select((route, index) => index == 0
                ? route with { FaultTargetBlockId = route.CancellationTargetBlockId }
                : route).ToArray(),
        }).Succeeded, "IR 22 must reject a forged fault successor");
        Check(!GuestModuleValidator.Validate(exceptionModule with
        {
            AsyncExceptionRoutes = routes.Select((route, index) => index == 0
                ? route with { OwnerLocalId = "local:forged_owner" }
                : route).ToArray(),
        }).Succeeded, "IR 22 must reject an unbound source Task owner");
        string faultRetainedId = firstRoute.AwaitBlockId + ":task_failed:fault:retained";
        GuestFunction sourceFunction = exceptionModule.Functions.Single(function =>
            function.Id == firstRoute.MethodFunctionId);
        Check(sourceFunction.Blocks.Any(block => block.Id == faultRetainedId),
            "fixture must contain the immediate fault owner branch");
        GuestFunction forgedSource = sourceFunction with
        {
            Blocks = sourceFunction.Blocks.Select(block => block.Id == faultRetainedId
                ? block with { Terminator = block.Terminator with
                    { TargetBlockId = firstRoute.NormalTargetBlockId } }
                : block).ToArray(),
        };
        GuestModule forgedFlow = exceptionModule with
        {
            Functions = exceptionModule.Functions.Select(function =>
                function.Id == forgedSource.Id ? forgedSource : function).ToArray(),
        };
        Check(GuestModuleValidator.Validate(forgedFlow).Diagnostics.Any(item =>
                item.Code == "ASIR1030"),
            "IR 22 must reject a fault branch redirected into the normal successor");
        Check(GuestModuleValidator.Validate(exceptionModule with
        {
            SchemaVersion = GuestTaskLanguageErrorValidator.AsyncSchemaVersion,
            IrVersion = GuestTaskLanguageErrorValidator.AsyncIrVersion,
            Provenance = exceptionModule.Provenance with
            {
                SemanticSchemaVersion = SemanticContract.AsyncLanguageErrorSchemaVersion,
                SemanticVersion = SemanticContract.AsyncLanguageErrorSemanticVersion,
            },
        }).Diagnostics.Any(item => item.Code == "ASIR1030"),
            "IR 21 must reject IR 22 route metadata even with paired provenance");
        Check(!GuestModuleValidator.Validate(exceptionModule with
        {
            SchemaVersion = GuestTaskLanguageErrorValidator.AsyncSchemaVersion,
            IrVersion = GuestTaskLanguageErrorValidator.AsyncIrVersion,
        }).Succeeded, "IR 21 must reject a downgraded Semantic 42 exception-flow module");
        Check(!GuestModuleValidator.Validate(exceptionModule with
        {
            Provenance = exceptionModule.Provenance with
            {
                SemanticSchemaVersion = SemanticContract.AsyncLanguageErrorSchemaVersion,
                SemanticVersion = SemanticContract.AsyncLanguageErrorSemanticVersion,
            },
        }).Succeeded, "IR 22 must reject spoofed Semantic 41 provenance");
        byte[] exceptionBytes = GuestIrSerializer.Serialize(exceptionModule);
        Check(exceptionBytes.SequenceEqual(GuestIrSerializer.Serialize(
            GuestIrSerializer.Deserialize(exceptionBytes))),
            "IR 22 must round-trip canonically");
        WasmCompilationResult exceptionWasm = WasmModuleCompiler.Compile(exceptionModule);
        Check(exceptionWasm.Succeeded && exceptionWasm.Bytes.Length > 8,
            "IR 22 must compile to WASM: " + string.Join(" | ",
                exceptionWasm.Diagnostics.Select(item => item.Message)));
        string exceptionCliDirectory = Path.Combine(Path.GetTempPath(),
            "avidscript-async-exception-cli-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(exceptionCliDirectory);
        try
        {
            string sourcePath = Path.Combine(exceptionCliDirectory, "flow.cs");
            string frontendPath = Path.Combine(exceptionCliDirectory, "flow.frontend.json");
            string facadePath = Path.Combine(exceptionCliDirectory, "continuations.generated.cs");
            string semanticPath = Path.Combine(exceptionCliDirectory, "flow.semantic.json");
            string guestPath = Path.Combine(exceptionCliDirectory, "flow.guest-ir.json");
            File.WriteAllText(sourcePath, exceptionSource);
            File.WriteAllBytes(frontendPath, FrontendSerializer.Serialize(exceptionFrontend));
            File.WriteAllText(facadePath, CSharpGuestContinuationTests.ReferenceFacade);
            int semanticExit = SemanticCommandLine.Run(new[]
            {
                "--source", sourcePath, "--source-id", exceptionSourceId,
                "--frontend", frontendPath, "--output", semanticPath,
                "--executable-reference-source", facadePath,
                "--async-exception-flow", "enabled",
            });
            Check(semanticExit == 1, "explicit Semantic CLI opt-in must publish a bounded schema 42 diagnostic artifact: exit="
                + semanticExit + " diagnostics=" + (File.Exists(semanticPath)
                    ? string.Join(" | ", SemanticSerializer.Deserialize(
                        File.ReadAllBytes(semanticPath)).Diagnostics.Select(item =>
                        item.Code + ":" + item.Message)) : "no artifact"));
            SemanticDocument publishedSemantic = SemanticSerializer.Deserialize(
                File.ReadAllBytes(semanticPath));
            Check(publishedSemantic.SchemaVersion
                == SemanticContract.AsyncExceptionFlowSchemaVersion,
                "Semantic CLI must retain the protected exception plan");
            Check(GuestCommandLine.Run(new[]
            {
                "--semantic", semanticPath, "--output", guestPath,
            }) != 0 && !File.Exists(guestPath),
                "Guest CLI default must still reject Semantic 42");
            Check(GuestCommandLine.Run(new[]
            {
                "--semantic", semanticPath, "--output", guestPath,
                "--language-errors", "bounded",
            }) == 0 && File.Exists(guestPath),
                "bounded Guest CLI must publish IR 22");
            Check(GuestIrSerializer.Deserialize(File.ReadAllBytes(guestPath))
                .SchemaVersion == GuestTaskLanguageErrorValidator.ExceptionFlowSchemaVersion,
                "published IR must retain schema 22");
        }
        finally
        {
            Directory.Delete(exceptionCliDirectory, recursive: true);
        }
        Check(!CSharpGuestLowerer.Lower(semantic, new string('a', 64)).Succeeded,
            "ordinary compilation must reject the diagnostic-only artifact");
        Check(!CSharpGuestLowerer.Lower(semantic with
        {
            RejectedAsyncExceptionFlows = Array.Empty<SemanticExceptionFlow>(),
        }, new string('a', 64), enableAsyncLanguageErrors: true).Succeeded,
            "IR 21 must reject even an empty diagnostic async exception sidecar");
        SemanticAsyncMethod taskMethod = semantic.AsyncMethods.Single(method =>
            method.ErrorPlan is not null);
        Check(!CSharpGuestLowerer.Lower(semantic with
        {
            AsyncMethods = semantic.AsyncMethods.Select(method => method == taskMethod
                ? method with
                {
                    Segments = method.Segments.Select(segment => segment.Ordinal
                        == method.EntrySegmentOrdinal
                        ? segment with
                        {
                            Transfer = segment.Transfer! with
                            {
                                ExceptionTypeId = "type:global::System.InvalidOperationException",
                            },
                        }
                        : segment).ToArray(),
                }
                : method).ToArray(),
        }, new string('a', 64), enableAsyncLanguageErrors: true).Succeeded,
            "IR 21 must reject preview-only catch metadata on executable transfers");
        SemanticAsyncSegment taskAwait = taskMethod.Segments.Single(segment =>
            segment.AwaitSite is not null);
        Check(!CSharpGuestLowerer.Lower(semantic with
        {
            AsyncMethods = semantic.AsyncMethods.Select(method => method == taskMethod
                ? method with
                {
                    Segments = method.Segments.Select(segment => segment.Ordinal
                        == taskAwait.Ordinal
                        ? segment with
                        {
                            Transfer = segment.Transfer! with
                            {
                                CancellationTarget = method.EntrySegmentOrdinal,
                            },
                        }
                        : segment).ToArray(),
                }
                : method).ToArray(),
        }, new string('a', 64), enableAsyncLanguageErrors: true).Succeeded,
            "IR 21 must reject a preview-only cancellation successor");
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
            && module.Imports.Count(item => item.Name == "avid_language_error_report_v1") == 1
            && module.Functions.SelectMany(function => function.Blocks)
                .SelectMany(block => block.Instructions)
                .Count(item => item.Op == "call"
                    && item.TargetId == "import:task_fault_language_error_v1") == 1
            && module.Functions.SelectMany(function => function.Blocks)
                .SelectMany(block => block.Instructions)
                .Count(item => item.Op == "call"
                    && item.TargetId == "import:language_error_report_v1") == 2,
            "IR 21 must bind Task fault and unhandled await report calls");
        GuestBasicBlock[] reportBlocks = module.Functions.SelectMany(function => function.Blocks)
            .Where(block => block.Instructions.Any(instruction =>
                instruction.TargetId == "import:language_error_report_v1"))
            .ToArray();
        Check(reportBlocks.Length == 2 && reportBlocks.All(block =>
        {
            var instructions = block.Instructions.ToList();
            int meta = instructions.FindIndex(item =>
                item.TargetId == "import:task_language_error_meta_v1");
            int root = instructions.FindIndex(item =>
                item.TargetId == "import:task_language_error_root_v1");
            int report = instructions.FindIndex(item =>
                item.TargetId == "import:language_error_report_v1");
            return meta >= 0 && root > meta && report > root
                && instructions[meta].OperandIds.SequenceEqual(instructions[root].OperandIds)
                && instructions[report].OperandIds[2] == instructions[root].ResultId;
        }), "ready and resumed failures must read metadata and root from the same owned task before reporting");
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
        return 3;
    }

    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
}
