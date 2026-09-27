using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpSemantic;

internal static class SemanticCancellationTokenTests
{
    public static int Run()
    {
        int count = 0;
        void Check(bool valid, string reason) { if (!valid) throw new InvalidOperationException(reason); count++; }
        SemanticDocument Compile(string body, string members = "")
        {
            var doc = Analyze(body, members);
            Check(SemanticContract.HasCancellationTokens(doc)
                && doc.Diagnostics.All(d => d.Severity != "error" || d.Code is "ASCS3001" or "ASCS5422"),
                "Token source: " + string.Join(" | ", doc.Diagnostics.Select(d => d.Code + ": " + d.Message)));
            Check(SemanticCancellationTokenValidator.IsValid(doc), "Independent token validation");
            Check(SemanticAsyncInvocationValidator.IsValid(doc), "Async invocation composition: "
                + $"sync={SemanticAsyncSynchronousExceptionValidator.IsValid(doc)}, errors={SemanticAsyncErrorPlanValidator.IsValid(doc)}, "
                + $"exceptions={SemanticAsyncExceptionPlanValidator.IsValid(doc)}, lifetimes={SemanticAsyncTaskLocalLifetimeValidator.IsValid(doc)}, "
                + $"members={SemanticAsyncMemberAssignmentValidator.IsValid(doc)}, catches={SemanticAsyncCatchVariableValidator.IsValid(doc)}, "
                + $"scopes={SemanticAsyncScopeValidator.IsValid(doc)}");
            Check(SemanticAsyncCatchVariableValidator.IsValid(doc), "Catch binding composition");
            byte[] json = SemanticSerializer.Serialize(doc);
            Check(json.SequenceEqual(SemanticSerializer.Serialize(Analyze(body, members)))
                && json.SequenceEqual(SemanticSerializer.Serialize(SemanticSerializer.Deserialize(json))),
                "Deterministic token projection and serialization");
            return doc;
        }

        const string body = """
            CancellationToken expected = GetToken();
            try { await AvidContinuations.NextTickAsync().WithCancellation(expected); return 0; }
            catch (OperationCanceledException error) { return error.CancellationToken == expected ? 7 : 9; }
            """;
        const string members = "public static AvidCancellationToken GetToken() => default;";
        var document = Compile(body, members);
        Check(Nodes(document).Any(node => node.Kind == SemanticCancellationTokens.FromAvid), "Facade conversion becomes a value intrinsic");
        Check(document.AsyncMethods.SelectMany(method => method.Segments).Any(segment =>
            segment.AwaitSite?.CancellationToken?.TypeId == SemanticCancellationTokens.TypeId), "Standard token passes to WithCancellation");
        Check(document.AsyncMethods.SelectMany(method => method.Segments).Any(segment =>
            segment.AwaitSite?.StateFrame?.Slots.Any(slot => slot.TypeId == SemanticCancellationTokens.TypeId) == true),
            "Token stays live in the async frame");
        Check(Nodes(document).Count(node => node.Kind == SemanticCancellationTokens.FromAvid) == 1,
            "Conversion does not clone the side-effecting operand in the source operation tree");
        var defaults = Compile("CancellationToken a = default; CancellationToken b = CancellationToken.None; CancellationToken c = new CancellationToken(); await AvidContinuations.NextTickAsync(); return a == b && b.Equals(c) && a != GetToken() ? 1 : 0;", members);
        Check(Nodes(defaults).Count(node => node.Kind == SemanticCancellationTokens.None) == 3, "Default, None and constructor normalize to token zero");
        Check(Nodes(defaults).Count(node => node.Kind == SemanticCancellationTokens.Compare) == 3, "Equality, inequality and typed Equals share nominal value semantics");
        Compile("CancellationToken saved = default; try { await AvidContinuations.NextTickAsync(); } catch (TaskCanceledException error) { saved = error.CancellationToken; } await AvidContinuations.NextTickAsync(); return saved == CancellationToken.None ? 1 : 0;");
        Compile("await AvidContinuations.NextTickAsync(); return Echo(CancellationToken.None).Equals(CancellationToken.None) ? 1 : 0;",
            "public static CancellationToken Echo(CancellationToken token) { return token; }");
        var nullReceiver = Compile("await AvidContinuations.NextTickAsync(); OperationCanceledException error = null; return error.CancellationToken == CancellationToken.None ? 1 : 0;");
        Check(nullReceiver.Types.Any(type => type.Id == "type:global::System.NullReferenceException"), "Implicit null failure has a registered exception hierarchy");
        var readSegments = nullReceiver.AsyncMethods.SelectMany(method => method.Segments).Where(segment => segment.Transfer?.Condition is { } condition
            && Walk(condition).Any(node => node.Kind == SemanticCancellationTokens.Read)).ToArray();
        Check(readSegments.Length > 0 && readSegments.All(segment => segment.SynchronousExceptionTarget is not null),
            "Token reads retain normal language exception routing");
        Compile("await AvidContinuations.NextTickAsync(); int result = await Use(CancellationToken.None); return result;",
            "public static async Task<int> Use(CancellationToken token) { await AvidContinuations.NextTickAsync().WithCancellation(token); return token == CancellationToken.None ? 1 : 0; }");
        var synchronousRead = Compile("await AvidContinuations.NextTickAsync(); return Read(null) == CancellationToken.None ? 1 : 0;",
            "public static CancellationToken Read(OperationCanceledException error) { try { return error.CancellationToken; } catch (NullReferenceException) { return CancellationToken.None; } }");
        Check(synchronousRead.ExceptionFlows?.Count == 1, "Reader composes with synchronous exception regions");
        var sync = AnalyzeSource("using System.Threading; public static class Script { public static bool Same(CancellationToken a, CancellationToken b) => a == b; }", Array.Empty<SemanticReferenceSource>());
        Check(SemanticContract.HasCancellationTokens(sync) && SemanticAsyncInvocationValidator.IsValid(sync),
            "Synchronous token values do not require a fake async method or named catch");

        foreach (string unsupported in new[] { "CancellationToken.None.CanBeCanceled", "CancellationToken.None.IsCancellationRequested",
            "new CancellationToken(true).CanBeCanceled", "CancellationToken.None.Equals((object)CancellationToken.None)",
            "CancellationToken.None.ToString() == null", "CancellationToken.None.GetType() == null" })
        {
            var rejected = Analyze("await AvidContinuations.NextTickAsync(); return " + unsupported + " ? 1 : 0;");
            Check(rejected.Diagnostics.Any(d => d.Code == SemanticCancellationTokens.DiagnosticCode)
                && !SemanticCancellationTokenValidator.IsValid(rejected), "Reject unsupported framework member: " + unsupported);
        }
        var legacy = Analyze(body, members, enabled: false);
        Check(!SemanticContract.HasCancellationTokens(legacy) && !Nodes(legacy).Any(node => node.Kind.StartsWith("cancellation_token_", StringComparison.Ordinal)),
            "New projection stays opt-in");
        const string shadowSource = "namespace System.Threading { public readonly struct CancellationToken { public static CancellationToken None => default; } } public static class Script { public static System.Threading.CancellationToken Get() => System.Threading.CancellationToken.None; }";
        var shadow = AnalyzeSource(shadowSource, Array.Empty<SemanticReferenceSource>());
        Check(!SemanticContract.HasCancellationTokens(shadow) && !Nodes(shadow).Any(node => node.Kind.StartsWith("cancellation_token_", StringComparison.Ordinal)),
            "Source lookalike cannot stand in for the BCL token");
        var hidden = Compile("try { await AvidContinuations.NextTickAsync(); return 0; } catch (CustomCancellation error) { return error.CancellationToken == CancellationToken.None ? 1 : 0; }",
            "public class CustomCancellation : OperationCanceledException { public new CancellationToken CancellationToken => default; }");
        Check(!Nodes(hidden).Any(node => node.Kind == SemanticCancellationTokens.Read), "Hidden property remains a source accessor");
        var fakeFacade = Facade.Replace("public static extern implicit operator CancellationToken(AvidCancellationToken token);",
            "public static implicit operator CancellationToken(AvidCancellationToken token) => default;")
            .Replace("[MethodImpl(MethodImplOptions.InternalCall)]", "");
        Check(Analyze(body, members, facade: fakeFacade).Diagnostics.Any(d => d.Code == SemanticCancellationTokens.DiagnosticCode),
            "Source-bodied facade conversion is not an intrinsic");
        var primaryFacade = AnalyzeSource(Facade.Replace("namespace AvidScript;", "namespace AvidScript {") + "}\n"
            + "public static class Script { public static CancellationToken Get(AvidScript.AvidCancellationToken token) => token; }",
            Array.Empty<SemanticReferenceSource>());
        Check(primaryFacade.Diagnostics.Any(d => d.Code == SemanticCancellationTokens.DiagnosticCode), "Primary-source facade conversion is not trusted");

        void Reject(SemanticDocument invalid, string reason) => Check(!SemanticCancellationTokenValidator.IsValid(invalid)
            && !SemanticAsyncInvocationValidator.IsValid(invalid), "Reject token mutation: " + reason);
        foreach (var version in new[] { (52, "1.61"), (53, "1.61"), (52, "1.62"), (54, "1.63") })
            Reject(document with { SchemaVersion = version.Item1, SemanticVersion = version.Item2 }, "wrong/old/future version");
        Reject(document with { Types = document.Types.Where(type => type.Id != SemanticCancellationTokens.TypeId).ToArray() }, "missing nominal type");
        Reject(document with { Types = document.Types.Where(type => type.Id != SemanticCancellationTokens.AvidTypeId).ToArray() }, "missing conversion input type");
        Reject(document with { Types = document.Types.Select(type => type.Id == SemanticCancellationTokens.TypeId ? type with { Kind = "class", IsValueType = false } : type).ToArray() }, "reference masquerading as token");
        Reject(document with { Types = document.Types.Append(document.Types.Single(type => type.Id == SemanticCancellationTokens.TypeId) with { Kind = "class" }).ToArray() }, "duplicate type identity with different layout");
        Reject(document with { TypeShapes = document.TypeShapes.Append(new(SemanticCancellationTokens.TypeId, "type:int64", null)).ToArray() }, "borrowed CLR/array layout");
        var read = Nodes(document).Single(node => node.Kind == SemanticCancellationTokens.Read);
        var cyclicChildren = new List<SemanticOperation>();
        var cycle = read with { Kind = "parenthesized", Children = cyclicChildren };
        cyclicChildren.Add(cycle);
        Reject(ReplaceMethodOperation(document, read, cycle), "cyclic operation graph");
        var conversion = Nodes(document).Single(node => node.Kind == SemanticCancellationTokens.FromAvid);
        foreach (var changed in new[] { read with { TypeId = "type:int64" }, read with { IsSupported = false },
            read with { IsChecked = true }, read with { IsLifted = true }, read with { IsTryCast = true },
            read with { IsPostfix = true }, read with { OperatorKind = "equals" }, read with { SymbolId = "forged" },
            read with { Constant = new("int64", "0") }, read with { CaptureId = "0" }, read with { TypeArgumentIds = new[] { "type:int64" } },
            read with { Children = Array.Empty<SemanticOperation>() }, read with { Children = new[] { read.Children[0], read.Children[0] } },
            read with { Children = new[] { read.Children[0] with { TypeId = "type:int64" } } },
            read with { Kind = "cancellation_token_future" } })
            Reject(ReplaceMethodOperation(document, read, changed), "noncanonical reader");
        Reject(ReplaceMethodOperation(document, conversion, conversion with { SymbolId = "forged" }), "missing conversion declaration");
        Reject(document with { Symbols = document.Symbols.Select(symbol => symbol.Id == conversion.SymbolId
            ? symbol with { IsExecutableReferenceSource = false } : symbol).ToArray() }, "untrusted conversion owner");
        Reject(document with { Callables = document.Callables.Select(callable => callable.MethodSymbolId == conversion.SymbolId
            ? callable with { HasBody = true } : callable).ToArray() }, "conversion has executable source body");
        var compare = Nodes(document).Single(node => node.Kind == SemanticCancellationTokens.Compare);
        Reject(ReplaceMethodOperation(document, compare, compare with { OperatorKind = "add" }), "invented token arithmetic");
        Reject(ReplaceMethodOperation(document, compare, compare with { Children = new[] { read, read with { TypeId = "type:int64" } } }), "mixed equality types");
        var method = document.AsyncMethods.Single();
        var segment = method.Segments.First(item => item.AwaitSite?.CancellationToken is not null);
        var badToken = read with { Kind = "cancellation_token_future" };
        Reject(document with { AsyncMethods = new[] { method with { Segments = method.Segments.Select(item => item == segment
            ? item with { AwaitSite = item.AwaitSite! with { CancellationToken = badToken } } : item).ToArray() } } }, "unknown operation hidden in await argument");
        var graph = sync.ControlFlowGraphs.Single();
        var block = graph.Blocks.First(item => item.BranchValue is not null);
        Reject(sync with { ControlFlowGraphs = new[] { graph with { Blocks = graph.Blocks.Select(item => item == block
            ? item with { BranchValue = badToken } : item).ToArray() } } }, "unknown operation hidden in CFG");
        var flow = synchronousRead.ExceptionFlows!.Single();
        var flowBlock = flow.Blocks!.First(item => item.BranchValue is not null);
        Reject(synchronousRead with { ExceptionFlows = new[] { flow with { Blocks = flow.Blocks!.Select(item => item == flowBlock
            ? item with { BranchValue = badToken } : item).ToArray() } } }, "unknown operation hidden in synchronous exception flow");
        bool optionsRejected = false;
        try { AnalyzeSource("public class Script {}", Array.Empty<SemanticReferenceSource>(), catchVariables: false); }
        catch (ArgumentException) { optionsRejected = true; }
        Check(optionsRejected, "Unsupported option composition is explicit");
        return count;
    }

