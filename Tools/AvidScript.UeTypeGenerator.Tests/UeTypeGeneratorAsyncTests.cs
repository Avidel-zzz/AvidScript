using System;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpSemantic;
using AvidScript.UeTypeGenerator;

internal static class UeTypeGeneratorAsyncTests
{
    public static int Run()
    {
        int checks = 0;
        void Check(bool valid, string message)
        {
            if (!valid) throw new InvalidOperationException(message);
            checks++;
        }
        void Reject(SemanticDocument document, bool bounded, string message)
        {
            try { UeTypeShellGenerator.Generate(SemanticSerializer.Serialize(document), "AvidScriptGenerated", "5.8", bounded); }
            catch (InvalidOperationException) { checks++; return; }
            throw new InvalidOperationException(message);
        }
        const string plain = """
            using System.Threading.Tasks;
            using AvidScript;
            [UClass] public partial class PlainTaskActor : AvidActor
            {
                [UProperty] public int Value { get; set; }
                [UFunction] public void Start() { Store(); }
                private async void Store() { int result = await Read(); Value = result; }
                private static async Task<int> Read() { return 42; }
            }
            """;
        SemanticDocument ordinary = Analyze(plain, false);
        Check(ordinary.Succeeded && ordinary.SchemaVersion == 35, "Expected an ordinary Task result artifact: "
            + ordinary.SchemaVersion + " / " + string.Join(" | ", ordinary.Diagnostics.Select(d => d.Code + ": " + d.Message)));
        var ordinaryShell = UeTypeShellGenerator.Generate(SemanticSerializer.Serialize(ordinary), "AvidScriptGenerated", "5.8");
        Check(ordinaryShell.Manifest.SemanticSchemaVersion == 35, "Ordinary Task shell must preserve its original version");

        SemanticDocument fault = Analyze(plain.Replace("return 42;", "throw new System.InvalidOperationException();", StringComparison.Ordinal), false);
        Check(!fault.Succeeded && fault.SchemaVersion == 41
            && fault.Diagnostics.Any(d => d.Severity == "error" && d.Code == "ASCS5422"), "Expected an async language-error preview");
        Reject(fault, false, "Default generation accepted async language errors");
        Check(UeTypeShellGenerator.Generate(SemanticSerializer.Serialize(fault), "AvidScriptGenerated", "5.8", true)
            .Manifest.SemanticSchemaVersion == 41, "Async error shell must preserve its original version");
        Reject(fault with { Diagnostics = Array.Empty<SemanticDiagnostic>() }, true, "Failed artifact without diagnostics accepted");
        Reject(fault with { Succeeded = true }, true, "Successful artifact containing errors accepted");
        var diagnostic = fault.Diagnostics.First(d => d.Severity == "error");
        Reject(fault with { Diagnostics = fault.Diagnostics.Append(diagnostic with { Code = "ASCS2001" }).ToArray() },
            true, "Unrelated semantic error accepted");

        SemanticDocument shared = Analyze(File.ReadAllText("Fixtures/Phase66/SharedTaskLifetime.cs"), true);
        Check(shared.SchemaVersion == 45
            && shared.Diagnostics.Where(d => d.Severity == "error").All(d => d.Code == "ASCS5422"),
            "Expected shared Task lifetime preview: " + string.Join(" | ", shared.Diagnostics.Select(d => d.Message)));
        Reject(shared, false, "Default generation accepted an async preview");
        byte[] artifact = SemanticSerializer.Serialize(shared);
        var generated = UeTypeShellGenerator.Generate(artifact, "AvidScriptGenerated", "5.8", true);
        Check(generated.Manifest.SemanticSchemaVersion == 45 && generated.Manifest.SemanticVersion == "1.54"
            && generated.Manifest.SemanticArtifactSha256 == Convert.ToHexString(SHA256.HashData(artifact)).ToLowerInvariant(),
            "Shell manifest must identify the exact original async artifact");
        Check(generated.Manifest.Types.Single().Properties.Single().Name == "Value"
            && generated.Manifest.Types.Single().Functions.Single().Name == "GetScriptValue",
            "Task helpers must not leak into UHT reflected declarations");
        var repeated = UeTypeShellGenerator.Generate(artifact, "AvidScriptGenerated", "5.8", true);
        Check(generated.Files.All(file => repeated.Files[file.Key].SequenceEqual(file.Value)), "Async native shell must be deterministic");

        Reject(shared with { SchemaVersion = 46, SemanticVersion = "1.55" }, true, "Future contract accepted");
        Reject(shared with { SemanticVersion = "1.53" }, true, "Mismatched schema/version accepted");
        Reject(shared with { SchemaVersion = 44, SemanticVersion = "1.53" }, true, "Task lifetime downgrade accepted");
        var taskMethod = shared.AsyncMethods.First(method => method.TaskLocalLifetimes is not null);
        Reject(shared with { AsyncMethods = shared.AsyncMethods.Select(method => method == taskMethod
            ? method with { TaskLocalLifetimes = null } : method).ToArray() }, true, "Missing Task ownership plan accepted");
        var protectedMethod = shared.AsyncMethods.First(method => method.ExceptionPlan is not null);
        Reject(shared with { AsyncMethods = shared.AsyncMethods.Select(method => method == protectedMethod
            ? method with { ExceptionPlan = null } : method).ToArray() }, true, "Missing cancellation plan accepted");
        return checks;
    }

    private static SemanticDocument Analyze(string source, bool cancellation)
    {
        const string sourceId = "Fixtures/Phase66/GeneratedAsyncType.cs";
        string hash = FrontendAnalyzer.Analyze(source, sourceId).Source.Sha256;
        return SemanticAnalyzer.Analyze(source, sourceId, hash,
            new[] { new SemanticReferenceSource(UeTypeGeneratorTests.Facade + AsyncFacade, "generated://AvidScript.UeTypes.cs") },
            new SemanticCompilerWorkspace(), enableAsyncExceptionFlow: cancellation,
            enableDirectAwaitCleanup: cancellation, enableAsyncCancellationFlow: cancellation);
    }

    private const string AsyncFacade = """

        public readonly struct AvidCancellationToken { internal readonly long Value; }
        public readonly struct AvidCancellationSource {
            private readonly long Value;
            public readonly AvidCancellationToken Token;
            public static AvidCancellationSource Create() => default;
            public bool Cancel() => true;
            public bool Release() => true;
        }
        public readonly struct AvidDelayAwaitable {
            private readonly int Marker;
            public AvidDelayAwaitable WithCancellation(AvidCancellationToken token) => default;
            public AvidDelayAwaiter GetAwaiter() => default;
        }
        public readonly struct AvidDelayAwaiter : System.Runtime.CompilerServices.INotifyCompletion {
            private readonly int Marker;
            public bool IsCompleted => false;
            public void OnCompleted(Action continuation) { }
            public void GetResult() { }
        }
        public static class AvidContinuations { public static AvidDelayAwaitable NextTickAsync() => default; }
        """;
}
