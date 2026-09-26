using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.CompilerServices;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal sealed record CSharpStaticField(string SymbolId, string OwnerTypeId, string TypeId);
internal sealed record CSharpStaticSourceType(string TypeId, bool BeforeFieldInit, string BodyId,
    string SourceId, int SourceLength, SemanticSpan Span, bool RequiresInitialization = true);

// Only the validated source preparation pass can associate this transient
// compiler context with an ordinary document. It is never a serialized opt-out.
internal sealed class CSharpStaticExecutionContext
{
    private static readonly ConditionalWeakTable<SemanticDocument, CSharpStaticExecutionContext> Contexts = new();
    private readonly HashSet<string> guardedAsyncFunctions = new(StringComparer.Ordinal);
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

    internal bool TryCompose(GuestModule module, out GuestModule? guarded, out string? error)
    {
        var guardIds = Types.Select(type => CSharpStaticInitializationGuards.FunctionId(type.TypeId)).ToHashSet(StringComparer.Ordinal);
        var bodies = Types.Select(type => new CSharpStaticInitializer(type.TypeId, CSharpGuestIds.Function(type.BodyId),
            module.LanguageErrorCatalog!.Sources.Single(site => site.SourceId == type.SourceId
                && site.Start == type.Span.Start && site.Length == type.Span.Length).Token)).ToArray();
        return CSharpStaticInitializationGuards.TryCompose(module with
        {
            Functions = module.Functions.Where(function => !guardIds.Contains(function.Id)).ToArray(),
        }, bodies, out guarded, out error);
    }

    internal GuestModule Apply(SemanticDocument document, GuestModule module)
    {
        var types = module.Types.ToDictionary(type => type.Id, StringComparer.Ordinal);
        var slots = Fields.Where(field => types[field.TypeId].Kind == "managed_ref")
            .Select(field => new GuestStaticSlot(Slot(field.SymbolId), field.TypeId)).ToArray();
        var callables = document.Callables.ToDictionary(callable => CSharpGuestIds.Function(callable.MethodSymbolId), StringComparer.Ordinal);
        var functions = module.Functions.Select(function =>
        {
            // Async guards already participate in source exception routing.
            // Reinserting them here would bypass catch/finally and Task ownership.
            if (guardedAsyncFunctions.Contains(function.Id)) return function;
            callables.TryGetValue(function.Id, out var callable);
            return function with { Blocks = AddGuards(document, function.Blocks,
                function.Parameters.Concat(function.Locals), callable, function.EntryBlockId,
                id => types.GetValueOrDefault(id)?.Kind, includeEntry: true) };
        }).ToArray();
        bool asyncSource = SemanticContract.HasAsyncSynchronousExceptions(document);
        return module with
        {
            SchemaVersion = asyncSource ? GuestStaticAsyncExecution.SchemaVersion
                : slots.Length == 0 ? module.SchemaVersion : GuestStaticStorage.SchemaVersion,
            IrVersion = asyncSource ? GuestStaticAsyncExecution.IrVersion
                : slots.Length == 0 ? module.IrVersion : GuestStaticStorage.IrVersion,
            StaticStorage = asyncSource ? new(GuestAsyncSynchronousExceptions.SchemaVersion, GuestAsyncSynchronousExceptions.IrVersion, slots)
                : slots.Length == 0 ? null : new(module.SchemaVersion, module.IrVersion, slots),
            Functions = functions,
        };
    }

    internal bool GuardAsync(CSharpFunctionLoweringContext context, string functionId,
        string entryBlockId, IReadOnlyList<GuestRegister> parameters, List<GuestBasicBlock> blocks)
    {
        var rewritten = AddGuards(context.Document, blocks, parameters.Concat(context.Locals),
            context.Callable, entryBlockId,
            id => context.TryGetGuestType(id, out var type) ? type.Kind : null,
            // Source callers check exact initialization before creating the Task.
            // An exported entry has no source caller; retain its entry check so
            // unsupported error routing is rejected rather than skipped.
            includeEntry: context.Callable.Export is not null
                && functionId == CSharpGuestIds.Function(context.Callable.MethodSymbolId));
        blocks.Clear();
        blocks.AddRange(rewritten);
        guardedAsyncFunctions.Add(functionId);
        return true;
    }

    private IReadOnlyList<GuestBasicBlock> AddGuards(SemanticDocument document,
        IReadOnlyList<GuestBasicBlock> blocks, IEnumerable<GuestRegister> registers,
        SemanticCallable? callable, string entryBlockId, Func<string, string?> typeKind, bool includeEntry)
    {
        var initializedTypes = Types.Where(type => type.RequiresInitialization)
            .Select(type => type.TypeId).ToHashSet(StringComparer.Ordinal);
        var exactTypes = Types.Where(type => type.RequiresInitialization && !type.BeforeFieldInit)
            .Select(type => type.TypeId).ToHashSet(StringComparer.Ordinal);
        var fieldOwners = Fields.Where(field => initializedTypes.Contains(field.OwnerTypeId))
            .ToDictionary(field => typeKind(field.TypeId) == "managed_ref"
                ? Slot(field.SymbolId) : CSharpGuestIds.Global(field.SymbolId), field => field.OwnerTypeId, StringComparer.Ordinal);
        var registerTypes = registers.ToDictionary(register => register.Id, register => register.TypeId, StringComparer.Ordinal);
        var asyncIds = document.AsyncMethods.Select(method => method.MethodSymbolId).ToHashSet(StringComparer.Ordinal);
        var asyncOwners = document.Callables.Where(target => target.IsStatic
                && asyncIds.Contains(target.MethodSymbolId) && exactTypes.Contains(target.ContainingTypeId))
            .ToDictionary(target => CSharpGuestIds.Function(target.MethodSymbolId), target => target.ContainingTypeId, StringComparer.Ordinal);
        bool entryGuard = includeEntry && callable is not null && exactTypes.Contains(callable.ContainingTypeId)
            && !callable.MethodSymbolId.StartsWith("$static:", StringComparison.Ordinal)
            && !(callable.IsConstructor && callable.IsStatic)
            && (callable.IsStatic || typeKind(callable.ContainingTypeId) == "struct");
        return blocks.Select(block =>
        {
            List<GuestInstruction> instructions = new();
            if (entryGuard && block.Id == entryBlockId) instructions.Add(Guard(callable!.ContainingTypeId));
            foreach (var instruction in block.Instructions)
            {
                // Guard at the load/store after operands have been evaluated.
                if (instruction.Op is "global_load" or "global_store" or "borrow_global" or GuestStaticStorage.GetOp or GuestStaticStorage.SetOp
                    && instruction.TargetId is { } field && fieldOwners.TryGetValue(field, out var owner))
                    instructions.Add(Guard(owner));
                if (instruction.Op == "managed_new" && instruction.ResultId is { } result
                    && registerTypes.TryGetValue(result, out var allocated) && exactTypes.Contains(allocated))
                    instructions.Add(Guard(allocated));
                if (instruction.Op == "call" && instruction.TargetId is { } callee
                    && asyncOwners.TryGetValue(callee, out var asyncOwner))
                    instructions.Add(Guard(asyncOwner));
                instructions.Add(instruction);
            }
            return block with { Instructions = instructions };
        }).ToArray();
    }
    private static GuestInstruction Guard(string owner) => new("call", null, Array.Empty<string>(),
        CSharpStaticInitializationGuards.FunctionId(owner), null, null);
}
