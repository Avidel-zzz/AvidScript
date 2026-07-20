using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

public static class CSharpGuestDebugMapProjector
{
    private const string FunctionPrefix = "function:";

    public static CSharpGuestDebugMap Project(
        SemanticDocument document,
        GuestModule module,
        string guestIrSha256)
    {
        ArgumentNullException.ThrowIfNull(document);
        ArgumentNullException.ThrowIfNull(module);
        ArgumentNullException.ThrowIfNull(guestIrSha256);

        string sourceId = NormalizeSourceId(document.Source.SourceId);
        if (!IsSha256(document.Source.Sha256)
            || !IsSha256(document.Source.FrontendSha256)
            || !IsSha256(module.Provenance.SemanticSha256)
            || !IsSha256(guestIrSha256)
            || !string.Equals(module.ModuleId, $"csharp:{document.Source.SourceId}", StringComparison.Ordinal)
            || !string.Equals(module.Provenance.SourceId, document.Source.SourceId, StringComparison.Ordinal)
            || !string.Equals(module.Provenance.SourceSha256, document.Source.Sha256, StringComparison.Ordinal)
            || !string.Equals(module.Provenance.FrontendSha256, document.Source.FrontendSha256, StringComparison.Ordinal))
        {
            throw new InvalidDataException("ASDEBUG1001: Debug map provenance is invalid or does not match the Guest module.");
        }

        Dictionary<string, SemanticCallable> callables = UniqueBy(
            document.Callables,
            callable => callable.MethodSymbolId,
            "callable");
        Dictionary<string, SemanticSymbol> symbols = UniqueBy(
            document.Symbols,
            symbol => symbol.Id,
            "symbol");
        HashSet<string> functionIds = new(StringComparer.Ordinal);
        HashSet<string> methodIds = new(StringComparer.Ordinal);
        List<CSharpGuestDebugFunction> functions = new(module.Functions.Count);
        for (int ordinal = 0; ordinal < module.Functions.Count; ++ordinal)
        {
            GuestFunction function = module.Functions[ordinal];
            if (!functionIds.Add(function.Id)
                || !function.Id.StartsWith(FunctionPrefix, StringComparison.Ordinal))
            {
                throw new InvalidDataException($"ASDEBUG1002: Guest function identity '{function.Id}' is invalid or duplicated.");
            }

            string methodId = function.Id[FunctionPrefix.Length..];
            if (!methodIds.Add(methodId)
                || !callables.TryGetValue(methodId, out SemanticCallable? callable)
                || !callable.HasBody
                || !symbols.TryGetValue(methodId, out SemanticSymbol? methodSymbol))
            {
                throw new InvalidDataException($"ASDEBUG1003: Guest function '{function.Id}' has no unique source method mapping.");
            }

            int functionIndex = checked(module.Imports.Count + ordinal);
            if (methodSymbol.Kind != "method")
            {
                continue;
            }

            if (methodSymbol.ContainingSymbolId is null
                || !symbols.TryGetValue(methodSymbol.ContainingSymbolId, out SemanticSymbol? ownerSymbol)
                || ownerSymbol.Kind != "type"
                || !IsValidSpan(methodSymbol.Span))
            {
                throw new InvalidDataException($"ASDEBUG1003: Guest function '{function.Id}' has no unique source method mapping.");
            }

            string ownerName = ownerSymbol.Signature.StartsWith("global::", StringComparison.Ordinal)
                ? ownerSymbol.Signature[8..]
                : ownerSymbol.Signature;
            functions.Add(new CSharpGuestDebugFunction(
                functionIndex,
                function.Id,
                methodId,
                $"{ownerName}.{methodSymbol.Signature}",
                methodSymbol.Span));
        }

        return new CSharpGuestDebugMap(
            1,
            "1.0",
            module.ModuleId,
            new CSharpGuestDebugSource(sourceId, document.Source.Sha256),
            new CSharpGuestDebugProvenance(
                document.Source.FrontendSha256,
                module.Provenance.SemanticSha256,
                guestIrSha256),
            functions);
    }

    private static Dictionary<string, T> UniqueBy<T>(
        IEnumerable<T> values,
        Func<T, string> keySelector,
        string kind)
    {
        Dictionary<string, T> result = new(StringComparer.Ordinal);
        foreach (T value in values)
        {
            string key = keySelector(value);
            if (string.IsNullOrWhiteSpace(key) || !result.TryAdd(key, value))
            {
                throw new InvalidDataException($"ASDEBUG1002: Semantic {kind} identity '{key}' is empty or duplicated.");
            }
        }
        return result;
    }

    private static string NormalizeSourceId(string sourceId)
    {
        if (string.IsNullOrWhiteSpace(sourceId) || Path.IsPathRooted(sourceId))
        {
            throw new InvalidDataException("ASDEBUG1001: Debug source identity must be project-relative.");
        }

        string normalized = sourceId.Replace('\\', '/');
        string[] segments = normalized.Split('/', StringSplitOptions.RemoveEmptyEntries);
        if (segments.Length == 0
            || segments.Any(segment => segment is "." or "..")
            || !string.Equals(normalized, string.Join('/', segments), StringComparison.Ordinal))
        {
            throw new InvalidDataException("ASDEBUG1001: Debug source identity is not canonical or escapes its project root.");
        }
        return normalized;
    }

    private static bool IsSha256(string value)
    {
        return value.Length == 64 && value.All(character =>
            character is >= '0' and <= '9' or >= 'a' and <= 'f');
    }

    private static bool IsValidSpan(SemanticSpan span)
    {
        return span.Start >= 0
            && span.Length >= 0
            && span.Line >= 0
            && span.Column >= 0
            && span.EndLine >= span.Line
            && span.EndColumn >= 0
            && (span.EndLine > span.Line || span.EndColumn >= span.Column);
    }
}
