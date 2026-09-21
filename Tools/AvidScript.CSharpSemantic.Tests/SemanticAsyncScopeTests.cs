using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;

internal static class SemanticAsyncScopeTests
{
    public static int Run()
    {
        int count = 0;
        void Check(bool condition, string message) { if (!condition) throw new InvalidOperationException(message); ++count; }
        const string source = """
            using System;
            using AvidScript;
            public static class Script {
                [AvidExport("avid_on_begin_play")]
                public static async void Begin() {
                    int seed = 1;
                    Func<int> root = () => seed;
                    for (int i = 0; i < 4; ++i) {
                        int copy = i;
                        Func<int> read = () => i + copy;
                        await AvidContinuations.NextTickAsync();
                        if (i == 1) continue;
                        if (i == 2) break;
                        copy += 2;
                    }
                    await AvidContinuations.NextTickAsync();
                }
            }
            """;
        SemanticDocument document = Analyze(source);
        Check(document.Succeeded, string.Join(" | ", document.Diagnostics.Select(item => item.Message)));
        Check(SemanticClosureContractValidator.IsValid(document), "async lexical scope contract validates");
        SemanticAsyncMethod method = document.AsyncMethods.Single();
        Check(method.Lowering == SemanticAsyncMethod.ContinuationCfgLowering && method.LexicalScopes.Count == 3,
            "activation, for initializer and loop body have separate scopes");
        Dictionary<string, int> entries = Trace(method);
        Check(entries["activation"] == 1 && entries["for_entry"] == 1 && entries["block_entry"] == 3,
            "initial invocation allocates once, loop body allocates per iteration, resume never reallocates within the same scope");
        SemanticAsyncLexicalScope body = method.LexicalScopes.Single(scope => scope.Kind == "block_entry");
        SemanticAsyncSegment awaitSegment = method.Segments.Single(segment => segment.AwaitSite is not null && body.Segments.Contains(segment.Ordinal));
        Check(body.Segments.Contains(awaitSegment.Transfer!.PrimaryTarget)
            && !body.Entries.Contains(new(awaitSegment.Ordinal, awaitSegment.Transfer.PrimaryTarget)), "await continuation stays in its original body environment");
        Check(document.ClosureEnvironments.All(environment => environment.Allocation is null
            && method.LexicalScopes.Any(scope => scope.Id == environment.Id && scope.Span == environment.Span)),
            "async environments use async scope identities, never synchronous block ordinals");
        Check(SemanticSerializer.Serialize(document).SequenceEqual(SemanticSerializer.Serialize(Analyze(source))), "deterministic scope projection");
        Check(SemanticClosureContractValidator.IsValid(SemanticSerializer.Deserialize(SemanticSerializer.Serialize(document))), "scope serialization round trip");

        SemanticDocument each = Analyze("""
            using System;
            using AvidScript;
            public static class Script {
                [AvidExport("avid_on_begin_play")]
                public static async void Begin() {
                    int[] values = new int[] { 1, 2 };
                    foreach (int item in values) {
                        Func<int> read = () => item;
                        await AvidContinuations.NextTickAsync();
                    }
                }
            }
            """);
        Check(each.Succeeded && SemanticClosureContractValidator.IsValid(each), "foreach async scope analysis");
        SemanticAsyncMethod eachMethod = each.AsyncMethods.Single();
        SemanticAsyncLexicalScope iteration = eachMethod.LexicalScopes.Single(scope => scope.Kind == "foreach_iteration");
        Check(iteration.Entries.Count == 1 && iteration.Entries[0].SourceBlockOrdinal is { } condition
            && eachMethod.Segments[condition].Transfer!.Kind == SemanticAsyncMethod.BranchTransferKind
            && !iteration.Segments.Contains(condition)
            && eachMethod.Segments[iteration.Entries[0].DestinationBlockOrdinal].Statements.Single().TargetSymbolId is { } itemId
            && each.ClosureEnvironments.Single().Cells.Single().SymbolId == itemId,
            "foreach iteration entry occurs before item binding, after the shared array/index condition");

        SemanticDocument direct = Analyze("""
            using System; using AvidScript;
            public static class Script {
                [AvidExport("avid_on_begin_play")]
                public static async void Begin() { int n = 1; Func<int> read = () => n; await AvidContinuations.NextTickAsync(); n++; }
            }
            """);
        Check(direct.Succeeded && SemanticClosureContractValidator.IsValid(direct)
            && direct.AsyncMethods.Single().Lowering == SemanticAsyncMethod.ContinuationCfgLowering,
            "top-level captured async uses the same resumable CFG scope contract");

        SemanticDocument repeat = Analyze("""
            using System; using AvidScript;
            public static class Script {
                [AvidExport("avid_on_begin_play")]
                public static async void Begin() {
                    int n = 0;
                    do {
                        int copy = n;
                        Func<int> read = () => copy;
                        await AvidContinuations.NextTickAsync();
                        n++;
                        if (n == 2) continue;
                    } while (n < 3);
                }
            }
            """);
        Check(repeat.Succeeded && SemanticClosureContractValidator.IsValid(repeat)
            && Trace(repeat.AsyncMethods.Single())["block_entry"] == 3, "do-while keeps first-entry and continue lifetimes distinct from resume");

        SemanticDocument unreachable = Analyze("""
            using System; using AvidScript;
            public static class Script {
                [AvidExport("avid_on_begin_play")]
                public static async void Begin() {
                    await AvidContinuations.NextTickAsync();
                    return;
                    { int value = 1; Func<int> read = () => value; }
                }
            }
            """);
        Check(unreachable.Succeeded && SemanticClosureContractValidator.IsValid(unreachable)
            && unreachable.AsyncMethods.Single().LexicalScopes.Single(scope => scope.Kind == "block_entry") is { Segments.Count: 0, Entries.Count: 0 },
            "unreachable capture keeps its identity without allocating an environment");

        SemanticAsyncLexicalScope[] malformed = {
            body with { Id = "foreign" }, body with { Ordinal = -1 }, body with { Kind = "unknown" },
            body with { Segments = null! }, body with { Segments = Array.Empty<int>() },
            body with { Segments = body.Segments.Append(body.Segments[0]).ToArray() },
            body with { Segments = new[] { int.MaxValue } }, body with { Entries = null! },
            body with { Entries = new SemanticClosureEntry[] { null! } }, body with { Entries = Array.Empty<SemanticClosureEntry>() },
            body with { Entries = body.Entries.Append(body.Entries[0]).ToArray() },
            body with { Span = body.Span with { Start = int.MaxValue, Length = int.MaxValue } },
        };
        foreach (SemanticAsyncLexicalScope invalid in malformed)
            Check(!SemanticClosureContractValidator.IsValid(With(method with {
                LexicalScopes = method.LexicalScopes.Select(scope => scope == body ? invalid : scope).ToArray() })), "malformed async scope fails closed");
        Check(!SemanticClosureContractValidator.IsValid(With(method with { LexicalScopes = Array.Empty<SemanticAsyncLexicalScope>() })), "missing async environment scope rejected");
        Check(!SemanticClosureContractValidator.IsValid(With(method with { LexicalScopes = method.LexicalScopes.Append(body).ToArray() })), "duplicate scope rejected");
        Check(!SemanticClosureContractValidator.IsValid(document with { SchemaVersion = 26, SemanticVersion = "1.30" }), "new scope metadata cannot enter legacy version");
        Check(SemanticClosureContractValidator.IsValid(With(method with { LexicalScopes = Array.Empty<SemanticAsyncLexicalScope>() })
            with { SchemaVersion = 26, SemanticVersion = "1.30" }), "legacy async capture analysis remains readable without allocation support");
        Check(!SemanticClosureContractValidator.IsValid(document with { SchemaVersion = 28, SemanticVersion = "1.32" }), "future scope contract rejected");
        return count;

        SemanticDocument With(SemanticAsyncMethod replacement) => document with { AsyncMethods = new[] { replacement } };
    }

