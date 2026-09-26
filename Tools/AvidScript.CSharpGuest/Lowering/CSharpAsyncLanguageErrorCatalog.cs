using System;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal static class CSharpAsyncLanguageErrorCatalog
{
    public static CSharpLanguageErrorTokenCatalog Build(SemanticDocument document)
    {
        var sites = document.AsyncMethods
            .SelectMany(method => method.ErrorPlan?.Throws.Select(site =>
                (method.ErrorPlan.SourceId, Site: site))
                ?? Array.Empty<(string SourceId, SemanticAsyncThrowSite Site)>())
            .Select(item => (item.SourceId, item.Site.ExceptionTypeId, item.Site.Span))
            .Concat(CSharpTaskResultAbi.SupportsCancellation(document)
                ? document.AsyncMethods
                    .SelectMany(method => method.Segments.Where(segment =>
                        CSharpAsyncCancellationLowerer.IsStatusAware(document, method, segment))
                        .Select(segment => (SourceId: document.Source.SourceId,
                            ExceptionTypeId: method.ExceptionPlan?.CancellationTypeId
                                ?? SemanticAsyncCancellationPlanValidator.CancellationTypeId, Span: segment.AwaitSite!.Span)))
                : Array.Empty<(string SourceId, string ExceptionTypeId, SemanticSpan Span)>())
            .ToArray();
        CSharpLanguageErrorTypeToken[] types = sites
            .Select(item => item.ExceptionTypeId)
            .Distinct(StringComparer.Ordinal)
            .OrderBy(id => id, StringComparer.Ordinal)
            .Select((id, index) => new CSharpLanguageErrorTypeToken(index + 1, id))
            .ToArray();
        CSharpLanguageErrorSourceToken[] sources = sites
            .Select(item => (item.SourceId, item.Span))
            .Distinct()
            .OrderBy(item => item.SourceId, StringComparer.Ordinal)
            .ThenBy(item => item.Span.Start)
            .ThenBy(item => item.Span.Length)
            .Select((item, index) => new CSharpLanguageErrorSourceToken(
                index + 1, item.SourceId, item.Span))
            .ToArray();
        return new(types, sources);
    }

    public static GuestLanguageErrorCatalog ToGuest(SemanticDocument document,
        CSharpLanguageErrorTokenCatalog tokens) => new(
        tokens.Types.Select(item => new GuestLanguageErrorTypeToken(
            item.Token, item.TypeId)).ToArray(),
        tokens.Sources.Select(item => new GuestLanguageErrorSourceToken(
            item.Token, item.SourceId, document.Source.Length,
            item.Span.Start, item.Span.Length, item.Span.Line, item.Span.Column,
            item.Span.EndLine, item.Span.EndColumn)).ToArray());
}
