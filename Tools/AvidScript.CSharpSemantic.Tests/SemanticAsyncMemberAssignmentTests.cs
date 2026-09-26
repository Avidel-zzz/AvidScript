using System;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpSemantic;

internal static class SemanticAsyncMemberAssignmentTests
{
    public static int Run()
    {
        int passed = 0;
        void Check(bool condition, string reason)
        {
            if (!condition) throw new InvalidOperationException(reason);
            passed++;
        }
        SemanticDocument Compile(string body, bool instance = false)
        {
            SemanticDocument document = Analyze(body, instance);
            Check(document.SchemaVersion == 47 && document.SemanticVersion == "1.56"
                && document.Diagnostics.All(item => item.Severity != "error" || item.Code == "ASCS5422"),
                "Member assignment source failed: " + string.Join(" | ", document.Diagnostics.Select(item => item.Code + ": " + item.Message)));
            Check(SemanticAsyncMemberAssignmentValidator.IsValid(document), "Invalid member assignment protocol: " + body);
            Check(SemanticAsyncInvocationValidator.IsValid(document), "Invalid async invocation contract: " + body);
            Check(SemanticAsyncScopeValidator.IsValid(document), "Invalid scope contract: " + body);
            byte[] bytes = SemanticSerializer.Serialize(document);
            Check(bytes.SequenceEqual(SemanticSerializer.Serialize(Analyze(body, instance)))
                && bytes.SequenceEqual(SemanticSerializer.Serialize(SemanticSerializer.Deserialize(bytes))),
                "Member assignment serialization is not canonical.");
            return document;
        }

        foreach (string body in new[]
        {
            "target.Field = await Read(mode); return 7;",
            "target.Property = await Read(mode); return 7;",
            "ITarget view = target; view.Property = await Read(mode); return 7;",
            "Select().Field = await Read(mode); return 7;",
            "Select().Property = await Read(mode); return 7;",
            "new Target().Property = await Read(mode); return 7;",
            "((Target)null!).Field = await Read(mode); return 7;",
            "Identity<Target>(target).Field = await Read(mode); return 7;",
            "Target captured = target; Action change = () => captured = Current; captured.Property = await Read(mode); change(); return 7;",
            "target.Field = await Read(mode); return 7; target.Property = await Read(mode);",
            "if (mode > 0) target.Field = await Read(mode); else target.Property = await Read(mode); return 7;",
            "for (int i = 0; i < 2; i++) { Select().Field = await Read(mode); } return 7;",
            "Task<int> pending = Read(mode); Task<int> alias = pending; target.Property = await alias; return 7;",
            "try { target.Property = await Read(mode); return 7; } finally { Trace++; }",
            "try { target.Field = await Read(mode); return 7; } catch (OperationCanceledException) { return 8; } finally { Trace++; }",
            "try { target.Property = await Read(mode); throw new ArgumentException(); } catch (ArgumentException) { return 8; } finally { Trace++; }",
        }) Compile(body);
        Compile("Field = await Script.Read(mode); return 7;", instance: true);
        Compile("Property = await Script.Read(mode); return 7;", instance: true);

        const string source = "Select().Field = await Read(mode); target.Property = await Read(mode); return 7;";
        SemanticDocument baseline = Compile(source);
        SemanticAsyncMethod method = baseline.AsyncMethods.Single(item => item.MethodSymbolId.Contains(".Run("));
        SemanticAsyncSegment segment = method.Segments.First(item => item.AwaitSite?.MemberAssignment is not null);
        SemanticAsyncAwaitSite site = segment.AwaitSite!;
        SemanticAsyncMemberAssignment plan = site.MemberAssignment!;
        SemanticAsyncSegment write = method.Segments[plan.WriteSegmentOrdinal];
        Check(segment.Statements.Single().Operation.Kind == "invocation"
            && write.Statements.Single().Operation.Children[0].Children[0].Kind == "local_reference",
            "An effectful receiver must be evaluated before await and loaded from its saved local for writeback.");
        Check(site.StateFrame!.Slots.Any(slot => slot.SymbolId == plan.ReceiverSymbolId)
            && !site.StateFrame.Slots.Any(slot => slot.SymbolId == site.ResultSymbolId),
            "Capture must survive suspension; unproduced results must not be stored in the frame.");

        SemanticDocument Replace(SemanticAsyncMethod changed) => baseline with
        {
            AsyncMethods = baseline.AsyncMethods.Select(item => item == method ? changed : item).ToArray(),
        };
        SemanticAsyncMethod ReplaceSegment(SemanticAsyncSegment changed) => method with
        {
            Segments = method.Segments.Select(item => item.Ordinal == changed.Ordinal ? changed : item).ToArray(),
        };
        void Reject(SemanticDocument document, string reason) =>
            Check(!SemanticAsyncMemberAssignmentValidator.IsValid(document)
                && !SemanticAsyncInvocationValidator.IsValid(document), reason);
        void RejectSite(SemanticAsyncAwaitSite changed, string reason) =>
            Reject(Replace(ReplaceSegment(segment with { AwaitSite = changed })), reason);

        foreach (var (schema, version) in new[] { (35, "1.44"), (45, "1.54"), (46, "1.55"), (48, "1.57"), (47, "1.55") })
            Reject(baseline with { SchemaVersion = schema, SemanticVersion = version }, "Forged or unsupported version.");
        RejectSite(site with { MemberAssignment = null }, "Omitted member plan.");
        RejectSite(site with { ResultStorageKind = "existing_local" }, "Disguised member result.");
        RejectSite(site with { ResultSymbolId = plan.ReceiverSymbolId }, "Receiver/result slot alias.");
        RejectSite(site with { MemberAssignment = plan with { WriteSegmentOrdinal = segment.Ordinal } }, "Self-referential write.");
        RejectSite(site with { StateFrame = site.StateFrame! with
        {
            Slots = site.StateFrame.Slots.Where(slot => slot.SymbolId != plan.ReceiverSymbolId).ToArray(),
        } }, "Missing captured receiver root.");
        Reject(Replace(method with { CompilerLocals = method.CompilerLocals.Where(local => local.SymbolId != plan.ReceiverSymbolId).ToArray() }),
            "Missing compiler local.");
        Reject(Replace(method with { CompilerLocals = method.CompilerLocals.Select(local => local.SymbolId == plan.ReceiverSymbolId
            ? local with { TypeId = "type:int32" } : local).ToArray() }), "Forged receiver type.");
        Reject(Replace(method with { EntrySegmentOrdinal = write.Ordinal }), "Entry skips capture and await.");
        Reject(Replace(ReplaceSegment(segment with { Statements = Array.Empty<SemanticAsyncStatement>() })), "Missing receiver evaluation.");
        Reject(Replace(ReplaceSegment(segment with { Statements = segment.Statements.Concat(segment.Statements).ToArray() })),
            "Receiver evaluated twice.");
        Reject(Replace(ReplaceSegment(segment with { Transfer = segment.Transfer! with { SecondaryTarget = write.Ordinal } })),
            "Failed Task writes its result.");
        Reject(Replace(ReplaceSegment(segment with { Transfer = segment.Transfer! with { CancellationTarget = write.Ordinal } })),
            "Cancelled Task writes its result.");
        Reject(Replace(ReplaceSegment(write with { Statements = write.Statements.Concat(write.Statements).ToArray() })),
            "Duplicated setter.");
        SemanticOperation writeOperation = write.Statements[0].Operation;
        Reject(Replace(ReplaceSegment(write with { Statements = new[] { new SemanticAsyncStatement(writeOperation with
        {
            Children = new[] { plan.Target, writeOperation.Children[1] },
        }, null) } })), "Receiver evaluated again after await.");
        Reject(Replace(ReplaceSegment(write with { Statements = new[] { new SemanticAsyncStatement(writeOperation with
        {
            Children = new[] { writeOperation.Children[0], writeOperation.Children[1] with { SymbolId = plan.ReceiverSymbolId } },
        }, null) } })), "Write reads the wrong result slot.");
        Reject(Replace(ReplaceSegment(write with { Transfer = write.Transfer! with { PrimaryTarget = write.Ordinal } })),
            "Repeated setter cycle.");
        foreach (string field in new[] { "target", "receiver_symbol_id", "write_segment_ordinal" })
        {
            JsonNode json = JsonNode.Parse(SemanticSerializer.Serialize(baseline))!;
            var serializedPlan = json["async_methods"]!.AsArray()
                .SelectMany(item => item!["segments"]!.AsArray())
                .Select(item => item!["await_site"]?["member_assignment"])
                .First(item => item is not null)!.AsObject();
            Check(serializedPlan.Remove(field), "Required plan field missing from serialization.");
            bool rejected = false;
            try { SemanticSerializer.Deserialize(Encoding.UTF8.GetBytes(json.ToJsonString())); }
            catch (InvalidDataException error) when (error.InnerException is JsonException) { rejected = true; }
            Check(rejected, "Missing assignment plan field must not receive a plausible default.");
        }

        SemanticDocument dead = Analyze("return 7; target.Field = await Read(mode);");
        Check(dead.Diagnostics.All(item => item.Severity != "error")
            && !SemanticContract.HasAsyncMemberAssignments(dead)
            && SemanticAsyncInvocationValidator.IsValid(dead), "Unreachable assignment left compiler slots or a capability behind.");

        DirectoryInfo? root = new(AppContext.BaseDirectory);
        const string fixtureId = "Fixtures/Phase66/AwaitMemberAssignment.cs";
        while (root is not null && !File.Exists(Path.Combine(root.FullName, fixtureId))) root = root.Parent;
        Check(root is not null, "Shared .NET/WASM source fixture is missing.");
        SemanticDocument shared = AnalyzeSource(File.ReadAllText(Path.Combine(root!.FullName, fixtureId)), fixtureId);
        Check(SemanticContract.HasAsyncMemberAssignments(shared)
            && shared.AsyncMethods.SelectMany(item => item.Segments).Count(item => item.AwaitSite?.MemberAssignment is not null) == 9
            && shared.Diagnostics.All(item => item.Severity != "error" || item.Code is "ASCS3001" or "ASCS5422"),
            "The 29-case shared fixture must project all nine member await sites.");
        Check(SemanticAsyncMemberAssignmentValidator.IsValid(shared), "Shared fixture member protocol is invalid.");
        Check(SemanticAsyncInvocationValidator.IsValid(shared), "Shared fixture async/error/ownership composition is invalid.");

        foreach (string body in new[]
        {
            "target.Field += await Read(mode); return 7;",
            "target.LongField = await Read(mode); return 7;",
            "target.ReadOnly = await Read(mode); return 7;",
            "ValueTarget value = new ValueTarget(); value.Field = await Read(mode); return 7;",
        }) Check(Analyze(body).Diagnostics.Any(item => item.Severity == "error"),
            "Unsupported lvalue must not be accepted as a reference capture.");
        return passed;
    }

