using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpSemantic;

internal static class SemanticAsyncTaskLocalLifetimeTests
{
    public static int Run()
    {
        int count = 0;
        void Check(bool valid, string message) { if (!valid) throw new InvalidOperationException(message); count++; }
        SemanticDocument AnalyzeBody(string body) => SemanticAsyncTests.Analyze(
            "using AvidScript; using System.Threading.Tasks; public static class Script { "
            + "public static async Task<int> First(int value) { await AvidContinuations.NextTickAsync(); return value; } "
            + "public static async Task<int> Second(int value) { await AvidContinuations.NextTickAsync(); return value + 10; } "
            + "public static async Task<int> Run(int limit) { " + body + " } }", "Scripts/TaskLocalLifetime.cs");
        const string body = "Task<int> pending = First(1); Task<int> saved = pending; "
            + "pending = Second(2); pending = pending; int first = await saved; int second = await pending; return first + second;";
        var document = AnalyzeBody(body);
        Check(document.Succeeded, string.Join(" | ", document.Diagnostics.Select(item => item.Message)));
        Check(document.SchemaVersion == 45 && document.SemanticVersion == "1.54", "new source emits version 45/1.54");
        Check(SemanticAsyncTaskLocalLifetimeValidator.IsValid(document), "lifetime document validates");
        Check(SemanticAsyncInvocationValidator.IsValid(document), "new invocation contract validates");
        Check(SemanticClosureContractValidator.IsValid(document), "new lifetime composes with the closure contract");
        var method = document.AsyncMethods.Single(item => item.MethodSymbolId.Contains(".Run("));
        Check(method.TaskLocalSymbolIds?.Count == 2 && method.TaskLocalLifetimes?.Count == 2, "all Task owners have source scopes");
        Check(method.Segments.Where(segment => segment.AwaitSite?.ProducerKind == "task_local")
            .All(segment => segment.AwaitSite!.TaskCallableId is null), "mutable local does not claim a fixed producer");
        Check(SemanticAsyncTaskLocalLifetimeValidator.TryAnalyze(method, out var flow), "derive ownership flow");
        Check(flow!.WritesBySegment.Values.SelectMany(writes => writes).Count() == 4,
            "initializers and replacements retain separate ordered write facts");
        var bytes = SemanticSerializer.Serialize(document);
        Check(SemanticAsyncInvocationValidator.IsValid(SemanticSerializer.Deserialize(bytes)), "reader round trip");
        Check(bytes.SequenceEqual(SemanticSerializer.Serialize(AnalyzeBody(body))), "deterministic lifetime projection");
        var mixed = AnalyzeBody(body + " } public static async Task<int> Single() { Task<int> single = First(3); int value = await single; return value;");
        Check(mixed.Succeeded && SemanticAsyncInvocationValidator.IsValid(mixed), "all Task-owning methods use the module contract");
        Check(mixed.AsyncMethods.Count(item => item.TaskLocalLifetimes is not null) == 2,
            "once-written locals also receive scope metadata in version 45");
        Check(!SemanticAsyncInvocationValidator.IsValid(mixed with { AsyncMethods = mixed.AsyncMethods.Select(item =>
            item.MethodSymbolId.Contains(".Single(") ? item with { TaskLocalLifetimes = null } : item).ToArray() }),
            "removing only one method's lifetime plan is rejected");

        foreach (string source in new[] {
            "Task<int> pending; if (limit > 0) pending = First(1); else pending = Second(2); int result = await pending; return result;",
            "Task<int> saved = First(1); for (int i = 0; i < limit; i++) { Task<int> iteration = Second(i); saved = iteration; int value = await iteration; if (value == 0) continue; if (value == 2) break; } int result = await saved; return result;",
            "int total = 0; while (total < limit) { Task<int> iteration = First(total); int value = await iteration; total += value; } return total;",
            "Task<int> pending = First(1); pending = Second(2); int result = await pending; return result;",
            "int total = 0; do { Task<int> pending = First(total); int result = await pending; total += result + 1; } while (total < limit); return total;",
            "Task<int> pending = First(1); { Task<int> saved = pending; pending = Second(2); int result = await saved; } int final = await pending; return final;"
        })
        {
            var candidate = AnalyzeBody(source);
            Check(candidate.Succeeded, string.Join(" | ", candidate.Diagnostics.Select(item => item.Message)));
            Check(SemanticAsyncInvocationValidator.IsValid(candidate), "branch, loop and replacement contracts validate");
        }

        var loop = AnalyzeBody("int total = 0; for (int i = 0; i < limit; i++) { Task<int> iteration = First(i); int value = await iteration; total += value; if (value == 0) continue; if (value == 2) break; } return total;");
        Check(loop.Succeeded, string.Join(" | ", loop.Diagnostics.Select(item => item.Message)));
        var loopMethod = loop.AsyncMethods.Single(item => item.MethodSymbolId.Contains(".Run("));
        Check(SemanticAsyncTaskLocalLifetimeValidator.TryAnalyze(loopMethod, out var loopFlow), "loop ownership derives");
        Check(loopFlow!.States.ReleasedOnEdge.Values.Any(ids => ids.Contains(loopMethod.TaskLocalSymbolIds![0])),
            "iteration exits retire the local owner");
        Check(loopMethod.Segments.Where(segment => segment.AwaitSite is not null).All(segment =>
            loopFlow.States.ReleasedOnEdge[new(segment.Ordinal, segment.Transfer!.PrimaryTarget)].Count == 0),
            "suspension within an iteration keeps the owner");
        var outerScope = loopMethod.LexicalScopes.Single(scope => scope.Kind == "activation");
        var wrongScope = loopMethod with { TaskLocalLifetimes = loopMethod.TaskLocalLifetimes!.Select(item =>
            item with { ScopeId = outerScope.Id }).ToArray() };
        Check(!SemanticAsyncInvocationValidator.IsValid(loop with { AsyncMethods = loop.AsyncMethods.Select(item =>
            item == loopMethod ? wrongScope : item).ToArray() }), "loop owner cannot be reparented to method scope");

        var pendingId = method.TaskLocalSymbolIds![0];
        var savedId = method.TaskLocalSymbolIds[1];
        var aliasRead = method.Segments.SelectMany(segment => segment.Statements)
            .Single(statement => statement.TargetSymbolId == savedId).Operation;
        var earlyRead = method with { Segments = method.Segments.Select(segment => segment with {
            Statements = segment.Statements.Select(statement => statement.TargetSymbolId == pendingId
                ? statement with { Operation = aliasRead with { SymbolId = savedId } } : statement).ToArray() }).ToArray() };
        Check(!SemanticAsyncTaskLocalLifetimeValidator.TryAnalyze(earlyRead, out _), "later writes cannot authorize an earlier alias read");
        var hiddenCall = method with { Segments = method.Segments.Select((segment, index) => index != 0 ? segment : segment with {
            Statements = segment.Statements.Concat(new[] { new SemanticAsyncStatement(
                method.Segments.SelectMany(item => item.Statements).First(statement => statement.TargetSymbolId == pendingId).Operation, null) }).ToArray() }).ToArray() };
        Check(!SemanticAsyncTaskLocalLifetimeValidator.IsValid(document with { AsyncMethods = document.AsyncMethods.Select(item =>
            item == method ? hiddenCall : item).ToArray() }), "unowned Task producer hidden in ordinary statements rejected");

        SemanticDocument Replace(SemanticAsyncMethod bad) => document with {
            AsyncMethods = document.AsyncMethods.Select(item => item == method ? bad : item).ToArray() };
        var invalid = new List<SemanticAsyncMethod> {
            method with { TaskLocalLifetimes = null },
            method with { TaskLocalLifetimes = Array.Empty<SemanticAsyncTaskLocalLifetime>() },
            method with { TaskLocalLifetimes = method.TaskLocalLifetimes!.Reverse().ToArray() },
            method with { TaskLocalLifetimes = method.TaskLocalLifetimes!.Select(item => item with { ScopeId = "foreign" }).ToArray() },
            method with { TaskLocalSymbolIds = new[] { method.TaskLocalSymbolIds![0] } },
            method with { LexicalScopes = Array.Empty<SemanticAsyncLexicalScope>() },
            method with { Segments = method.Segments.Select(segment => segment.AwaitSite?.ProducerKind != "task_local" ? segment
                : segment with { AwaitSite = segment.AwaitSite with { TaskCallableId = "forged:producer" } }).ToArray() },
        };
        foreach (var bad in invalid)
            Check(!SemanticAsyncInvocationValidator.IsValid(Replace(bad)), "tampered lifetime rejected");
        Check(!SemanticAsyncInvocationValidator.IsValid(document with { SchemaVersion = 44, SemanticVersion = "1.53" }),
            "legacy schema rejects new lifetime data");
        Check(!SemanticAsyncInvocationValidator.IsValid(document with { SemanticVersion = "1.53" }), "version mismatch rejected");

        foreach (string source in new[] {
            "Task<int> pending; if (limit > 0) pending = First(1); int result = await pending; return result;",
            "Task<int> pending = First(1); pending = null; int result = await pending; return result;",
            "Task<int> pending = First(1); pending = Second(2); System.Func<Task<int>> capture = () => pending; int result = await pending; return result;",
            "Task<int> pending = First(1); Task<int> saved = pending = Second(2); int result = await saved; return result;"
        }) Check(!AnalyzeBody(source).Succeeded, "unsupported or uninitialized source rejected");

        const string exceptionSource = """
            using System; using System.Threading.Tasks;
            public static class Script {
                public static int Finalized;
                public static async Task<int> Child(int value) { if (value < 0) throw new InvalidOperationException(); return value; }
                public static async Task<int> Run(int limit) {
                    Task<int> pending = Child(1);
                    try {
                        for (int i = 0; i < limit; i++) {
                            Task<int> iteration = Child(i); pending = iteration;
                            int value = await iteration; if (value == 2) return value;
                        }
                        int result = await pending; return result;
                    } catch (Exception) { return 9; } finally { Finalized++; }
                }
            }
            """;
        foreach (bool cancellation in new[] { false, true })
        {
            var exceptions = SemanticAnalyzer.Analyze(exceptionSource, "Scripts/TaskLocalExceptions.cs",
                FrontendAnalyzer.Analyze(exceptionSource, "Scripts/TaskLocalExceptions.cs").Source.Sha256,
                Array.Empty<SemanticReferenceSource>(), new SemanticCompilerWorkspace(),
                enableAsyncExceptionFlow: true, enableAsyncCancellationFlow: cancellation);
            Check(exceptions.Diagnostics.All(item => item.Severity != "error" || item.Code == "ASCS5422"),
                string.Join(" | ", exceptions.Diagnostics.Select(item => item.Message)));
            Check(SemanticAsyncInvocationValidator.IsValid(exceptions), "lifetime scopes compose with exception/cancellation dispatch");
        }
        return count;
    }
}
