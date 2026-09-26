using System;
using System.Linq;
using System.Security.Cryptography;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;

internal static class CSharpGuestAsyncSynchronousExceptionTests
{
    public static int Run()
    {
        const string source = """
            using System; using System.Threading.Tasks;
            public static class Script {
                public static int Read(int mode) { if (mode == 0) throw new ArgumentException(); return 7; }
                public static async Task<int> Run(int mode) { return Read(mode); }
            }
            """;
        const string sourceId = "Scripts/SynchronousExceptionPublication.cs";
        string hash = FrontendAnalyzer.Analyze(source, sourceId).Source.Sha256;
        var document = SemanticAnalyzer.Analyze(source, sourceId, hash,
            Array.Empty<SemanticReferenceSource>(), new SemanticCompilerWorkspace(),
            enableAsyncExceptionFlow: true, enableAsyncCancellationFlow: true,
            enableAsyncSynchronousExceptions: true);
        int count = 0;
        void Check(bool valid, string reason)
        {
            if (!valid) throw new InvalidOperationException(reason);
            count++;
        }
        Check(SemanticAsyncInvocationValidator.IsValid(document), "New source routes must validate before publication is attempted");
        foreach (bool enabled in new[] { false, true })
        foreach (var candidate in new[]
        {
            document,
            document with { SchemaVersion = 47, SemanticVersion = "1.56" },
            document with { SemanticVersion = "1.58" },
            document with { AsyncMethods = document.AsyncMethods.Select(method => method with
            {
                Segments = method.Segments.Select(segment => segment with { SynchronousExceptionTarget = null }).ToArray(),
            }).ToArray() },
        })
        {
            string semanticHash = Convert.ToHexString(SHA256.HashData(SemanticSerializer.Serialize(candidate))).ToLowerInvariant();
            var result = CSharpGuestLowerer.Lower(candidate, semanticHash, enableAsyncLanguageErrors: enabled);
            Check(!result.Succeeded && result.Module is null && result.Diagnostics.Any(item => item.Code == "ASCG1026"),
                "Unintegrated or disguised routes must never publish a Guest module");
        }
        return count;
    }
}