    private static SemanticDocument Analyze(string body, string members = "", bool enabled = true, string? facade = null) =>
        AnalyzeSource("using System; using System.Threading; using System.Threading.Tasks; using AvidScript; public static class Script { "
            + members + " public static async Task<int> Run() { " + body + " } }",
            new[] { new SemanticReferenceSource(facade ?? Facade, "generated://Continuation.cs", true) }, enabled);

    private static SemanticDocument AnalyzeSource(string source, IReadOnlyList<SemanticReferenceSource> references,
        bool enabled = true, bool catchVariables = true)
    {
        const string id = "Scripts/CancellationTokenValues.cs";
        return SemanticAnalyzer.Analyze(source, id, FrontendAnalyzer.Analyze(source, id).Source.Sha256,
            references, new SemanticCompilerWorkspace(), enableAsyncExceptionFlow: true, enableDirectAwaitCleanup: true,
            enableAsyncCancellationFlow: true, enableAsyncSynchronousExceptions: true,
            enableAsyncCatchVariables: catchVariables, enableCancellationTokens: enabled);
    }

    private static IEnumerable<SemanticOperation> Walk(SemanticOperation root) =>
        new[] { root }.Concat(root.Children.SelectMany(Walk));
    private static IEnumerable<SemanticOperation> Nodes(SemanticDocument document) => document.Methods.SelectMany(method => Walk(method.Root));
    private static SemanticDocument ReplaceMethodOperation(SemanticDocument doc, SemanticOperation original, SemanticOperation changed)
    {
        SemanticOperation Replace(SemanticOperation node) => ReferenceEquals(node, original) ? changed
            : node with { Children = node.Children.Select(Replace).ToArray() };
        return doc with { Methods = doc.Methods.Select(method => method with { Root = Replace(method.Root) }).ToArray() };
    }

    private const string Facade = """
        using System; using System.Runtime.CompilerServices; using System.Threading;
        namespace AvidScript;
        public readonly struct AvidCancellationToken {
            internal readonly long Value;
            internal AvidCancellationToken(long value) { Value = value; }
            [MethodImpl(MethodImplOptions.InternalCall)]
            public static extern implicit operator CancellationToken(AvidCancellationToken token);
        }
        public static class AvidContinuations { public static AvidDelayAwaitable NextTickAsync() => default; }
        public readonly struct AvidDelayAwaitable {
            public AvidDelayAwaitable WithCancellation(AvidCancellationToken token) => default;
            public AvidDelayAwaitable WithCancellation(CancellationToken token) => default;
            public AvidDelayAwaiter GetAwaiter() => default;
        }
        public readonly struct AvidDelayAwaiter : ICriticalNotifyCompletion {
            public bool IsCompleted => false;
            public void GetResult() { }
            public void OnCompleted(Action continuation) { }
            public void UnsafeOnCompleted(Action continuation) { }
        }
        """;
}
