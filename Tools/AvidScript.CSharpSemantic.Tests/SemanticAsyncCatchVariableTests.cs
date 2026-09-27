using System;
using System.Linq;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpSemantic;

internal static class SemanticAsyncCatchVariableTests
{
    public static int Run()
    {
        int count = 0;
        void Check(bool valid, string reason) { if (!valid) throw new InvalidOperationException(reason); count++; }
        SemanticDocument Compile(string body)
        {
            var document = Analyze(body);
            Check(SemanticContract.HasAsyncCatchVariables(document)
                && document.Diagnostics.All(d => d.Severity != "error" || d.Code is "ASCS3001" or "ASCS5422"),
                "Catch source: " + string.Join(" | ", document.Diagnostics.Select(d => d.Code + ": " + d.Message)));
            Check(SemanticAsyncCatchVariableValidator.IsValid(document), "Catch variable bindings");
            Check(SemanticAsyncInvocationValidator.IsValid(document),
                $"Catch invocation contract: synchronous={SemanticAsyncSynchronousExceptionValidator.IsValid(document)}, " +
                $"errors={SemanticAsyncErrorPlanValidator.IsValid(document)}, " +
                $"exceptions={SemanticAsyncExceptionPlanValidator.IsValid(document)}, " +
                $"taskLocals={SemanticAsyncTaskLocalLifetimeValidator.IsValid(document)}, " +
                $"members={SemanticAsyncMemberAssignmentValidator.IsValid(document)}");
            Check(SemanticAsyncScopeValidator.IsValid(document), "Catch lexical scope contract");
            byte[] json = SemanticSerializer.Serialize(document);
            Check(json.SequenceEqual(SemanticSerializer.Serialize(Analyze(body)))
                && json.SequenceEqual(SemanticSerializer.Serialize(SemanticSerializer.Deserialize(json))), "Deterministic catch projection and round trip");
            return document;
        }
        const string body = "try { await AvidContinuations.NextTickAsync(); return 1; } catch (OperationCanceledException error) { return error == null ? 0 : 7; }";
        var document = Compile(body);
        Compile("try { await AvidContinuations.NextTickAsync(); return 1; } catch (Exception unused) { return 7; }");
        Compile("try { await AvidContinuations.NextTickAsync(); return 1; } catch (OperationCanceledException error) { return error == null ? 0 : 7; } catch (ArgumentException error) { return error == null ? 0 : 8; }");
        Compile("try { try { await AvidContinuations.NextTickAsync(); return 1; } catch (OperationCanceledException inner) { throw; } } catch (Exception outer) { return outer == null ? 0 : 9; }");
        var escaped = Compile("Exception saved = null; try { await AvidContinuations.NextTickAsync(); } catch (OperationCanceledException error) { saved = error; } await AvidContinuations.NextTickAsync(); return saved == null ? 0 : 7;");
        Check(escaped.AsyncMethods.SelectMany(method => method.Segments).Any(segment =>
            segment.AwaitSite?.StateFrame?.Slots.Any(slot => slot.TypeId == "type:global::System.Exception") == true),
            "An escaped alias remains live across a later await");
        var legacy = Analyze(body, enabled: false);
        Check(!SemanticContract.HasAsyncCatchVariables(legacy) && legacy.Diagnostics.Any(d => d.Code == "ASCS3002"),
            "Catch variables remain opt-in: " + string.Join(" | ", legacy.Diagnostics.Select(d => d.Code + ": " + d.Message)));
        Check(Analyze("await AvidContinuations.NextTickAsync(); return 1;") is { SchemaVersion: 50, SemanticVersion: "1.59" },
            "Enabling the option without catch variables preserves the prior contract");
        foreach (string unsupported in new[] {
            "try { await AvidContinuations.NextTickAsync(); return 1; } catch (Exception error) when (error != null) { return 7; }",
            "try { await AvidContinuations.NextTickAsync(); return 1; } catch (Exception error) { await AvidContinuations.NextTickAsync(); return 7; }" })
            Check(Analyze(unsupported).Diagnostics.Any(d => d.Code == "ASCS3002"), "Filters and catch suspension require their own contracts");

        var method = document.AsyncMethods.Single(method => method.ExceptionPlan?.Catches.Any(handler => handler.ExceptionVariableSymbolId is not null) == true);
        var handler = method.ExceptionPlan!.Catches.Single();
        string variable = handler.ExceptionVariableSymbolId!;
        var entry = method.Segments.Single(segment => segment.Statements.Any(statement => statement.Operation.Kind == SemanticAsyncCatchVariableValidator.BindingOperationKind));
        var binding = entry.Statements.Single();
        void Reject(SemanticDocument invalid, string reason) => Check(!SemanticAsyncCatchVariableValidator.IsValid(invalid)
            && !SemanticAsyncInvocationValidator.IsValid(invalid), "Reject catch mutation: " + reason);
        SemanticDocument WithMethod(SemanticAsyncMethod changed) => document with
        { AsyncMethods = document.AsyncMethods.Select(item => item == method ? changed : item).ToArray() };
        SemanticDocument WithEntry(SemanticAsyncSegment changed) => WithMethod(method with
        { Segments = method.Segments.Select(item => item == entry ? changed : item).ToArray() });
        foreach (var changed in new[] {
            binding.Operation with { TypeId = "type:int32" },
            binding.Operation with { SymbolId = "symbol:forged" },
            binding.Operation with { IsSupported = false },
            binding.Operation with { Constant = new("int32", "0") },
            binding.Operation with { IsTryCast = true },
            binding.Operation with { Span = method.Span },
            binding.Operation with { Children = new[] { binding.Operation } } })
            Reject(WithEntry(entry with { Statements = new[] { binding with { Operation = changed } } }), "noncanonical binding");
        Reject(WithEntry(entry with { Statements = Array.Empty<SemanticAsyncStatement>() }), "missing initialization");
        Reject(WithEntry(entry with { Statements = new[] { binding, binding } }), "duplicate initialization");
        Reject(WithEntry(entry with { Statements = new[] { binding with { TargetSymbolId = "symbol:forged" } } }), "wrong destination local");
        Reject(WithEntry(entry with { Transfer = entry.Transfer! with { PrimaryTarget = entry.Ordinal } }), "self-looping initialization");
        Reject(WithMethod(method with { ExceptionPlan = method.ExceptionPlan with
        { Catches = new[] { handler with { ExceptionVariableSymbolId = null } } } }), "unlisted binding");
        Reject(document with { Symbols = document.Symbols.Where(symbol => symbol.Id != variable).ToArray() }, "missing Roslyn local");
        Reject(document with { Symbols = document.Symbols.Select(symbol => symbol.Id == variable
            ? symbol with { ContainingSymbolId = "symbol:another_method" } : symbol).ToArray() }, "wrong local owner");
        var dispatch = method.Segments.Single(segment => segment.Transfer?.Kind == SemanticAsyncMethod.CatchMatchTransferKind);
        Reject(WithMethod(method with { Segments = method.Segments.Select(segment => segment == dispatch
            ? segment with { Transfer = segment.Transfer! with { SecondaryTarget = entry.Ordinal } } : segment).ToArray() }),
            "failed catch match enters the binding");
        Reject(WithMethod(method with { Segments = method.Segments.Select(segment => segment.Ordinal == entry.Transfer!.PrimaryTarget
            ? segment with { Transfer = entry.Transfer with { PrimaryTarget = entry.Ordinal } } : segment).ToArray() }),
            "handler reenters its initialization");
        Reject(WithMethod(method with { Segments = method.Segments.Select(segment => segment == dispatch
            ? segment with { Transfer = segment.Transfer! with { PrimaryTarget = entry.Transfer!.PrimaryTarget } } : segment).ToArray() }),
            "dispatch bypasses initialization");
        var normal = method.Segments.First(segment => !method.ExceptionPlan.Regions.Single(region => region.RoslynRegionOrdinal == handler.RegionOrdinal).Segments.Contains(segment.Ordinal)
            && segment != dispatch);
        var reference = binding.Operation with { Kind = "local_reference" };
        Reject(WithMethod(method with { Segments = method.Segments.Select(segment => segment == normal
            ? segment with { Statements = segment.Statements.Append(new SemanticAsyncStatement(reference, null)).ToArray() } : segment).ToArray() }),
            "catch local referenced outside its region");
        foreach (var version in new[] { (50, "1.59"), (51, "1.60"), (52, "1.59"), (53, "1.62") })
            Reject(document with { SchemaVersion = version.Item1, SemanticVersion = version.Item2 }, "wrong or old version");
        foreach (bool staticInitialization in new[] { false, true })
        {
            bool rejected = false;
            try { Analyze(body, enabled: true, synchronous: staticInitialization, staticInitialization: staticInitialization); }
            catch (ArgumentException) { rejected = true; }
            Check(rejected, "Unsupported option composition fails explicitly");
        }
        return count;
    }

