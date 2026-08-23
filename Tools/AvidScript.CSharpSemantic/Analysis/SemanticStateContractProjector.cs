using System;
using System.Collections.Generic;
using System.Linq;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;

namespace AvidScript.CSharpSemantic;

internal static class SemanticStateContractProjector
{
    private const string CompatiblePolicy = "compatible";
    private const string ExplicitPolicy = "explicit";

    public static SemanticStateContractProjection Project(
        SemanticCompilationContext context,
        SemanticTypeRegistry typeRegistry)
    {
        ArgumentNullException.ThrowIfNull(context);
        ArgumentNullException.ThrowIfNull(typeRegistry);

        StateContractAttributes attributes = StateContractAttributes.Create(context.Compilation);
        SemanticModel semanticModel = context.Compilation.GetSemanticModel(context.PrimaryUnit.SyntaxTree);
        INamedTypeSymbol[] owners = context.PrimaryUnit.SyntaxTree.GetRoot()
            .DescendantNodes()
            .OfType<ClassDeclarationSyntax>()
            .Select(declaration => semanticModel.GetDeclaredSymbol(declaration))
            .OfType<INamedTypeSymbol>()
            .GroupBy(SemanticSymbolProjector.GetSymbolId, StringComparer.Ordinal)
            .Select(group => group.First())
            .OrderBy(SemanticSymbolProjector.GetSymbolId, StringComparer.Ordinal)
            .ToArray();
        List<SemanticStateContract> contracts = new();
        List<SemanticDiagnostic> diagnostics = new();
        foreach (INamedTypeSymbol owner in owners)
        {
            contracts.Add(ProjectOwner(context, owner, attributes, typeRegistry, diagnostics));
        }

        return new SemanticStateContractProjection(
            contracts,
            diagnostics
                .OrderBy(diagnostic => diagnostic.Span.Start)
                .ThenBy(diagnostic => diagnostic.Code, StringComparer.Ordinal)
                .ToArray());
    }

    private static SemanticStateContract ProjectOwner(
        SemanticCompilationContext context,
        INamedTypeSymbol owner,
        StateContractAttributes attributes,
        SemanticTypeRegistry typeRegistry,
        ICollection<SemanticDiagnostic> diagnostics)
    {
        AttributeData[] contractAttributes = GetAttributes(owner, attributes.Contract);
        if (contractAttributes.Length > 1)
        {
            diagnostics.Add(CreateDiagnostic(
                "ASSTATE1001",
                "State contract attributes must not be duplicated.",
                context,
                contractAttributes[1]));
        }

        AttributeData? contractAttribute = contractAttributes.FirstOrDefault();
        string policy = GetPolicy(contractAttribute);
        int version = GetVersion(contractAttribute);
        if (version is < 1 or > 65535)
        {
            diagnostics.Add(CreateDiagnostic(
                "ASSTATE1003",
                "State contract version must be within 1..65535.",
                context,
                contractAttribute));
        }

        IFieldSymbol[] fields = owner.GetMembers()
            .OfType<IFieldSymbol>()
            .Where(field => !field.IsImplicitlyDeclared)
            .OrderBy(SemanticSymbolProjector.GetSymbolId, StringComparer.Ordinal)
            .ToArray();
        HashSet<string> currentNames = fields
            .Select(field => field.Name)
            .ToHashSet(StringComparer.Ordinal);
        Dictionary<string, IFieldSymbol> aliasOwners = new(StringComparer.Ordinal);
        List<SemanticStateFieldContract> contracts = new();
        foreach (IFieldSymbol field in fields)
        {
            AttributeData[] persistAttributes = GetAttributes(field, attributes.Persist);
            AttributeData[] transientAttributes = GetAttributes(field, attributes.Transient);
            if (persistAttributes.Length > 1)
            {
                diagnostics.Add(CreateDiagnostic(
                    "ASSTATE1001",
                    "AvidPersist must not be duplicated on a field.",
                    context,
                    persistAttributes[1]));
            }
            if (transientAttributes.Length > 1)
            {
                diagnostics.Add(CreateDiagnostic(
                    "ASSTATE1001",
                    "AvidTransient must not be duplicated on a field.",
                    context,
                    transientAttributes[1]));
            }
            if (persistAttributes.Length > 0 && transientAttributes.Length > 0)
            {
                diagnostics.Add(CreateDiagnostic(
                    "ASSTATE1001",
                    "AvidPersist and AvidTransient cannot both apply to a field.",
                    context,
                    transientAttributes[0]));
            }

            string disposition = transientAttributes.Length > 0
                ? "transient"
                : persistAttributes.Length > 0
                    ? "persist"
                    : "implicit";
            AttributeData[] aliasAttributes = GetAttributes(field, attributes.Alias);
            string[] aliases = ProjectAliases(
                context,
                field,
                aliasAttributes,
                currentNames,
                aliasOwners,
                diagnostics);
            bool participates = disposition == "persist" ||
                (policy == CompatiblePolicy && disposition == "implicit");
            if (participates
                && attributes.Subscription is not null
                && SymbolEqualityComparer.Default.Equals(field.Type, attributes.Subscription))
            {
                diagnostics.Add(CreateFieldDiagnostic(
                    "ASSTATE1005",
                    "AvidSubscription is a runtime capability and must be marked AvidTransient.",
                    context,
                    field));
            }
            if (aliases.Length > 0 && !participates)
            {
                diagnostics.Add(CreateDiagnostic(
                    "ASSTATE1004",
                    "State aliases require a field that participates in state migration.",
                    context,
                    aliasAttributes[0]));
            }
            contracts.Add(new SemanticStateFieldContract(
                SemanticSymbolProjector.GetSymbolId(field),
                disposition,
                aliases));
        }

        return new SemanticStateContract(
            typeRegistry.Register(owner),
            policy,
            version,
            contracts);
    }

