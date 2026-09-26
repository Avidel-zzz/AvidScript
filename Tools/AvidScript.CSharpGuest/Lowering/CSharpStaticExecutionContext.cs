using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.CompilerServices;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal sealed record CSharpStaticField(string SymbolId, string OwnerTypeId, string TypeId);
internal sealed record CSharpStaticSourceType(string TypeId, bool BeforeFieldInit, string BodyId,
    string SourceId, int SourceLength, SemanticSpan Span);

// Only the validated source preparation pass can associate this transient
// compiler context with an ordinary document. It is never a serialized opt-out.
internal sealed class CSharpStaticExecutionContext
{
    private static readonly ConditionalWeakTable<SemanticDocument, CSharpStaticExecutionContext> Contexts = new();
    public IReadOnlyList<CSharpStaticField> Fields { get; }
    public IReadOnlyList<CSharpStaticSourceType> Types { get; }
    public static string Slot(string symbolId) => "static:field:" + symbolId;
    internal CSharpStaticExecutionContext(IReadOnlyList<CSharpStaticField> fields, IReadOnlyList<CSharpStaticSourceType> types)
    { Fields = fields; Types = types; }
    internal void Attach(SemanticDocument document) => Contexts.Add(document, this);
    internal static CSharpStaticExecutionContext? Find(SemanticDocument document) =>
        Contexts.TryGetValue(document, out var context) ? context : null;
    internal bool Owns(string? symbolId) => Fields.Any(field => field.SymbolId == symbolId);
    internal static bool UsesSlot(CSharpFunctionLoweringContext context, string? symbolId, string? typeId) =>
        Find(context.Document)?.Owns(symbolId) == true
        && context.TryGetGuestType(typeId, out var type) && type.Kind == "managed_ref";

    internal GuestModule Apply(SemanticDocument document, GuestModule module)
    {
        var types = module.Types.ToDictionary(type => type.Id, StringComparer.Ordinal);
        var slots = Fields.Where(field => types[field.TypeId].Kind == "managed_ref")
            .Select(field => new GuestStaticSlot(Slot(field.SymbolId), field.TypeId)).ToArray();
        var fieldOwners = Fields.ToDictionary(field => types[field.TypeId].Kind == "managed_ref"
            ? Slot(field.SymbolId) : CSharpGuestIds.Global(field.SymbolId), field => field.OwnerTypeId, StringComparer.Ordinal);
        var exactTypes = Types.Where(type => !type.BeforeFieldInit).Select(type => type.TypeId).ToHashSet(StringComparer.Ordinal);
        var callables = document.Callables.ToDictionary(callable => CSharpGuestIds.Function(callable.MethodSymbolId), StringComparer.Ordinal);
        var functions = module.Functions.Select(function =>
        {
            var registers = function.Parameters.Concat(function.Locals).ToDictionary(register => register.Id, register => register.TypeId, StringComparer.Ordinal);
            callables.TryGetValue(function.Id, out var callable);
            bool entryGuard = callable is not null && exactTypes.Contains(callable.ContainingTypeId)
                && !callable.MethodSymbolId.StartsWith("$static:", StringComparison.Ordinal)
                && !(callable.IsConstructor && callable.IsStatic)
                && (callable.IsStatic || types.GetValueOrDefault(callable.ContainingTypeId)?.Kind == "struct");
            return function with { Blocks = function.Blocks.Select(block =>
            {
                List<GuestInstruction> instructions = new();
                if (entryGuard && block.Id == function.EntryBlockId) instructions.Add(Guard(callable!.ContainingTypeId));
                foreach (var instruction in block.Instructions)
                {
                    // Insert at the actual load/store, after operand evaluation.
                    if (instruction.Op is "global_load" or "global_store" or "borrow_global" or GuestStaticStorage.GetOp or GuestStaticStorage.SetOp
                        && instruction.TargetId is { } field && fieldOwners.TryGetValue(field, out var owner))
                        instructions.Add(Guard(owner));
                    // Arguments have already been evaluated; no instance exists yet.
                    if (instruction.Op == "managed_new" && instruction.ResultId is { } result
                        && registers.TryGetValue(result, out var allocated) && exactTypes.Contains(allocated))
                        instructions.Add(Guard(allocated));
                    instructions.Add(instruction);
                }
                return block with { Instructions = instructions };
            }).ToArray() };
        }).ToArray();
        return module with
        {
            SchemaVersion = slots.Length == 0 ? module.SchemaVersion : GuestStaticStorage.SchemaVersion,
            IrVersion = slots.Length == 0 ? module.IrVersion : GuestStaticStorage.IrVersion,
            StaticStorage = slots.Length == 0 ? null : new(module.SchemaVersion, module.IrVersion, slots),
            Functions = functions,
        };
    }
    private static GuestInstruction Guard(string owner) => new("call", null, Array.Empty<string>(),
        CSharpStaticInitializationGuards.FunctionId(owner), null, null);
}
