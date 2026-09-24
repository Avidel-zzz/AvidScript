using System;
using System.Collections.Generic;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal sealed record CSharpLanguageCatchMatch(
    int TypeToken, string HandlerBlockId, bool CaptureError = false);
internal sealed record CSharpLanguageCatchRoute(
    string SourceBlockId, IReadOnlyList<CSharpLanguageCatchMatch> Matches);
internal static class CSharpLanguageCatchContext
{
    public static string OutcomeRegister(string handlerBlockId) =>
        "language_catch:outcome:" + handlerBlockId;
}

// Converts ordinary lowered functions into IR 16 outcome functions. Exception
// producers and UE boundary adapters are separate steps; this pass refuses a
// raw export or an unresolved caller rather than publishing an invalid module.
public static class CSharpLanguageOutcomeRewriter
{
    public static bool TryRewrite(
        SemanticDocument semantic,
        GuestModule module,
        IReadOnlySet<string> affectedFunctionIds,
        out GuestModule? rewritten,
        out string? error) =>
        TryRewriteCore(semantic, module, affectedFunctionIds,
            new HashSet<string>(StringComparer.Ordinal),
            new Dictionary<string, IReadOnlyList<CSharpLanguageCatchRoute>>(StringComparer.Ordinal),
            new Dictionary<string, IReadOnlyList<CSharpLanguageCleanupRoute>>(StringComparer.Ordinal),
            out rewritten, out error);

    internal static bool TryRewriteWithProducers(
        SemanticDocument semantic,
        GuestModule module,
        IReadOnlySet<string> affectedFunctionIds,
        IReadOnlySet<string> producerFunctionIds,
        out GuestModule? rewritten,
        out string? error) =>
        TryRewriteCore(semantic, module, affectedFunctionIds,
            producerFunctionIds,
            new Dictionary<string, IReadOnlyList<CSharpLanguageCatchRoute>>(StringComparer.Ordinal),
            new Dictionary<string, IReadOnlyList<CSharpLanguageCleanupRoute>>(StringComparer.Ordinal),
            out rewritten, out error);

    internal static bool TryRewriteWithHandlers(
        SemanticDocument semantic,
        GuestModule module,
        IReadOnlySet<string> affectedFunctionIds,
        IReadOnlySet<string> producerFunctionIds,
        IReadOnlyDictionary<string, IReadOnlyList<CSharpLanguageCatchRoute>> catchRoutes,
        IReadOnlyDictionary<string, IReadOnlyList<CSharpLanguageCleanupRoute>> cleanupRoutes,
        out GuestModule? rewritten,
        out string? error) =>
        TryRewriteCore(semantic, module, affectedFunctionIds,
            producerFunctionIds, catchRoutes, cleanupRoutes, out rewritten, out error);

