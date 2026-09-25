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
        bool enableDataLaneFusion = true,
        bool enableDebugInstrumentation = false,
        bool enableAsyncLanguageErrors = false) =>
        LowerCore(document, semanticSha256, enableDataLaneFusion,
            enableDebugInstrumentation, Array.Empty<GuestFunction>(), false,
            enableAsyncLanguageErrors);

    internal static CSharpGuestLoweringResult LowerWithFunctionSubstitutes(
        SemanticDocument document,
        string semanticSha256,
        IReadOnlyList<GuestFunction> substitutes,
        bool includeLanguageExceptionReference = false) =>
        LowerCore(document, semanticSha256, enableDataLaneFusion: true,
            enableDebugInstrumentation: false, substitutes,
            includeLanguageExceptionReference, enableAsyncLanguageErrors: false);

    private static CSharpGuestLoweringResult LowerCore(
        SemanticDocument document,
        string semanticSha256,
        bool enableDataLaneFusion,
        bool enableDebugInstrumentation,
        IReadOnlyList<GuestFunction> substitutes,
        bool includeLanguageExceptionReference,
        bool enableAsyncLanguageErrors)
    {
        ArgumentNullException.ThrowIfNull(document);
        ArgumentNullException.ThrowIfNull(semanticSha256);
        ArgumentNullException.ThrowIfNull(substitutes);

        List<GuestDiagnostic> diagnostics = new();
        bool directCleanup = document.SchemaVersion == SemanticContract.DirectAwaitCleanupSchemaVersion
            && document.SemanticVersion == SemanticContract.DirectAwaitCleanupSemanticVersion;
        bool asyncExceptionFlow = enableAsyncLanguageErrors
            && (document.SchemaVersion == SemanticContract.AsyncExceptionFlowSchemaVersion
                && document.SemanticVersion == SemanticContract.AsyncExceptionFlowSemanticVersion
                || directCleanup);
        bool asyncLanguageErrors = enableAsyncLanguageErrors && (
            asyncExceptionFlow && (!directCleanup || document.AsyncMethods.Any(method =>
                method.ErrorPlan is not null || method.Segments.Any(segment =>
                    segment.AwaitSite?.ProducerKind is "task_call" or "task_local")))
            || document.SchemaVersion == SemanticContract.AsyncLanguageErrorSchemaVersion
                && document.SemanticVersion == SemanticContract.AsyncLanguageErrorSemanticVersion);
        ValidateInput(document, semanticSha256, asyncLanguageErrors, diagnostics);
        if (directCleanup && !enableAsyncLanguageErrors)
            Add(diagnostics, "ASCG1004", "Direct await cleanup requires bounded language-error mode.");
        if (asyncLanguageErrors && document.ExceptionFlows is not null)
            Add(diagnostics, "ASCG1004", "Async Task faults cannot yet share a module with synchronous language-error methods.");
        if (diagnostics.Count == 0 && enableDebugInstrumentation && CSharpClosureLayout.UsesManagedDelegates(document))
            Add(diagnostics, "ASCG1024", "Debug pause frames require persistent managed roots before captured closures or delegate lists can be instrumented.");
        if (diagnostics.Count != 0)
        {
            return Failure(diagnostics);
        }

        document = CSharpUeDispatch.ExpandReachability(document);
        CSharpTypeLoweringResult typeResult = CSharpTypeLowerer.Lower(document);
        if (!typeResult.Succeeded)
        {
            return Failure(typeResult.Diagnostics);
        }

        IReadOnlyList<GuestType> moduleTypes = typeResult.Types;
        if (asyncLanguageErrors)
        {
            if (moduleTypes.Any(type => type.Id is "type:language_error_payload"
                    or "type:language_error_root"))
                return Failure(new[] { new GuestDiagnostic("ASCG1003", "error",
                    "The async language-error root type identity is already occupied.", null) });
            GuestTypeLayoutResult errorTypes = GuestDataLayout.ComputeTypes(moduleTypes.Concat(new[]
            {
                new GuestType("type:language_error_payload", "struct", "memory",
                    new[] { new GuestField("field:code", "code", "type:int32", 0) },
                    null, null, 0, 1),
                new GuestType("type:language_error_root", "managed_ref", "i64",
                    Array.Empty<GuestField>(), "type:language_error_payload", null, 8, 8),
            }).ToArray());
            if (!errorTypes.Succeeded)
                return Failure(new[] { new GuestDiagnostic("ASCG1003", "error",
                    "The async language-error root has an invalid layout.", null) });
            moduleTypes = errorTypes.Types;
        }
        if (includeLanguageExceptionReference)
        {
            if (!document.Types.Any(type => type.Id == CSharpThrowProducerLowerer.ExceptionTypeId
                    && type.Kind == "class" && !type.IsValueType)
                || moduleTypes.Any(type => type.Id == CSharpThrowProducerLowerer.ExceptionTypeId))
                return Failure(new[] { new GuestDiagnostic("ASCG1003", "error",
                    "The language exception reference has no unique class type.", null) });
            moduleTypes = moduleTypes.Append(new GuestType(
                CSharpThrowProducerLowerer.ExceptionTypeId, "managed_ref", "i64",
                Array.Empty<GuestField>(), null, null, 8, 8)).ToArray();
            if (!moduleTypes.Any(type => type.Id == "type:object"))
                moduleTypes = moduleTypes.Append(new GuestType(
                    "type:object", "managed_ref", "i64",
                    Array.Empty<GuestField>(), null, null, 8, 8)).ToArray();
        }
        Dictionary<string, GuestType> guestTypes = moduleTypes.ToDictionary(
            type => type.Id, StringComparer.Ordinal);
        foreach (SemanticCallable root in document.Callables.Where(callable =>
            document.Reachability?.RootCallableIds.Contains(callable.MethodSymbolId) == true || callable.Export is not null))
        {
            if (CSharpManagedDelegateLowerer.ContainsReference(root.ReturnTypeId, guestTypes)
                || root.Parameters.Any(parameter => CSharpManagedDelegateLowerer.ContainsReference(parameter.TypeId, guestTypes)))
                Add(diagnostics, "ASCG1024", $"Entrypoint '{root.MethodSymbolId}' cannot expose module-local delegate references to the host.");
        }
        IReadOnlySet<string>? reachableCallableIds = GetReachableCallableIds(document);
        foreach (SemanticUeMethodEntry method in document.UeMethodCatalog?.Methods ?? Array.Empty<SemanticUeMethodEntry>())
            if (method.HasGuestBody && method.ReturnRefKind != "none"
                && (reachableCallableIds is null || reachableCallableIds.Contains(method.MethodSymbolId)))
                Add(diagnostics, "ASCG1024", $"UE method '{method.MethodSymbolId}' requires a borrowed-return execution contract; lowering it as a value return would lose alias semantics.");
        if (diagnostics.Count != 0) return Failure(diagnostics);
        GuestGlobal[] globals = LowerGlobals(document, guestTypes, diagnostics);
        GuestImport[] imports = LowerImports(document, reachableCallableIds, guestTypes, diagnostics);
        CSharpGuestDataPool dataPool = new(moduleTypes);
        List<GuestFunction> functions = LowerFunctions(
            document,
            reachableCallableIds,
            guestTypes,
            dataPool,
            substitutes.Select(function => function.Id).ToHashSet(StringComparer.Ordinal),
            diagnostics).ToList();
        functions.AddRange(substitutes);
        CSharpReferenceObjects.AddGuards(document, functions);
        CSharpUeReceivers.AddGuards(document, functions);
        functions.AddRange(CSharpUeDelegateBinding.Build(document));
        functions.AddRange(CSharpDelegateComposition.Build(document));
        functions.AddRange(CSharpEventSubscriptions.Build(document));
        imports = imports.Concat(CSharpEventSubscriptions.Imports(document)).ToArray();
        if (CSharpClosureLayout.UsesManagedDelegates(document))
            imports = imports.Append(new GuestImport(CSharpClosureLayout.HeapImport, GuestManagedHeap.ImportModule, GuestManagedHeap.ImportName,
                Enumerable.Repeat(CSharpGuestIds.AddressTypeId, 4).ToArray(), CSharpGuestIds.AddressTypeId)).ToArray();
        if ((includeLanguageExceptionReference || asyncLanguageErrors) && !imports.Any(import =>
                import.Module == GuestManagedHeap.ImportModule
                && import.Name == GuestManagedHeap.ImportName))
            imports = imports.Append(new GuestImport("import:language_error_heap",
                GuestManagedHeap.ImportModule, GuestManagedHeap.ImportName,
                Enumerable.Repeat("type:int32", 4).ToArray(), "type:int32")).ToArray();
        CSharpAsyncLoweringResult asyncMethods = CSharpAsyncLowerer.Lower(
            document,
            guestTypes,
            dataPool,
            diagnostics);
        functions.AddRange(asyncMethods.Functions);
        if (CSharpTaskResultAbi.Supports(document))
            imports = imports.Append(CSharpTaskResultAbi.Import())
                .Append(CSharpTaskResultAbi.BindProducerImport())
                .Append(CSharpTaskResultAbi.PropagateFailureImport()).ToArray();
        if (asyncLanguageErrors)
            imports = imports.Append(CSharpTaskResultAbi.RetainForContinuationImport())
                .Append(CSharpTaskResultAbi.FaultLanguageErrorImport())
                .Append(CSharpTaskResultAbi.LanguageErrorMetaImport())
                .Append(CSharpTaskResultAbi.LanguageErrorRootImport())
                .Append(CSharpTaskResultAbi.LanguageErrorReportImport()).ToArray();
        else if (directCleanup)
            imports = imports.Append(CSharpTaskResultAbi.RetainForContinuationImport()).ToArray();
        if (document.SchemaVersion is SemanticContract.TaskLocalSchemaVersion
            or SemanticContract.TaskAssignmentSchemaVersion
            or SemanticContract.TaskExistingLocalSchemaVersion
            or SemanticContract.TaskAliasSchemaVersion)
            imports = imports.Append(CSharpTaskResultAbi.RetainForContinuationImport()).ToArray();
        functions.AddRange(CSharpClosureDelegateLowerer.BuildThunks(document, functions));
        functions.AddRange(CSharpDelegateIdentityLowerer.Build(document, functions));
        imports = CSharpAsyncManagedState.AppendImports(imports, functions);
        imports = CSharpUeReceivers.AppendImports(document, imports, functions);
        if (functions.SelectMany(function => function.Blocks).SelectMany(block => block.Instructions)
            .Any(instruction => instruction.Op == "call" && instruction.TargetId == CSharpUeDelegateBinding.ImportId))
            imports = imports.Append(new GuestImport(CSharpUeDelegateBinding.ImportId, "avidscript", CSharpUeDelegateBinding.ImportName,
                new[] { "type:uint64" }, "type:int32")).ToArray();
        imports = AppendUePropertyImports(
            document,
            imports,
            functions,
            guestTypes,
            diagnostics);
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
            imports = imports.Concat(delegateEvents.Imports).ToArray();
        }
        CSharpContinuationLoweringResult? continuations = CSharpContinuationLowerer.Lower(
            document,
            guestTypes,
            functions,
            asyncMethods.ResumeRoutes,
            diagnostics);
        if (continuations is not null)
        {
            functions.Add(continuations.Function);
        }
        imports = AppendArrayCapabilityImports(
            imports,
            functions,
            guestTypes,
            diagnostics);

        List<GuestExport> exports = LowerExports(document, functions, diagnostics).ToList();
        CSharpUeLifecycleCompatibilityLoweringResult? ueLifecycle =
            CSharpUeLifecycleCompatibilityLowerer.Lower(document, exports, diagnostics);
        if (ueLifecycle is not null)
        {
            functions.AddRange(ueLifecycle.Functions);
            exports.AddRange(ueLifecycle.Exports);
        }
        if (gameplayEvents is not null)
        {
            exports.Add(gameplayEvents.Export);
        }
        if (delegateEvents is not null)
        {
            exports.AddRange(delegateEvents.Exports);
        }
        if (continuations is not null)
        {
            exports.Add(continuations.Export);
        }
        if (diagnostics.Count != 0)
        {
            return Failure(diagnostics);
        }

        List<GuestImport> methodImports = imports.ToList();
        IReadOnlyList<GuestFramedExport> framedExports = CSharpUeMethodFrames.Lower(document, functions, methodImports, diagnostics);
        imports = methodImports.ToArray();
        if (diagnostics.Count != 0) return Failure(diagnostics);

        if (enableDebugInstrumentation)
        {
            CSharpGuestDebugInstrumentationResult instrumentation =
                CSharpGuestDebugInstrumenter.Instrument(
                    $"csharp:{document.Source.SourceId}",
                    moduleTypes,
                    imports,
                    functions,
                    exports,
                    GetDebugResumableFunctionIds(document, functions));
            moduleTypes = instrumentation.Types;
            imports = instrumentation.Imports.ToArray();
            functions = instrumentation.Functions.ToList();
            exports = instrumentation.Exports.ToList();
            diagnostics.AddRange(instrumentation.Diagnostics);
            if (diagnostics.Count != 0)
            {
                return Failure(diagnostics);
            }
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
            directCleanup ? GuestTaskLanguageErrorValidator.DirectCleanupSchemaVersion
                : asyncExceptionFlow ? GuestTaskLanguageErrorValidator.ExceptionFlowSchemaVersion
                : asyncLanguageErrors ? GuestTaskLanguageErrorValidator.AsyncSchemaVersion
                : document.SchemaVersion is SemanticContract.TaskLocalSchemaVersion
                or SemanticContract.TaskAssignmentSchemaVersion
                or SemanticContract.TaskExistingLocalSchemaVersion
                or SemanticContract.TaskAliasSchemaVersion
                ? 19 : CSharpTaskResultAbi.Supports(document)
                    ? 18 : GuestModuleValidator.CurrentSchemaVersion,
            directCleanup ? GuestTaskLanguageErrorValidator.DirectCleanupIrVersion
                : asyncExceptionFlow ? GuestTaskLanguageErrorValidator.ExceptionFlowIrVersion
                : asyncLanguageErrors ? GuestTaskLanguageErrorValidator.AsyncIrVersion
                : document.SchemaVersion is SemanticContract.TaskLocalSchemaVersion
                or SemanticContract.TaskAssignmentSchemaVersion
                or SemanticContract.TaskExistingLocalSchemaVersion
                or SemanticContract.TaskAliasSchemaVersion
                ? "1.18" : CSharpTaskResultAbi.Supports(document)
                    ? "1.17" : GuestModuleValidator.CurrentIrVersion,
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
            Array.Empty<GuestDiagnostic>())
        {
            FunctionReferences = CSharpManagedDelegateLowerer.BuildContracts(document, moduleTypes, functions),
            FramedExports = framedExports,
            LanguageErrorCatalog = asyncLanguageErrors
                ? CSharpAsyncLanguageErrorCatalog.ToGuest(document,
                    CSharpAsyncLanguageErrorCatalog.Build(document)) : null,
            AsyncExceptionRoutes = asyncExceptionFlow
                ? document.AsyncMethods.Where(method => method.ExceptionPlan is not null)
                    .SelectMany(method => method.Segments
                        .Where(segment => segment.AwaitSite is not null
                            && segment.Transfer?.SecondaryTarget is >= 0)
                        .Select(segment => new GuestAsyncExceptionRoute(
                            CSharpGuestIds.Function(method.MethodSymbolId),
                            segment.AwaitSite!.CallbackId,
                            CSharpGuestIds.AsyncSegmentBlock(method.MethodSymbolId,
                                segment.Ordinal),
                            CSharpGuestIds.AsyncSegmentBlock(method.MethodSymbolId,
                                segment.Transfer!.PrimaryTarget),
                            CSharpGuestIds.AsyncSegmentBlock(method.MethodSymbolId,
                                segment.Transfer.SecondaryTarget),
                            CSharpGuestIds.AsyncSegmentBlock(method.MethodSymbolId,
                                segment.Transfer.CancellationTarget!.Value),
                            CSharpGuestIds.Local(
                                CSharpTaskResultAbi.ExceptionSourceSlot(method)),
                            CSharpGuestIds.Local(
                                CSharpTaskResultAbi.ExceptionTypeSlot(method)))))
                    .OrderBy(route => route.CallbackId).ToArray() : null,
            DirectAwaitRoutes = directCleanup
                ? document.AsyncMethods.Where(method => method.ExceptionPlan is not null)
                    .SelectMany(method => method.Segments
                        .Where(segment => segment.AwaitSite?.ProducerKind is "delay" or "next_tick"
                            && segment.Transfer?.CancellationTarget is >= 0)
                        .Select(segment => new GuestDirectAwaitRoute(
                            CSharpGuestIds.Function(method.MethodSymbolId),
                            segment.AwaitSite!.CallbackId,
                            segment.AwaitSite.ProducerKind,
                            CSharpGuestIds.AsyncSegmentBlock(method.MethodSymbolId,
                                segment.Ordinal),
                            CSharpGuestIds.AsyncSegmentBlock(method.MethodSymbolId,
                                segment.Transfer!.PrimaryTarget),
                            CSharpGuestIds.AsyncSegmentBlock(method.MethodSymbolId,
                                segment.Transfer.CancellationTarget!.Value),
                            CSharpGuestIds.Import(document.Callables.Single(callable =>
                                callable.Import is { Module: "avidscript",
                                    Name: "avid_continuation_delay_cancel_resume_v1" })
                                .MethodSymbolId))))
                    .OrderBy(route => route.CallbackId).ToArray() : null,
        };
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
        bool asyncLanguageErrors,
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
            || !document.Succeeded && !asyncLanguageErrors
            || document.Diagnostics.Any(diagnostic => diagnostic.Severity == "error"
                && !(asyncLanguageErrors && diagnostic.Code == "ASCS5422"))
            || asyncLanguageErrors && !SemanticAsyncErrorPlanValidator.IsValid(document)
            || !IsSha256(document.Source.Sha256)
            || !IsSha256(document.Source.FrontendSha256)
            || !IsSha256(semanticSha256))
        {
            Add(diagnostics, "ASCG1001", "Semantic artifact is failed, unsupported, or has invalid provenance.");
        }

        if (document.ClosureEnvironments.Any(environment => (environment.Allocation is null
                && !document.AsyncMethods.Any(method => method.MethodSymbolId == environment.OwnerMethodSymbolId
                    && method.Lowering == SemanticAsyncMethod.ContinuationCfgLowering
                    && method.LexicalScopes.Any(scope => scope.Id == environment.Id)))
            || environment.Cells.Any(cell => cell.Kind == "receiver" && !CSharpReferenceObjects.Types(document).Contains(cell.TypeId)
                && !CSharpUeReceivers.IsType(document, cell.TypeId))))
            Add(diagnostics, "ASCG1024", "Closure execution requires validated synchronous or resumable scope allocation metadata and supported source reference or UE receivers.");

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
            if (guestTypes[symbol.TypeId].Kind is "factory_ref" or "object_type_ref" or "composite_ref"
                || CSharpManagedDelegateLowerer.ContainsReference(symbol.TypeId, guestTypes))
            {
                Add(diagnostics, "ASCG1003", $"Static field '{symbol.Id}' cannot store a session-bound capability.");
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
                    || reachableCallableIds.Contains(callable.MethodSymbolId)
                    || IsRequiredAsyncImport(document, callable)))
            .OrderBy(callable => callable.MethodSymbolId, StringComparer.Ordinal))
        {
            string[] parameterTypeIds = callable.Parameters
                .OrderBy(parameter => parameter.Ordinal)
                .Select(CSharpAbiTypeMapper.ParameterType)
                .ToArray();
            if (callable.Import is { Module: "env", Name: "continuation_result_read" }
                && parameterTypeIds.Length == 5)
            {
                parameterTypeIds[3] = CSharpGuestIds.AddressTypeId;
            }
            if (callable.Import is
                    { Module: "env", Name: "continuation_state_store" or "continuation_state_read" }
                && parameterTypeIds.Length == 3)
            {
                parameterTypeIds[1] = CSharpGuestIds.AddressTypeId;
            }
            if (!callable.IsStatic)
            {
                parameterTypeIds = new[] { callable.ContainingTypeId }
                    .Concat(parameterTypeIds)
                    .ToArray();
            }
            if (CSharpManagedDelegateLowerer.ContainsReference(callable.ReturnTypeId, guestTypes)
                || (!callable.IsStatic && CSharpManagedDelegateLowerer.ContainsReference(callable.ContainingTypeId, guestTypes))
                || callable.Parameters.Any(parameter => CSharpManagedDelegateLowerer.ContainsReference(parameter.TypeId, guestTypes))
                || !guestTypes.ContainsKey(callable.ReturnTypeId)
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

    private static GuestImport[] AppendUePropertyImports(
        SemanticDocument document,
        IReadOnlyList<GuestImport> imports,
        IReadOnlyList<GuestFunction> functions,
        IReadOnlyDictionary<string, GuestType> guestTypes,
        List<GuestDiagnostic> diagnostics)
    {
        HashSet<string> calledTargets = functions
            .SelectMany(function => function.Blocks)
            .SelectMany(block => block.Instructions)
            .Where(instruction => instruction.Op == "call" && instruction.TargetId is not null)
            .Select(instruction => instruction.TargetId!)
            .ToHashSet(StringComparer.Ordinal);
        if (calledTargets.Count == 0)
        {
            return imports.ToArray();
        }

        List<GuestImport> result = imports.ToList();
        foreach (CSharpUePropertyAccessPlan plan in CSharpUePropertyAccessPlan.Build(document)
            .Values
            .OrderBy(plan => plan.RuntimePlan.TypeOrdinal)
            .ThenBy(plan => plan.RuntimePlan.MemberOrdinal))
        {
            Append(plan.Getter);
            Append(plan.Setter);
        }
        return result.ToArray();

        void Append(SemanticCallable? callable)
        {
            if (callable is null)
            {
                return;
            }
            string importId = CSharpGuestIds.Import(callable.MethodSymbolId);
            if (!calledTargets.Contains(importId))
            {
                return;
            }

            string[] parameterTypeIds = new[] { callable.ContainingTypeId }
                .Concat(callable.Parameters
                    .OrderBy(parameter => parameter.Ordinal)
                    .Select(CSharpAbiTypeMapper.ParameterType))
                .ToArray();
            if (!guestTypes.ContainsKey(callable.ReturnTypeId)
                || parameterTypeIds.Any(typeId => !guestTypes.ContainsKey(typeId)))
            {
                Add(
                    diagnostics,
                    "ASCG1003",
                    $"UE property import '{callable.MethodSymbolId}' has unsupported ABI types.");
                return;
            }

            result.Add(new GuestImport(
                importId,
                callable.Import!.Module,
                callable.Import.Name,
                parameterTypeIds,
                callable.ReturnTypeId));
        }
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
        IReadOnlySet<string> substitutedFunctionIds,
        List<GuestDiagnostic> diagnostics)
    {
        Dictionary<string, SemanticControlFlowGraph> graphs = document.ControlFlowGraphs
            .GroupBy(graph => graph.MethodSymbolId, StringComparer.Ordinal)
            .ToDictionary(group => group.Key, group => group.First(), StringComparer.Ordinal);
        List<GuestFunction> functions = new();
        foreach (SemanticCallable callable in document.Callables
            .Where(callable => callable.HasBody
                && !substitutedFunctionIds.Contains(CSharpGuestIds.Function(callable.MethodSymbolId))
                && !CSharpClassReferencePolicy.IsIntrinsicConstructor(callable)
                && !CSharpClassReferencePolicy.IsIntrinsicUpcast(document, callable)
                && !CSharpObjectCapabilityPolicy.IsIntrinsicConstructor(callable)
                && !CSharpCompositeValueCapabilityPolicy.IsIntrinsicConstructor(callable)
                && !document.AsyncMethods.Any(method =>
                    method.MethodSymbolId == callable.MethodSymbolId)
                && !IsAsyncFacadeIntrinsic(document, callable)
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

    private static bool IsRequiredAsyncImport(
        SemanticDocument document,
        SemanticCallable callable)
    {
		if (document.AsyncMethods.Count == 0 || callable.Import is not { } import)
		{
			return false;
		}
	if (import.Module == "env" && import.Name == "continuation_delay")
	{
		return document.AsyncMethods
			.SelectMany(method => method.Segments)
			.Any(segment => segment.AwaitSite?.ProducerKind is "delay" or "next_tick"
				&& segment.Transfer?.CancellationTarget is null);
	}
	if (import.Module == "avidscript"
		&& import.Name == "avid_continuation_delay_cancel_resume_v1")
	{
		return document.AsyncMethods
			.SelectMany(method => method.Segments)
			.Any(segment => segment.AwaitSite?.ProducerKind is "delay" or "next_tick"
				&& segment.Transfer?.CancellationTarget is >= 0);
	}
		if (import.Module == "env" && import.Name == "continuation_load_object")
		{
			return document.AsyncMethods
				.SelectMany(method => method.Segments)
				.Any(segment => segment.AwaitSite?.ProducerKind == "object_load");
		}
		if (import.Module == "env" && import.Name == "continuation_bind_cancel")
		{
			return document.AsyncMethods
				.SelectMany(method => method.Segments)
				.Any(segment => segment.AwaitSite?.CancellationToken is not null);
		}
		if (import.Module == "env" && import.Name == "continuation_result_read")
		{
			return document.AsyncMethods
				.SelectMany(method => method.Segments)
				.Any(segment => segment.AwaitSite?.PayloadKind
					== SemanticContinuationCallback.ResultSlotPayloadKind);
		}
		if (import.Module == "env"
			&& import.Name is "continuation_state_store" or "continuation_state_read")
		{
			return CSharpAsyncClosureState.Frames(document).Any();
		}
		if (import.Module == "env" && import.Name == "continuation_cancel")
		{
			return CSharpAsyncClosureState.Frames(document).Any();
		}
		string producerIdentity = $"binding_latent|{import.Module}|{import.Name}";
		return document.AsyncMethods
			.SelectMany(method => method.Segments)
			.Any(segment => segment.AwaitSite?.ProducerKind == producerIdentity);
    }

    private static bool IsAsyncFacadeIntrinsic(
        SemanticDocument document,
        SemanticCallable callable)
    {
        if (document.AsyncMethods.Count == 0)
        {
            return false;
        }

        bool builtInContinuationFacade = callable.ContainingTypeId
                == "type:global::AvidScript.AvidContinuations"
            && ((callable.MethodSymbolId.Contains(".DelayAsync(", StringComparison.Ordinal)
                    && callable.Parameters.Count == 1)
                || (callable.MethodSymbolId.Contains(".NextTickAsync(", StringComparison.Ordinal)
                    && callable.Parameters.Count == 0));
        bool builtInObjectFacade = callable.ContainingTypeId
                == "type:global::AvidScript.AvidAssets"
            && callable.MethodSymbolId.Contains(".LoadObjectAsync(", StringComparison.Ordinal)
            && callable.Parameters.Count == 1;
        bool generatedLatentFacade = callable.IsStatic
            && callable.Import is null
            && callable.Export is null
            && (callable.ReturnTypeId == "type:global::AvidScript.AvidDelayAwaitable"
                || callable.ReturnTypeId.StartsWith(
                    "type:global::AvidScript.AvidOutcomeAwaitable<",
                    StringComparison.Ordinal))
            && callable.MethodSymbolId.Contains("Async(", StringComparison.Ordinal)
            && HasMatchingGeneratedLatentProducer(document, callable);
        bool cancellationMarker = callable.MethodSymbolId.Contains(
                ".WithCancellation(",
                StringComparison.Ordinal)
            && callable.Parameters.Count == 1
            && callable.Parameters[0].TypeId
                == "type:global::AvidScript.AvidCancellationToken"
            && (callable.ContainingTypeId is
                    "type:global::AvidScript.AvidDelayAwaitable" or
                    "type:global::AvidScript.AvidObjectAwaitable"
                || callable.ContainingTypeId.StartsWith(
                    "type:global::AvidScript.AvidOutcomeAwaitable<",
                    StringComparison.Ordinal));
        bool generatedOutcomeScaffold = callable.ContainingTypeId is
            "type:global::AvidScript.AvidOutcome<T>" or
            "type:global::AvidScript.AvidOutcomeAwaitable<T>" or
            "type:global::AvidScript.AvidOutcomeAwaiter<T>";
        return builtInContinuationFacade
            || builtInObjectFacade
            || generatedLatentFacade
            || cancellationMarker
            || generatedOutcomeScaffold;
    }

    private static bool HasMatchingGeneratedLatentProducer(
        SemanticDocument document,
        SemanticCallable facade)
    {
        foreach (SemanticAsyncAwaitSite awaitSite in document.AsyncMethods
            .SelectMany(method => method.Segments)
            .Where(segment => segment.AwaitSite is not null)
            .Select(segment => segment.AwaitSite!))
        {
            bool providerFacade = facade.ReturnTypeId.StartsWith(
                "type:global::AvidScript.AvidOutcomeAwaitable<",
                StringComparison.Ordinal);
            if (!awaitSite.ProducerKind.StartsWith("binding_latent|", StringComparison.Ordinal)
                || providerFacade != (awaitSite.PayloadKind
                    == SemanticContinuationCallback.ResultSlotPayloadKind)
                || !facade.Parameters.Select(parameter => parameter.TypeId)
                    .SequenceEqual(awaitSite.Arguments.Select(argument => argument.TypeId)))
            {
                continue;
            }

            string[] identity = awaitSite.ProducerKind.Split('|');
            if (identity.Length == 3
                && document.Callables.Any(candidate =>
                    candidate.Import is { } import
                    && import.Module == identity[1]
                    && import.Name == identity[2]
                    && candidate.ReturnTypeId == "type:int64"
                    && candidate.Parameters.Count == facade.Parameters.Count + 1
                    && candidate.Parameters[^1].TypeId == "type:int32"))
            {
                return true;
            }
        }
        return false;
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

        HashSet<string> exportNames = exports
            .Select(export => export.Name)
            .ToHashSet(StringComparer.Ordinal);
        foreach (SemanticUeFunctionDeclaration function in document.UeTypeDeclarations
            .SelectMany(type => type.Functions)
            .Where(function => !function.Flags.Contains("blueprint_implementable_event"))
            .OrderBy(function => function.MethodSymbolId, StringComparer.Ordinal))
        {
            string functionId = CSharpGuestIds.Function(function.MethodSymbolId);
            string exportName = SemanticUeTypeRuntimeContract.GetFunctionExportName(function.MethodSymbolId);
            if (!functionIds.Contains(functionId))
            {
                Add(diagnostics, "ASCG1002", $"UE function export '{exportName}' has no lowered function.");
                continue;
            }
            if (!exportNames.Add(exportName))
            {
                Add(diagnostics, "ASCG1003", $"UE function export '{exportName}' is duplicated.");
                continue;
            }
            exports.Add(new GuestExport(exportName, functionId));
        }

        return exports.OrderBy(export => export.Name, StringComparer.Ordinal).ToArray();
    }

    private static IReadOnlySet<string>? GetReachableCallableIds(SemanticDocument document)
    {
        if (document.SchemaVersion < 5)
        {
            return null;
        }

        return document.Reachability!.ReachableCallableIds.ToHashSet(StringComparer.Ordinal);
    }

    private static IReadOnlySet<string> GetDebugResumableFunctionIds(
        SemanticDocument document,
        IReadOnlyList<GuestFunction> functions)
    {
        HashSet<string> asyncMethodIds = document.AsyncMethods
            .Select(method => method.MethodSymbolId)
            .ToHashSet(StringComparer.Ordinal);
        HashSet<string> guestCallTargets = functions
            .SelectMany(function => function.Blocks)
            .SelectMany(block => block.Instructions)
            .Where(instruction => (instruction.Op is "call" or "function_ref") && instruction.TargetId is not null)
            .Select(instruction => instruction.TargetId!)
            .ToHashSet(StringComparer.Ordinal);
        return document.Callables
            .Where(callable => callable.Export is not null
                && !string.Equals(
                    callable.Export.Name,
                    CSharpGuestIds.UeEndPlayCompatibilityExportName,
                    StringComparison.Ordinal)
                && callable.HasBody
                && callable.ReturnTypeId == CSharpGuestIds.VoidTypeId
                && !asyncMethodIds.Contains(callable.MethodSymbolId)
                && !guestCallTargets.Contains(CSharpGuestIds.Function(callable.MethodSymbolId)))
            .Select(callable => CSharpGuestIds.Function(callable.MethodSymbolId))
            .ToHashSet(StringComparer.Ordinal);
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
