using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

public static class CSharpGuestDebugMapProjector
{
    private const string FunctionPrefix = "function:";
    private static readonly HashSet<string> SourceLessGeneratedFunctionIds = new(StringComparer.Ordinal)
    {
        CSharpGuestIds.GameplayEventFunctionId,
        CSharpGuestIds.ContinuationFunctionId,
        CSharpGuestIds.ContinuationV2FunctionId,
        CSharpGuestIds.UeBeginPlayCompatibilityFunctionId,
        CSharpGuestIds.UeTickCompatibilityFunctionId,
        CSharpGuestIds.UeEndPlayCompatibilityFunctionId,
    };

    public static CSharpGuestDebugMap Project(
        SemanticDocument document,
        GuestModule module,
        string guestIrSha256,
        string frontendArtifactSha256)
    {
        ArgumentNullException.ThrowIfNull(document);
        ArgumentNullException.ThrowIfNull(module);
        ArgumentNullException.ThrowIfNull(guestIrSha256);
        ArgumentNullException.ThrowIfNull(frontendArtifactSha256);

        string sourceId = NormalizeSourceId(document.Source.SourceId);
        if (!IsSha256(document.Source.Sha256)
            || !IsSha256(document.Source.FrontendSha256)
            || !IsSha256(module.Provenance.SemanticSha256)
            || !IsSha256(guestIrSha256)
            || !IsSha256(frontendArtifactSha256)
            || !string.Equals(module.ModuleId, $"csharp:{document.Source.SourceId}", StringComparison.Ordinal)
            || !string.Equals(module.Provenance.SourceId, document.Source.SourceId, StringComparison.Ordinal)
            || !string.Equals(module.Provenance.SourceSha256, document.Source.Sha256, StringComparison.Ordinal)
            || !string.Equals(module.Provenance.FrontendSha256, document.Source.FrontendSha256, StringComparison.Ordinal))
        {
            throw new InvalidDataException("ASDEBUG1001: Debug map provenance is invalid or does not match the Guest module.");
        }

        Dictionary<string, SemanticCallable> callables = UniqueBy(
            document.Callables,
            callable => callable.MethodSymbolId,
            "callable");
        Dictionary<string, SemanticSymbol> symbols = UniqueBy(
            document.Symbols,
            symbol => symbol.Id,
            "symbol");
        Dictionary<string, AsyncResumeDebugTarget> asyncResumeTargets = BuildAsyncResumeTargets(document);
        HashSet<string> functionIds = new(StringComparer.Ordinal);
        HashSet<string> methodIds = new(StringComparer.Ordinal);
        HashSet<string> probeIds = new(StringComparer.Ordinal);
        List<CSharpGuestDebugFunction> functions = new(module.Functions.Count);
        for (int ordinal = 0; ordinal < module.Functions.Count; ++ordinal)
        {
            GuestFunction function = module.Functions[ordinal];
            if (!functionIds.Add(function.Id)
                || !function.Id.StartsWith(FunctionPrefix, StringComparison.Ordinal))
            {
                throw new InvalidDataException($"ASDEBUG1002: Guest function identity '{function.Id}' is invalid or duplicated.");
            }

            if (SourceLessGeneratedFunctionIds.Contains(function.Id)
                || function.Id.StartsWith(CSharpGuestIds.DelegateEventFunctionPrefix, StringComparison.Ordinal))
            {
                continue;
            }

            int functionIndex = checked(module.Imports.Count + ordinal);
            if (asyncResumeTargets.TryGetValue(function.Id, out AsyncResumeDebugTarget? resumeTarget))
            {
                AddAsyncResumeFunction(
                    functions,
                    methodIds,
                    callables,
                    symbols,
                    functionIndex,
                    function,
                    resumeTarget,
                    module.ModuleId,
                    probeIds);
                continue;
            }

            string methodId = function.Id[FunctionPrefix.Length..];
            if (!methodIds.Add(methodId)
                || !callables.TryGetValue(methodId, out SemanticCallable? callable)
                || !callable.HasBody
                || !symbols.TryGetValue(methodId, out SemanticSymbol? methodSymbol))
            {
                throw new InvalidDataException($"ASDEBUG1003: Guest function '{function.Id}' has no unique source method mapping.");
            }

            if (methodSymbol.Kind != "method")
            {
                continue;
            }

            if (methodSymbol.ContainingSymbolId is null
                || !symbols.TryGetValue(methodSymbol.ContainingSymbolId, out SemanticSymbol? ownerSymbol)
                || ownerSymbol.Kind != "type"
                || !IsValidSpan(methodSymbol.Span))
            {
                throw new InvalidDataException($"ASDEBUG1003: Guest function '{function.Id}' has no unique source method mapping.");
            }

            string ownerName = ownerSymbol.Signature.StartsWith("global::", StringComparison.Ordinal)
                ? ownerSymbol.Signature[8..]
                : ownerSymbol.Signature;
            functions.Add(new CSharpGuestDebugFunction(
                functionIndex,
                function.Id,
                methodId,
                $"{ownerName}.{methodSymbol.Signature}",
                methodSymbol.Span,
                BuildSequencePoints(module.ModuleId, function, probeIds),
                BuildFrameLayout(document, module, function, callable, methodSymbol.Span)));
        }

        return new CSharpGuestDebugMap(
            2,
            "2.0",
            module.ModuleId,
            module.Imports.Count,
            module.Functions.Count,
            new CSharpGuestDebugSource(sourceId, document.Source.Sha256),
            new CSharpGuestDebugProvenance(
                frontendArtifactSha256,
                module.Provenance.SemanticSha256,
                guestIrSha256,
                null),
            functions);
    }

