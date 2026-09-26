using System;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.Loader;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;

internal static class CSharpGuestStaticInitializationContractTests
{
    public static int Run()
    {
        int count = 0;
        void Check(bool condition, string message)
        {
            if (!condition) throw new InvalidOperationException("Static initialization contract: " + message);
            count++;
        }
        Verify("once", """
            public static class Log { public static int Trace; public static int Mark(int n) { Trace = Trace * 10 + n; return n; } }
            public class Cache {
                public static int Z = Log.Mark(1), A = Log.Mark(2);
                static Cache() { Log.Mark(3); }
                public static int Read() { return Z + A; }
            }
            public static class Script { public static int Run() { Cache.Read(); return Log.Trace; } }
            """, 123, 123);
        Verify("cycle", """
            public class A { public static int Value = B.Value + 1; static A() {} }
            public class B { public static int Value = A.Value + 2; static B() {} }
            public static class Script { public static int Run() { return A.Value * 10 + B.Value; } }
            """, 32, 32);
        Verify("reentry", """
            public class Cache {
                public static int First = ReadLater(), Later = 7;
                static Cache() {} public static int ReadLater() { return Later; }
            }
            public static class Script { public static int Run() { return Cache.First * 10 + Cache.Later; } }
            """, 7, 7);
        Verify("construct", """
            public static class Log { public static int Trace; public static int Mark(int n) { Trace = Trace * 10 + n; return n; } }
            public class Cache {
                public static int Shared = Log.Mark(1);
                public int Value = Log.Mark(2);
                static Cache() { Log.Mark(3); }
                public Cache() { Log.Mark(4); }
            }
            public static class Script { public static int Run() { Cache value = new Cache(); return Log.Trace; } }
            """, 1324, 132424);
        Verify("generic", """
            public static class Log { public static int Count; public static int Next() { Count++; return Count; } }
            public class Cache<T> { public static int Value = Log.Next(); static Cache() {} }
            public static class Script { public static int Run() { return Cache<int>.Value * 10 + Cache<long>.Value; } }
            """, 12, 12);
        Verify("failure", """
            public static class Attempts { public static int Count; }
            public class Broken {
                public static int Value = Create(); static Broken() {}
                private static int Create() { Attempts.Count++; throw new System.InvalidOperationException(); }
            }
            public static class Script { public static int Run() { return Broken.Value; } }
            """, 0, 0, failure: true);
        return count;

        void Verify(string name, string source, int first, int second, bool failure = false)
        {
            string sourceId = "Scripts/StaticInitialization_" + name + ".cs";
            var document = SemanticAnalyzer.Analyze(source, sourceId, FrontendAnalyzer.Analyze(source, sourceId).Source.Sha256,
                Array.Empty<SemanticReferenceSource>(), new SemanticCompilerWorkspace(), enableStaticInitialization: true);
            Check(SemanticStaticInitializationValidator.IsValid(document), name + " projected source contract");
            byte[] bytes = SemanticSerializer.Serialize(document);
            var restored = SemanticSerializer.Deserialize(bytes);
            Check(SemanticStaticInitializationValidator.IsValid(restored), name + " serialized source contract");
            foreach (var candidate in new[] { restored,
                restored with { SchemaVersion = restored.StaticInitialization!.BaseSchemaVersion, SemanticVersion = restored.StaticInitialization.BaseSemanticVersion },
                restored with { StaticInitialization = null }, restored with { SchemaVersion = 49, SemanticVersion = "1.58" } })
            {
                var result = CSharpGuestLowerer.Lower(candidate, new string('a', 64), enableAsyncLanguageErrors: true);
                Check(!result.Succeeded && result.Module is null && result.Diagnostics.Any(item => item.Code == "ASCG1025"),
                    name + " unfinished initializer contract reached Guest publication");
            }
            var references = ((string)AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES")!).Split(Path.PathSeparator)
                .Select(path => MetadataReference.CreateFromFile(path));
            var compilation = CSharpCompilation.Create("StaticInitializationOracle", new[] { CSharpSyntaxTree.ParseText(source) },
                references, new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary, optimizationLevel: OptimizationLevel.Release));
            using MemoryStream assemblyBytes = new();
            var emitted = compilation.Emit(assemblyBytes);
            Check(emitted.Success, string.Join(" | ", emitted.Diagnostics));
            assemblyBytes.Position = 0;
            var context = new AssemblyLoadContext("static-initialization-oracle", isCollectible: true);
            try
            {
                Assembly assembly = context.LoadFromStream(assemblyBytes);
                MethodInfo run = assembly.GetType("Script")!.GetMethod("Run")!;
                foreach (int expected in new[] { first, second })
                {
                    if (!failure) Check((int)run.Invoke(null, null)! == expected, name + " .NET initialization order / once-only execution");
                    else
                    {
                        bool failed = false;
                        try { run.Invoke(null, null); }
                        catch (TargetInvocationException error)
                        {
                            failed = error.InnerException is TypeInitializationException { InnerException: InvalidOperationException };
                        }
                        Check(failed, name + " .NET cached TypeInitializationException and inner error");
                        Check((int)assembly.GetType("Attempts")!.GetField("Count")!.GetValue(null)! == 1,
                            name + " failed initializer must not be retried");
                    }
                }
            }
            finally { context.Unload(); }
            string? directory = Environment.GetEnvironmentVariable("AVIDSCRIPT_STATIC_INITIALIZER_DIR");
            if (!string.IsNullOrWhiteSpace(directory))
            {
                Directory.CreateDirectory(directory);
                File.WriteAllText(Path.Combine(directory, name + ".cs"), source);
                File.WriteAllBytes(Path.Combine(directory, name + ".semantic.json"), bytes);
            }
        }
    }
}
