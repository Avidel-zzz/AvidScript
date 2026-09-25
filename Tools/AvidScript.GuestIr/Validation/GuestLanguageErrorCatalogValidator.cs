using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;

namespace AvidScript.GuestIr;

internal static class GuestLanguageErrorCatalogValidator
{
    public const int SchemaVersion = 17;
    public const string IrVersion = "1.16";
    private const string DiagnosticCode = "ASIR1027";

    public static void Validate(GuestValidationContext context)
    {
        GuestModule module = context.Module;
        GuestLanguageErrorCatalog? catalog = module.LanguageErrorCatalog;
        bool correctVersion = module.SchemaVersion == SchemaVersion && module.IrVersion == IrVersion
            || module.SchemaVersion == GuestTaskLanguageErrorValidator.SchemaVersion
                && module.IrVersion == GuestTaskLanguageErrorValidator.IrVersion
            || module.SchemaVersion == GuestTaskLanguageErrorValidator.AsyncSchemaVersion
                && module.IrVersion == GuestTaskLanguageErrorValidator.AsyncIrVersion
            || module.SchemaVersion == GuestTaskLanguageErrorValidator.ExceptionFlowSchemaVersion
                && module.IrVersion == GuestTaskLanguageErrorValidator.ExceptionFlowIrVersion;
        if (catalog is null)
        {
            if (correctVersion) Add(context, "This Guest IR version requires a language-error token catalog.");
            return;
        }
        if (!correctVersion)
        {
            Add(context, "Language-error tokens require Guest IR 17/1.16 or the combined Task/error version.");
            return;
        }
        if (catalog.Types.Count is 0 or > 256 || catalog.Sources.Count is 0 or > 1024)
        {
            Add(context, "Language-error token counts are outside the bounded catalog.");
            return;
        }

        string? previousType = null;
        for (int index = 0; index < catalog.Types.Count; ++index)
        {
            GuestLanguageErrorTypeToken entry = catalog.Types[index];
            if (entry.Token != index + 1 || string.IsNullOrWhiteSpace(entry.TypeId)
                || !entry.TypeId.StartsWith("type:", StringComparison.Ordinal)
                || entry.TypeId.Length > 1024 || entry.TypeId.Any(char.IsControl)
                || previousType is not null
                    && string.CompareOrdinal(previousType, entry.TypeId) >= 0)
                Add(context, $"Language-error type token {index + 1} is invalid or not canonical.");
            previousType = entry.TypeId;
        }

        (string SourceId, int Start, int Length)? previousSource = null;
        Dictionary<string, int> sourceLengths = new(StringComparer.Ordinal);
        for (int index = 0; index < catalog.Sources.Count; ++index)
        {
            GuestLanguageErrorSourceToken entry = catalog.Sources[index];
            bool ordered = previousSource is null
                || string.CompareOrdinal(previousSource.Value.SourceId, entry.SourceId) < 0
                || previousSource.Value.SourceId == entry.SourceId
                    && (previousSource.Value.Start < entry.Start
                        || previousSource.Value.Start == entry.Start
                            && previousSource.Value.Length < entry.Length);
            if (entry.Token != index + 1 || string.IsNullOrWhiteSpace(entry.SourceId)
                || entry.SourceId.Length > 1024
                || entry.SourceId.IndexOfAny(new[] { '\0', '\r', '\n' }) >= 0
                || entry.SourceLength is < 1 or > 16_000_000
                || entry.Start < 0 || entry.Length < 1
                || entry.Length > entry.SourceLength
                || entry.Start > entry.SourceLength - entry.Length
                || entry.Line < 0 || entry.Column < 0
                || entry.EndLine < entry.Line || entry.EndColumn < 0
                || entry.Line > entry.SourceLength || entry.EndLine > entry.SourceLength
                || entry.Column > entry.SourceLength || entry.EndColumn > entry.SourceLength
                || entry.EndLine == entry.Line && entry.EndColumn < entry.Column
                || !ordered
                || sourceLengths.TryGetValue(entry.SourceId, out int sourceLength)
                    && sourceLength != entry.SourceLength)
                Add(context, $"Language-error source token {index + 1} has an invalid span or ordering.");
            sourceLengths.TryAdd(entry.SourceId, entry.SourceLength);
            previousSource = (entry.SourceId, entry.Start, entry.Length);
        }

        ValidateTokenStores(context, catalog);
    }

