using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

// Replaces the ordinary CFG's synthetic return at each validated local throw
// with a rooted language-error result and its source-derived catch edge.
internal static class CSharpLocalThrowLowerer
{
    public static bool TryLowerReplacing(
        SemanticExceptionFlow flow,
        IReadOnlyList<CSharpLocalThrowSite> sites,
        IReadOnlyList<CSharpNormalReturnCleanupSite> normalReturns,
        IReadOnlyList<CSharpRethrowSite> rethrows,
        CSharpLanguageErrorTokenCatalog catalog,
        IReadOnlyList<CSharpLanguageCatchRoute> catchRoutes,
        GuestModule module,
        out GuestFunction? lowered,
        out string? error)
    {
        lowered = null;
        error = null;
        string functionId = CSharpGuestIds.Function(flow.MethodSymbolId);
        GuestFunction[] matches = module.Functions.Where(item => item.Id == functionId).ToArray();
        if (sites.Count + rethrows.Count + normalReturns.Count == 0
            || sites.Count + rethrows.Count != flow.Throws.Count || matches.Length != 1
            || module.LanguageOutcomeTypes?.All(item => item.TypeId != matches[0].ReturnTypeId) != false
            || !module.Types.Any(type => type.Id == "type:language_error_root"
                && type.Kind == "managed_ref")
            || !module.Imports.Any(import => import.Module == GuestManagedHeap.ImportModule
                && import.Name == GuestManagedHeap.ImportName))
            return Fail("The local throw has no unique outcome function or managed error root.", out error);

        GuestFunction function = matches[0];
        List<GuestRegister> locals = function.Locals.ToList();
        HashSet<string> registerIds = function.Parameters.Concat(locals)
            .Select(register => register.Id).ToHashSet(StringComparer.Ordinal);
        Dictionary<string, GuestBasicBlock> blocks = function.Blocks
            .ToDictionary(block => block.Id, StringComparer.Ordinal);
        Dictionary<string, CSharpLanguageCatchRoute> routes = catchRoutes
            .ToDictionary(route => route.SourceBlockId, StringComparer.Ordinal);
        Dictionary<string, string> registerTypes = function.Parameters.Concat(locals)
            .ToDictionary(register => register.Id, register => register.TypeId,
                StringComparer.Ordinal);
        Dictionary<int, (string OutcomeId, int SiteCount)> branchingShared = new();
        foreach (IGrouping<int, CSharpBranchingCleanup> group in sites
            .Where(site => site.BranchingCleanup is not null)
            .Select(site => site.BranchingCleanup!)
            .Concat(normalReturns.Where(site => site.BranchingCleanup is not null)
                .Select(site => site.BranchingCleanup!))
            .GroupBy(route => route.ExitBlockOrdinal))
        {
            CSharpBranchingCleanup[] routeVariants = group.ToArray();
            CSharpBranchingCleanup route = routeVariants[0];
            if (routeVariants.Length > 16 || routeVariants.Any(other =>
                    !other.BlockOrdinals.SequenceEqual(route.BlockOrdinals))
                || sites.Any(site => site.BranchingCleanup?.ExitBlockOrdinal
                    == group.Key && site.ReplacesThrowBlockOrdinal is not null))
                return Fail("Shared branching cleanup needs one source-derived route.",
                    out error);
            string outcomeId = "language_error:branch_cleanup:"
                + group.Key.ToString(CultureInfo.InvariantCulture) + ":outcome";
            if (!registerIds.Add(outcomeId)
                || !blocks.TryGetValue(function.EntryBlockId, out GuestBasicBlock? entry))
                return Fail("Shared branching cleanup has no unique outcome.", out error);
            locals.Add(new GuestRegister(outcomeId, function.ReturnTypeId));
            blocks[function.EntryBlockId] = entry with
            {
                Instructions = new[]
                {
                    new GuestInstruction("stack_alloc", outcomeId,
                        Array.Empty<string>(), null, null, null),
                }.Concat(entry.Instructions).ToArray(),
            };
            branchingShared.Add(group.Key, (outcomeId,
                sites.Count(site => site.BranchingCleanup?.ExitBlockOrdinal == group.Key)));
        }
        Dictionary<int, HashSet<string>> normalBranchEntrances = new();
        if (normalReturns.Count > 16
            || normalReturns.Select(site => site.BlockOrdinal).Distinct().Count()
                != normalReturns.Count)
            return Fail("Normal returns need distinct bounded cleanup paths.", out error);
        foreach (CSharpNormalReturnCleanupSite site in normalReturns)
        {
            string returnId = CSharpGuestIds.Block(flow.MethodSymbolId, site.BlockOrdinal);
            string cleanupId = CSharpGuestIds.Block(flow.MethodSymbolId,
                site.CleanupBlockOrdinal);
            if (!blocks.TryGetValue(returnId, out GuestBasicBlock? normal)
                || !blocks.TryGetValue(cleanupId, out GuestBasicBlock? cleanup))
                return Fail("A normal return has no unique synchronous cleanup.",
                    out error);
            HashSet<string> visited = new(StringComparer.Ordinal);
            for (int depth = 0; normal.Terminator.Kind == "branch_if"; ++depth)
            {
                GuestInstruction[] instructions = normal.Instructions.ToArray();
                if (depth >= 16 || !visited.Add(normal.Id)
                    || instructions.Length < 2
                    || instructions[^2] is not { Op: "call", ResultId: not null } call
                    || instructions[^1] is not { Op: "field_load", ResultId: not null,
                        TargetId: "field:status" } status
                    || status.OperandIds.Count != 1
                    || status.OperandIds[0] != call.ResultId
                    || normal.Terminator.ConditionValueId != status.ResultId
                    || normal.Terminator.TargetBlockId is not { } errorId
                    || !blocks.ContainsKey(errorId)
                    || normal.Terminator.FalseTargetBlockId is not { } successId
                    || !blocks.TryGetValue(successId, out GuestBasicBlock? success)
                    || instructions.Take(instructions.Length - 2).Any(instruction =>
                        instruction.Op is "call" or "call_indirect"))
                    return Fail("A normal return has an unchecked call before synchronous cleanup.",
                        out error);
                normal = success;
            }
            if (normal.Terminator.Kind != "return"
                || normal.Terminator.ReturnValueId is null
                || normal.Instructions.Any(instruction => instruction.Op is
                    "call" or "call_indirect"))
                return Fail("A normal return has an unchecked call before synchronous cleanup.",
                    out error);
            if (site.BranchingCleanup is { } branching)
            {
                if (site.CleanupBlockOrdinal != branching.EntryBlockOrdinal
                    || !branchingShared.TryGetValue(branching.ExitBlockOrdinal,
                        out (string OutcomeId, int SiteCount) shared)
                    || normal.Instructions.Count < 4)
                    return Fail("A normal return has no shared branching cleanup.", out error);
                int returnTail = normal.Instructions.Count - 4;
                GuestInstruction[] returnSuffix = normal.Instructions.Skip(returnTail).ToArray();
                if (returnSuffix[0].Op != "stack_alloc"
                    || returnSuffix[0].ResultId != normal.Terminator.ReturnValueId
                    || returnSuffix[1].Op != "constant"
                    || returnSuffix[1].ResultId is null
                    || returnSuffix[1].Constant is not { Kind: "int32", Value: "0" }
                    || returnSuffix[2].Op != "field_store"
                    || returnSuffix[2].TargetId != "field:status"
                    || !returnSuffix[2].OperandIds.SequenceEqual(new[]
                        { returnSuffix[0].ResultId!, returnSuffix[1].ResultId })
                    || returnSuffix[3].Op != "field_store"
                    || returnSuffix[3].TargetId != "field:value"
                    || returnSuffix[3].OperandIds.Count != 2
                    || returnSuffix[3].OperandIds[0] != returnSuffix[0].ResultId)
                    return Fail("The branching return has no verified value suffix.", out error);
                blocks[normal.Id] = normal with
                {
                    Instructions = normal.Instructions.Take(returnTail)
                        .Concat(new[]
                        {
                            returnSuffix[1],
                            Store(shared.OutcomeId, "status", returnSuffix[1].ResultId!),
                            Store(shared.OutcomeId, "value", returnSuffix[3].OperandIds[1]),
                        }).ToArray(),
                    Terminator = new GuestTerminator("branch", null, cleanupId,
                        null, null),
                };
                if (!normalBranchEntrances.TryGetValue(branching.ExitBlockOrdinal,
                        out HashSet<string>? entrances))
                    normalBranchEntrances.Add(branching.ExitBlockOrdinal,
                        entrances = new HashSet<string>(StringComparer.Ordinal));
                entrances.Add(normal.Id);
                continue;
            }
            if (cleanup.Terminator.Kind != "return"
                || cleanup.Instructions.Count < 4
                || cleanup.Instructions.Any(instruction => instruction.Op is
                    "call" or "call_indirect"))
                return Fail("A normal return has no unique synchronous cleanup.",
                    out error);
            int tail = cleanup.Instructions.Count - 4;
            GuestInstruction[] suffix = cleanup.Instructions.Skip(tail).ToArray();
            if (tail == 0 || suffix[0].Op != "stack_alloc"
                || suffix[0].ResultId != cleanup.Terminator.ReturnValueId
                || suffix[1].Op != "constant"
                || suffix[1].Constant is not { Kind: "int32", Value: "0" }
                || suffix[2].Op != "field_store"
                || suffix[2].TargetId != "field:status"
                || suffix[2].OperandIds.Count != 2
                || suffix[2].OperandIds[0] != suffix[0].ResultId
                || suffix[2].OperandIds[1] != suffix[1].ResultId
                || suffix[3].Op != "field_store"
                || suffix[3].TargetId != "field:value"
                || suffix[3].OperandIds.Count != 2
                || suffix[3].OperandIds[0] != suffix[0].ResultId)
                return Fail("The normal return cleanup has no verified return suffix.",
                    out error);
            Dictionary<string, string> renamed = new(StringComparer.Ordinal);
            List<GuestInstruction> copied = new();
            foreach (GuestInstruction instruction in cleanup.Instructions.Take(tail))
            {
                string[] operands = instruction.OperandIds.Select(id =>
                    renamed.TryGetValue(id, out string? replacement) ? replacement : id)
                    .ToArray();
                string? target = instruction.TargetId is { } oldTarget
                    && renamed.TryGetValue(oldTarget, out string? newTarget)
                        ? newTarget : instruction.TargetId;
                string? result = null;
                if (instruction.ResultId is { } oldResult)
                {
                    if (!registerTypes.TryGetValue(oldResult, out string? typeId))
                        return Fail("The normal return cleanup has an untyped value.",
                            out error);
                    result = "normal_cleanup:"
                        + site.BlockOrdinal.ToString(CultureInfo.InvariantCulture)
                        + ":" + oldResult;
                    if (!registerIds.Add(result))
                        return Fail("The normal return cleanup reused a register.",
                            out error);
                    locals.Add(new GuestRegister(result, typeId));
                    renamed.Add(oldResult, result);
                }
                copied.Add(instruction with
                {
                    ResultId = result,
                    OperandIds = operands,
                    TargetId = target,
                });
            }
            blocks[normal.Id] = normal with
            {
                Instructions = normal.Instructions.Concat(copied).ToArray(),
            };
        }
        Dictionary<int, (string OutcomeId, int SiteCount)> sharedCleanups =
            new(branchingShared);
        foreach (IGrouping<int, CSharpLocalThrowSite> group in sites
            .Where(site => site.ReplacesThrowBlockOrdinal is null
                && site.CleanupBlockOrdinals is { Count: > 0 })
            .GroupBy(site => site.CleanupBlockOrdinals![^1]))
        {
            CSharpLocalThrowSite[] owners = group.ToArray();
            if (owners.Length < 2) continue;
            IReadOnlyList<int> route = owners[0].CleanupBlockOrdinals!;
            if (owners.Length > 16 || owners.Any(site =>
                    site.ReplacesThrowBlockOrdinal is not null
                    || !site.CleanupBlockOrdinals!.SequenceEqual(route)))
                return Fail("Shared cleanup needs one bounded source-derived route.",
                    out error);
            string outcomeId = "local_throw:shared_cleanup:"
                + group.Key.ToString(CultureInfo.InvariantCulture) + ":outcome";
            if (!registerIds.Add(outcomeId)
                || !blocks.TryGetValue(function.EntryBlockId,
                    out GuestBasicBlock? entry))
                return Fail("Shared cleanup has no unique entry outcome.", out error);
            locals.Add(new GuestRegister(outcomeId, function.ReturnTypeId));
            blocks[function.EntryBlockId] = entry with
            {
                Instructions = new[]
                {
                    new GuestInstruction("stack_alloc", outcomeId,
                        Array.Empty<string>(), null, null, null),
                }.Concat(entry.Instructions).ToArray(),
            };
            sharedCleanups.Add(group.Key, (outcomeId, owners.Length));
        }
        HashSet<string> preparedCleanupReturns = new(StringComparer.Ordinal);
        foreach (IGrouping<int, CSharpNormalReturnCleanupSite> group in normalReturns
            .Where(site => site.BranchingCleanup is not null)
            .GroupBy(site => site.BranchingCleanup!.ExitBlockOrdinal))
        {
            if (sites.Any(site => site.BranchingCleanup?.ExitBlockOrdinal == group.Key))
                continue;
            CSharpBranchingCleanup route = group.First().BranchingCleanup!;
            string exitId = CSharpGuestIds.Block(flow.MethodSymbolId, group.Key);
            if (!normalBranchEntrances.TryGetValue(group.Key,
                    out HashSet<string>? owners)
                || !branchingShared.TryGetValue(group.Key,
                    out (string OutcomeId, int SiteCount) shared)
                || !ValidBranchingCleanup(flow, blocks, owners, route)
                || !blocks.TryGetValue(exitId, out GuestBasicBlock? cleanup)
                || !TryRewriteSyntheticCleanupReturn(cleanup, shared.OutcomeId,
                    out GuestBasicBlock? rewritten))
                return Fail("Normal returns need one verified branching cleanup exit.",
                    out error);
            blocks[exitId] = rewritten!;
            preparedCleanupReturns.Add(exitId);
        }

        foreach (CSharpLocalThrowSite site in sites)
        {
            string blockId = CSharpGuestIds.Block(flow.MethodSymbolId, site.BlockOrdinal);
            IReadOnlyList<int> cleanupOrdinals = site.CleanupBlockOrdinals
                ?? Array.Empty<int>();
            if (cleanupOrdinals.Count > 16
                || cleanupOrdinals.Distinct().Count() != cleanupOrdinals.Count
                || cleanupOrdinals.Contains(site.BlockOrdinal))
                return Fail("The local throw has an invalid cleanup route.", out error);
            string[] cleanupIds = cleanupOrdinals.Select(ordinal =>
                CSharpGuestIds.Block(flow.MethodSymbolId, ordinal)).ToArray();
            CSharpBranchingCleanup? branchingCleanup = site.BranchingCleanup;
            if (branchingCleanup is not null && (cleanupOrdinals.Count != 0
                || site.ReplacesThrowBlockOrdinal is not null))
                return Fail("A branching cleanup cannot share or replace another cleanup route.",
                    out error);
            string? cleanupId = branchingCleanup is null
                ? cleanupIds.FirstOrDefault()
                : CSharpGuestIds.Block(flow.MethodSymbolId,
                    branchingCleanup.EntryBlockOrdinal);
            string? replacedOutcome = site.ReplacesThrowBlockOrdinal is { } replacedOrdinal
                ? "local_throw:" + replacedOrdinal.ToString(CultureInfo.InvariantCulture) + ":outcome"
                : null;
            bool replacementContinuesCleanup = sites.Any(other =>
                other.ReplacesThrowBlockOrdinal == site.BlockOrdinal
                && cleanupOrdinals.Count > 1
                && cleanupOrdinals.Take(cleanupOrdinals.Count - 1)
                    .Contains(other.BlockOrdinal)
                && other.CleanupBlockOrdinals is { } remaining
                && remaining.SequenceEqual(cleanupOrdinals.SkipWhile(ordinal =>
                    ordinal != other.BlockOrdinal).Skip(1)));
            bool replacesLastCleanup = replacedOutcome is not null && cleanupId is null
                && sites.Any(other => other.BlockOrdinal == site.ReplacesThrowBlockOrdinal
                    && other.CleanupBlockOrdinals is { Count: > 0 } cleanups
                    && cleanups[^1] == site.BlockOrdinal);
            bool replacesIntermediateCleanup = replacedOutcome is not null
                && cleanupId is not null
                && sites.Any(other => other.BlockOrdinal == site.ReplacesThrowBlockOrdinal
                    && other.CleanupBlockOrdinals is { Count: > 1 } cleanups
                    && cleanups.Take(cleanups.Count - 1).Contains(site.BlockOrdinal)
                    && site.CleanupBlockOrdinals is { } remaining
                    && remaining.SequenceEqual(cleanups.SkipWhile(ordinal =>
                        ordinal != site.BlockOrdinal).Skip(1)));
            if (!blocks.TryGetValue(blockId, out GuestBasicBlock? original)
                || cleanupId is null && original.Terminator.Kind != "return"
                || cleanupId is not null && (original.Terminator.Kind != "branch"
                    || original.Terminator.TargetBlockId != cleanupId)
                || replacedOutcome is not null && (
                    cleanupId is null && original.Terminator.ReturnValueId != replacedOutcome
                    || !replacesLastCleanup && !replacesIntermediateCleanup
                    || original.Instructions.Any(instruction => instruction.Op is not
                        ("constant" or "global_load" or "binary" or "global_store")))
                || replacedOutcome is null && original.Instructions.Any(instruction =>
                    instruction.Op is not ("constant" or "stack_alloc" or "field_store")
                    || instruction.Op == "field_store"
                    && instruction.TargetId is not ("field:status" or "field:value")))
                return Fail("The local throw placeholder contains an unaccounted side effect.", out error);
            CSharpLanguageErrorTypeToken? type = catalog.Types.SingleOrDefault(item =>
                item.TypeId == site.Site.ExceptionTypeId);
            CSharpLanguageErrorSourceToken? source = catalog.Sources.SingleOrDefault(item =>
                item.SourceId == flow.SourceId && item.Span == site.Site.Span);
            if (type is null || source is null)
                return Fail("The local throw is absent from the source token catalog.", out error);
            CSharpLanguageCatchMatch? catchMatch = routes.TryGetValue(blockId,
                    out CSharpLanguageCatchRoute? route)
                ? route.Matches.SingleOrDefault(match => match.TypeToken == type.Token)
                : null;
            string? handler = catchMatch?.HandlerBlockId;
            if (handler is not null && !blocks.ContainsKey(handler))
                return Fail("The local catch target is absent from the lowered function.", out error);
            if (cleanupId is not null && handler is not null)
                return Fail("A local throw cannot skip cleanup to enter a catch.", out error);

            string prefix = "local_throw:" + site.BlockOrdinal.ToString(CultureInfo.InvariantCulture) + ":";
            bool shared = false;
            int sharedSiteCount = 1;
            string outcome = prefix + "outcome";
            int? sharedKey = branchingCleanup?.ExitBlockOrdinal
                ?? (cleanupOrdinals.Count != 0 ? cleanupOrdinals[^1] : null);
            if (sharedKey is { } key
                && sharedCleanups.TryGetValue(key,
                    out (string OutcomeId, int SiteCount) sharedCleanup))
            {
                shared = true;
                sharedSiteCount = sharedCleanup.SiteCount;
                outcome = sharedCleanup.OutcomeId;
            }
            string root = prefix + "root";
            string status = prefix + "status", errorType = prefix + "type";
            string sourceToken = prefix + "source";
            List<GuestRegister> added = new()
            {
                new(root, "type:language_error_root"),
                new(status, "type:int32"),
                new(errorType, "type:int32"),
                new(sourceToken, "type:int32"),
            };
            if (!shared) added.Insert(0, new GuestRegister(outcome, function.ReturnTypeId));
            if (added.Any(register => !registerIds.Add(register.Id)))
                return Fail("The local throw register identity is already in use.", out error);
            locals.AddRange(added);
            List<GuestInstruction> instructions = new()
            {
                Constant(status, GuestLanguageOutcomeType.LanguageErrorStatus),
                Constant(errorType, type.Token),
                Constant(sourceToken, source.Token),
                new("managed_new", root, Array.Empty<string>(), null, null, null),
                new("managed_set", null, new[] { root, errorType }, "field:code", null, null),
                Store(outcome, "status", status),
                Store(outcome, "error_type", errorType),
                Store(outcome, "source", sourceToken),
                Store(outcome, "error_root", root),
            };
            if (!shared)
                instructions.Insert(0, new GuestInstruction("stack_alloc", outcome,
                    Array.Empty<string>(), null, null, null));
            if (catchMatch is { CaptureError: true })
            {
                string capture = CSharpLanguageCatchContext.OutcomeRegister(handler!);
                if (!locals.Any(register => register.Id == capture
                    && register.TypeId == function.ReturnTypeId))
                    return Fail("The local catch has no owned error context.", out error);
                instructions.AddRange(new[]
                {
                    Store(capture, "status", status),
                    Store(capture, "error_type", errorType),
                    Store(capture, "source", sourceToken),
                    Store(capture, "error_root", root),
                });
            }
            blocks[blockId] = original with
            {
                Instructions = replacedOutcome is null
                    ? instructions : original.Instructions.Concat(instructions).ToArray(),
                Terminator = cleanupId is not null
                    ? new GuestTerminator("branch", null, cleanupId, null, null)
                    : handler is null
                    ? new GuestTerminator("return", null, null, null, outcome)
                    : new GuestTerminator("branch", null, handler, null, null),
            };
            if (cleanupId is not null)
            {
                if (branchingCleanup is not null)
                {
                    HashSet<string> owners = sites.Where(other =>
                            other.BranchingCleanup?.ExitBlockOrdinal
                                == branchingCleanup.ExitBlockOrdinal)
                        .Select(other => CSharpGuestIds.Block(flow.MethodSymbolId,
                            other.BlockOrdinal)).ToHashSet(StringComparer.Ordinal);
                    if (normalBranchEntrances.TryGetValue(
                            branchingCleanup.ExitBlockOrdinal,
                            out HashSet<string>? normalOwners))
                        owners.UnionWith(normalOwners);
                    if (!ValidBranchingCleanup(flow, blocks, owners,
                            branchingCleanup))
                        return Fail("The local throw has no bounded branching cleanup graph.",
                            out error);
                }
                for (int index = 0; branchingCleanup is null && index < cleanupIds.Length;
                    ++index)
                {
                    string id = cleanupIds[index];
                    bool last = index == cleanupIds.Length - 1;
                    if (!blocks.TryGetValue(id, out GuestBasicBlock? current)
                        || current.Instructions.Any(instruction => instruction.Op is
                            "call" or "call_indirect")
                        || function.Blocks.Count(block => block.Terminator.Kind == "branch"
                            && block.Terminator.TargetBlockId == id)
                            != (index == 0 ? sharedSiteCount : 1)
                        || !last && (current.Terminator.Kind != "branch"
                            || current.Terminator.TargetBlockId != cleanupIds[index + 1])
                        || last && current.Terminator.Kind != "return")
                        return Fail("The local throw has no unique linear cleanup chain.",
                            out error);
                }
                if (replacementContinuesCleanup)
                    continue;
                string lastCleanupId = branchingCleanup is null ? cleanupIds[^1]
                    : CSharpGuestIds.Block(flow.MethodSymbolId,
                        branchingCleanup.ExitBlockOrdinal);
                GuestBasicBlock cleanup = blocks[lastCleanupId];
                if (!preparedCleanupReturns.Add(lastCleanupId))
                {
                    if (!shared || cleanup.Terminator.ReturnValueId != outcome)
                        return Fail("Shared cleanup changed its error outcome.", out error);
                    continue;
                }
                if (!TryRewriteSyntheticCleanupReturn(cleanup, outcome,
                        out GuestBasicBlock? rewritten))
                    return Fail("The cleanup's synthetic normal return changed shape.", out error);
                blocks[lastCleanupId] = rewritten!;
            }
        }
        foreach (CSharpRethrowSite site in rethrows)
        {
            string blockId = CSharpGuestIds.Block(flow.MethodSymbolId, site.BlockOrdinal);
            string capture = CSharpLanguageCatchContext.OutcomeRegister(blockId);
            CSharpLanguageCatchMatch? outerMatch = routes.TryGetValue(blockId,
                    out CSharpLanguageCatchRoute? outerRoute)
                ? outerRoute.Matches.SingleOrDefault(match =>
                    catalog.Types.Count == 1 && match.TypeToken == catalog.Types[0].Token)
                : null;
            if (!blocks.TryGetValue(blockId, out GuestBasicBlock? original)
                || original.Terminator.Kind != "return"
                || original.Terminator.ReturnValueId is null
                || original.Instructions.Any(instruction => instruction.Op is not
                    ("constant" or "stack_alloc" or "field_store")
                    || instruction.Op == "field_store"
                        && instruction.TargetId is not ("field:status" or "field:value"))
                || !locals.Any(register => register.Id == capture
                    && register.TypeId == function.ReturnTypeId)
                || catalog.Types.Count != 1
                || !routes.Values.SelectMany(route => route.Matches).Any(match =>
                    match.HandlerBlockId == blockId && match.CaptureError))
                return Fail("The rethrow has no unique captured error context.", out error);
            List<GuestInstruction> forwarding = new();
            if (outerMatch is { CaptureError: true })
            {
                string outerCapture = CSharpLanguageCatchContext.OutcomeRegister(
                    outerMatch.HandlerBlockId);
                if (outerCapture == capture || !locals.Any(register =>
                    register.Id == outerCapture && register.TypeId == function.ReturnTypeId))
                    return Fail("The outer rethrow has no owned error context.", out error);
                string statusId = "rethrow:" + site.BlockOrdinal.ToString(
                    CultureInfo.InvariantCulture) + ":status";
                if (!registerIds.Add(statusId))
                    return Fail("The rethrow register identity is already in use.", out error);
                locals.Add(new GuestRegister(statusId, "type:int32"));
                forwarding.Add(Constant(statusId, GuestLanguageOutcomeType.LanguageErrorStatus));
                forwarding.Add(Store(outerCapture, "status", statusId));
                foreach ((string field, string typeId) in new[]
                {
                    ("error_type", "type:int32"),
                    ("source", "type:int32"),
                    ("error_root", "type:language_error_root"),
                })
                {
                    string id = "rethrow:" + site.BlockOrdinal.ToString(
                        CultureInfo.InvariantCulture) + ":" + field;
                    if (!registerIds.Add(id))
                        return Fail("The rethrow register identity is already in use.", out error);
                    locals.Add(new GuestRegister(id, typeId));
                    forwarding.Add(new GuestInstruction("field_load", id,
                        new[] { capture }, "field:" + field, null, null));
                    forwarding.Add(Store(outerCapture, field, id));
                }
            }
            blocks[blockId] = original with
            {
                Instructions = forwarding,
                Terminator = outerMatch is null
                    ? new GuestTerminator("return", null, null, null, capture)
                    : new GuestTerminator("branch", null,
                        outerMatch.HandlerBlockId, null, null),
            };
        }
        lowered = function with
        {
            Locals = locals,
            Blocks = function.Blocks.Select(block => blocks[block.Id]).ToArray(),
        };
        return true;
    }

