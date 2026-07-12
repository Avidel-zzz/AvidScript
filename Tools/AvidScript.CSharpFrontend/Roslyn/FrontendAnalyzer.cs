using System;
using System.Globalization;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.Text;

namespace AvidScript.CSharpFrontend;

public static class FrontendAnalyzer
{
    private const int SchemaVersion = 1;
    private const string Language = "csharp";
    private const string FrontendVersion = "1.0";

    private static readonly CSharpParseOptions ParseOptions = new(
        languageVersion: LanguageVersion.CSharp12,
        documentationMode: DocumentationMode.Parse,
        kind: SourceCodeKind.Regular);

    public static FrontendDocument Analyze(string source, string sourceId)
    {
        ArgumentNullException.ThrowIfNull(source);
        ArgumentNullException.ThrowIfNull(sourceId);

        string normalizedSourceId = NormalizeSourceId(sourceId);
        SourceText sourceText = SourceText.From(source, new UTF8Encoding(false));
        SyntaxTree tree = CSharpSyntaxTree.ParseText(
            sourceText,
            ParseOptions,
            normalizedSourceId);
        SyntaxNode root = tree.GetRoot();

        FrontendToken[] tokens = root.DescendantTokens(descendIntoTrivia: false)
            .Select(token => new FrontendToken(
                token.Kind().ToString(),
                token.Text,
                token.ValueText,
                CreateSpan(sourceText, token.Span)))
            .ToArray();

        FrontendTrivia[] trivia = root.DescendantTrivia(descendIntoTrivia: false)
            .Select(item => new FrontendTrivia(
                item.Kind().ToString(),
                item.ToFullString(),
                CreateSpan(sourceText, item.Span)))
            .ToArray();

        FrontendDiagnostic[] diagnostics = tree.GetDiagnostics()
            .Select(item => CreateDiagnostic(sourceText, item))
            .OrderBy(item => item.Span.Start)
            .ThenBy(item => item.Span.Length)
            .ThenBy(item => item.Code, StringComparer.Ordinal)
            .ThenBy(item => item.Message, StringComparer.Ordinal)
            .ToArray();

        bool succeeded = diagnostics.All(item => item.Severity != "error");
        return new FrontendDocument(
            SchemaVersion,
            Language,
            FrontendVersion,
            new FrontendSource(
                normalizedSourceId,
                ComputeSha256(source),
                source.Length),
            succeeded,
            new FrontendSyntax(root.Kind().ToString(), CreateSpan(sourceText, root.FullSpan)),
            tokens,
            trivia,
            diagnostics);
    }

    private static FrontendDiagnostic CreateDiagnostic(SourceText sourceText, Diagnostic diagnostic)
    {
        TextSpan span = diagnostic.Location.IsInSource
            ? diagnostic.Location.SourceSpan
            : default;

        return new FrontendDiagnostic(
            diagnostic.Id,
            diagnostic.Severity.ToString().ToLowerInvariant(),
            diagnostic.GetMessage(CultureInfo.InvariantCulture),
            CreateSpan(sourceText, span));
    }

    private static FrontendSpan CreateSpan(SourceText sourceText, TextSpan span)
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

    private static string ComputeSha256(string source)
    {
        byte[] hash = SHA256.HashData(Encoding.UTF8.GetBytes(source));
        return Convert.ToHexString(hash).ToLowerInvariant();
    }

    private static string NormalizeSourceId(string sourceId)
    {
        return sourceId.Replace('\\', '/');
    }
}
