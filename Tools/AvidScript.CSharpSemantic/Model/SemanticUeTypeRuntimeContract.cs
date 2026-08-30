using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Security.Cryptography;
using System.Text;

namespace AvidScript.CSharpSemantic;

public static class SemanticUeTypeRuntimeContract
{
    public const string HostModule = "avidscript";
    public const string ExportPrefix = "avid_ue_";
    public const string PropertyImportPrefix = "avid_ue_property_";

    public static string GetFunctionExportName(string methodSymbolId)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(methodSymbolId);
        string hash = Convert.ToHexString(
            SHA256.HashData(Encoding.UTF8.GetBytes(methodSymbolId))).ToLowerInvariant();
        return ExportPrefix + hash[..32];
    }

    public static IReadOnlyDictionary<string, int> BuildMemberOrdinals(
        SemanticUeTypeDeclaration declaration)
    {
        ArgumentNullException.ThrowIfNull(declaration);
        return declaration.Properties
            .Select(property => property.SymbolId)
            .Concat(declaration.Functions.Select(function => function.MethodSymbolId))
            .OrderBy(id => id, StringComparer.Ordinal)
            .Select((id, ordinal) => new { id, ordinal })
            .ToDictionary(item => item.id, item => item.ordinal, StringComparer.Ordinal);
    }

    public static IReadOnlyList<SemanticUePropertyRuntimePlan> BuildPropertyPlans(
        SemanticDocument document)
    {
        ArgumentNullException.ThrowIfNull(document);
        IReadOnlyDictionary<string, SemanticSymbol> symbols = document.Symbols.ToDictionary(
            symbol => symbol.Id,
            StringComparer.Ordinal);
        ILookup<string?, SemanticCallable> callablesByAssociatedSymbol = document.Callables
            .ToLookup(callable => callable.AssociatedSymbolId, StringComparer.Ordinal);
        List<SemanticUePropertyRuntimePlan> plans = new();

        for (int typeOrdinal = 0; typeOrdinal < document.UeTypeDeclarations.Count; ++typeOrdinal)
        {
            SemanticUeTypeDeclaration declaration = document.UeTypeDeclarations[typeOrdinal];
            IReadOnlyDictionary<string, int> memberOrdinals = BuildMemberOrdinals(declaration);
            foreach (SemanticUePropertyDeclaration property in declaration.Properties)
            {
                if (!symbols.TryGetValue(property.SymbolId, out SemanticSymbol? symbol)
                    || symbol.Kind is not ("field" or "property"))
                {
                    throw new InvalidOperationException(
                        $"UE property '{property.SymbolId}' has no canonical semantic symbol.");
                }

                SemanticCallable[] accessors = callablesByAssociatedSymbol[property.SymbolId]
                    .Where(callable => !callable.IsStatic)
                    .ToArray();
                bool hasGetter = symbol.Kind == "field"
                    || accessors.Any(callable =>
                        callable.Parameters.Count == 0
                        && callable.ReturnTypeId != "type:void");
                bool hasSetter = symbol.Kind == "field"
                    ? !symbol.IsReadonly
                    : accessors.Any(callable =>
                        callable.Parameters.Count == 1
                        && callable.ReturnTypeId == "type:void");
                int memberOrdinal = memberOrdinals[property.SymbolId];
                plans.Add(new SemanticUePropertyRuntimePlan(
                    typeOrdinal,
                    memberOrdinal,
                    declaration.TypeId,
                    property.SymbolId,
                    property.TypeId,
                    hasGetter ? GetPropertyImportName(typeOrdinal, memberOrdinal, "get") : string.Empty,
                    hasSetter ? GetPropertyImportName(typeOrdinal, memberOrdinal, "set") : string.Empty));
            }
        }

        return plans;
    }

    private static string GetPropertyImportName(
        int typeOrdinal,
        int memberOrdinal,
        string access)
    {
        if (typeOrdinal < 0 || memberOrdinal < 0)
        {
            throw new ArgumentOutOfRangeException(
                typeOrdinal < 0 ? nameof(typeOrdinal) : nameof(memberOrdinal));
        }
        return PropertyImportPrefix
            + typeOrdinal.ToString(CultureInfo.InvariantCulture)
            + "_"
            + memberOrdinal.ToString(CultureInfo.InvariantCulture)
            + "_"
            + access;
    }
}

public sealed record SemanticUePropertyRuntimePlan(
    int TypeOrdinal,
    int MemberOrdinal,
    string OwnerTypeId,
    string PropertySymbolId,
    string ValueTypeId,
    string GetterImportName,
    string SetterImportName);