    private static SemanticDocument Analyze(string body, bool instance = false)
    {
        string source = "using System; using System.Threading.Tasks; using AvidScript; "
            + "public struct ValueTarget { public int Field; } "
            + "public interface ITarget { int Property { get; set; } } "
            + "public sealed class Target : ITarget { public int Field; public long LongField; public int Property { get; set; } public int ReadOnly { get; } "
            + (instance ? "public async Task<int> Run(int mode) { " + body + " } " : "") + "} "
            + "public static class Script { public static int Trace; public static Target Current = new Target(); "
            + "public static Target Select() { Trace++; return Current; } "
            + "public static T Identity<T>(T value) { return value; } "
            + "public static async Task<int> Read(int mode) { if (mode == 1) return 9; await AvidContinuations.NextTickAsync(); return 7; } "
            + (instance ? "" : "public static async Task<int> Run(Target target, int mode) { " + body + " } ") + "}";
        const string sourceId = "Scripts/AwaitMemberAssignment.cs";
        return AnalyzeSource(source, sourceId);
    }

    internal static SemanticDocument AnalyzeSource(string source, string sourceId, bool enableStaticInitialization = false)
    {
        return SemanticAnalyzer.Analyze(source, sourceId, FrontendAnalyzer.Analyze(source, sourceId).Source.Sha256,
            new[] { new SemanticReferenceSource(Facade, "generated://Continuation.cs", true) }, new SemanticCompilerWorkspace(),
            enableAsyncExceptionFlow: true, enableDirectAwaitCleanup: true, enableAsyncCancellationFlow: true,
            enableStaticInitialization: enableStaticInitialization);
    }

    private const string Facade = """
        using System; using System.Runtime.CompilerServices;
        namespace AvidScript;
        public readonly struct AvidCancellationToken { public readonly long Value; }
        public readonly struct AvidCancellationSource { public AvidCancellationToken Token => default; }
        public static class AvidContinuations { public static AvidDelayAwaitable NextTickAsync() => default; }
        public readonly struct AvidDelayAwaitable {
            public AvidDelayAwaitable WithCancellation(AvidCancellationToken token) => this;
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