    private static void ValidateTokenStores(
        GuestValidationContext context,
        GuestLanguageErrorCatalog catalog)
    {
        HashSet<int> typeTokens = catalog.Types.Select(entry => entry.Token).ToHashSet();
        HashSet<int> sourceTokens = catalog.Sources.Select(entry => entry.Token).ToHashSet();
        HashSet<int> usedTypes = new(), usedSources = new();
        Dictionary<string, (string TypeField, string SourceField)> outcomeFields =
            context.Module.LanguageOutcomeTypes?
                .Where(item => context.Types.TryGetValue(item.TypeId, out GuestType? type)
                    && type.Fields.Count >= 3)
                .ToDictionary(item => item.TypeId, item =>
                {
                    GuestType type = context.Types[item.TypeId];
                    return (type.Fields[1].Id, type.Fields[2].Id);
                }, StringComparer.Ordinal)
            ?? new Dictionary<string, (string, string)>(StringComparer.Ordinal);
        foreach (GuestFunction function in context.Module.Functions)
        {
            Dictionary<string, string> registerTypes = function.Parameters.Concat(function.Locals)
                .GroupBy(register => register.Id, StringComparer.Ordinal)
                .ToDictionary(group => group.Key, group => group.First().TypeId,
                    StringComparer.Ordinal);
            Dictionary<string, GuestInstruction> definitions = new(StringComparer.Ordinal);
            HashSet<string> ambiguous = new(StringComparer.Ordinal);
            foreach (GuestInstruction instruction in function.Blocks.SelectMany(block => block.Instructions))
            {
                if (instruction.ResultId is { } resultId
                    && !definitions.TryAdd(resultId, instruction)) ambiguous.Add(resultId);
                if (instruction.Op is "local_store" or "address_of" or "borrow_address"
                    && instruction.TargetId is { } targetId) ambiguous.Add(targetId);
            }
            foreach (GuestInstruction store in function.Blocks.SelectMany(block => block.Instructions)
                .Where(instruction => instruction.Op == "field_store"
                    && instruction.OperandIds.Count == 2))
            {
                if (!registerTypes.TryGetValue(store.OperandIds[0], out string? ownerType)
                    || !outcomeFields.TryGetValue(ownerType, out var fields)
                    || store.TargetId != fields.TypeField && store.TargetId != fields.SourceField)
                    continue;
                bool typeField = store.TargetId == fields.TypeField;
                string value = store.OperandIds[1];
                if (ambiguous.Contains(value) || !definitions.TryGetValue(value, out GuestInstruction? origin))
                {
                    Add(context, $"Function '{function.Id}' has an untraceable language-error token.");
                    continue;
                }
                if (origin.Op == "field_load"
                    && origin.OperandIds.Count == 1
                    && registerTypes.TryGetValue(origin.OperandIds[0], out string? sourceType)
                    && outcomeFields.TryGetValue(sourceType, out var sourceFields)
                    && origin.TargetId == (typeField ? sourceFields.TypeField : sourceFields.SourceField))
                    continue;
                if (origin.Op != "constant" || origin.Constant is not { Kind: "int32", Value: { } text }
                    || !int.TryParse(text, NumberStyles.None, CultureInfo.InvariantCulture, out int token)
                    || token.ToString(CultureInfo.InvariantCulture) != text
                    || !(typeField ? typeTokens : sourceTokens).Contains(token))
                {
                    Add(context, $"Function '{function.Id}' stores an unknown language-error token.");
                    continue;
                }
                (typeField ? usedTypes : usedSources).Add(token);
            }
            if (context.Module.SchemaVersion == GuestTaskLanguageErrorValidator.AsyncSchemaVersion
                && context.Module.IrVersion == GuestTaskLanguageErrorValidator.AsyncIrVersion
                || context.Module.SchemaVersion == GuestTaskLanguageErrorValidator.ExceptionFlowSchemaVersion
                && context.Module.IrVersion == GuestTaskLanguageErrorValidator.ExceptionFlowIrVersion)
            {
                foreach (GuestInstruction call in function.Blocks.SelectMany(block => block.Instructions)
                    .Where(instruction => instruction.Op == "call"
                        && instruction.TargetId == GuestTaskLanguageErrorValidator.ImportId))
                {
                    if (call.OperandIds.Count != 4
                        || !LiteralToken(call.OperandIds[1], typeTokens, definitions,
                            ambiguous, out int typeToken)
                        || !LiteralToken(call.OperandIds[2], sourceTokens, definitions,
                            ambiguous, out int sourceToken))
                    {
                        Add(context, $"Function '{function.Id}' has an untraceable async language-error token.");
                        continue;
                    }
                    usedTypes.Add(typeToken);
                    usedSources.Add(sourceToken);
                }
            }
        }
        if (!typeTokens.SetEquals(usedTypes) || !sourceTokens.SetEquals(usedSources))
            Add(context, "Language-error catalog contains a token with no literal producer.");
    }

    private static bool LiteralToken(string register, HashSet<int> allowed,
        IReadOnlyDictionary<string, GuestInstruction> definitions,
        IReadOnlySet<string> ambiguous, out int token)
    {
        token = 0;
        return !ambiguous.Contains(register)
            && definitions.TryGetValue(register, out GuestInstruction? origin)
            && origin.Op == "constant"
            && origin.Constant is { Kind: "int32", Value: { } value }
            && int.TryParse(value, NumberStyles.None, CultureInfo.InvariantCulture,
                out token)
            && token.ToString(CultureInfo.InvariantCulture) == value
            && allowed.Contains(token);
    }

    private static void Add(GuestValidationContext context, string message) =>
        context.Add(DiagnosticCode, message);
}