    private static bool TryRewriteCore(
        SemanticDocument semantic,
        GuestModule module,
        IReadOnlySet<string> affectedFunctionIds,
        IReadOnlySet<string> producerFunctionIds,
        IReadOnlyDictionary<string, IReadOnlyList<CSharpLanguageCatchRoute>> catchRoutes,
        IReadOnlyDictionary<string, IReadOnlyList<CSharpLanguageCleanupRoute>> cleanupRoutes,
        out GuestModule? rewritten,
        out string? error)
    {
        rewritten = null;
        error = null;
        if (semantic is null || module is null || affectedFunctionIds is null || affectedFunctionIds.Count == 0
            || catchRoutes is null || catchRoutes.Keys.Any(id => !affectedFunctionIds.Contains(id)
                || producerFunctionIds.Contains(id))
            || cleanupRoutes is null || cleanupRoutes.Keys.Any(id => !affectedFunctionIds.Contains(id)
                || producerFunctionIds.Contains(id) || catchRoutes.ContainsKey(id))
            || module.LanguageOutcomeTypes is not null)
            return Fail("Expected an ordinary Guest module and a nonempty outcome effect set.", out error);
        if (!semantic.Succeeded || semantic.ExceptionFlows is not null
            || !CSharpSemanticInputValidator.IsValid(semantic))
            return Fail("The Semantic input is not an executable ordinary artifact.", out error);
        GuestValidationResult sourceValidation = GuestModuleValidator.Validate(module);
        if (!sourceValidation.Succeeded)
            return Fail("The ordinary Guest module failed validation.", out error);
        if (semantic.Source.SourceId != module.Provenance.SourceId
            || semantic.Source.Sha256 != module.Provenance.SourceSha256
            || semantic.Source.FrontendSha256 != module.Provenance.FrontendSha256
            || semantic.SchemaVersion != module.Provenance.SemanticSchemaVersion
            || semantic.SemanticVersion != module.Provenance.SemanticVersion)
            return Fail("Semantic source and Guest provenance do not match.", out error);
        Dictionary<string, GuestFunction> functions = module.Functions.ToDictionary(
            function => function.Id, StringComparer.Ordinal);
        Dictionary<string, SemanticCallable> sourceCallables = semantic.Callables
            .ToDictionary(callable => CSharpGuestIds.Function(callable.MethodSymbolId), StringComparer.Ordinal);
        foreach (string id in affectedFunctionIds)
        {
            if (!sourceCallables.TryGetValue(id, out SemanticCallable? callable))
                return Fail($"Affected function '{id}' has no Semantic callable.", out error);
            SemanticMethodBody[] bodies = semantic.Methods.Where(method =>
                method.MethodSymbolId == callable.MethodSymbolId).ToArray();
            if (bodies.Length != 1)
                return Fail($"Affected function '{id}' has no unique Semantic body.", out error);
            if (!producerFunctionIds.Contains(id)
                && (semantic.AsyncMethods.Any(method => method.MethodSymbolId == callable.MethodSymbolId)
                    || !catchRoutes.ContainsKey(id) && !cleanupRoutes.ContainsKey(id)
                        && ContainsStructuredCleanup(bodies[0].Root)))
                return Fail($"Function '{callable.MethodSymbolId}' needs cleanup-aware outcome lowering.", out error);
        }
        if (affectedFunctionIds.Any(id => !functions.ContainsKey(id))
            || module.Exports.Any(export => affectedFunctionIds.Contains(export.FunctionId))
            || module.FramedExports.Any(export => affectedFunctionIds.Contains(export.FunctionId))
            || module.FunctionReferences.Any(reference => reference.TargetFunctionIds.Any(affectedFunctionIds.Contains)))
            return Fail("An affected function has no body or needs a Host/function-reference adapter.", out error);
        foreach (GuestFunction function in module.Functions)
            if (!affectedFunctionIds.Contains(function.Id)
                && function.Blocks.SelectMany(block => block.Instructions)
                    .Any(instruction => instruction.Op == "call"
                        && instruction.TargetId is { } target
                        && affectedFunctionIds.Contains(target)))
                return Fail($"Caller '{function.Id}' is missing from the outcome effect closure.", out error);

        GuestType? int32 = module.Types.FirstOrDefault(type => type.Id == "type:int32"
            && type.Kind == "scalar" && type.Storage == "i32");
        if (int32 is null) return Fail("The outcome status has no canonical int32 type.", out error);
        const string payloadId = "type:language_error_payload";
        const string rootId = "type:language_error_root";
        if (module.Types.Any(type => type.Id is payloadId or rootId))
            return Fail("The language-error root type identity is already occupied.", out error);
        List<GuestType> rawTypes = module.Types.ToList();
        rawTypes.Add(new GuestType(payloadId, "struct", "memory",
            new[] { new GuestField("field:code", "code", int32.Id, 0) }, null, null, 0, 1));
        rawTypes.Add(new GuestType(rootId, "managed_ref", "i64",
            Array.Empty<GuestField>(), payloadId, null, 8, 8));
        Dictionary<string, string> outcomeByValueType = new(StringComparer.Ordinal);
        foreach (string valueType in affectedFunctionIds.Select(id => functions[id].ReturnTypeId)
            .Distinct(StringComparer.Ordinal).OrderBy(id => id, StringComparer.Ordinal))
        {
            bool hasValue = valueType != "type:void";
            string outcomeId = "type:language_outcome:" + Convert.ToHexString(
                SHA256.HashData(Encoding.UTF8.GetBytes(valueType))).ToLowerInvariant();
            if (module.Types.Any(type => type.Id == outcomeId))
                return Fail($"Outcome type identity for '{valueType}' is already occupied.", out error);
            GuestField[] fields =
            {
                new("field:status", "status", int32.Id, 0),
                new("field:error_type", "error_type", int32.Id, 0),
                new("field:source", "source", int32.Id, 0),
                new("field:error_root", "error_root", rootId, 0),
            };
            rawTypes.Add(new GuestType(outcomeId, "struct", "memory",
                hasValue ? fields.Append(new GuestField("field:value", "value", valueType, 0)).ToArray() : fields,
                null, null, 0, 1));
            outcomeByValueType.Add(valueType, outcomeId);
        }
        GuestTypeLayoutResult typeLayout = GuestDataLayout.ComputeTypes(rawTypes);
        if (!typeLayout.Succeeded)
            return Fail("The language-outcome type layout is invalid.", out error);
        List<GuestImport> imports = module.Imports.ToList();
        if (!imports.Any(import => import.Module == GuestManagedHeap.ImportModule
            && import.Name == GuestManagedHeap.ImportName))
            imports.Add(new GuestImport("import:language_error_heap", GuestManagedHeap.ImportModule,
                GuestManagedHeap.ImportName, Enumerable.Repeat(int32.Id, 4).ToArray(), int32.Id));

        List<GuestFunction> rewrittenFunctions = new();
        foreach (GuestFunction function in module.Functions)
        {
            if (!affectedFunctionIds.Contains(function.Id))
            {
                rewrittenFunctions.Add(function);
                continue;
            }
            if (!TryRewriteFunction(function, functions, affectedFunctionIds, outcomeByValueType,
                catchRoutes.TryGetValue(function.Id, out var routes) ? routes : Array.Empty<CSharpLanguageCatchRoute>(),
                cleanupRoutes.TryGetValue(function.Id, out var cleanups)
                    ? cleanups : Array.Empty<CSharpLanguageCleanupRoute>(),
                cleanupRoutes.ContainsKey(function.Id),
                int32.Id, rootId, out GuestFunction? result, out error)) return false;
            rewrittenFunctions.Add(result!);
        }
        GuestLayoutResult layout = GuestLayoutBuilder.Build(typeLayout.Types,
            module.Globals, module.DataSegments, module.MemoryLayout.StateStart);
        if (!layout.Succeeded || layout.Layout is null)
            return Fail("The language-outcome module layout is invalid.", out error);
        GuestModule candidate = module with
        {
            SchemaVersion = 16,
            IrVersion = "1.15",
            Types = typeLayout.Types,
            Imports = imports,
            Functions = rewrittenFunctions,
            MemoryLayout = layout.Layout,
            DataSegments = layout.DataSegments,
            LanguageOutcomeTypes = outcomeByValueType.OrderBy(pair => pair.Key, StringComparer.Ordinal)
                .Select(pair => new GuestLanguageOutcomeType(pair.Value,
                    pair.Key == "type:void" ? null : pair.Key)).ToArray(),
        };
        GuestValidationResult validation = GuestModuleValidator.Validate(candidate);
        if (!validation.Succeeded)
            return Fail(string.Join(" | ", validation.Diagnostics.Select(item => item.Message)), out error);
        rewritten = candidate;
        return true;
    }

