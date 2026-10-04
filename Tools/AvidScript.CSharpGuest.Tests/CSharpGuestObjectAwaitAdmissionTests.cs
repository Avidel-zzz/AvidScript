using System;
using System.Linq;
using System.Security.Cryptography;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;

internal static class CSharpGuestObjectAwaitAdmissionTests
{
    public static int Run()
    {
        int count = 0;
        void Check(bool condition, string message)
        {
            if (!condition) throw new InvalidOperationException("Object await admission: " + message);
            count++;
        }
        // Source-side capability must not silently fall through an older IR
        // envelope. Replace this boundary only when the paired object payload,
        // status dispatch and ownership validator are implemented together.
        foreach (bool staticState in new[] { false, true })
        foreach (bool tokens in new[] { false, true })
        foreach (bool voidOwner in new[] { false, true })
        {
            string source = "using System; using System.Threading; using System.Threading.Tasks; using AvidScript; "
                + "public static class Script { " + (staticState ? "static int State = 1; " : "")
                + "public static async " + (voidOwner ? "void" : "Task<int>") + " Run("
                + (tokens ? "CancellationToken token" : "") + ") { try { "
                + "var loaded = await AvidAssets.LoadObjectAsync(\"/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture\")"
                + (tokens ? ".WithCancellation(token)" : "") + "; " + (voidOwner ? "return;" : "return 1;")
                + " } catch (OperationCanceledException error) { " + (voidOwner ? "return;" : "return 7;")
                + " } finally { " + (staticState ? "State = State + 1;" : "int cleanup = 1;") + " } } }";
            var semantic = Analyze(source, voidOwner: voidOwner);
            Check(SemanticObjectAwaitCancellation.Has(semantic)
                && SemanticComposableCapabilityValidator.IsValid(semantic)
                && SemanticObjectAwaitCancellationValidator.IsValid(semantic)
                && SemanticAsyncInvocationValidator.IsValid(semantic), "valid source projection: " + Describe(semantic));
            string hash = Convert.ToHexString(SHA256.HashData(SemanticSerializer.Serialize(semantic))).ToLowerInvariant();
            Check(CSharpLanguageCapabilityCompiler.IsRequired(semantic), "object source selects capability admission");
            Check(!CSharpLanguageCapabilityCompiler.TryLower(semantic, hash, out var module, out var error)
                && module is null && error == "Object await cancellation requires the paired Guest execution contract.",
                "valid source cannot publish an incomplete execution envelope");
            var ordinary = CSharpGuestLowerer.Lower(semantic, hash);
            Check(!ordinary.Succeeded && ordinary.Module is null, "ordinary lowering cannot bypass capability admission");
            Check(!CSharpLanguageCapabilityCompiler.TryLower(semantic with { CapabilityManifest = null }, hash, out _, out _),
                "removing the manifest does not authorize execution");
        }
        const string legacy = """
            using System.Runtime.InteropServices; using AvidScript;
            public static class Script {
                [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                public static async void BeginPlay() {
                    await AvidAssets.LoadObjectAsync("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture");
                }
            }
            """;
        var current = Analyze(legacy);
        var previous = Analyze(legacy, objectCancellation: false);
        byte[] currentBytes = SemanticSerializer.Serialize(current);
        Check(current.CapabilityManifest is null && currentBytes.SequenceEqual(SemanticSerializer.Serialize(previous)),
            "ordinary async void object source retains canonical bytes");
        var lowered = CSharpGuestLowerer.Lower(current, Convert.ToHexString(SHA256.HashData(currentBytes)).ToLowerInvariant());
        Check(lowered.Succeeded && lowered.Module?.Imports.Any(import => import.Module == "env"
            && import.Name == "continuation_load_object") == true, "legacy object producer remains executable: "
                + string.Join(" | ", lowered.Diagnostics.Select(diagnostic => diagnostic.Message)));
        return count;
    }

    private static SemanticDocument Analyze(string source, bool voidOwner = false, bool objectCancellation = true) =>
        SemanticAnalyzer.Analyze(source, "Scripts/ObjectAwaitAdmission.cs",
            FrontendAnalyzer.Analyze(source, "Scripts/ObjectAwaitAdmission.cs").Source.Sha256,
            new[] { new SemanticReferenceSource(Facade, "generated://ObjectAwaitAdmission.cs", true) },
            new SemanticCompilerWorkspace(), enableAsyncExceptionFlow: true,
            enableDirectAwaitCleanup: objectCancellation, enableAsyncCancellationFlow: true,
            enableStaticInitialization: true, enableAsyncSynchronousExceptions: true,
            enableAsyncCatchVariables: true, enableCancellationTokens: true, enableAsyncVoidErrorOwner: voidOwner);

    private static string Describe(SemanticDocument document) => $"{document.SchemaVersion}/{document.SemanticVersion}; "
        + string.Join(" | ", document.Diagnostics.Select(diagnostic => diagnostic.Code + ": " + diagnostic.Message));

    private static string Facade => CSharpGuestCancellationTokenTests.AsyncFacade.Replace(
        "public AvidObjectAwaitable WithCancellation(AvidCancellationToken token) => default;",
        "public AvidObjectAwaitable WithCancellation(AvidCancellationToken token) => default; public AvidObjectAwaitable WithCancellation(System.Threading.CancellationToken token) => default;",
        StringComparison.Ordinal);
}
