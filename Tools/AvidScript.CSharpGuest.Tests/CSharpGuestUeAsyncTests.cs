using System;
using System.IO;
using System.Linq;
using System.Text.Json;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.WasmBackend;

internal static class CSharpGuestUeAsyncTests
{
    public static int Run()
    {
        foreach (bool capture in new[] { false, true })
        {
            string source = Source.Replace("// CAPTURE", capture ? "Func<int> next = () => Value + amount;" : "", StringComparison.Ordinal)
                .Replace("// CALL", capture ? "Action<int> start = Second.Run; start(3);" : "Second.Run(3);", StringComparison.Ordinal)
                .Replace("NEXT", capture ? "next()" : "Value + amount", StringComparison.Ordinal);
            var document = CSharpGuestContinuationTests.Analyze(source, "Scripts/UeAsync.cs", Facade);
            var lowered = CSharpGuestLowerer.Lower(document, new string('a', 64));
            Check(lowered.Succeeded, string.Join(" | ", lowered.Diagnostics.Select(d => d.Message)));
            var module = lowered.Module!;
            var compiled = WasmModuleCompiler.Compile(module);
            Check(compiled.Succeeded, string.Join(" | ", compiled.Diagnostics.Select(d => d.Message)));
            Check(compiled.Bytes.SequenceEqual(WasmModuleCompiler.Compile(module).Bytes), "UE async WASM is deterministic");
            var declaration = document.UeTypeDeclarations.Single();
            var entry = declaration.Functions.Single();
            var ordinals = SemanticUeTypeRuntimeContract.BuildMemberOrdinals(declaration);
            string? directory = Environment.GetEnvironmentVariable("AVIDSCRIPT_MANAGED_HEAP_WASM_DIR");
            if (string.IsNullOrWhiteSpace(directory)) continue;
            Directory.CreateDirectory(directory);
            string stem = Path.Combine(directory, capture ? "csharp-ue-async-capture" : "csharp-ue-async-scalar");
            File.WriteAllBytes(stem + ".wasm", compiled.Bytes);
            File.WriteAllText(stem + ".json", JsonSerializer.Serialize(new {
                type_id = declaration.TypeId, method_id = entry.MethodSymbolId,
                member_ordinal = ordinals[entry.MethodSymbolId],
                export_name = SemanticUeTypeRuntimeContract.GetFunctionExportName(entry.MethodSymbolId),
                properties = SemanticUeTypeRuntimeContract.BuildPropertyPlans(document).Select(plan => new {
                    member_ordinal = plan.MemberOrdinal, stable_member_id = plan.PropertySymbolId,
                    name = declaration.Properties.Single(property => property.SymbolId == plan.PropertySymbolId).Name,
                    getter_import_name = plan.GetterImportName, setter_import_name = plan.SetterImportName
                }).ToArray(),
                imports = module.Imports.Select(import => new { module = import.Module, name = import.Name }).ToArray()
            }));
        }
        return 2;
    }

    // Appended to the continuation fixture's file-scoped AvidScript namespace.
    private const string Facade = """
        [AttributeUsage(AttributeTargets.Class)] public sealed class UClassAttribute : Attribute { }
        [AttributeUsage(AttributeTargets.Method)] public sealed class UFunctionAttribute : Attribute { }
        [AttributeUsage(AttributeTargets.Property)] public sealed class UPropertyAttribute : Attribute { }
        public abstract class AvidActor { }
        """;

    private const string Source = """
        using System; using AvidScript;
        [UClass] public partial class ReceiverActor : AvidActor {
            private static ReceiverActor First;
            private static ReceiverActor Second;
            [UProperty] public int Value { get; set; }
            [UFunction] public int GetScriptValue() {
                if (First == null) { First = this; return 11; }
                if (Second == null) { Second = this; return 22; }
                // CALL
                return Value;
            }
            private async void Run(int amount) {
                Value += amount;
                // CAPTURE
                amount++;
                await AvidContinuations.NextTickAsync();
                Value = NEXT;
                amount++;
                await AvidContinuations.NextTickAsync();
                Value = NEXT;
            }
        }
        """;

    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
}