    private static Dictionary<string, AsyncResumeDebugTarget> BuildAsyncResumeTargets(
        SemanticDocument document)
    {
        Dictionary<string, AsyncResumeDebugTarget> targets = new(StringComparer.Ordinal);
        foreach (SemanticAsyncMethod method in document.AsyncMethods)
        {
            if (method.Lowering == SemanticAsyncMethod.ContinuationCfgLowering)
            {
                foreach (SemanticAsyncSegment awaitSegment in method.Segments
                    .Where(segment => segment.AwaitSite is not null))
                {
                    SemanticAsyncAwaitSite incoming = awaitSegment.AwaitSite!;
                    int targetOrdinal = awaitSegment.Transfer?.PrimaryTarget ?? -1;
                    SemanticAsyncSegment? targetSegment = method.Segments
                        .SingleOrDefault(segment => segment.Ordinal == targetOrdinal);
                    string functionId = CSharpGuestIds.AsyncResumeFunction(incoming.CallbackId);
                    if (targetSegment is null
                        || !targets.TryAdd(functionId, new AsyncResumeDebugTarget(
                            method.MethodSymbolId,
                            targetSegment.Ordinal,
                            targetSegment.Span)))
                    {
                        throw new InvalidDataException(
                            $"ASDEBUG1003: Async CFG resume function identity '{functionId}' has no unique source segment mapping.");
                    }
                }
                continue;
            }

            for (int index = 1; index < method.Segments.Count; ++index)
            {
                SemanticAsyncAwaitSite? incoming = method.Segments[index - 1].AwaitSite;
                if (incoming is null)
                {
                    throw new InvalidDataException(
                        $"ASDEBUG1003: Async method '{method.MethodSymbolId}' has a disconnected resume segment.");
                }

                string functionId = CSharpGuestIds.AsyncResumeFunction(incoming.CallbackId);
                if (!targets.TryAdd(functionId, new AsyncResumeDebugTarget(
                    method.MethodSymbolId,
                    method.Segments[index].Ordinal,
                    method.Segments[index].Span)))
                {
                    throw new InvalidDataException(
                        $"ASDEBUG1002: Async resume function identity '{functionId}' is duplicated.");
                }
            }
        }
        return targets;
    }