    private static bool ValidBranchingCleanup(
        SemanticExceptionFlow flow,
        IReadOnlyDictionary<string, GuestBasicBlock> blocks,
        IReadOnlySet<string> entryOwners,
        CSharpBranchingCleanup cleanup)
    {
        int count = cleanup.ExitBlockOrdinal - cleanup.EntryBlockOrdinal + 1;
        if (count is < 3 or > 16
            || !cleanup.BlockOrdinals.SequenceEqual(
                Enumerable.Range(cleanup.EntryBlockOrdinal, count)))
            return false;
        string[] ids = cleanup.BlockOrdinals.Select(ordinal =>
            CSharpGuestIds.Block(flow.MethodSymbolId, ordinal)).ToArray();
        HashSet<string> cleanupIds = ids.ToHashSet(StringComparer.Ordinal);
        string entryId = ids[0], exitId = ids[^1];
        HashSet<string> reached = new(StringComparer.Ordinal) { entryId };
        for (int index = 0; index < ids.Length; ++index)
        {
            string id = ids[index];
            if (!reached.Contains(id) || !blocks.TryGetValue(id, out GuestBasicBlock? block)
                || block.Instructions.Any(instruction => instruction.Op is not
                    ("constant" or "global_load" or "global_store" or "local_load"
                        or "local_store" or "field_load" or "field_store" or "binary"
                        or "unary" or "stack_alloc")))
                return false;
            if (id == exitId)
            {
                if (block.Terminator.Kind != "return") return false;
                continue;
            }
            string[] targets = block.Terminator.Kind switch
            {
                "branch" when block.Terminator.TargetBlockId is not null =>
                    new[] { block.Terminator.TargetBlockId },
                "branch_if" when block.Terminator.TargetBlockId is not null
                    && block.Terminator.FalseTargetBlockId is not null
                    && block.Terminator.ConditionValueId is not null =>
                    new[] { block.Terminator.TargetBlockId,
                        block.Terminator.FalseTargetBlockId },
                _ => Array.Empty<string>(),
            };
            if (targets.Length is < 1 or > 2
                || targets.Distinct(StringComparer.Ordinal).Count() != targets.Length
                || targets.Any(target => !cleanupIds.Contains(target)
                    || Array.IndexOf(ids, target) <= index))
                return false;
            reached.UnionWith(targets);
        }
        foreach (GuestBasicBlock block in blocks.Values)
        {
            string?[] targets =
            {
                block.Terminator.TargetBlockId,
                block.Terminator.FalseTargetBlockId,
            };
            if (targets.Any(target => target is not null && cleanupIds.Contains(target)
                && !cleanupIds.Contains(block.Id)
                && (target != entryId || !entryOwners.Contains(block.Id))))
                return false;
        }
        return reached.Count == ids.Length;
    }

