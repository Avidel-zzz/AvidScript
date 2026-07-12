using Microsoft.CodeAnalysis.Text;

namespace AvidScript.CSharpFrontend;

internal static class FrontendSpanFactory
{
    public static FrontendSpan Create(SourceText sourceText, TextSpan span)
    {
        LinePositionSpan lineSpan = sourceText.Lines.GetLinePositionSpan(span);
        return new FrontendSpan(
            span.Start,
            span.Length,
            span.End,
            lineSpan.Start.Line,
            lineSpan.Start.Character,
            lineSpan.End.Line,
            lineSpan.End.Character);
    }
}
