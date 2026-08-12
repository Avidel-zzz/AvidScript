using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

public static class CSharpGuestLowerer
{
    public static CSharpGuestLoweringResult Lower(
        SemanticDocument document,
        string semanticSha256,
        bool enableDataLaneFusion = true)
    {
        ArgumentNullException.ThrowIfNull(document);
        ArgumentNullException.ThrowIfNull(semanticSha256);

        List<GuestDiagnostic> diagnostics = new();
        ValidateInput(document, semanticSha256, diagnostics);
        if (diagnostics.Count != 0)
        {
            return Failure(diagnostics);
        }

        CSharpTypeLoweringResult typeResult = CSharpTypeLowerer.Lower(document);
        if (!typeResult.Succeeded)
        {
            return Failure(typeResult.Diagnostics);
        }

        Dictionary<string, GuestType> guestTypes = typeResult.Types.ToDictionary(
            type => type.Id,
            StringComparer.Ordinal);
        IReadOnlyList<GuestType> moduleTypes = typeResult.Types;
        IReadOnlySet<string>? reachableCallableIds = GetReachableCallableIds(document);
        GuestGlobal[] globals = LowerGlobals(document, guestTypes, diagnostics);
        GuestImport[] imports = LowerImports(document, reachableCallableIds, guestTypes, diagnostics);
        CSharpGuestDataPool dataPool = new(typeResult.Types);
        List<GuestFunction> functions = LowerFunctions(
            document,
            reachableCallableIds,
            guestTypes,
            dataPool,
            diagnostics).ToList();
        if (enableDataLaneFusion)
        {
            CSharpDataLaneFusionResult fusion = CSharpDataLaneFusionPass.Run(
                document,
                moduleTypes,
                imports,
                functions);
            if (!fusion.Succeeded)
            {
                foreach (GuestDiagnostic diagnostic in fusion.Diagnostics)
                {
                    Add(diagnostics, "ASCG1003", diagnostic.Message);
                }

                return Failure(diagnostics);
            }

            moduleTypes = fusion.Types;
            imports = fusion.Imports.ToArray();
            functions = fusion.Functions.ToList();
        }
        CSharpGameplayEventLoweringResult? gameplayEvents = CSharpGameplayEventLowerer.Lower(
            document,
            guestTypes,
            functions,
            diagnostics);
        if (gameplayEvents is not null)
        {
            functions.Add(gameplayEvents.Function);
        }
        CSharpDelegateEventLoweringResult? delegateEvents = CSharpDelegateEventLowerer.Lower(
            document,
            guestTypes,
            functions,
            diagnostics);
        if (delegateEvents is not null)
        {
            functions.AddRange(delegateEvents.Functions);
        }
        imports = AppendArrayCapabilityImports(
            imports,
            functions,
            guestTypes,
            diagnostics);

        List<GuestExport> exports = LowerExports(document, functions, diagnostics).ToList();
        if (gameplayEvents is not null)
        {
            exports.Add(gameplayEvents.Export);
        }
        if (delegateEvents is not null)
        {
            exports.AddRange(delegateEvents.Exports);
        }
        if (diagnostics.Count != 0)
        {
            return Failure(diagnostics);
        }

        GuestLayoutResult layout = GuestLayoutBuilder.Build(
            moduleTypes,
            globals,
            dataPool.Segments);
        if (!layout.Succeeded || layout.Layout is null)
        {
            foreach (GuestDiagnostic diagnostic in layout.Diagnostics)
            {
                Add(diagnostics, "ASCG1003", diagnostic.Message);
            }

            return Failure(diagnostics);
        }

        GuestModule module = new(
            GuestModuleValidator.CurrentSchemaVersion,
            GuestModuleValidator.CurrentIrVersion,
            $"csharp:{document.Source.SourceId}",
            "csharp",
            new GuestProvenance(
                document.Source.SourceId,
                document.Source.Sha256,
                document.Source.FrontendSha256,
                semanticSha256,
                document.SchemaVersion,
                document.SemanticVersion),
            true,
            layout.Layout,
            moduleTypes,
            imports,
            globals,
            layout.DataSegments,
            functions,
            exports,
            Array.Empty<GuestDiagnostic>());
        GuestValidationResult validation = GuestModuleValidator.Validate(module);
        if (!validation.Succeeded)
        {
            foreach (GuestDiagnostic diagnostic in validation.Diagnostics)
            {
                Add(diagnostics, "ASCG1006", $"{diagnostic.Code}: {diagnostic.Message}");
            }

            return Failure(diagnostics);
        }

        return new CSharpGuestLoweringResult(true, module, Array.Empty<GuestDiagnostic>());
    }

