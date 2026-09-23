using System;
using System.Linq;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpSemantic;

internal static class SemanticClosureTests
{
    public static int Run()
    {
        int count = 0;
        const string source = """
            using System;
            public static class Script {
                public static Func<int> Make(int n) {
                    int shared = n;
                    int Read() => ++shared;
                    Func<int> first = () => ++shared + n;
                    Func<int> second = () => Read() - n;
                    return n > 0 ? first : second;
                }
            }
            """;
        SemanticDocument document = Analyze(source);
        Check(document.SchemaVersion == 29 && document.SemanticVersion == "1.33"
            && document.Succeeded && document.ControlFlowGraphs.Count != 0,
            "captured lambda analysis must publish executable CFGs and allocation metadata for Guest lowering");
        Check(SemanticClosureContractValidator.IsValid(document), "shared plan contract");
        SemanticClosureEnvironment environment = document.ClosureEnvironments.Single();
        Check(environment.ScopeKind == "activation" && environment.Cells.Count == 2,
            "parameter and body local share the owner activation environment");
        Check(document.ClosureBindings.Count == 3 && document.ClosureBindings.Count(binding => binding.IsDelegateTarget) == 2,
            "direct local reader and both delegates must bind the same cells");
        string sharedId = environment.Cells.Single(cell => cell.Kind == "local").SymbolId;
        Check(document.ClosureBindings.All(binding => binding.CellSymbolIds.Contains(sharedId)),
            "transitive capture must retain the original mutable cell identity");
        Check(SemanticSerializer.Serialize(document).SequenceEqual(SemanticSerializer.Serialize(
            SemanticSerializer.Deserialize(SemanticSerializer.Serialize(document)))), "canonical round trip");
        Check(SemanticSerializer.Serialize(document).SequenceEqual(SemanticSerializer.Serialize(Analyze(source))), "deterministic repeated projection");
        Check(document.ClosureEnvironments.Select(item => item.Id).SequenceEqual(Analyze("\n\n" + source).ClosureEnvironments.Select(item => item.Id)),
            "allocation scope identities must not depend on whitespace");

        SemanticDocument loops = Analyze("""
            using System;
            public static class Script {
                public static Func<int> Make(int[] values) {
                    Func<int> result = null;
                    for (int i = 0; i < 3; i++) { int copy = i; result = () => i + copy; }
                    foreach (int item in values) { result = () => item; }
                    return result;
                }
            }
            """);
        Check(SemanticClosureContractValidator.IsValid(loops), "loop plan contract");
        Check(loops.ClosureEnvironments.Select(item => item.ScopeKind).OrderBy(item => item)
            .SequenceEqual(new[] { "block_entry", "for_entry", "foreach_iteration" }),
            "for initializer shares one cell; body local and foreach iteration allocate on re-entry");
        Check(loops.ClosureEnvironments.All(item => item.Cells.Count == 1), "loop cells remain distinct");
        SemanticDocument siblings = Analyze("""
            using System;
            public static class Script { public static Func<int> Make(bool flag) {
                if (flag) { int value = 1; return () => value; }
                else { int value = 2; return () => value; }
            } }
            """);
        Check(SemanticClosureContractValidator.IsValid(siblings) && siblings.ClosureEnvironments.Count == 2
            && siblings.ClosureEnvironments.Select(item => item.Id).Distinct().Count() == 2,
            "same-named variables in sibling scopes must not alias");
        SemanticDocument cycle = Analyze("using System; public static class Script { public static Func<int> Make() { Func<int> self = null; self = () => self(); return self; } }");
        Check(SemanticClosureContractValidator.IsValid(cycle) && cycle.ClosureEnvironments.Single().Cells.Single().TypeId.Contains("System.Func", StringComparison.Ordinal),
            "delegate-valued cells expose cycles that the runtime collector must trace");
        SemanticDocument invalidRef = Analyze("using System; public static class Script { public static Func<int> Make(ref int value) => () => value; }");
        Check(!invalidRef.Succeeded && invalidRef.ClosureEnvironments.Count == 0
            && invalidRef.Diagnostics.Any(item => item.Code == "CS1628"), "Roslyn ref capture errors must remain authoritative");
        SemanticDocument nested = Analyze("using System; public static class Script { public static Func<int, Func<int>> Make() => x => () => x; }");
        Check(SemanticClosureContractValidator.IsValid(nested) && nested.ClosureEnvironments.Single().OwnerMethodSymbolId.Contains(":lambda:", StringComparison.Ordinal),
            "a nested lambda captures its creating lambda's activation");
        SemanticDocument receiver = Analyze("using System; public class Script { int value; public Func<int> Make() => () => value; }");
        Check(SemanticClosureContractValidator.IsValid(receiver) && receiver.ClosureEnvironments.Single().Cells.Single().Kind == "receiver",
            "instance receiver is an identity cell, not a copied field");
        SemanticDocument setter = Analyze("using System; public class Script { int field; public int Value { set { Func<int> read = () => value; field = read(); } } }");
        Check(SemanticClosureContractValidator.IsValid(setter) && setter.ClosureEnvironments.Single().Cells.Single().Kind == "parameter",
            "implicit setter value has an activation owner");
        SemanticDocument group = Analyze("using System; public static class Script { public static Func<int> Make(int n) { int Read() => ++n; return Read; } }");
        Check(group.Succeeded && SemanticClosureContractValidator.IsValid(group) && group.ClosureBindings.Single().IsDelegateTarget,
            "capturing method groups need the same environment contract as lambdas");
        SemanticDocument direct = Analyze("public static class Script { public static int Run(int n) { int Read() => ++n; return Read(); } }");
        Check(direct.Succeeded && SemanticClosureContractValidator.IsValid(direct) && direct.ClosureEnvironments.Count == 0,
            "direct-only local calls do not allocate managed environments");
        SemanticDocument unsupportedScope = Analyze("using System; public static class Script { public static Func<int> Make() { int.TryParse(\"1\", out int value); return () => value; } }");
        Check(unsupportedScope.Diagnostics.Any(item => item.Code == "ASCS4004") && unsupportedScope.ClosureEnvironments.Count == 0,
            "unimplemented designation scopes must not produce partial allocation plans");

        SemanticClosureBinding binding = document.ClosureBindings[0];
        SemanticClosureCell cell = environment.Cells[0];
        foreach (SemanticDocument malformed in new[]
        {
            document with { ClosureEnvironments = null! },
            document with { ClosureBindings = null! },
            document with { ClosureEnvironments = Array.Empty<SemanticClosureEnvironment>(), ClosureBindings = Array.Empty<SemanticClosureBinding>() },
            document with { ClosureEnvironments = new[] { environment, environment } },
            document with { ClosureEnvironments = new[] { environment with { Id = "forged" } } },
            document with { ClosureEnvironments = new[] { environment with { OwnerMethodSymbolId = "missing" } } },
            document with { ClosureEnvironments = new[] { environment with { ScopeKind = "unknown" } } },
            document with { ClosureEnvironments = new[] { environment with { ScopeOrdinal = -1 } } },
            document with { ClosureEnvironments = new[] { environment with { Cells = new[] { cell with { TypeId = "type:int64" } } } } },
            document with { ClosureEnvironments = new[] { environment with { Cells = new[] { cell, cell } } } },
            document with { ClosureBindings = Array.Empty<SemanticClosureBinding>() },
            document with { ClosureBindings = document.ClosureBindings.Append(binding).ToArray() },
            document with { ClosureBindings = document.ClosureBindings.Select(item => item == binding ? item with { CellSymbolIds = new[] { "missing" } } : item).ToArray() },
            document with { ClosureBindings = document.ClosureBindings.Select(item => item == binding ? item with { IsDelegateTarget = !item.IsDelegateTarget } : item).ToArray() },
            document with { SchemaVersion = 21, SemanticVersion = "1.25" },
        }) Check(!SemanticClosureContractValidator.IsValid(malformed), "malformed, missing, duplicated or legacy environment plan must fail closed");
        return count;

        void Check(bool condition, string message) { if (!condition) throw new InvalidOperationException(message); count++; }
    }

    private static SemanticDocument Analyze(string source)
    {
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, "Scripts/ClosurePlan.cs");
        return SemanticAnalyzer.Analyze(source, "Scripts/ClosurePlan.cs", frontend.Source.Sha256);
    }
}
