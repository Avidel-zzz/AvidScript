using System;
using System.Linq;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.WasmBackend;

internal static class CSharpGuestMethodCatalogTests
{
    public static int Run()
    {
        SemanticDocument document = CSharpGuestLexicalCaptureTests.Analyze("""
            using System;
            using System.Runtime.InteropServices;
            public class Counter { public int Value; public int Read() => Value; }
            public static class Script {
                [UnmanagedCallersOnly(EntryPoint = "run")]
                public static int Run() { Counter c = new Counter(); c.Value = 7; Func<int> read = c.Read; return read(); }
            }
            """);
        int count = 0;
        var current = CSharpGuestLowerer.Lower(document, new string('a', 64));
        Check(current.Succeeded && WasmModuleCompiler.Compile(current.Module!).Succeeded, "new catalog contract retains existing execution");
        SemanticDocument legacy = CSharpGuestSemanticFixture.WithoutNamedGenericShapes(document)
            with { SchemaVersion = 25, SemanticVersion = "1.29", UeMethodCatalog = null };
        var previous = CSharpGuestLowerer.Lower(legacy, new string('a', 64));
        Check(previous.Succeeded && WasmModuleCompiler.Compile(previous.Module!).Succeeded, "1.29 instance delegates still execute without inventing catalog metadata");
        foreach (SemanticDocument malformed in new[] {
            document with { UeMethodCatalog = null },
            document with { UeMethodCatalog = document.UeMethodCatalog! with { SchemaVersion = 2 } },
            legacy with { UeMethodCatalog = document.UeMethodCatalog },
        })
        {
            var result = CSharpGuestLowerer.Lower(malformed, new string('a', 64));
            Check(!result.Succeeded && result.Module is null && result.Diagnostics.Any(item => item.Code == "ASCG1001"), "missing/future/downgraded catalog rejects at Guest boundary");
        }
        const string source = """
            using AvidScript;
            [UClass] public partial class Actor : AvidActor {
                private int Read() => 7;
                [UFunction] public int Entry() => Read();
            }
            """;
        const string facade = """
            namespace AvidScript {
                [System.AttributeUsage(System.AttributeTargets.Class)] public sealed class UClassAttribute : System.Attribute { }
                [System.AttributeUsage(System.AttributeTargets.Method)] public sealed class UFunctionAttribute : System.Attribute { }
                public abstract class AvidActor { }
            }
            """;
        const string path = "Scripts/UeMethodCatalog.cs";
        SemanticDocument ue = SemanticAnalyzer.Analyze(source, path, FrontendAnalyzer.Analyze(source, path).Source.Sha256,
            new[] { new SemanticReferenceSource(facade, "generated://UeMethodFacade.cs") });
        foreach (SemanticDocument input in new[] { ue,
            CSharpGuestSemanticFixture.WithoutNamedGenericShapes(ue) with
            { SchemaVersion = 25, SemanticVersion = "1.29", UeMethodCatalog = null } })
        {
            var result = CSharpGuestLowerer.Lower(input, new string('a', 64));
            Check(result.Succeeded && WasmModuleCompiler.Compile(result.Module!).Succeeded, "new and previous UE receiver contracts produce WASM");
        }
        foreach (SemanticUeMethodCatalog malformed in new[] {
            ue.UeMethodCatalog! with { Types = Array.Empty<SemanticUeMethodType>() },
            ue.UeMethodCatalog! with { Methods = ue.UeMethodCatalog!.Methods.Select(item => item with { SignatureId = "forged" }).ToArray() },
        })
        {
            var result = CSharpGuestLowerer.Lower(ue with { UeMethodCatalog = malformed }, new string('a', 64));
            Check(!result.Succeeded && result.Module is null && result.Diagnostics.Any(item => item.Code == "ASCG1001"), "UE method directory is checked before producing executable output");
        }
        const string borrowedSource = """
            using AvidScript;
            [UClass] public partial class Actor : AvidActor {
                private static int state;
                private ref int Borrow() => ref state;
                [UFunction] public int Entry() => Borrow();
            }
            """;
        SemanticDocument borrowed = SemanticAnalyzer.Analyze(borrowedSource, path, FrontendAnalyzer.Analyze(borrowedSource, path).Source.Sha256,
            new[] { new SemanticReferenceSource(facade, "generated://UeMethodFacade.cs") });
        var borrowedResult = CSharpGuestLowerer.Lower(borrowed, new string('a', 64));
        Check(borrowed.Succeeded && !borrowedResult.Succeeded && borrowedResult.Module is null
            && borrowedResult.Diagnostics.Any(item => item.Code == "ASCG1024" && item.Message.Contains("borrowed-return", StringComparison.Ordinal)),
            "reference return is represented, but cannot silently become a value return");
        return count;
        void Check(bool condition, string message) { if (!condition) throw new InvalidOperationException(message); count++; }
    }
}