    private static void ValidateInput(
        SemanticDocument document,
        string semanticSha256,
        List<GuestDiagnostic> diagnostics)
    {
        if (!CSharpSemanticInputValidator.IsValid(document))
        {
            Add(diagnostics, "ASCG1001", "Semantic artifact object graph is null, duplicated, or malformed.");
            return;
        }

        if (document.SchemaVersion < 4
            || !string.Equals(document.Language, "csharp", StringComparison.Ordinal)
            || string.IsNullOrWhiteSpace(document.SemanticVersion)
            || !document.Succeeded
            || document.Diagnostics.Any(diagnostic => diagnostic.Severity == "error")
            || !IsSha256(document.Source.Sha256)
            || !IsSha256(document.Source.FrontendSha256)
            || !IsSha256(semanticSha256))
        {
            Add(diagnostics, "ASCG1001", "Semantic artifact is failed, unsupported, or has invalid provenance.");
        }

        if (document.ControlFlowGraphs
            .GroupBy(graph => graph.MethodSymbolId, StringComparer.Ordinal)
            .Any(group => group.Count() != 1))
        {
            Add(diagnostics, "ASCG1002", "Semantic artifact contains duplicate control-flow graph identities.");
        }
    }

    private static GuestGlobal[] LowerGlobals(
        SemanticDocument document,
        IReadOnlyDictionary<string, GuestType> guestTypes,
        List<GuestDiagnostic> diagnostics)
    {
        List<GuestGlobal> globals = new();
        foreach (SemanticSymbol symbol in document.Symbols
            .Where(symbol => symbol.Kind == "field" && symbol.IsStatic)
            .OrderBy(symbol => symbol.Id, StringComparer.Ordinal))
        {
            if (symbol.TypeId is null || !guestTypes.ContainsKey(symbol.TypeId))
            {
                Add(diagnostics, "ASCG1003", $"Static field '{symbol.Id}' has no Guest value type.");
                continue;
            }
            if (guestTypes[symbol.TypeId].Kind is "factory_ref" or "object_type_ref")
            {
                Add(diagnostics, "ASCG1003", $"Static field '{symbol.Id}' cannot store a nominal object capability.");
                continue;
            }

            globals.Add(new GuestGlobal(
                CSharpGuestIds.Global(symbol.Id),
                symbol.TypeId,
                true,
                new GuestConstant("zero", null)));
        }

        return globals.ToArray();
    }

    private static GuestImport[] LowerImports(
        SemanticDocument document,
        IReadOnlySet<string>? reachableCallableIds,
        IReadOnlyDictionary<string, GuestType> guestTypes,
        List<GuestDiagnostic> diagnostics)
    {
        List<GuestImport> imports = new();
        foreach (SemanticCallable callable in document.Callables
            .Where(callable => callable.Import is not null
                && (reachableCallableIds is null
                    || reachableCallableIds.Contains(callable.MethodSymbolId)))
            .OrderBy(callable => callable.MethodSymbolId, StringComparer.Ordinal))
        {
            string[] parameterTypeIds = callable.Parameters
                .OrderBy(parameter => parameter.Ordinal)
                .Select(CSharpAbiTypeMapper.ParameterType)
                .ToArray();
            if (!callable.IsStatic)
            {
                parameterTypeIds = new[] { callable.ContainingTypeId }
                    .Concat(parameterTypeIds)
                    .ToArray();
            }
            if (!guestTypes.ContainsKey(callable.ReturnTypeId)
                || parameterTypeIds.Any(typeId => !guestTypes.ContainsKey(typeId)))
            {
                Add(diagnostics, "ASCG1003", $"Import '{callable.MethodSymbolId}' has unsupported ABI types.");
                continue;
            }

            imports.Add(new GuestImport(
                CSharpGuestIds.Import(callable.MethodSymbolId),
                callable.Import!.Module,
                callable.Import.Name,
                parameterTypeIds,
                callable.ReturnTypeId,
                DispatchClass: "semantic",
                OptimizationClass: callable.Optimization?.OptimizationClass ?? "none",
                BindingOrdinal: callable.Optimization?.BindingOrdinal ?? -1));
        }

        return imports.ToArray();
    }

