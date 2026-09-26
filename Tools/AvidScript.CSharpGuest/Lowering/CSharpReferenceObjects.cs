using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.CompilerServices;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal static class CSharpReferenceObjects
{
    private sealed record Layout(string DefinitionTypeId, IReadOnlyDictionary<string, string> TypeArguments);
    private sealed record Plan(IReadOnlySet<string> Types, IReadOnlyDictionary<string, Layout> Layouts);
    private static readonly ConditionalWeakTable<SemanticDocument, Plan> Plans = new();
    public static IReadOnlySet<string> Types(SemanticDocument document) => Plans.GetValue(document, Analyze).Types;
    public static string Payload(string type) => "type:$class:payload:" + type;
    public static string Guard(string type) => "function:$class:require:" + type;

    private static Plan Analyze(SemanticDocument document)
    {
        Dictionary<string, SemanticClassType> classes = document.ClassTypes.ToDictionary(
            type => type.TypeId, StringComparer.Ordinal);
        Dictionary<string, SemanticType> semanticTypes = document.Types.ToDictionary(
            type => type.Id, StringComparer.Ordinal);
        Dictionary<string, SemanticTypeShape> shapes = document.TypeShapes.ToDictionary(
            shape => shape.TypeId, StringComparer.Ordinal);
        Dictionary<string, Layout> candidates = new(StringComparer.Ordinal);
        foreach (SemanticClassType type in document.ClassTypes)
        {
            if (!Eligible(type) || document.UeTypeDeclarations.Any(ue => ue.TypeId == type.TypeId))
                continue;
            string definitionId = type.TypeId;
            Dictionary<string, string> arguments = new(StringComparer.Ordinal);
            if (type.IsGeneric)
            {
                if (!shapes.TryGetValue(type.TypeId, out SemanticTypeShape? shape)
                    || shape.GenericDefinitionTypeId is not { } genericDefinitionId
                    || genericDefinitionId == type.TypeId
                    || !classes.TryGetValue(genericDefinitionId, out SemanticClassType? definition)
                    || !Eligible(definition) || !definition.IsGeneric
                    || !shapes.TryGetValue(genericDefinitionId, out SemanticTypeShape? definitionShape)
                    || definitionShape.GenericArgumentTypeIds is not { } formals
                    || shape.GenericArgumentTypeIds is not { } actuals
                    || formals.Count == 0 || formals.Count != actuals.Count
                    || formals.Distinct(StringComparer.Ordinal).Count() != formals.Count
                    || formals.Any(id => !semanticTypes.TryGetValue(id, out SemanticType? formal)
                        || formal.Kind != "type_parameter")
                    || actuals.Any(id => !IsClosed(id, semanticTypes, shapes,
                        new HashSet<string>(StringComparer.Ordinal))))
                    continue;
                definitionId = genericDefinitionId;
                arguments = formals.Zip(actuals).ToDictionary(
                    pair => pair.First, pair => pair.Second, StringComparer.Ordinal);
            }
            if (!document.Symbols.Any(symbol => symbol.Kind == "type" && symbol.TypeId == definitionId))
                continue;
            if (type.HasInstanceInitializers
                && !SemanticInstanceInitializerContract.IsNormalized(document, definitionId)) continue;
            candidates.Add(type.TypeId, new Layout(definitionId, arguments));
        }
        bool Reachable(string id) => document.Reachability is null || document.Reachability.ReachableCallableIds.Contains(id);
        HashSet<string> used = new(StringComparer.Ordinal);
        Stack<SemanticOperation> pending = new(document.Methods.Where(method => Reachable(method.MethodSymbolId)).Select(method => method.Root));
        foreach (SemanticBasicBlock block in document.ControlFlowGraphs.Where(graph => Reachable(graph.MethodSymbolId)).SelectMany(graph => graph.Blocks))
        {
            foreach (SemanticOperation operation in block.Operations) pending.Push(operation);
            if (block.BranchValue is not null) pending.Push(block.BranchValue);
        }
        while (pending.TryPop(out SemanticOperation? operation))
        {
            if (operation.TypeId is { } id) used.Add(id);
            foreach (SemanticOperation child in operation.Children) pending.Push(child);
        }
        foreach (SemanticCallable callable in document.Callables.Where(callable => Reachable(callable.MethodSymbolId)))
        {
            if (!callable.IsStatic) used.Add(callable.ContainingTypeId);
            used.Add(callable.ReturnTypeId);
            foreach (SemanticCallableParameter parameter in callable.Parameters) used.Add(parameter.TypeId);
        }
        // Value and closure layouts are emitted even for unused helper bodies.
        // Their reference fields still need a concrete type in the layout table.
        foreach (SemanticType type in document.Types.Where(type => type.Kind == "struct")) used.Add(type.Id);
        foreach (SemanticClosureCell cell in document.ClosureEnvironments.SelectMany(environment => environment.Cells)) used.Add(cell.TypeId);
        foreach (SemanticAsyncStateSlot input in document.AsyncMethods.SelectMany(method => method.InvocationInputs)) used.Add(input.TypeId);
        // Discover reference fields inside used value types as well as recursive object graphs.
        var owners = document.Symbols.Where(symbol => symbol.Kind == "type" && symbol.TypeId is not null)
            .ToDictionary(symbol => symbol.Id, symbol => symbol.TypeId!, StringComparer.Ordinal);
        bool changed;
        do { changed = false; foreach (SemanticSymbol field in document.Symbols.Where(symbol => symbol.Kind == "field" && !symbol.IsStatic))
            if (field.ContainingSymbolId is { } owner && owners.TryGetValue(owner, out string? type) && used.Contains(type)
                && field.TypeId is { } fieldType) changed |= used.Add(fieldType); } while (changed);
        foreach (string id in candidates.Keys.Where(id => !used.Contains(id)).ToArray())
            candidates.Remove(id);
        return new(candidates.Keys.ToHashSet(StringComparer.Ordinal), candidates);
    }

    public static bool AddTypes(SemanticDocument document, List<GuestType> types,
        List<GuestDiagnostic> diagnostics)
    {
        Plan plan = Plans.GetValue(document, Analyze);
        Dictionary<string, SemanticType> semanticTypes = document.Types.ToDictionary(
            type => type.Id, StringComparer.Ordinal);
        Dictionary<string, SemanticTypeShape> shapes = document.TypeShapes.ToDictionary(
            shape => shape.TypeId, StringComparer.Ordinal);
        foreach (string id in Types(document).OrderBy(id => id, StringComparer.Ordinal))
        {
            Layout layout = plan.Layouts[id];
            SemanticSymbol[] owners = document.Symbols.Where(symbol => symbol.Kind == "type"
                && symbol.TypeId == layout.DefinitionTypeId).ToArray();
            if (owners.Length != 1)
            {
                diagnostics.Add(new GuestDiagnostic("ASCG1024", "error",
                    $"Reference type '{id}' has no unique source definition.", null));
                return false;
            }
            string owner = owners[0].Id;
            List<GuestField> closedFields = new();
            foreach (SemanticSymbol field in document.Symbols.Where(symbol => symbol.Kind == "field"
                && !symbol.IsStatic && symbol.ContainingSymbolId == owner)
                .OrderBy(symbol => symbol.Id, StringComparer.Ordinal))
            {
                if (field.TypeId is null || !SemanticGenericTypeSubstitution.TryClose(
                        field.TypeId, layout.TypeArguments, semanticTypes, shapes,
                        out string closedTypeId))
                {
                    diagnostics.Add(new GuestDiagnostic("ASCG1024", "error",
                        $"Reference field '{field.Id}' has no closed type layout.", null));
                    return false;
                }
                closedFields.Add(new GuestField(field.Id, field.Name, closedTypeId, 0));
            }
            GuestField[] fields = closedFields.ToArray();
            if (fields.Length == 0) fields = new[] { new GuestField("$empty:" + id, "$storage", "type:uint8", 0) };
            types.Add(new(Payload(id), "struct", "memory", fields, null, null, 0, 1));
            types.Add(new(id, "managed_ref", "i64", Array.Empty<GuestField>(), Payload(id), null, 8, 8));
        }
        if (Types(document).Count == 0) return true;
        types.Add(new("type:object", "managed_ref", "i64", Array.Empty<GuestField>(), null, null, 8, 8));
        if (types.All(type => type.Id != "type:bool")) types.Add(new("type:bool", "scalar", "i32", Array.Empty<GuestField>(), null, null, 4, 4));
        if (types.All(type => type.Id != "type:void")) types.Add(new("type:void", "void", "none", Array.Empty<GuestField>(), null, null, 0, 1));
        return true;
    }

    public static bool IsClosedImplicitConstructor(SemanticDocument document,
        string? closedTypeId, SemanticCallable constructor)
    {
        if (closedTypeId is null || constructor.HasBody || constructor.Parameters.Count != 0
            || !Plans.GetValue(document, Analyze).Layouts.TryGetValue(closedTypeId, out Layout? layout)
            || layout.DefinitionTypeId == closedTypeId
            || constructor.ContainingTypeId != layout.DefinitionTypeId)
            return false;
        return document.ClassTypes.Any(type => type.TypeId == closedTypeId
            && type.HasImplicitDefaultConstructor);
    }

    private static bool Eligible(SemanticClassType type) => type.IsSourceDeclared
        && !type.IsStatic && !type.IsAbstract && !type.IsRecord && !type.HasPrimaryConstructor
        && !type.HasStaticInitialization
        && !type.HasImplicitInstanceStorage && !type.HasVirtualMembers && !type.HasFinalizer
        && type.BaseTypeId == "type:object"
        && (type.InterfaceTypeIds.Count == 0
            || type.IsSealed && type.InterfaceTypeIds.Count == 1
                && type.InterfaceTypeIds[0] == "type:global::System.IDisposable");

    private static bool IsClosed(string typeId,
        IReadOnlyDictionary<string, SemanticType> types,
        IReadOnlyDictionary<string, SemanticTypeShape> shapes,
        HashSet<string> visiting)
    {
        if (!types.TryGetValue(typeId, out SemanticType? type)
            || type.Kind == "type_parameter" || !visiting.Add(typeId))
            return false;
        if (!shapes.TryGetValue(typeId, out SemanticTypeShape? shape)) return true;
        if (shape.ElementTypeId is { } elementId
            && !IsClosed(elementId, types, shapes, new HashSet<string>(visiting, StringComparer.Ordinal)))
            return false;
        return shape.GenericArgumentTypeIds?.All(id => IsClosed(id, types, shapes,
            new HashSet<string>(visiting, StringComparer.Ordinal))) != false;
    }

    public static bool IsField(CSharpFunctionLoweringContext context, SemanticOperation operation) =>
        operation.Kind == "field_reference" && operation.Children.Count == 1
        && Types(context.Document).Contains(operation.Children[0].TypeId ?? "");

    public static void Require(GuestRegister value, List<GuestInstruction> instructions) =>
        instructions.Add(new("call", null, new[] { value.Id }, Guard(value.TypeId), null, null));

    public static void AddGuards(SemanticDocument document, List<GuestFunction> functions)
    {
        var methods = document.Callables.Where(callable => !callable.IsStatic && callable.HasBody && Types(document).Contains(callable.ContainingTypeId))
            .ToDictionary(callable => CSharpGuestIds.Function(callable.MethodSymbolId), callable => callable.ContainingTypeId, StringComparer.Ordinal);
        for (int i = 0; i < functions.Count; i++)
            if (methods.TryGetValue(functions[i].Id, out string? type))
                functions[i] = functions[i] with { Blocks = functions[i].Blocks.Select(block => block.Id == functions[i].EntryBlockId
                    ? block with { Instructions = new[] { new GuestInstruction("call", null, new[] { functions[i].Parameters[0].Id }, Guard(type), null, null) }
                        .Concat(block.Instructions).ToArray() } : block).ToArray() };
        foreach (string type in Types(document).OrderBy(id => id, StringComparer.Ordinal))
            functions.Add(new(Guard(type), new[] { new GuestRegister("receiver", type) },
                new[] { new GuestRegister("null", type), new GuestRegister("is_null", "type:bool") }, "type:void", "entry", new[] {
                    new GuestBasicBlock("entry", new[] { new GuestInstruction("constant", "null", Array.Empty<string>(), null, null, new("null", null)),
                        new GuestInstruction("binary", "is_null", new[] { "receiver", "null" }, null, "equals", null) }, new("branch_if", "is_null", "failed", "ok", null)),
                    new GuestBasicBlock("failed", Array.Empty<GuestInstruction>(), new("trap", null, null, null, null)),
                    new GuestBasicBlock("ok", Array.Empty<GuestInstruction>(), new("return", null, null, null, null)) }));
    }
}