    private static void AddAsyncResumeFunction(
        ICollection<CSharpGuestDebugFunction> functions,
        ISet<string> methodIds,
        IReadOnlyDictionary<string, SemanticCallable> callables,
        IReadOnlyDictionary<string, SemanticSymbol> symbols,
        int functionIndex,
        GuestFunction function,
        AsyncResumeDebugTarget target,
        string moduleId,
        ISet<string> probeIds)
    {
        string functionId = function.Id;
        if (!callables.TryGetValue(target.MethodSymbolId, out SemanticCallable? callable)
            || !callable.HasBody
            || !symbols.TryGetValue(target.MethodSymbolId, out SemanticSymbol? methodSymbol)
            || methodSymbol.Kind != "method"
            || methodSymbol.ContainingSymbolId is null
            || !symbols.TryGetValue(methodSymbol.ContainingSymbolId, out SemanticSymbol? ownerSymbol)
            || ownerSymbol.Kind != "type"
            || !IsValidSpan(target.Span))
        {
            throw new InvalidDataException(
                $"ASDEBUG1003: Guest async resume function '{functionId}' has no unique source segment mapping.");
        }

        string debugMethodId = string.Concat(
            target.MethodSymbolId,
            "#async_resume:",
            target.SegmentOrdinal.ToString(CultureInfo.InvariantCulture));
        if (!methodIds.Add(debugMethodId))
        {
            throw new InvalidDataException(
                $"ASDEBUG1003: Guest async resume function '{functionId}' has a duplicated debug identity.");
        }

        string ownerName = ownerSymbol.Signature.StartsWith("global::", StringComparison.Ordinal)
            ? ownerSymbol.Signature[8..]
            : ownerSymbol.Signature;
        functions.Add(new CSharpGuestDebugFunction(
            functionIndex,
            functionId,
            debugMethodId,
            $"{ownerName}.{methodSymbol.Signature} [async resume {target.SegmentOrdinal}]",
            target.Span,
            BuildSequencePoints(moduleId, function, probeIds)));
    }

    private static IReadOnlyList<CSharpGuestDebugSequencePoint> BuildSequencePoints(
        string moduleId,
        GuestFunction function,
        ISet<string> probeIds)
    {
        List<CSharpGuestDebugSequencePoint> sequencePoints = new();
        HashSet<string> semanticOperationIds = new(StringComparer.Ordinal);
        void AddSequencePoint(GuestDebugLocation debugLocation, string guestInstructionId)
        {
            SemanticSpan span = new(
                debugLocation.Start,
                debugLocation.Length,
                debugLocation.Line,
                debugLocation.Column,
                debugLocation.EndLine,
                debugLocation.EndColumn);
            if (!semanticOperationIds.Add(debugLocation.SemanticOperationId)
                || !IsValidSpan(span)
                || debugLocation.Kind is not ("hidden" or "statement" or "call" or "await" or "return"))
            {
                throw new InvalidDataException(
                    $"ASDEBUG1004: Function '{function.Id}' contains an invalid or duplicated sequence point.");
            }
            string? probeId = debugLocation.Hidden
                ? null
                : CSharpGuestDebugProbeIdentity.Create(
                    moduleId,
                    function.Id,
                    debugLocation.SemanticOperationId);
            if (probeId is not null && !probeIds.Add(probeId))
            {
                throw new InvalidDataException(
                    $"ASDEBUG1005: Function '{function.Id}' contains a colliding debug probe identity.");
            }
            sequencePoints.Add(new CSharpGuestDebugSequencePoint(
                -1,
                guestInstructionId,
                debugLocation.SemanticOperationId,
                probeId,
                debugLocation.Kind,
                debugLocation.Hidden,
                span));
        }

        for (int blockIndex = 0; blockIndex < function.Blocks.Count; ++blockIndex)
        {
            GuestBasicBlock block = function.Blocks[blockIndex];
            for (int instructionIndex = 0;
                instructionIndex < block.Instructions.Count;
                ++instructionIndex)
            {
                GuestDebugLocation? debugLocation = block.Instructions[instructionIndex].DebugLocation;
                if (debugLocation is null)
                {
                    continue;
                }
                AddSequencePoint(
                    debugLocation,
                    GuestDebugIdentity.Instruction(function.Id, block.Id, instructionIndex));
            }

            if (block.Terminator.DebugLocation is { } terminatorDebugLocation)
            {
                AddSequencePoint(
                    terminatorDebugLocation,
                    GuestDebugIdentity.Terminator(function.Id, block.Id));
            }
        }
        return sequencePoints;
    }