    private static GuestImport[] AppendArrayCapabilityImports(
        IReadOnlyList<GuestImport> imports,
        IReadOnlyList<GuestFunction> functions,
        IReadOnlyDictionary<string, GuestType> guestTypes,
        List<GuestDiagnostic> diagnostics)
    {
        HashSet<string> operations = functions
            .SelectMany(function => function.Blocks)
            .SelectMany(block => block.Instructions)
            .Select(instruction => instruction.Op)
            .Where(op => op is "array_length" or "array_load" or "array_store"
                or "array_region_load" or "array_region_store")
            .ToHashSet(StringComparer.Ordinal);
        if (operations.Count == 0)
        {
            return imports.ToArray();
        }

        if (!guestTypes.ContainsKey(CSharpGuestIds.Int32TypeId)
            || !guestTypes.ContainsKey(CSharpGuestIds.AddressTypeId))
        {
            Add(diagnostics, "ASCG1003", "Array capability intrinsics require canonical i32 and address types.");
            return imports.ToArray();
        }

        List<GuestImport> result = imports.ToList();
        bool hasRegionAccess = operations.Contains("array_region_load")
            || operations.Contains("array_region_store");
        if (operations.Contains("array_length") || hasRegionAccess)
        {
            result.Add(new GuestImport(
                GuestArrayCapabilityIntrinsics.LengthImportId,
                GuestArrayCapabilityIntrinsics.Module,
                GuestArrayCapabilityIntrinsics.LengthImportName,
                new[] { CSharpGuestIds.Int32TypeId },
                CSharpGuestIds.Int32TypeId));
        }

        string[] accessParameters =
        {
            CSharpGuestIds.Int32TypeId,
            CSharpGuestIds.Int32TypeId,
            CSharpGuestIds.AddressTypeId,
            CSharpGuestIds.Int32TypeId,
        };
        if (operations.Contains("array_load"))
        {
            result.Add(new GuestImport(
                GuestArrayCapabilityIntrinsics.LoadImportId,
                GuestArrayCapabilityIntrinsics.Module,
                GuestArrayCapabilityIntrinsics.LoadImportName,
                accessParameters,
                CSharpGuestIds.Int32TypeId));
        }

        if (operations.Contains("array_store"))
        {
            result.Add(new GuestImport(
                GuestArrayCapabilityIntrinsics.StoreImportId,
                GuestArrayCapabilityIntrinsics.Module,
                GuestArrayCapabilityIntrinsics.StoreImportName,
                accessParameters,
                CSharpGuestIds.Int32TypeId));
        }

        if (hasRegionAccess)
        {
            result.Add(new GuestImport(
                GuestArrayCapabilityIntrinsics.ReadRangeImportId,
                GuestArrayCapabilityIntrinsics.Module,
                GuestArrayCapabilityIntrinsics.ReadRangeImportName,
                new[]
                {
                    CSharpGuestIds.Int32TypeId,
                    CSharpGuestIds.Int32TypeId,
                    CSharpGuestIds.AddressTypeId,
                    CSharpGuestIds.Int32TypeId,
                    CSharpGuestIds.Int32TypeId,
                },
                CSharpGuestIds.Int32TypeId));
        }

        if (operations.Contains("array_region_store"))
        {
            result.Add(new GuestImport(
                GuestArrayCapabilityIntrinsics.WriteRangeImportId,
                GuestArrayCapabilityIntrinsics.Module,
                GuestArrayCapabilityIntrinsics.WriteRangeImportName,
                new[]
                {
                    CSharpGuestIds.Int32TypeId,
                    CSharpGuestIds.Int32TypeId,
                    CSharpGuestIds.AddressTypeId,
                    CSharpGuestIds.Int32TypeId,
                    CSharpGuestIds.Int32TypeId,
                },
                CSharpGuestIds.Int32TypeId));
        }

        return result.ToArray();
    }

