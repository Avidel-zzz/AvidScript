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
        if (sites.Count + rethrows.Count == 0
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
                || !blocks.TryGetValue(cleanupId, out GuestBasicBlock? cleanup)
                || normal.Terminator.Kind != "return"
                || normal.Terminator.ReturnValueId is null
                || normal.Instructions.Any(instruction => instruction.Op is
                    "call" or "call_indirect")
                || cleanup.Terminator.Kind != "return"
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
            blocks[returnId] = normal with
            {
                Instructions = normal.Instructions.Concat(copied).ToArray(),
            };
        }
        Dictionary<int, (string OutcomeId, int SiteCount)> sharedCleanups = new();
        foreach (IGrouping<int, CSharpLocalThrowSite> group in sites
            .Where(site => site.CleanupBlockOrdinals is { Count: > 0 })
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
            string? cleanupId = cleanupIds.FirstOrDefault();
            string? replacedOutcome = site.ReplacesThrowBlockOrdinal is { } replacedOrdinal
                ? "local_throw:" + replacedOrdinal.ToString(CultureInfo.InvariantCulture) + ":outcome"
                : null;
            if (!blocks.TryGetValue(blockId, out GuestBasicBlock? original)
                || cleanupId is null && original.Terminator.Kind != "return"
                || cleanupId is not null && (original.Terminator.Kind != "branch"
                    || original.Terminator.TargetBlockId != cleanupId)
                || replacedOutcome is not null && (cleanupId is not null
                    || original.Terminator.ReturnValueId != replacedOutcome
                    || !sites.Any(other => other.BlockOrdinal == site.ReplacesThrowBlockOrdinal
                        && other.CleanupBlockOrdinals is { Count: > 0 } cleanups
                        && cleanups[^1] == site.BlockOrdinal)
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
            if (cleanupOrdinals.Count != 0
                && sharedCleanups.TryGetValue(cleanupOrdinals[^1],
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
                for (int index = 0; index < cleanupIds.Length; ++index)
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
                string lastCleanupId = cleanupIds[^1];
                GuestBasicBlock cleanup = blocks[lastCleanupId];
                if (!preparedCleanupReturns.Add(lastCleanupId))
                {
                    if (!shared || cleanup.Terminator.ReturnValueId != outcome)
                        return Fail("Shared cleanup changed its error outcome.", out error);
                    continue;
                }
                if (cleanup.Instructions.Count < 4)
                    return Fail("The local throw has no unique linear cleanup return.", out error);
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
                    return Fail("The cleanup's synthetic normal return changed shape.", out error);
                blocks[lastCleanupId] = cleanup with
                {
                    Instructions = cleanup.Instructions.Take(tail).ToArray(),
                    Terminator = new GuestTerminator("return", null, null, null, outcome),
                };
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