    private static CSharpGuestDebugFrameLayout? BuildFrameLayout(
        SemanticDocument document,
        GuestModule module,
        GuestFunction function,
        SemanticCallable callable,
        SemanticSpan methodSpan)
    {
        GuestType? frame = module.Types.SingleOrDefault(type =>
            type.Id == CSharpGuestDebugProbeAbi.FrameTypePrefix + function.Id);
        if (frame is null)
        {
            return null;
        }
        if (frame.Kind != "struct"
            || frame.Storage != "memory"
            || frame.Size <= 0
            || frame.Size > 4096
            || frame.Fields.Count == 0
            || frame.Fields[0].Name != CSharpGuestDebugProbeAbi.FrameRouteFieldName)
        {
            throw new InvalidDataException(
                $"ASDEBUG1006: Function '{function.Id}' has an invalid debug frame layout.");
        }

        int sourceParameterCount = function.Parameters.TakeWhile(parameter =>
            !parameter.Id.StartsWith("value:debug_resumable:", StringComparison.Ordinal)).Count();
        int sourceLocalCount = frame.Fields.Count - 1 - sourceParameterCount;
        if (sourceLocalCount < 0 || sourceLocalCount > function.Locals.Count)
        {
            throw new InvalidDataException(
                $"ASDEBUG1006: Function '{function.Id}' debug frame slots do not match its registers.");
        }

        GuestRegister[] registers = function.Parameters.Take(sourceParameterCount)
            .Concat(function.Locals.Take(sourceLocalCount))
            .ToArray();
        if (registers.Length != frame.Fields.Count - 1)
        {
            throw new InvalidDataException(
                $"ASDEBUG1006: Function '{function.Id}' debug frame register count is invalid.");
        }

        Dictionary<string, SemanticSymbol> symbols = document.Symbols.ToDictionary(
            symbol => symbol.Id,
            StringComparer.Ordinal);
        Dictionary<string, GuestType> types = module.Types.ToDictionary(
            type => type.Id,
            StringComparer.Ordinal);
        SemanticMethodBody? method = document.Methods.SingleOrDefault(item =>
            item.MethodSymbolId == callable.MethodSymbolId);
        List<CSharpGuestDebugVariable> variables = new();
        for (int index = 0; index < registers.Length; ++index)
        {
            GuestRegister register = registers[index];
            string? symbolId = TryGetSourceSymbolId(register.Id);
            if (symbolId is null)
            {
                continue;
            }
            if (!symbols.TryGetValue(symbolId, out SemanticSymbol? symbol)
                || symbol.TypeId is null
                || symbol.TypeId != register.TypeId
                || symbol.Kind is not ("parameter" or "local")
                || !types.TryGetValue(register.TypeId, out GuestType? type))
            {
                throw new InvalidDataException(
                    $"ASDEBUG1006: Debug register '{register.Id}' has no valid source variable mapping.");
            }

            GuestField field = frame.Fields[index + 1];
            if (field.TypeId != register.TypeId
                || field.Offset < 0
                || type.Size <= 0
                || field.Offset > frame.Size - type.Size)
            {
                throw new InvalidDataException(
                    $"ASDEBUG1006: Debug variable '{symbolId}' exceeds its frame bounds.");
            }

            SemanticSpan scope = symbol.Kind == "parameter"
                ? methodSpan
                : FindLocalScope(method?.Root, symbol);
            variables.Add(new CSharpGuestDebugVariable(
                symbol.Id,
                symbol.Name,
                symbol.Kind,
                symbol.TypeId,
                type.Kind,
                type.Storage,
                field.Offset,
                type.Size,
                symbol.Span,
                scope));
        }
        return new CSharpGuestDebugFrameLayout(frame.Size, variables);
    }