    private static GuestFunction[] LowerFunctions(
        SemanticDocument document,
        IReadOnlySet<string>? reachableCallableIds,
        IReadOnlyDictionary<string, GuestType> guestTypes,
        CSharpGuestDataPool dataPool,
        List<GuestDiagnostic> diagnostics)
    {
        Dictionary<string, SemanticControlFlowGraph> graphs = document.ControlFlowGraphs
            .GroupBy(graph => graph.MethodSymbolId, StringComparer.Ordinal)
            .ToDictionary(group => group.Key, group => group.First(), StringComparer.Ordinal);
        List<GuestFunction> functions = new();
        foreach (SemanticCallable callable in document.Callables
            .Where(callable => callable.HasBody
                && !CSharpClassReferencePolicy.IsIntrinsicConstructor(callable)
                && !CSharpClassReferencePolicy.IsIntrinsicUpcast(document, callable)
                && !CSharpObjectCapabilityPolicy.IsIntrinsicConstructor(callable)
                && (reachableCallableIds is null
                    || reachableCallableIds.Contains(callable.MethodSymbolId)))
            .OrderBy(callable => callable.MethodSymbolId, StringComparer.Ordinal))
        {
            if (!graphs.TryGetValue(callable.MethodSymbolId, out SemanticControlFlowGraph? graph))
            {
                Add(diagnostics, "ASCG1002", $"Callable '{callable.MethodSymbolId}' has no control-flow graph.");
                continue;
            }

            GuestFunction? function = CSharpControlFlowLowerer.Lower(
                document, callable, graph, guestTypes, dataPool, diagnostics);
            if (function is not null)
            {
                functions.Add(function);
            }
        }

        return functions.ToArray();
    }

    private static GuestExport[] LowerExports(
        SemanticDocument document,
        IReadOnlyList<GuestFunction> functions,
        List<GuestDiagnostic> diagnostics)
    {
        HashSet<string> functionIds = functions.Select(function => function.Id).ToHashSet(StringComparer.Ordinal);
        List<GuestExport> exports = new();
        foreach (SemanticCallable callable in document.Callables
            .Where(callable => callable.Export is not null)
            .OrderBy(callable => callable.Export!.Name, StringComparer.Ordinal))
        {
            string functionId = CSharpGuestIds.Function(callable.MethodSymbolId);
            if (!functionIds.Contains(functionId))
            {
                Add(diagnostics, "ASCG1002", $"Export '{callable.Export!.Name}' has no lowered function.");
                continue;
            }

            exports.Add(new GuestExport(callable.Export!.Name, functionId));
        }

        return exports.ToArray();
    }

    private static IReadOnlySet<string>? GetReachableCallableIds(SemanticDocument document)
    {
        if (document.SchemaVersion < 5)
        {
            return null;
        }

        return document.Reachability!.ReachableCallableIds.ToHashSet(StringComparer.Ordinal);
    }

    private static CSharpGuestLoweringResult Failure(IEnumerable<GuestDiagnostic> diagnostics)
    {
        GuestDiagnostic[] ordered = diagnostics
            .OrderBy(diagnostic => diagnostic.Code, StringComparer.Ordinal)
            .ThenBy(diagnostic => diagnostic.Message, StringComparer.Ordinal)
            .ToArray();
        return new CSharpGuestLoweringResult(false, null, ordered);
    }

    private static void Add(List<GuestDiagnostic> diagnostics, string code, string message)
    {
        diagnostics.Add(new GuestDiagnostic(code, "error", message, null));
    }

    private static bool IsSha256(string value)
    {
        return value.Length == 64 && value.All(character =>
            (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'));
    }
}