    private static bool TryRewriteFunction(
        GuestFunction function,
        IReadOnlyDictionary<string, GuestFunction> functions,
        IReadOnlySet<string> affected,
        IReadOnlyDictionary<string, string> outcomeByValueType,
        IReadOnlyList<CSharpLanguageCatchRoute> catchRoutes,
        IReadOnlyList<CSharpLanguageCleanupRoute> cleanupRoutes,
        bool requireCleanupCoverage,
        string int32TypeId,
        string rootTypeId,
        out GuestFunction? rewritten,
        out string? error)
    {
        rewritten = null;
        error = null;
        string outcomeType = outcomeByValueType[function.ReturnTypeId];
        List<GuestRegister> locals = function.Locals.ToList();
        HashSet<string> registerIds = function.Parameters.Concat(function.Locals)
            .Select(register => register.Id).ToHashSet(StringComparer.Ordinal);
        HashSet<string> blockIds = function.Blocks.Select(block => block.Id)
            .ToHashSet(StringComparer.Ordinal);
        Dictionary<string, GuestBasicBlock> sourceBlocks = function.Blocks
            .ToDictionary(block => block.Id, StringComparer.Ordinal);
        Dictionary<string, string> registerTypes = function.Parameters.Concat(function.Locals)
            .ToDictionary(register => register.Id, register => register.TypeId, StringComparer.Ordinal);
        if (catchRoutes.Any(route => !blockIds.Contains(route.SourceBlockId)
                || route.Matches.Count == 0
                || route.Matches.Any(match => match.TypeToken <= 0
                    || !blockIds.Contains(match.HandlerBlockId))
                || route.Matches.Select(match => match.TypeToken).Distinct().Count()
                    != route.Matches.Count)
            || catchRoutes.Select(route => route.SourceBlockId).Distinct(StringComparer.Ordinal).Count()
                != catchRoutes.Count
            || catchRoutes.SelectMany(route => route.Matches)
                .GroupBy(match => match.HandlerBlockId, StringComparer.Ordinal)
                .Any(group => group.Select(match => match.CaptureError).Distinct().Count() != 1))
            return Fail($"Function '{function.Id}' has an invalid catch route.", out error);
        if (cleanupRoutes.Any(route => !blockIds.Contains(route.SourceBlockId)
                || route.CleanupBlockIds.Any(id => !blockIds.Contains(id)))
            || cleanupRoutes.Select(route => route.SourceBlockId)
                .Distinct(StringComparer.Ordinal).Count() != cleanupRoutes.Count)
            return Fail($"Function '{function.Id}' has an invalid cleanup route.", out error);
        Dictionary<string, CSharpLanguageCatchRoute> catchByBlock = catchRoutes
            .ToDictionary(route => route.SourceBlockId, StringComparer.Ordinal);
        Dictionary<string, CSharpLanguageCleanupRoute> cleanupByBlock = cleanupRoutes
            .ToDictionary(route => route.SourceBlockId, StringComparer.Ordinal);
        string[] capturedHandlers = catchRoutes.SelectMany(route => route.Matches)
            .Where(match => match.CaptureError).Select(match => match.HandlerBlockId)
            .Distinct(StringComparer.Ordinal).OrderBy(id => id, StringComparer.Ordinal).ToArray();
        Dictionary<string, string> capturedOutcomes = new(StringComparer.Ordinal);
        foreach (string handlerId in capturedHandlers)
        {
            string id = CSharpLanguageCatchContext.OutcomeRegister(handlerId);
            if (!registerIds.Add(id))
                return Fail($"Function '{function.Id}' has a conflicting catch context.", out error);
            locals.Add(new GuestRegister(id, outcomeType));
            capturedOutcomes.Add(handlerId, id);
        }
        int registerOrdinal = 0, blockOrdinal = 0;
        string Register(string typeId)
        {
            string id;
            do { id = "outcome:local:" + registerOrdinal++; } while (!registerIds.Add(id));
            locals.Add(new GuestRegister(id, typeId));
            return id;
        }
        string Block()
        {
            string id;
            do { id = "outcome:block:" + blockOrdinal++; } while (!blockIds.Add(id));
            return id;
        }
        List<GuestBasicBlock> blocks = new();
        foreach (GuestBasicBlock source in function.Blocks)
        {
            string currentId = source.Id;
            List<GuestInstruction> instructions = new();
            if (source.Id == function.EntryBlockId)
                foreach (string handlerId in capturedHandlers)
                    instructions.Add(new GuestInstruction("stack_alloc", capturedOutcomes[handlerId],
                        Array.Empty<string>(), null, null, null));
            foreach (GuestInstruction instruction in source.Instructions)
            {
                if (instruction.Op != "call" || instruction.TargetId is not { } target
                    || !affected.Contains(target))
                {
                    instructions.Add(instruction);
                    continue;
                }
                GuestFunction callee = functions[target];
                if (requireCleanupCoverage && !cleanupByBlock.ContainsKey(source.Id))
                    return Fail($"Function '{function.Id}' has an unchecked error cleanup path.", out error);
                string calledOutcome = outcomeByValueType[callee.ReturnTypeId];
                string result = Register(calledOutcome);
                string status = Register(int32TypeId);
                string errorBlock = Block(), successBlock = Block();
                instructions.Add(instruction with { ResultId = result });
                instructions.Add(new GuestInstruction("field_load", status,
                    new[] { result }, "field:status", null, null));
                blocks.Add(new GuestBasicBlock(currentId, instructions,
                    new GuestTerminator("branch_if", status, errorBlock, successBlock, null)));
                string unhandledBlock = errorBlock;
                if (catchByBlock.TryGetValue(source.Id, out CSharpLanguageCatchRoute? catchRoute))
                {
                    string errorType = Register(int32TypeId);
                    string testBlock = errorBlock;
                    for (int matchOrdinal = 0; matchOrdinal < catchRoute.Matches.Count; ++matchOrdinal)
                    {
                        CSharpLanguageCatchMatch match = catchRoute.Matches[matchOrdinal];
                        string key = Register(int32TypeId), equal = Register(int32TypeId);
                        string nextBlock = Block();
                        List<GuestInstruction> tests = new();
                        if (matchOrdinal == 0)
                            tests.Add(new GuestInstruction("field_load", errorType,
                                new[] { result }, "field:error_type", null, null));
                        tests.Add(new GuestInstruction("constant", key, Array.Empty<string>(),
                            null, null, new GuestConstant("int32", match.TypeToken.ToString(
                                System.Globalization.CultureInfo.InvariantCulture))));
                        tests.Add(new GuestInstruction("binary", equal,
                            new[] { errorType, key }, null, "equals", null));
                        string matchedTarget = match.CaptureError ? Block() : match.HandlerBlockId;
                        blocks.Add(new GuestBasicBlock(testBlock, tests,
                            new GuestTerminator("branch_if", equal,
                                matchedTarget, nextBlock, null)));
                        if (match.CaptureError)
                        {
                            string capture = capturedOutcomes[match.HandlerBlockId];
                            string sourceToken = Register(int32TypeId);
                            string errorRoot = Register(rootTypeId);
                            string one = Register(int32TypeId);
                            blocks.Add(new GuestBasicBlock(matchedTarget, new GuestInstruction[]
                            {
                                new("field_load", sourceToken, new[] { result }, "field:source", null, null),
                                new("field_load", errorRoot, new[] { result }, "field:error_root", null, null),
                                new("constant", one, Array.Empty<string>(), null, null,
                                    new GuestConstant("int32", "1")),
                                Store(capture, "status", one),
                                Store(capture, "error_type", errorType),
                                Store(capture, "source", sourceToken),
                                Store(capture, "error_root", errorRoot),
                            }, new GuestTerminator("branch", null,
                                match.HandlerBlockId, null, null)));
                        }
                        testBlock = nextBlock;
                    }
                    unhandledBlock = testBlock;
                }
                if (cleanupByBlock.TryGetValue(source.Id, out CSharpLanguageCleanupRoute? cleanupRoute))
                {
                    foreach (string cleanupId in cleanupRoute.CleanupBlockIds)
                    {
                        GuestBasicBlock cleanup = sourceBlocks[cleanupId];
                        if (cleanup.Terminator.Kind != "branch"
                            || cleanup.Instructions.Any(item => item.Op == "call_indirect"
                                || item.Op == "call" && item.TargetId is { } called
                                    && affected.Contains(called)))
                            return Fail($"Function '{function.Id}' has a throwing or branching finally.", out error);
                        Dictionary<string, string> renamed = new(StringComparer.Ordinal);
                        List<GuestInstruction> copied = new();
                        foreach (GuestInstruction item in cleanup.Instructions)
                        {
                            string[] operands = item.OperandIds.Select(id =>
                                renamed.TryGetValue(id, out string? replacement) ? replacement : id).ToArray();
                            string? targetId = item.TargetId is { } oldTarget
                                && renamed.TryGetValue(oldTarget, out string? newTarget)
                                    ? newTarget : item.TargetId;
                            string? resultId = null;
                            if (item.ResultId is { } oldResult)
                            {
                                if (!registerTypes.TryGetValue(oldResult, out string? typeId))
                                    return Fail($"Function '{function.Id}' has an untyped finally value.", out error);
                                resultId = Register(typeId);
                                renamed[oldResult] = resultId;
                            }
                            copied.Add(item with
                            {
                                ResultId = resultId,
                                OperandIds = operands,
                                TargetId = targetId,
                            });
                        }
                        string nextBlock = Block();
                        blocks.Add(new GuestBasicBlock(unhandledBlock, copied,
                            new GuestTerminator("branch", null, nextBlock, null, null)));
                        unhandledBlock = nextBlock;
                    }
                }
                if (calledOutcome != outcomeType)
                {
                    string ownError = Register(outcomeType);
                    string errorType = Register(int32TypeId), sourceId = Register(int32TypeId);
                    string errorRoot = Register(rootTypeId);
                    string one = Register(int32TypeId);
                    blocks.Add(new GuestBasicBlock(unhandledBlock, new GuestInstruction[]
                    {
                        new("field_load", errorType, new[] { result }, "field:error_type", null, null),
                        new("field_load", sourceId, new[] { result }, "field:source", null, null),
                        new("field_load", errorRoot, new[] { result }, "field:error_root", null, null),
                        new("stack_alloc", ownError, Array.Empty<string>(), null, null, null),
                        new("constant", one, Array.Empty<string>(), null, null, new GuestConstant("int32", "1")),
                        Store(ownError, "status", one),
                        Store(ownError, "error_type", errorType),
                        Store(ownError, "source", sourceId),
                        Store(ownError, "error_root", errorRoot),
                    }, new GuestTerminator("return", null, null, null, ownError)));
                }
                else blocks.Add(new GuestBasicBlock(unhandledBlock,
                    Array.Empty<GuestInstruction>(), new GuestTerminator("return", null, null, null, result)));
                currentId = successBlock;
                instructions = new();
                if (instruction.ResultId is { } originalResult)
                    instructions.Add(new GuestInstruction("field_load", originalResult,
                        new[] { result }, "field:value", null, null));
            }
            GuestTerminator terminator = source.Terminator;
            if (terminator.Kind == "return")
            {
                string value = terminator.ReturnValueId ?? "";
                if ((function.ReturnTypeId == "type:void") != (value.Length == 0))
                    return Fail($"Function '{function.Id}' has an invalid normal return.", out error);
                string result = Register(outcomeType), zero = Register(int32TypeId);
                instructions.Add(new GuestInstruction("stack_alloc", result,
                    Array.Empty<string>(), null, null, null));
                instructions.Add(new GuestInstruction("constant", zero,
                    Array.Empty<string>(), null, null, new GuestConstant("int32", "0")));
                instructions.Add(Store(result, "status", zero));
                if (value.Length != 0) instructions.Add(Store(result, "value", value));
                terminator = new GuestTerminator("return", null, null, null, result);
            }
            blocks.Add(new GuestBasicBlock(currentId, instructions, terminator));
        }
        rewritten = function with { ReturnTypeId = outcomeType, Locals = locals, Blocks = blocks };
        return true;
    }

    private static GuestInstruction Store(string owner, string field, string value) =>
        new("field_store", null, new[] { owner, value }, "field:" + field, null, null);

    private static bool ContainsStructuredCleanup(SemanticOperation root)
    {
        Stack<SemanticOperation> pending = new();
        pending.Push(root);
        while (pending.TryPop(out SemanticOperation? operation))
        {
            if (operation.Kind is "try" or "catch_clause" or "throw" or "loop" or "await") return true;
            foreach (SemanticOperation child in operation.Children) pending.Push(child);
        }
        return false;
    }

    private static bool Fail(string message, out string? error)
    {
        error = message;
        return false;
    }
}