    private static SemanticDocument Analyze(string body, bool enabled = true, bool synchronous = true, bool staticInitialization = false)
    {
        string source = "using System; using System.Threading.Tasks; using AvidScript; public static class Script { public static async Task<int> Run() { " + body + " } }";
        const string sourceId = "Scripts/AsyncCatchVariables.cs";
        return SemanticAnalyzer.Analyze(source, sourceId, FrontendAnalyzer.Analyze(source, sourceId).Source.Sha256,
            new[] { new SemanticReferenceSource(Facade, "generated://Continuation.cs", true) }, new SemanticCompilerWorkspace(),
            enableAsyncExceptionFlow: true, enableDirectAwaitCleanup: true, enableAsyncCancellationFlow: true,
            enableAsyncSynchronousExceptions: synchronous, enableStaticInitialization: staticInitialization, enableAsyncCatchVariables: enabled);
    }

    private const string Facade = """
        using System; using System.Runtime.CompilerServices;
        namespace AvidScript;
        public static class AvidContinuations { public static AvidDelayAwaitable NextTickAsync() => default; }
        public readonly struct AvidDelayAwaitable { public AvidDelayAwaiter GetAwaiter() => default; }
        public readonly struct AvidDelayAwaiter : ICriticalNotifyCompletion {
            public bool IsCompleted => false;
            public void GetResult() { }
            public void OnCompleted(Action continuation) { }
            public void UnsafeOnCompleted(Action continuation) { }
        }
        """;
}
