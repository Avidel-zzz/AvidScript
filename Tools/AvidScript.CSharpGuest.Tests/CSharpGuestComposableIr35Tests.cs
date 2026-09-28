using System;
using System.Linq;
using System.Security.Cryptography;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class CSharpGuestComposableIr35Tests
{
    public static int Run()
    {
        int count = 0;
        void Check(bool valid, string reason)
        {
            if (!valid) throw new InvalidOperationException("Composable IR 35: " + reason);
            count++;
        }

        const string source = """
            using System.Runtime.InteropServices;
            public class Cache<T> { public static int Value; }
            public static class Script {
                [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")] public static void Begin() {}
                [UnmanagedCallersOnly(EntryPoint = "run")] public static int ExportRun() { return Run(); }
                public static int Run() {
                    Cache<int>.Value += 1; Cache<long>.Value += 2;
                    return Cache<int>.Value * 10 + Cache<long>.Value;
                }
            }
            """;
        const string sourceId = "Scripts/ComposableStaticBase17.cs";
        var semantic = SemanticAnalyzer.Analyze(source, sourceId,
            FrontendAnalyzer.Analyze(source, sourceId).Source.Sha256,
            Array.Empty<SemanticReferenceSource>(), new SemanticCompilerWorkspace(), enableStaticInitialization: true);
        byte[] semanticBytes = SemanticSerializer.Serialize(semantic);
        string semanticHash = Convert.ToHexString(SHA256.HashData(semanticBytes)).ToLowerInvariant();
        Check(CSharpStaticInitializationCompiler.TryLower(semantic, semanticHash, out var staticModule, out string? error)
            && staticModule is not null, "C# static initializer lowering: " + error);
        Check(staticModule!.SchemaVersion == 27 && staticModule.StaticStorage is { BaseSchemaVersion: 17, BaseIrVersion: "1.16" }
            && staticModule.LanguageOutcomeTypes is not null && staticModule.LanguageErrorCatalog is not null,
            "C# static initialization needs the language-error base even with an int field");

        GuestType[] types = staticModule.Types.Any(type => type.Id == "type:int64")
            ? staticModule.Types.Append(GuestCancellationTokens.ValueType()).ToArray()
            : staticModule.Types.Concat(new[] {
                new GuestType("type:int64", "scalar", "i64", Array.Empty<GuestField>(), null, null, 8, 8),
                GuestCancellationTokens.ValueType(),
            }).ToArray();
        GuestLayoutResult layout = GuestLayoutBuilder.Build(types, staticModule.Globals, staticModule.DataSegments);
        Check(layout.Succeeded && layout.Layout is not null, "combined module layout");
        GuestFunction tokenProbe = new("function:token_probe", Array.Empty<GuestRegister>(), new[] {
            new GuestRegister("identity", "type:int64"), new GuestRegister("token", GuestCancellationTokens.TypeId),
            new GuestRegister("roundtrip", "type:int64"),
        }, "type:int64", "entry", new[] { new GuestBasicBlock("entry", new[] {
            new GuestInstruction("constant", "identity", Array.Empty<string>(), null, null, new("int64", "7")),
            new GuestInstruction("stack_alloc", "token", Array.Empty<string>(), null, null, null),
            new GuestInstruction("field_store", null, new[] { "token", "identity" }, GuestCancellationTokens.FieldId, null, null),
            new GuestInstruction("field_load", "roundtrip", new[] { "token" }, GuestCancellationTokens.FieldId, null, null),
        }, new GuestTerminator("return", null, null, null, "roundtrip")) });
        GuestModule composed = staticModule with
        {
            SchemaVersion = GuestComposableCapabilities.SchemaVersion,
            IrVersion = GuestComposableCapabilities.IrVersion,
            // The token probe is hand-added to an existing C# IR 27 module.
            // Keep its old source provenance visible; this is not C# Semantic 54 lowering.
            Language = "guest-ir",
            Types = types,
            MemoryLayout = layout.Layout!,
            Functions = staticModule.Functions.Append(tokenProbe).ToArray(),
            CancellationTokens = new(17, "1.16"),
            CapabilityManifest = GuestCapabilityManifest.Create(17, "1.16", new[] {
                new GuestCapability(GuestComposableCapabilities.StaticStorage, 1),
                new GuestCapability(GuestComposableCapabilities.CancellationTokenValue, 1),
            }),
        };
        GuestValidationResult validation = GuestModuleValidator.Validate(composed);
        Check(validation.Succeeded, "real static-initializer module plus token value: "
            + string.Join(" | ", validation.Diagnostics.Select(item => item.Code + ": " + item.Message)));
        byte[] bytes = GuestIrSerializer.Serialize(composed);
        Check(GuestModuleValidator.Validate(GuestIrSerializer.Deserialize(bytes)).Succeeded
            && bytes.SequenceEqual(GuestIrSerializer.Serialize(GuestIrSerializer.Deserialize(bytes))),
            "canonical base-17 static initialization and token IR");
        Check(!GuestModuleValidator.Validate(composed with { Language = "csharp" }).Succeeded,
            "a composed C# artifact cannot claim an older Semantic source version");
        Check(!GuestModuleValidator.Validate(composed with { LanguageErrorCatalog = null }).Succeeded,
            "base 17 cannot omit its language-error catalog");
        Check(!GuestModuleValidator.Validate(composed with { LanguageOutcomeTypes = null }).Succeeded,
            "base 17 cannot omit its outcome layout");
        Check(!GuestModuleValidator.Validate(composed with { CapabilityManifest = composed.CapabilityManifest! with
            { ExecutionBaseSchemaVersion = 14, ExecutionBaseIrVersion = "1.13" } }).Succeeded,
            "base 17 plans cannot be relabeled as base 14");
        Check(!GuestModuleValidator.Validate(composed with { Types = composed.Types.Where(type =>
            type.Id != GuestCancellationTokens.TypeId).ToArray() }).Succeeded,
            "base 17 token plan requires the nominal value type");
        var badReport = composed with { Imports = composed.Imports.Select(import =>
            import.Id == "import:language_error_report_v1"
                ? import with { ParameterTypeIds = new[] { "type:int32", "type:int32", "type:int64" } }
                : import).ToArray() };
        Check(GuestModuleValidator.Validate(badReport).Diagnostics.Any(item => item.Code == "ASIR1013"),
            "base 17 cannot weaken the managed error-report import signature: "
                + string.Join(", ", composed.Imports.Select(import => import.Id)));
        Check(GuestModuleValidator.Validate(composed with { LanguageErrorCatalog = composed.LanguageErrorCatalog! with
            { Sources = composed.LanguageErrorCatalog.Sources.Select((site, index) =>
                index == 0 ? site with { Start = -1 } : site).ToArray() } }).Diagnostics.Any(item => item.Code == "ASIR1027"),
            "base 17 cannot bypass language-error source bounds");
        Check(!WasmModuleCompiler.Compile(composed).Succeeded,
            "WASM publication remains closed until IR 35 provenance and native loading are implemented");
        return count;
    }
}
