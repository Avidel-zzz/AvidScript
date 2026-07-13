using Microsoft.CodeAnalysis.Text;

namespace AvidScript.CSharpSemantic;

internal static class SemanticSpanFactory
{
    public static SemanticSpan Create(SourceText sourceText, TextSpan span)
    {
        LinePositionSpan lineSpan = sourceText.Lines.GetLinePositionSpan(span);
        return new SemanticSpan(
            span.Start,
            span.Length,
            lineSpan.Start.Line,
            lineSpan.Start.Character,
            lineSpan.End.Line,
            lineSpan.End.Character);
    }

    public static SemanticSpan Empty => new(0, 0, 0, 0, 0, 0);
}
