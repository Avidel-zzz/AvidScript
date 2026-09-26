using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

// One numbering operation for every error source in a composed execution.
// Legacy profiles retain their original synchronous/async catalog boundaries.
internal static class CSharpLanguageErrorCatalogBuilder
{
    internal static CSharpLanguageErrorTokenCatalog ForSynchronous(
        SemanticDocument document, IReadOnlyList<SemanticExceptionFlow> flows) =>
        Build(document, flows, SemanticContract.HasAsyncSynchronousExceptions(document));

    internal static CSharpLanguageErrorTokenCatalog ForAsync(SemanticDocument document) =>
        Build(document, SemanticContract.HasAsyncSynchronousExceptions(document)
            ? document.ExceptionFlows ?? Array.Empty<SemanticExceptionFlow>()
            : Array.Empty<SemanticExceptionFlow>(), includeAsync: true);

    internal static bool TryToGuest(SemanticDocument document, CSharpLanguageErrorTokenCatalog tokens,
        out GuestLanguageErrorCatalog? catalog, out string? error)
    {
        catalog = null;
        error = null;
        var sources = new[] { (SourceId: document.Source.SourceId, SourceLength: document.Source.Length) }
            .Concat(document.ExceptionFlows?.Select(flow => (flow.SourceId, flow.SourceLength))
                ?? Array.Empty<(string SourceId, int SourceLength)>())
            .Concat(document.AsyncMethods.Where(method => method.ErrorPlan is not null)
                .Select(method => (method.ErrorPlan!.SourceId, method.ErrorPlan.SourceLength)))
            .Concat(CSharpStaticExecutionContext.Find(document)?.Types.Select(type => (type.SourceId, type.SourceLength))
                ?? Array.Empty<(string SourceId, int SourceLength)>());
        Dictionary<string, int> lengths = new(StringComparer.Ordinal);
        foreach (var source in sources)
        {
            if (string.IsNullOrWhiteSpace(source.SourceId) || source.SourceLength < 0
                || lengths.TryGetValue(source.SourceId, out int length) && length != source.SourceLength)
            {
                error = "Exception sources disagree on a source unit's identity or length.";
                return false;
            }
            lengths[source.SourceId] = source.SourceLength;
        }
        if (tokens.Sources.Any(site => !lengths.TryGetValue(site.SourceId, out int length)
            || site.Span.Start < 0 || site.Span.Length < 0 || site.Span.Start > length - site.Span.Length))
        {
            error = "An exception source token has no matching source unit or lies outside it.";
            return false;
        }
        catalog = new(tokens.Types.Select(type => new GuestLanguageErrorTypeToken(type.Token, type.TypeId)).ToArray(),
            tokens.Sources.Select(site => new GuestLanguageErrorSourceToken(site.Token, site.SourceId,
                lengths[site.SourceId], site.Span.Start, site.Span.Length, site.Span.Line, site.Span.Column,
                site.Span.EndLine, site.Span.EndColumn)).ToArray());
        return true;
    }

    private static CSharpLanguageErrorTokenCatalog Build(SemanticDocument document,
        IReadOnlyList<SemanticExceptionFlow> flows, bool includeAsync)
    {
        var sites = includeAsync ? AsyncSites(document).ToArray()
            : Array.Empty<(string SourceId, string TypeId, SemanticSpan Span)>();
        IEnumerable<string> types = sites.Select(site => site.TypeId);
        IEnumerable<(string SourceId, SemanticSpan Span)> sources = sites
            .Select(site => (site.SourceId, site.Span));
        if (CSharpStaticExecutionContext.Find(document) is { } staticContext)
        {
            types = types.Append(CSharpStaticInitializationGuards.ExceptionType);
            sources = sources.Concat(staticContext.Types.Select(type => (type.SourceId, type.Span)));
        }
        return CSharpThrowProducerLowerer.BuildCatalog(flows, types, sources);
    }

    private static IEnumerable<(string SourceId, string TypeId, SemanticSpan Span)> AsyncSites(
        SemanticDocument document)
    {
        foreach (var method in document.AsyncMethods)
        {
            if (method.ErrorPlan is { } errors)
                foreach (var site in errors.Throws)
                    yield return (errors.SourceId, site.ExceptionTypeId, site.Span);
            if (CSharpTaskResultAbi.SupportsCancellation(document))
                foreach (var segment in method.Segments.Where(segment =>
                    CSharpAsyncCancellationLowerer.IsStatusAware(document, method, segment)))
                    yield return (document.Source.SourceId,
                        method.ExceptionPlan?.CancellationTypeId
                            ?? SemanticAsyncCancellationPlanValidator.CancellationTypeId,
                        segment.AwaitSite!.Span);
        }
    }
}
