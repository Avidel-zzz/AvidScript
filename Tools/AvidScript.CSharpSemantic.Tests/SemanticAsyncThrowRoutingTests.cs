using System;
using System.Linq;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpSemantic;

internal static class SemanticAsyncThrowRoutingTests
{
    public static int Run()
    {
        int passed = 0;
        void Check(bool condition, string reason)
        {
            if (!condition) throw new InvalidOperationException(reason);
            passed++;
        }
        SemanticDocument Compile(string body)
        {
            SemanticDocument result = Analyze(body);
            Check(result.SchemaVersion == 46 && result.SemanticVersion == "1.55"
                && result.Diagnostics.All(item => item.Severity != "error" || item.Code == "ASCS5422"),
                "Routed throw source failed: " + string.Join(" | ", result.Diagnostics.Select(item => item.Code + ": " + item.Message)));
            Check(SemanticAsyncInvocationValidator.IsValid(result), "Invalid invocation contract: " + body);
            Check(SemanticAsyncScopeValidator.IsValid(result), "Invalid lexical scopes: " + body);
            return result;
        }
        const string nested = """
            try {
                try { int value = await Read(); if (mode == 1) throw new ArgumentException(); return value; }
                catch (OperationCanceledException) { throw new InvalidOperationException(); }
                finally { Trace++; if (mode == 2) throw new ArgumentException(); }
            } catch (ArgumentException) { Trace += 10; return 90; }
              catch (InvalidOperationException) { return 80; }
            finally { Trace += 100; }
            """;
        SemanticDocument document = Compile(nested);
        SemanticAsyncMethod method = document.AsyncMethods.Single(item => item.MethodSymbolId.Contains(".Run("));
        var raises = method.Segments.Where(segment => segment.Transfer?.Kind == SemanticAsyncMethod.RaiseExceptionTransferKind).ToArray();
        Check(raises.Length >= 3 && method.ErrorPlan!.Throws.Count == raises.Length,
            "Every duplicated cleanup and explicit throw needs its own source site");
        Check(SemanticAsyncExceptionOwnerFlow.TryAnalyze(method, out var owners)
            && raises.Any(segment => owners[segment.Ordinal].HasFlag(SemanticAsyncExceptionOwnerState.HandledCancellation))
            && raises.Any(segment => owners[segment.Ordinal].HasFlag(SemanticAsyncExceptionOwnerState.Normal))
            && raises.Any(segment => owners[segment.Ordinal].HasFlag(SemanticAsyncExceptionOwnerState.Cancellation)),
            "Replacement analysis must accept normal, handled and propagating cancellation");
        byte[] bytes = SemanticSerializer.Serialize(document);
        Check(bytes.SequenceEqual(SemanticSerializer.Serialize(SemanticSerializer.Deserialize(bytes)))
            && bytes.SequenceEqual(SemanticSerializer.Serialize(Analyze(nested))), "Routed throw serialization must be deterministic");

        void Reject(SemanticDocument candidate, string reason) =>
            Check(!SemanticAsyncInvocationValidator.IsValid(candidate), reason);
        SemanticDocument Replace(SemanticAsyncMethod changed) => document with
        {
            AsyncMethods = document.AsyncMethods.Select(item => item == method ? changed : item).ToArray(),
        };
        SemanticAsyncMethod Transfer(int ordinal, SemanticAsyncControlTransfer changed) => method with
        {
            Segments = method.Segments.Select(segment => segment.Ordinal == ordinal
                ? segment with { Transfer = changed } : segment).ToArray(),
        };
        foreach (var (schema, version) in new[] { (42, "1.51"), (43, "1.52"), (44, "1.53"), (45, "1.54") })
            Reject(document with { SchemaVersion = schema, SemanticVersion = version }, "Older contracts must reject routed throws");
        Reject(document with { SemanticVersion = "1.54" }, "Mismatched routed throw version");
        Reject(document with { SchemaVersion = 47, SemanticVersion = "1.56" }, "Future routed throw version");
        Reject(Replace(method with { ErrorPlan = null }), "Missing throw site plan");
        Reject(Replace(method with { ErrorPlan = method.ErrorPlan! with { Throws = method.ErrorPlan.Throws.Skip(1).ToArray() } }),
            "Omitted duplicated cleanup site");

        foreach (var segment in raises)
        {
            var transfer = segment.Transfer!;
            foreach (int target in new[] { -1, method.Segments.Count, segment.Ordinal })
                Reject(Replace(Transfer(segment.Ordinal, transfer with { PrimaryTarget = target })), "Invalid or cyclic exception target");
            var otherTarget = method.ExceptionPlan!.ExceptionScopes!.SelectMany(scope => new[] { scope.DispatchTarget, scope.UnwindTarget })
                .FirstOrDefault(target => target != transfer.PrimaryTarget, -1);
            if (otherTarget >= 0)
                Reject(Replace(Transfer(segment.Ordinal, transfer with { PrimaryTarget = otherTarget })), "Bypassed cleanup or wrong sibling dispatch");
            Reject(Replace(Transfer(segment.Ordinal, transfer with { SecondaryTarget = transfer.PrimaryTarget })), "Forged second throw edge");
            Reject(Replace(Transfer(segment.Ordinal, transfer with { CancellationTarget = transfer.PrimaryTarget })), "Forged cancellation edge");
            Reject(Replace(Transfer(segment.Ordinal, transfer with { Kind = SemanticAsyncMethod.ThrowTransferKind })), "Routed throw cannot become terminal");
            var region = method.ExceptionPlan!.Regions.Where(item => item.Segments.Contains(segment.Ordinal))
                .OrderBy(item => item.SourceSpan.Length).First();
            Reject(Replace(method with { ExceptionPlan = method.ExceptionPlan with
            {
                Regions = method.ExceptionPlan.Regions.Select(item => item == region
                    ? item with { Segments = item.Segments.Where(ordinal => ordinal != segment.Ordinal).ToArray() } : item).ToArray(),
            } }), "A copied throw cannot lose its source region ownership");
        }

        foreach (string body in new[]
        {
            "try { await AvidContinuations.NextTickAsync(); throw new ArgumentException(); } catch (ArgumentException) { return 1; }",
            "try { await AvidContinuations.NextTickAsync(); return 1; } finally { throw new ArgumentException(); }",
            "try { await AvidContinuations.NextTickAsync(); } catch (OperationCanceledException) { throw new ArgumentException(); } return 1;",
            "try { throw new ArgumentException(); } catch (ArgumentException) { Trace++; } await AvidContinuations.NextTickAsync(); return 1;",
            "await AvidContinuations.NextTickAsync(); try { throw new ArgumentException(); } finally { Trace++; }",
            "try { await AvidContinuations.NextTickAsync(); } finally { Trace++; } throw new ArgumentException();",
            "Task<int> pending = Read(); Task<int> alias = pending; try { int value = await pending; throw new ArgumentException(); } catch (ArgumentException) { Trace++; } int result = await alias; return result;",
            "Task<int> pending = Read(); try { for (int i = 0; i < 2; i++) { Task<int> local = Read(); pending = local; int value = await local; if (value > 0) throw new ArgumentException(); } } catch (ArgumentException) { Trace++; } int result = await pending; return result;",
        }) Compile(body);

        SemanticDocument locals = Compile("Task<int> pending = Read(); Task<int> alias = pending; "
            + "try { int value = await pending; throw new ArgumentException(); } catch (ArgumentException) { Trace++; } "
            + "int result = await alias; return result;");
        var localMethod = locals.AsyncMethods.Single(item => item.MethodSymbolId.Contains(".Run("));
        Check(localMethod.TaskLocalLifetimes is { Count: 2 }, "New exception edges require complete Task lifetimes");
        Check(SemanticAsyncTaskLocalLifetimeValidator.TryAnalyze(localMethod, out var lifetimes)
            && lifetimes!.States.PossibleAtEntry.Any(pair => localMethod.Segments[pair.Key].Transfer?.Kind
                == SemanticAsyncMethod.RaiseExceptionTransferKind && pair.Value.Count == 2),
            "Aliases remain owned across a routed throw inside their lexical scope");
        Check(!SemanticAsyncInvocationValidator.IsValid(locals with
        {
            AsyncMethods = locals.AsyncMethods.Select(item => item == localMethod
                ? item with { TaskLocalLifetimes = null } : item).ToArray(),
        }), "Task lifetime omission must not fall back to the old contract");

        foreach (string body in new[]
        {
            "try { await AvidContinuations.NextTickAsync(); } catch { try { throw new ArgumentException(); } catch { } } return 1;",
            "try { await AvidContinuations.NextTickAsync(); } finally { await AvidContinuations.NextTickAsync(); } return 1;",
            "try { await AvidContinuations.NextTickAsync(); } catch (Exception error) { throw new ArgumentException(); } return 1;",
            "try { await AvidContinuations.NextTickAsync(); throw new ArgumentException(\"message\"); } catch { return 1; }",
        }) Check(!SemanticAsyncInvocationValidator.IsValid(Analyze(body)), "Unsupported exception shape must remain rejected");
        Check(!SemanticAsyncInvocationValidator.IsValid(Analyze(nested, false)), "Routed throws require the cancellation preview");
        return passed;
    }

    private static SemanticDocument Analyze(string body, bool cancellation = true)
    {
        string source = "using System; using System.Threading.Tasks; using AvidScript; public static class Script { "
            + "public static int Trace; public static async Task<int> Read() { await AvidContinuations.NextTickAsync(); return 7; } "
            + "public static async Task<int> Run(int mode) { " + body + " } }";
        const string sourceId = "Scripts/AsyncThrowRouting.cs";
        return SemanticAnalyzer.Analyze(source, sourceId, FrontendAnalyzer.Analyze(source, sourceId).Source.Sha256,
            new[] { new SemanticReferenceSource(Facade, "generated://Cancellation.cs", true) }, new SemanticCompilerWorkspace(),
            enableAsyncExceptionFlow: true, enableDirectAwaitCleanup: true, enableAsyncCancellationFlow: cancellation);
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