    private static bool TryRewriteSyntheticCleanupReturn(
        GuestBasicBlock cleanup, string outcome, out GuestBasicBlock? rewritten)
    {
        rewritten = null;
        if (cleanup.Terminator.Kind != "return"
            || cleanup.Instructions.Count < 4)
            return false;
        int tail = cleanup.Instructions.Count - 4;
        GuestInstruction[] suffix = cleanup.Instructions.Skip(tail).ToArray();
        string? normalOutcome = cleanup.Terminator.ReturnValueId;
        if (normalOutcome is null || suffix[0].Op != "stack_alloc"
            || suffix[0].ResultId != normalOutcome
            || suffix[1].Op != "constant"
            || suffix[1].Constant is not { Kind: "int32", Value: "0" }
            || suffix[2].Op != "field_store" || suffix[2].TargetId != "field:status"
            || suffix[2].OperandIds.Count != 2
            || suffix[2].OperandIds[0] != normalOutcome
            || suffix[2].OperandIds[1] != suffix[1].ResultId
            || suffix[3].Op != "field_store" || suffix[3].TargetId != "field:value"
            || suffix[3].OperandIds.Count != 2
            || suffix[3].OperandIds[0] != normalOutcome)
            return false;
        rewritten = cleanup with
        {
            Instructions = cleanup.Instructions.Take(tail).ToArray(),
            Terminator = new GuestTerminator("return", null, null, null, outcome),
        };
        return true;
    }

    private static GuestInstruction Constant(string register, int value) =>
        new("constant", register, Array.Empty<string>(), null, null,
            new GuestConstant("int32", value.ToString(CultureInfo.InvariantCulture)));

    private static GuestInstruction Store(string owner, string field, string value) =>
        new("field_store", null, new[] { owner, value }, "field:" + field, null, null);

    private static bool Fail(string message, out string? error)
    {
        error = message;
        return false;
    }
}
