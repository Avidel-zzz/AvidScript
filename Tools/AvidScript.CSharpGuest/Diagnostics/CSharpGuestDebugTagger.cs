using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal static class CSharpGuestDebugTagger
{
    public static void TagFirstEmitted(
        IList<GuestInstruction> instructions,
        int startIndex,
        SemanticOperation operation,
        string semanticOperationId,
        string? kind = null)
    {
        TagFirstEmitted(
            instructions,
            startIndex,
            operation.Span,
            semanticOperationId,
            kind ?? Classify(operation));
    }

    public static void TagFirstEmitted(
        IList<GuestInstruction> instructions,
        int startIndex,
        SemanticSpan span,
        string semanticOperationId,
        string kind,
        bool hidden = false)
    {
        if (startIndex < 0 || startIndex >= instructions.Count)
        {
            return;
        }

        GuestInstruction first = instructions[startIndex];
        if (first.DebugLocation is not null)
        {
            return;
        }
        instructions[startIndex] = first with
        {
            DebugLocation = Create(span, semanticOperationId, kind, hidden),
        };
    }

    public static GuestDebugLocation Create(
        SemanticSpan span,
        string semanticOperationId,
        string kind,
        bool hidden = false)
    {
        return new GuestDebugLocation(
            semanticOperationId,
            kind,
            hidden,
            span.Start,
            span.Length,
            span.Line,
            span.Column,
            span.EndLine,
            span.EndColumn);
    }

    public static string OperationId(string methodSymbolId, string lane, int ordinal)
    {
        return $"{methodSymbolId}#{lane}:operation:{ordinal}";
    }

    private static string Classify(SemanticOperation operation)
    {
        if (operation.Kind == "await")
        {
            return "await";
        }
        if (operation.Kind == "return")
        {
            return "return";
        }
        if (Enumerate(operation).Any(item => item.Kind == "invocation"))
        {
            return "call";
        }
        return "statement";
    }

    private static IEnumerable<SemanticOperation> Enumerate(SemanticOperation operation)
    {
        yield return operation;
        foreach (SemanticOperation child in operation.Children)
        {
            foreach (SemanticOperation descendant in Enumerate(child))
            {
                yield return descendant;
            }
        }
    }
}
