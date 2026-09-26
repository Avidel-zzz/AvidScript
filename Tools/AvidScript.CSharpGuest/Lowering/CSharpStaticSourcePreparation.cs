using System;
using System.Collections.Generic;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using AvidScript.CSharpSemantic;

namespace AvidScript.CSharpGuest;

// Private executable normalization. The original versioned document is checked
// first and remains the source/provenance of the published module.
internal static class CSharpStaticSourcePreparation
{
    internal static bool TryPrepare(SemanticDocument source, out SemanticDocument? ordinary,
        out CSharpStaticExecutionContext? execution, out string? error)
    {
        ordinary = null; execution = null; error = null;
        if (!SemanticStaticInitializationValidator.IsValid(source) || !source.Succeeded
            || source.AsyncMethods.Count != 0 || source.ExceptionFlows is not null
            || source.RejectedAsyncExceptionFlows is not null || source.UeTypeDeclarations is not { Count: 0 })
        { error = "Static source execution requires a valid synchronous source plan; async, exception and generated UE routes are not connected yet."; return false; }
        // The static envelope validates ownership and initializer plans. Reuse
        // the base reader for every ordinary callable, symbol and CFG invariant
        // before building dictionaries or specializing any source body.
        SemanticOperation WithoutOwner(SemanticOperation operation) => operation with
        {
            StaticFieldOwnerTypeId = null,
            Children = operation.Children.Select(WithoutOwner).ToArray(),
        };
        var validationView = source with
        {
            SchemaVersion = source.StaticInitialization!.BaseSchemaVersion,
            SemanticVersion = source.StaticInitialization.BaseSemanticVersion,
            StaticInitialization = null,
            Methods = source.Methods.Select(body => body with { Root = WithoutOwner(body.Root) }).ToArray(),
            ControlFlowGraphs = source.ControlFlowGraphs.Select(graph => graph with { Blocks = graph.Blocks.Select(block => block with
            {
                Operations = block.Operations.Select(WithoutOwner).ToArray(),
                BranchValue = block.BranchValue is null ? null : WithoutOwner(block.BranchValue),
            }).ToArray() }).ToArray(),
        };
        if (!CSharpSemanticInputValidator.IsValid(validationView))
        { error = "Static source has a malformed base Semantic contract."; return false; }
        var types = source.Types.ToDictionary(type => type.Id, StringComparer.Ordinal);
        var shapes = source.TypeShapes.ToDictionary(shape => shape.TypeId, StringComparer.Ordinal);
        var plans = source.StaticInitialization!.Types.ToDictionary(type => type.TypeId, StringComparer.Ordinal);
        var symbols = source.Symbols.ToDictionary(symbol => symbol.Id, StringComparer.Ordinal);
        var callables = source.Callables.ToDictionary(callable => callable.MethodSymbolId, StringComparer.Ordinal);
        var bodies = source.Methods.ToDictionary(body => body.MethodSymbolId, StringComparer.Ordinal);
        var graphs = source.ControlFlowGraphs.ToDictionary(graph => graph.MethodSymbolId, StringComparer.Ordinal);
        List<SemanticSymbol> addedSymbols = new();
        List<SemanticCallable> addedCallables = new();
        List<SemanticMethodBody> addedBodies = new();
        List<SemanticControlFlowGraph> addedGraphs = new();
        List<CSharpStaticField> fields = new();
        List<CSharpStaticSourceType> owners = new();
        string? failure = null;
        var emptyMap = new Dictionary<string, string>(StringComparer.Ordinal);
        var fieldIds = new Dictionary<(string Field, string Owner), string>();
        var closedOwners = new List<(string Id, SemanticStaticTypeInitialization Plan, Dictionary<string, string> Arguments)>();

        bool IsClosed(string id, int depth = 0) => depth < 32 && types.TryGetValue(id, out var type)
            && type.Kind != "type_parameter" && (!shapes.TryGetValue(id, out var shape)
                || (shape.ElementTypeId is null || IsClosed(shape.ElementTypeId, depth + 1))
                    && (shape.GenericArgumentTypeIds is null || shape.GenericArgumentTypeIds.All(argument => IsClosed(argument, depth + 1))));
        string? MapType(string? id, IReadOnlyDictionary<string, string> map)
        {
            if (id is null || map.Count == 0) return id;
            if (!SemanticGenericTypeSubstitution.TryClose(id, map, types, shapes, out var closed))
                failure ??= "Static initialization needs a registered closed type: " + id;
            return closed;
        }
        foreach (var type in source.Types.Where(type => IsClosed(type.Id)).OrderBy(type => type.Id, StringComparer.Ordinal))
        {
            string definition = shapes.TryGetValue(type.Id, out var shape) && shape.GenericDefinitionTypeId is { } generic ? generic : type.Id;
            if (!plans.TryGetValue(definition, out var plan)) continue;
            Dictionary<string, string> arguments = new(StringComparer.Ordinal);
            if (definition != type.Id)
            {
                var parameters = shapes[definition].GenericArgumentTypeIds!;
                arguments = parameters.Zip(shape!.GenericArgumentTypeIds!).ToDictionary(pair => pair.First, pair => pair.Second, StringComparer.Ordinal);
            }
            closedOwners.Add((type.Id, plan, arguments));
            foreach (var field in plan.Fields)
            {
                var symbol = symbols[field.FieldSymbolId];
                string suffix = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(type.Id + "\n" + symbol.Id))).ToLowerInvariant();
                string id = "symbol:field:$static:" + suffix;
                string fieldType = MapType(symbol.TypeId, arguments)!;
                fieldIds.Add((symbol.Id, type.Id), id);
                fields.Add(new(id, type.Id, fieldType));
                addedSymbols.Add(symbol with { Id = id, Name = symbol.Name + "$" + suffix, TypeId = fieldType });
            }
        }
        if (closedOwners.Count == 0 || closedOwners.Count + fields.Count > 4096)
        { error = "Static execution needs a bounded set of closed storage owners."; return false; }

        SemanticOperation Rewrite(SemanticOperation operation, IReadOnlyDictionary<string, string> map,
            IReadOnlyDictionary<string, string>? localSymbols = null)
        {
            if (operation.TypeArgumentIds is null)
            { failure ??= "Static initializer operation has missing type arguments."; return operation; }
            string? id = operation.SymbolId;
            string? owner = MapType(operation.StaticFieldOwnerTypeId, map);
            if (owner is not null && id is not null && fieldIds.TryGetValue((id, owner), out var field)) id = field;
            else if (owner is not null && IsClosed(owner)) failure ??= "Static access has no closed field storage: " + id;
            if (id is not null && localSymbols?.TryGetValue(id, out var local) == true) id = local;
            string[] arguments = operation.TypeArgumentIds.Select(argument => MapType(argument, map)!).ToArray();
            if (id is not null && callables.TryGetValue(id, out var callable)
                && callable.GenericTypeParameterIds is { Count: > 0 } && arguments.All(IsClosedArgument))
                id = Specialize(callable, arguments);
            return operation with
            {
                SymbolId = id, TypeId = MapType(operation.TypeId, map), StaticFieldOwnerTypeId = null,
                TypeArgumentIds = arguments,
                Children = operation.Children.Select(child => Rewrite(child, map, localSymbols)).ToArray(),
            };
        }
        bool IsClosedArgument(string id) => IsClosed(id);
        SemanticControlFlowGraph RewriteGraph(SemanticControlFlowGraph graph, string id,
            IReadOnlyDictionary<string, string> map, IReadOnlyDictionary<string, string>? locals = null) => graph with
        {
            MethodSymbolId = id, Blocks = graph.Blocks.Select(block => block with
            {
                Operations = block.Operations.Select(operation => Rewrite(operation, map, locals)).ToArray(),
                BranchValue = block.BranchValue is null ? null : Rewrite(block.BranchValue, map, locals),
            }).ToArray(),
        };
        var instances = source.Callables.Select(callable => callable.MethodSymbolId).ToHashSet(StringComparer.Ordinal);
        string Specialize(SemanticCallable definition, IReadOnlyList<string> arguments)
        {
            string id = SemanticContract.GenericInstanceId(definition.MethodSymbolId, arguments);
            if (instances.Contains(id)) return id;
            if (instances.Count > 4096 || definition.GenericTypeParameterIds!.Count != arguments.Count
                || !bodies.TryGetValue(definition.MethodSymbolId, out var body)
                || !graphs.TryGetValue(definition.MethodSymbolId, out var graph))
            { failure ??= "Generic initializer call has no executable source body: " + definition.MethodSymbolId; return id; }
            instances.Add(id);
            var map = definition.GenericTypeParameterIds.Zip(arguments).ToDictionary(pair => pair.First, pair => pair.Second, StringComparer.Ordinal);
            var locals = source.Symbols.Where(symbol => symbol.Id == definition.MethodSymbolId || symbol.ContainingSymbolId == definition.MethodSymbolId)
                .ToDictionary(symbol => symbol.Id, symbol => symbol.Id.Replace(definition.MethodSymbolId, id, StringComparison.Ordinal), StringComparer.Ordinal);
            foreach (var symbol in source.Symbols.Where(symbol => locals.ContainsKey(symbol.Id)))
                addedSymbols.Add(symbol with { Id = locals[symbol.Id], TypeId = MapType(symbol.TypeId, map),
                    ContainingSymbolId = symbol.ContainingSymbolId == definition.MethodSymbolId ? id : symbol.ContainingSymbolId });
            addedCallables.Add(definition with { MethodSymbolId = id, ContainingTypeId = MapType(definition.ContainingTypeId, map)!,
                ReturnTypeId = MapType(definition.ReturnTypeId, map)!, GenericTypeParameterIds = Array.Empty<string>(),
                GenericDefinitionSymbolId = definition.MethodSymbolId, GenericArgumentTypeIds = arguments,
                Parameters = definition.Parameters.Select(parameter => parameter with { SymbolId = locals[parameter.SymbolId], TypeId = MapType(parameter.TypeId, map)! }).ToArray() });
            addedBodies.Add(body with { MethodSymbolId = id, Root = Rewrite(body.Root, map, locals) });
            addedGraphs.Add(RewriteGraph(graph, id, map, locals));
            return id;
        }
        void AddMethod(string id, string owner, SemanticOperation body, SemanticControlFlowGraph graph)
        {
            addedSymbols.Add(new(id, "method", id, null, "type:void", id, true, "private", body.Span));
            addedCallables.Add(new(id, owner, "type:void", Array.Empty<SemanticCallableParameter>(), true, false, true, null, null, null,
                Dispatch: new(false, false, false, false, null, null, Array.Empty<string>()), GenericTypeParameterIds: Array.Empty<string>()));
            addedBodies.Add(new(id, body)); addedGraphs.Add(graph);
        }
        foreach (var (owner, plan, arguments) in closedOwners)
        {
            var declaration = source.Symbols.Single(symbol => symbol.Kind == "type" && symbol.TypeId == plan.TypeId);
            var firstField = plan.Fields.FirstOrDefault();
            if (firstField is null && declaration.IsExecutableReferenceSource)
            { failure ??= "A fieldless reference-source initializer needs explicit source-unit provenance."; continue; }
            SemanticSpan span = firstField?.Span ?? declaration.Span;
            string bodyId = "$static:body:" + owner;
            owners.Add(new(owner, plan.BeforeFieldInit, bodyId, firstField?.SourceId ?? source.Source.SourceId,
                firstField?.SourceLength ?? source.Source.Length, span));
            List<SemanticOperation> calls = new();
            foreach (var field in plan.Fields.Where(field => field.Initializer is not null))
            {
                string id = "$static:field_init:" + fieldIds[(field.FieldSymbolId, owner)];
                AddMethod(id, owner, Rewrite(field.Initializer!, arguments), RewriteGraph(field.ControlFlowGraph!, id, arguments));
                calls.Add(Call(id, span));
            }
            if (plan.ConstructorMethodId is { } constructor)
            {
                var callable = callables[constructor];
                if (callable.GenericTypeParameterIds is { Count: > 0 })
                    constructor = Specialize(callable, callable.GenericTypeParameterIds.Select(parameter => arguments[parameter]).ToArray());
                calls.Add(Call(constructor, span, callable.GenericTypeParameterIds is { Count: > 0 }
                    ? callable.GenericTypeParameterIds.Select(parameter => arguments[parameter]).ToArray() : null));
            }
            AddMethod(bodyId, owner, Block(calls, span), LinearGraph(bodyId, calls));
            string guardId = "$static:ensure:" + owner;
            AddMethod(guardId, owner, Block(Array.Empty<SemanticOperation>(), span), LinearGraph(guardId, Array.Empty<SemanticOperation>()));
        }
        var rewrittenBodies = source.Methods.Select(body => body with { Root = Rewrite(body.Root, emptyMap) }).ToArray();
        var rewrittenGraphs = source.ControlFlowGraphs.Select(graph => RewriteGraph(graph, graph.MethodSymbolId, emptyMap)).ToArray();
        if (failure is not null) { error = failure; return false; }
        ordinary = source with
        {
            SchemaVersion = source.StaticInitialization.BaseSchemaVersion, SemanticVersion = source.StaticInitialization.BaseSemanticVersion,
            StaticInitialization = null,
            Symbols = source.Symbols.Where(symbol => !(symbol.Kind == "field" && symbol.IsStatic && !symbol.IsConst))
                .Concat(addedSymbols).OrderBy(symbol => symbol.Id, StringComparer.Ordinal).ToArray(),
            Callables = source.Callables.Concat(addedCallables).OrderBy(callable => callable.MethodSymbolId, StringComparer.Ordinal).ToArray(),
            Methods = rewrittenBodies.Concat(addedBodies).OrderBy(body => body.MethodSymbolId, StringComparer.Ordinal).ToArray(),
            ControlFlowGraphs = rewrittenGraphs.Concat(addedGraphs).OrderBy(graph => graph.MethodSymbolId, StringComparer.Ordinal).ToArray(),
            ClassTypes = source.ClassTypes.Select(type => type with { HasStaticInitialization = false }).ToArray(),
        };
        ordinary = ordinary with { Reachability = SemanticReachability.ExpandForExecution(ordinary,
            addedCallables.Select(callable => callable.MethodSymbolId).ToArray()) };
        execution = new(fields, owners);
        execution.Attach(ordinary);
        return true;
    }

    private static SemanticOperation Call(string id, SemanticSpan span, IReadOnlyList<string>? arguments = null) => new("invocation", true, null, false, false, false, false,
        "type:void", id, arguments ?? Array.Empty<string>(), null, null, null, null, null, span, Array.Empty<SemanticOperation>(), new("static", null, false));
    private static SemanticOperation Block(IReadOnlyList<SemanticOperation> operations, SemanticSpan span) => new("block", true, null, false, false, false, false,
        null, null, Array.Empty<string>(), null, null, null, null, null, span, operations);
    private static SemanticControlFlowGraph LinearGraph(string id, IReadOnlyList<SemanticOperation> operations)
    {
        var first = new SemanticControlFlowEdge(0, 1, "fallthrough", "regular");
        var last = new SemanticControlFlowEdge(1, 2, "fallthrough", "regular");
        return new(id, 0, 2, new[] {
            new SemanticBasicBlock(0, "entry", true, "none", Array.Empty<SemanticOperation>(), null, Array.Empty<SemanticControlFlowEdge>(), new[] { first }),
            new SemanticBasicBlock(1, "block", true, "none", operations, null, new[] { first }, new[] { last }),
            new SemanticBasicBlock(2, "exit", true, "none", Array.Empty<SemanticOperation>(), null, new[] { last }, Array.Empty<SemanticControlFlowEdge>()),
        });
    }
}
