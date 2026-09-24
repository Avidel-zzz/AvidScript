using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

public sealed record CSharpLanguageErrorTypeToken(int Token, string TypeId);
public sealed record CSharpLanguageErrorSourceToken(int Token, string SourceId, SemanticSpan Span);
public sealed record CSharpLanguageErrorTokenCatalog(
    IReadOnlyList<CSharpLanguageErrorTypeToken> Types,
    IReadOnlyList<CSharpLanguageErrorSourceToken> Sources);
public sealed record CSharpThrowProducerResult(
    GuestFunction Function,
    CSharpLanguageErrorTokenCatalog Catalog);

// First source-backed language-error producer. The accepted form is deliberately
// exact: System.Exception's zero-argument constructor has no user code to skip.
// Other constructors, handlers, cleanup, and rethrow need their own lowering.
public static class CSharpThrowProducerLowerer
{
    internal const string ExceptionTypeId = "type:global::System.Exception";
    internal const string ExceptionConstructorId =
        "symbol:method:global::System.Exception..ctor():void";
    private const string RootTypeId = "type:language_error_root";

    public static bool TryLower(
        SemanticDocument semantic,
        SemanticExceptionFlow flow,
        GuestModule module,
        out CSharpThrowProducerResult? result,
        out string? error) =>
        TryLowerCore(semantic, flow, module, replaceExisting: false, out result, out error);

    internal static bool TryLowerReplacing(
        SemanticDocument semantic,
        SemanticExceptionFlow flow,
        GuestModule module,
        out CSharpThrowProducerResult? result,
        out string? error) =>
        TryLowerCore(semantic, flow, module, replaceExisting: true, out result, out error);