    private static string[] ProjectAliases(
        SemanticCompilationContext context,
        IFieldSymbol field,
        IReadOnlyList<AttributeData> attributes,
        ISet<string> currentNames,
        IDictionary<string, IFieldSymbol> aliasOwners,
        ICollection<SemanticDiagnostic> diagnostics)
    {
        HashSet<string> aliases = new(StringComparer.Ordinal);
        foreach (AttributeData attribute in attributes)
        {
            string? alias = attribute.ConstructorArguments.Length == 1
                ? attribute.ConstructorArguments[0].Value as string
                : null;
            if (string.IsNullOrWhiteSpace(alias) ||
                !SyntaxFacts.IsValidIdentifier(alias) ||
                string.Equals(alias, field.Name, StringComparison.Ordinal) ||
                currentNames.Contains(alias))
            {
                diagnostics.Add(CreateDiagnostic(
                    "ASSTATE1002",
                    "State aliases must be unique C# identifiers that do not name a current field.",
                    context,
                    attribute));
                continue;
            }
            string validAlias = alias;
            if (!aliases.Add(validAlias) ||
                (aliasOwners.TryGetValue(validAlias, out IFieldSymbol? existing) &&
                    !SymbolEqualityComparer.Default.Equals(existing, field)))
            {
                diagnostics.Add(CreateDiagnostic(
                    "ASSTATE1002",
                    "State aliases must not be duplicated or conflict with another field alias.",
                    context,
                    attribute));
                continue;
            }
            aliasOwners.TryAdd(validAlias, field);
        }

        return aliases.OrderBy(alias => alias, StringComparer.Ordinal).ToArray();
    }

    private static AttributeData[] GetAttributes(ISymbol symbol, INamedTypeSymbol? canonicalAttribute)
    {
        if (canonicalAttribute is null)
        {
            return Array.Empty<AttributeData>();
        }

        return symbol.GetAttributes()
            .Where(attribute => SymbolEqualityComparer.Default.Equals(attribute.AttributeClass, canonicalAttribute))
            .ToArray();
    }

    private static string GetPolicy(AttributeData? attribute)
    {
        if (attribute?.ConstructorArguments.Length == 1 &&
            attribute.ConstructorArguments[0].Value is int mode &&
            mode == 1)
        {
            return ExplicitPolicy;
        }

        return CompatiblePolicy;
    }

    private static int GetVersion(AttributeData? attribute)
    {
        if (attribute is null)
        {
            return 1;
        }

        foreach (KeyValuePair<string, TypedConstant> argument in attribute.NamedArguments)
        {
            if (argument.Key == "Version" && argument.Value.Value is int version)
            {
                return version;
            }
        }

        return 1;
    }

    private static SemanticDiagnostic CreateDiagnostic(
        string code,
        string message,
        SemanticCompilationContext context,
        AttributeData? attribute)
    {
        SyntaxReference? syntaxReference = attribute?.ApplicationSyntaxReference;
        return new SemanticDiagnostic(
            code,
            "error",
            message,
            syntaxReference is null
                ? SemanticSpanFactory.Empty
                : SemanticSpanFactory.Create(context.SourceText, syntaxReference.Span));
    }

    private static SemanticDiagnostic CreateFieldDiagnostic(
        string code,
        string message,
        SemanticCompilationContext context,
        IFieldSymbol field)
    {
        SyntaxReference? syntaxReference = field.DeclaringSyntaxReferences.FirstOrDefault();
        return new SemanticDiagnostic(
            code,
            "error",
            message,
            syntaxReference is null
                ? SemanticSpanFactory.Empty
                : SemanticSpanFactory.Create(context.SourceText, syntaxReference.Span));
    }

    private sealed record StateContractAttributes(
        INamedTypeSymbol? Contract,
        INamedTypeSymbol? Persist,
        INamedTypeSymbol? Transient,
        INamedTypeSymbol? Alias,
        INamedTypeSymbol? Subscription)
    {
        public static StateContractAttributes Create(Compilation compilation)
        {
            return new StateContractAttributes(
                compilation.GetTypeByMetadataName("AvidScript.AvidStateContractAttribute"),
                compilation.GetTypeByMetadataName("AvidScript.AvidPersistAttribute"),
                compilation.GetTypeByMetadataName("AvidScript.AvidTransientAttribute"),
                compilation.GetTypeByMetadataName("AvidScript.AvidStateAliasAttribute"),
                compilation.GetTypeByMetadataName("AvidScript.AvidSubscription"));
        }
    }
}