    // This interpreter verifies allocation points against actual control transfers,
    // including await edges; it does not claim Guest closure execution support.
    private static Dictionary<string, int> Trace(SemanticAsyncMethod method)
    {
        Dictionary<string, int> counts = new(StringComparer.Ordinal), values = new(StringComparer.Ordinal);
        int? previous = null;
        int current = method.EntrySegmentOrdinal;
        for (int step = 0; step < 150; ++step)
        {
            foreach (SemanticAsyncLexicalScope scope in method.LexicalScopes)
                if (scope.Entries.Contains(new(previous, current))) counts[scope.Kind] = counts.GetValueOrDefault(scope.Kind) + 1;
            SemanticAsyncSegment segment = method.Segments[current];
            foreach (SemanticAsyncStatement statement in segment.Statements)
            {
                int value = Eval(statement.Operation);
                if (statement.TargetSymbolId is { } target) values[target] = value;
            }
            SemanticAsyncControlTransfer transfer = segment.Transfer!;
            if (transfer.Kind == SemanticAsyncMethod.ReturnTransferKind) return counts;
            previous = current;
            current = transfer.Kind == SemanticAsyncMethod.BranchTransferKind && Eval(transfer.Condition!) == 0
                ? transfer.SecondaryTarget : transfer.PrimaryTarget;
        }
        throw new InvalidOperationException("async scope trace exceeded bounded steps");

        int Eval(SemanticOperation op)
        {
            switch (op.Kind)
            {
                case "expression_statement": case "conversion": return Eval(op.Children[0]);
                case "literal": return int.Parse(op.Constant!.Value!, System.Globalization.CultureInfo.InvariantCulture);
                case "delegate_creation": return 0;
                case "local_reference": return values.GetValueOrDefault(op.SymbolId!);
                case SemanticAsyncMethod.BlockOperationKind:
                    foreach (SemanticOperation child in op.Children) Eval(child);
                    return 0;
                case SemanticAsyncMethod.LocalDeclarationOperationKind: return values[op.SymbolId!] = Eval(op.Children.Single());
                case "assignment": return values[op.Children[0].SymbolId!] = Eval(op.Children[1]);
                case "compound_assignment": return values[op.Children[0].SymbolId!] += Eval(op.Children[1]);
                case "increment_or_decrement": return ++values[op.Children[0].SymbolId!];
                case "binary":
                    int left = Eval(op.Children[0]), right = Eval(op.Children[1]);
                    return op.OperatorKind switch { "less_than" => left < right ? 1 : 0, "equals" => left == right ? 1 : 0,
                        "add" => left + right, _ => throw new InvalidOperationException("unexpected operator " + op.OperatorKind) };
                default: throw new InvalidOperationException("unexpected operation " + op.Kind);
            }
        }
    }

    private static SemanticDocument Analyze(string source) => SemanticAsyncTests.Analyze(source, "Scripts/AsyncScopes.cs");
}