    private static bool TryLowerCore(
        SemanticDocument semantic,
        SemanticExceptionFlow flow,
        GuestModule module,
        bool replaceExisting,
        out CSharpThrowProducerResult? result,
        out string? error)
    {
        result = null;
        error = null;
        if (semantic is null || flow is null || module is null
            || !SemanticExceptionFlowContractValidator.IsValid(semantic)
            || semantic.ExceptionFlows is null
            || !semantic.ExceptionFlows.Contains(flow)
            || semantic.Diagnostics.Any(diagnostic => diagnostic.Severity == "error"
                && diagnostic.Code != "ASCS3001"))
            return Fail("The source is not a validated exception diagnostic artifact.", out error);
        if (!GuestModuleValidator.Validate(module).Succeeded)
            return Fail("The target Guest module is invalid.", out error);
        SemanticCallable[] matches = semantic.Callables.Where(item =>
            item.MethodSymbolId == flow.MethodSymbolId).Take(2).ToArray();
        if (matches.Length != 1 || !matches[0].HasBody || !matches[0].IsStatic
            || matches[0].Parameters.Count != 0)
            return Fail("The throw producer requires a unique static parameterless method.", out error);
        SemanticCallable callable = matches[0];
        if (flow.Throws.Count != 1 || flow.Catches.Count != 0
            || flow.Regions.Any(region => region.Kind is not ("root" or "local_lifetime"))
            || flow.Blocks is not { Count: 3 } blocks
            || blocks[0].Kind != "entry" || blocks[1].Kind != "block"
            || blocks[2].Kind != "exit" || !blocks[1].IsReachable
            || blocks.Any(block => block.Operations.Count != 0)
            || blocks[0].BranchValue is not null || blocks[2].BranchValue is not null
            || flow.Branches.Count != 2
            || !flow.Branches.Any(branch => branch.SourceBlockOrdinal == 0
                && branch.DestinationBlockOrdinal == 1 && branch.Semantics == "regular")
            || !flow.Branches.Any(branch => branch.SourceBlockOrdinal == 1
                && branch.DestinationBlockOrdinal == -1 && branch.Semantics == "throw"))
            return Fail("The throw method needs an unsupported branch, handler, or cleanup path.", out error);
        SemanticThrowSite site = flow.Throws[0];
        SemanticOperation? expression = blocks[1].BranchValue;
        if (site.Kind != "throw" || site.ExceptionTypeId != ExceptionTypeId
            || expression is not { Kind: "object_creation", IsSupported: true }
            || expression.TypeId != ExceptionTypeId
            || expression.SymbolId != ExceptionConstructorId
            || expression.Children.Count != 0
            || expression.Span.Start < site.Span.Start
            || expression.Span.End > site.Span.End)
            return Fail("Only a zero-argument System.Exception constructor is currently executable.", out error);

        CSharpLanguageErrorTokenCatalog catalog = BuildCatalog(semantic.ExceptionFlows);
        int typeToken = catalog.Types.Single(item => item.TypeId == ExceptionTypeId).Token;
        int sourceToken = catalog.Sources.Single(item => item.SourceId == flow.SourceId
            && item.Span.Start == site.Span.Start && item.Span.Length == site.Span.Length).Token;
        GuestLanguageOutcomeType? outcome = module.LanguageOutcomeTypes?.SingleOrDefault(item =>
            item.ValueTypeId == (callable.ReturnTypeId == "type:void" ? null : callable.ReturnTypeId));
        GuestType? root = module.Types.SingleOrDefault(item => item.Id == RootTypeId);
        if (outcome is null || root is not { Kind: "managed_ref", ElementTypeId: not null }
            || !module.Types.Any(item => item.Id == outcome.TypeId)
            || !module.Types.Any(item => item.Id == root.ElementTypeId
                && item.Fields.Any(field => field.Id == "field:code"
                    && field.TypeId == "type:int32"))
            || module.Functions.Count(item => item.Id == CSharpGuestIds.Function(flow.MethodSymbolId))
                != (replaceExisting ? 1 : 0)
            || replaceExisting && module.Functions.Single(item =>
                item.Id == CSharpGuestIds.Function(flow.MethodSymbolId)).ReturnTypeId != outcome.TypeId)
            return Fail("The Guest module lacks a unique matching outcome and managed error root.", out error);

        string functionId = CSharpGuestIds.Function(flow.MethodSymbolId);
        GuestFunction function = new(functionId, Array.Empty<GuestRegister>(), new[]
        {
            new GuestRegister("throw:outcome", outcome.TypeId),
            new GuestRegister("throw:root", RootTypeId),
            new GuestRegister("throw:status", "type:int32"),
            new GuestRegister("throw:type", "type:int32"),
            new GuestRegister("throw:source", "type:int32"),
        }, outcome.TypeId, "throw:entry", new[]
        {
            new GuestBasicBlock("throw:entry", new GuestInstruction[]
            {
                new("stack_alloc", "throw:outcome", Array.Empty<string>(), null, null, null),
                Constant("throw:status", GuestLanguageOutcomeType.LanguageErrorStatus),
                Constant("throw:type", typeToken),
                Constant("throw:source", sourceToken),
                new("managed_new", "throw:root", Array.Empty<string>(), null, null, null),
                new("managed_set", null, new[] { "throw:root", "throw:type" }, "field:code", null, null),
                Store("throw:outcome", "status", "throw:status"),
                Store("throw:outcome", "error_type", "throw:type"),
                Store("throw:outcome", "source", "throw:source"),
                Store("throw:outcome", "error_root", "throw:root"),
            }, new GuestTerminator("return", null, null, null, "throw:outcome")),
        });
        result = new(function, catalog);
        return true;
    }

    internal static CSharpLanguageErrorTokenCatalog BuildCatalog(
        IReadOnlyList<SemanticExceptionFlow> flows)
    {
        CSharpLanguageErrorTypeToken[] types = flows
            .SelectMany(flow => flow.Throws.Where(site => site.Kind == "throw")
                .Select(site => site.ExceptionTypeId))
            .Where(id => id is not null)
            .Distinct(StringComparer.Ordinal)
            .OrderBy(id => id, StringComparer.Ordinal)
            .Select((id, index) => new CSharpLanguageErrorTypeToken(index + 1, id!))
            .ToArray();
        CSharpLanguageErrorSourceToken[] sources = flows
            .SelectMany(flow => flow.Throws.Where(site => site.Kind == "throw")
                .Select(site => (flow.SourceId, site.Span)))
            .Distinct()
            .OrderBy(item => item.SourceId, StringComparer.Ordinal)
            .ThenBy(item => item.Span.Start)
            .ThenBy(item => item.Span.Length)
            .Select((item, index) => new CSharpLanguageErrorSourceToken(
                index + 1, item.SourceId, item.Span))
            .ToArray();
        return new(types, sources);
    }

    private static GuestInstruction Constant(string register, int value) =>
        new("constant", register, Array.Empty<string>(), null, null,
            new GuestConstant("int32", value.ToString(System.Globalization.CultureInfo.InvariantCulture)));
    private static GuestInstruction Store(string owner, string field, string value) =>
        new("field_store", null, new[] { owner, value }, "field:" + field, null, null);
    private static bool Fail(string message, out string? error)
    {
        error = message;
        return false;
    }
}
