using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;

namespace AvidScript.UeTypeGenerator;

internal static class UeTypeGenerationPlanner
{
    private static readonly IReadOnlyDictionary<string, string> NativeRootTypes =
        new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["actor"] = "AActor",
            ["actor_component"] = "UActorComponent",
            ["world_subsystem"] = "UTickableWorldSubsystem",
            ["game_instance_subsystem"] = "UGameInstanceSubsystem",
        };

    public static IReadOnlyList<UeTypeManifestEntry> Plan(
        SemanticDocument document,
        string moduleName)
    {
        if (document.SchemaVersion != SemanticContract.CurrentSchemaVersion
            || document.SemanticVersion != SemanticContract.CurrentSemanticVersion
            || !document.Succeeded
            || document.Diagnostics.Any(diagnostic => diagnostic.Severity == "error"))
        {
            throw new InvalidOperationException("Semantic artifact is failed or does not match the current UE type generator contract.");
        }
        if (!SemanticUeTypeContractValidator.TryValidate(document, out string validationError))
        {
            throw new InvalidOperationException("Semantic UE type contract is invalid: " + validationError);
        }

        Dictionary<string, string> scriptCppNames = document.UeTypeDeclarations.ToDictionary(
            declaration => declaration.TypeId,
            declaration => GetCppName(declaration.Kind, declaration.EngineName),
            StringComparer.Ordinal);
        Dictionary<string, SemanticCallable> callables = document.Callables.ToDictionary(
            callable => callable.MethodSymbolId,
            StringComparer.Ordinal);
        Dictionary<string, SemanticSymbol> symbols = document.Symbols.ToDictionary(
            symbol => symbol.Id,
            StringComparer.Ordinal);
        UeCppTypeMapper typeMapper = new(document.Types, document.TypeShapes, scriptCppNames);
        IReadOnlyDictionary<string, SemanticUePropertyRuntimePlan> propertyPlans =
            SemanticUeTypeRuntimeContract.BuildPropertyPlans(document).ToDictionary(
                plan => plan.PropertySymbolId,
                StringComparer.Ordinal);
        List<UeTypeManifestEntry> result = new(document.UeTypeDeclarations.Count);

        for (int typeOrdinal = 0; typeOrdinal < document.UeTypeDeclarations.Count; ++typeOrdinal)
        {
            SemanticUeTypeDeclaration declaration = document.UeTypeDeclarations[typeOrdinal];
            IReadOnlyDictionary<string, int> memberOrdinals =
                SemanticUeTypeRuntimeContract.BuildMemberOrdinals(declaration);
            UePropertyManifestEntry[] properties = declaration.Properties.Select(property =>
                new UePropertyManifestEntry(
                    memberOrdinals[property.SymbolId],
                    property.SymbolId,
                    property.Name,
                    typeMapper.MapProperty(property.TypeId),
                    property.Flags,
                    property.Category,
                    property.ReplicatedUsing,
                    propertyPlans[property.SymbolId].GetterImportName,
                    propertyPlans[property.SymbolId].SetterImportName)).ToArray();
            UeFunctionManifestEntry[] functions = declaration.Functions.Select(function =>
                PlanFunction(
                    declaration,
                    function,
                    memberOrdinals[function.MethodSymbolId],
                    callables[function.MethodSymbolId],
                    symbols[function.MethodSymbolId],
                    typeMapper)).ToArray();
            string baseCppName = scriptCppNames.TryGetValue(declaration.BaseTypeId, out string? scriptBase)
                ? scriptBase
                : NativeRootTypes[declaration.Kind];

            result.Add(new UeTypeManifestEntry(
                typeOrdinal,
                declaration.TypeId,
                declaration.SymbolId,
                declaration.EngineName,
                scriptCppNames[declaration.TypeId],
                $"/Script/{moduleName}.{declaration.EngineName}",
                declaration.Kind,
                baseCppName,
                declaration.Flags,
                properties,
                functions));
        }
        return result;
    }

    private static UeFunctionManifestEntry PlanFunction(
        SemanticUeTypeDeclaration owner,
        SemanticUeFunctionDeclaration function,
        int memberOrdinal,
        SemanticCallable callable,
        SemanticSymbol symbol,
        UeCppTypeMapper typeMapper)
    {
        if (callable.Parameters.Select(parameter => parameter.Ordinal)
            .SequenceEqual(Enumerable.Range(0, callable.Parameters.Count)) == false
            || callable.Parameters.Select(parameter => parameter.Name).Any(name => !IsIdentifier(name))
            || callable.Parameters.Select(parameter => parameter.Name).Distinct(StringComparer.Ordinal).Count()
                != callable.Parameters.Count)
        {
            throw new InvalidOperationException($"UFunction '{function.MethodSymbolId}' has invalid parameters.");
        }
        UeFunctionParameterEntry[] parameters = callable.Parameters.Select(parameter =>
            new UeFunctionParameterEntry(
                parameter.Ordinal,
                parameter.Name,
                typeMapper.MapCallable(parameter.TypeId, parameter.RefKind),
                parameter.RefKind)).ToArray();
        return new UeFunctionManifestEntry(
            memberOrdinal,
            function.MethodSymbolId,
            function.Name,
            GetNativeName(owner.Kind, function),
            SemanticUeTypeRuntimeContract.GetFunctionExportName(function.MethodSymbolId),
            typeMapper.MapCallable(callable.ReturnTypeId),
            parameters,
            function.Flags,
            function.Category,
            symbol.Accessibility);
    }

    private static string GetCppName(string kind, string engineName)
    {
        return (kind == "actor" ? "A" : "U") + engineName;
    }

    private static string GetNativeName(string kind, SemanticUeFunctionDeclaration function)
    {
        if (!function.Flags.Contains("lifecycle"))
        {
            return function.Name;
        }
        return (kind, function.Name) switch
        {
            ("actor_component", "Tick") => "TickComponent",
            _ => function.Name,
        };
    }

    private static bool IsIdentifier(string value)
    {
        return value.Length > 0
            && (char.IsAsciiLetter(value[0]) || value[0] == '_')
            && value.Skip(1).All(character => char.IsAsciiLetterOrDigit(character) || character == '_');
    }
}
