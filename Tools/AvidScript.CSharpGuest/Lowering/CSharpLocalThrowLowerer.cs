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
        if (sites.Count == 0 || sites.Count != flow.Throws.Count || matches.Length != 1
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

        foreach (CSharpLocalThrowSite site in sites)
        {
            string blockId = CSharpGuestIds.Block(flow.MethodSymbolId, site.BlockOrdinal);
            string? cleanupId = site.CleanupBlockOrdinal is { } cleanupOrdinal
                ? CSharpGuestIds.Block(flow.MethodSymbolId, cleanupOrdinal) : null;
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
                        && other.CleanupBlockOrdinal == site.BlockOrdinal)
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
            string? handler = routes.TryGetValue(blockId, out CSharpLanguageCatchRoute? route)
                ? route.Matches.SingleOrDefault(match => match.TypeToken == type.Token)?.HandlerBlockId
                : null;
            if (handler is not null && !blocks.ContainsKey(handler))
                return Fail("The local catch target is absent from the lowered function.", out error);
            if (cleanupId is not null && handler is not null)
                return Fail("A local throw cannot skip cleanup to enter a catch.", out error);

            string prefix = "local_throw:" + site.BlockOrdinal.ToString(CultureInfo.InvariantCulture) + ":";
            string outcome = prefix + "outcome", root = prefix + "root";
            string status = prefix + "status", errorType = prefix + "type";
            string sourceToken = prefix + "source";
            GuestRegister[] added =
            {
                new(outcome, function.ReturnTypeId),
                new(root, "type:language_error_root"),
                new(status, "type:int32"),
                new(errorType, "type:int32"),
                new(sourceToken, "type:int32"),
            };
            if (added.Any(register => !registerIds.Add(register.Id)))
                return Fail("The local throw register identity is already in use.", out error);
            locals.AddRange(added);
            GuestInstruction[] instructions =
            {
                new("stack_alloc", outcome, Array.Empty<string>(), null, null, null),
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
                if (!blocks.TryGetValue(cleanupId, out GuestBasicBlock? cleanup)
                    || cleanup.Terminator.Kind != "return"
                    || cleanup.Instructions.Count < 4
                    || cleanup.Instructions.Any(instruction => instruction.Op is "call" or "call_indirect")
                    || function.Blocks.Count(block => block.Terminator.Kind == "branch"
                        && block.Terminator.TargetBlockId == cleanupId) != 1)
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
                blocks[cleanupId] = cleanup with
                {
                    Instructions = cleanup.Instructions.Take(tail).ToArray(),
                    Terminator = new GuestTerminator("return", null, null, null, outcome),
                };
            }
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