    private static string? TryGetSourceSymbolId(string registerId)
    {
        const string ParameterPrefix = "value:parameter:";
        const string LocalPrefix = "value:local:";
        if (registerId.StartsWith(ParameterPrefix, StringComparison.Ordinal))
        {
            return registerId[ParameterPrefix.Length..];
        }
        if (registerId.StartsWith(LocalPrefix, StringComparison.Ordinal))
        {
            return registerId[LocalPrefix.Length..];
        }
        return null;
    }

    private static SemanticSpan FindLocalScope(
        SemanticOperation? root,
        SemanticSymbol local)
    {
        if (root is null)
        {
            throw new InvalidDataException(
                $"ASDEBUG1006: Local '{local.Id}' has no semantic method body.");
        }

        List<SemanticOperation> ancestors = new();
        SemanticSpan? result = null;
        bool Visit(SemanticOperation operation)
        {
            ancestors.Add(operation);
            if (operation.Kind == "variable_declarator"
                && operation.SymbolId == local.Id)
            {
                result = ancestors.AsEnumerable().Reverse()
                    .FirstOrDefault(IsLexicalScope)?.Span;
                ancestors.RemoveAt(ancestors.Count - 1);
                return true;
            }
            foreach (SemanticOperation child in operation.Children)
            {
                if (Visit(child))
                {
                    ancestors.RemoveAt(ancestors.Count - 1);
                    return true;
                }
            }
            ancestors.RemoveAt(ancestors.Count - 1);
            return false;
        }

        if (!Visit(root) || result is null || !IsValidSpan(result))
        {
            throw new InvalidDataException(
                $"ASDEBUG1006: Local '{local.Id}' has no bounded lexical scope.");
        }
        return result;
    }

    private static bool IsLexicalScope(SemanticOperation operation)
    {
        return operation.Kind is "block" or "loop" or "switch_case";
    }

    private static Dictionary<string, T> UniqueBy<T>(
        IEnumerable<T> values,
        Func<T, string> keySelector,
        string kind)
    {
        Dictionary<string, T> result = new(StringComparer.Ordinal);
        foreach (T value in values)
        {
            string key = keySelector(value);
            if (string.IsNullOrWhiteSpace(key) || !result.TryAdd(key, value))
            {
                throw new InvalidDataException($"ASDEBUG1002: Semantic {kind} identity '{key}' is empty or duplicated.");
            }
        }
        return result;
    }

    private static string NormalizeSourceId(string sourceId)
    {
        if (string.IsNullOrWhiteSpace(sourceId) || Path.IsPathRooted(sourceId))
        {
            throw new InvalidDataException("ASDEBUG1001: Debug source identity must be project-relative.");
        }

        string normalized = sourceId.Replace('\\', '/');
        string[] segments = normalized.Split('/', StringSplitOptions.RemoveEmptyEntries);
        if (segments.Length == 0
            || segments.Any(segment => segment is "." or "..")
            || !string.Equals(normalized, string.Join('/', segments), StringComparison.Ordinal))
        {
            throw new InvalidDataException("ASDEBUG1001: Debug source identity is not canonical or escapes its project root.");
        }
        return normalized;
    }

    private static bool IsSha256(string value)
    {
        return value.Length == 64 && value.All(character =>
            character is >= '0' and <= '9' or >= 'a' and <= 'f');
    }

    private static bool IsValidSpan(SemanticSpan span)
    {
        return span.Start >= 0
            && span.Length >= 0
            && span.Line >= 0
            && span.Column >= 0
            && span.EndLine >= span.Line
            && span.EndColumn >= 0
            && (span.EndLine > span.Line || span.EndColumn >= span.Column);
    }

    private sealed record AsyncResumeDebugTarget(
        string MethodSymbolId,
        int SegmentOrdinal,
        SemanticSpan Span);
}
