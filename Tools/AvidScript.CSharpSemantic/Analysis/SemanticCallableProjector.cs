using System;
using System.Collections.Generic;
using System.Linq;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp.Syntax;

namespace AvidScript.CSharpSemantic;

internal sealed record SemanticCallableProjection(
    IReadOnlyList<SemanticCallable> Callables,
    IReadOnlyList<SemanticDiagnostic> Diagnostics);

internal static class SemanticCallableProjector
{
    private const string DllImportAttributeName =
        "global::System.Runtime.InteropServices.DllImportAttribute";
    private const string UnmanagedCallersOnlyAttributeName =
        "global::System.Runtime.InteropServices.UnmanagedCallersOnlyAttribute";
    private const string AvidExportAttributeName =
        "global::AvidScript.AvidExportAttribute";
    private const string DataLaneAttributeName =
        "global::AvidScript.AvidScriptDataLaneAttribute";

    public static SemanticCallableProjection Project(
        SemanticCompilationContext context,
        SemanticTypeRegistry typeRegistry)
    {
        HashSet<string> bodyIds = SemanticExecutableBodyResolver.Resolve(context)
            .Select(body => SemanticSymbolProjector.GetSymbolId(body.Method))
            .ToHashSet(StringComparer.Ordinal);
        Dictionary<string, SemanticCallable> callables = new(StringComparer.Ordinal);
        Dictionary<string, IMethodSymbol> exportOwners = new(StringComparer.Ordinal);
        List<SemanticDiagnostic> diagnostics = new();

        foreach (SemanticCompilationUnit unit in context.ProjectionUnits)
        {
            SemanticModel semanticModel = context.Compilation.GetSemanticModel(
                unit.SyntaxTree,
                ignoreAccessibility: false);
            SyntaxNode root = unit.SyntaxTree.GetRoot();
            foreach (IMethodSymbol method in EnumerateDeclaredMethods(root, semanticModel))
            {
                string methodId = SemanticSymbolProjector.GetSymbolId(method);
                SemanticCallableImport? import = ProjectImport(method, diagnostics, unit);
                SemanticCallableExport? export = ProjectExport(method, diagnostics, unit);
                bool hasBody = bodyIds.Contains(methodId);
                SemanticCallableOptimization? optimization =
                    ProjectOptimization(method, import, hasBody, diagnostics, unit);
                if (export is not null && !exportOwners.TryAdd(export.Name, method))
                {
                    diagnostics.Add(CreateDiagnostic(
                        "ASCS5003",
                        $"WASM export name '{export.Name}' is declared by more than one callable.",
                        method,
                        unit));
                }

                SemanticCallable callable = new(
                    methodId,
                    typeRegistry.Register(method.ContainingType),
                    typeRegistry.Register(method.ReturnType),
                    method.Parameters
                        .OrderBy(parameter => parameter.Ordinal)
                        .Select(parameter => new SemanticCallableParameter(
                            parameter.Ordinal,
                            SemanticSymbolProjector.GetSymbolId(parameter),
                            parameter.Name,
                            typeRegistry.Register(parameter.Type),
                            MapRefKind(parameter.RefKind)))
                        .ToArray(),
                    method.IsStatic,
                    method.MethodKind == MethodKind.Constructor,
                    hasBody,
                    method.AssociatedSymbol is { } associated
                        ? SemanticSymbolProjector.GetSymbolId(associated)
                        : null,
                    import,
                    export,
                    optimization);
                if (!callables.TryAdd(methodId, callable))
                {
                    throw new InvalidOperationException($"Duplicate semantic callable id: {methodId}");
                }
            }
        }

        return new SemanticCallableProjection(
            callables.Values.OrderBy(callable => callable.MethodSymbolId, StringComparer.Ordinal).ToArray(),
            diagnostics
                .OrderBy(diagnostic => diagnostic.Span.Start)
                .ThenBy(diagnostic => diagnostic.Code, StringComparer.Ordinal)
                .ToArray());
    }

    private static IEnumerable<IMethodSymbol> EnumerateDeclaredMethods(
        SyntaxNode root,
        SemanticModel semanticModel)
    {
        foreach (BaseMethodDeclarationSyntax declaration in root.DescendantNodes()
            .OfType<BaseMethodDeclarationSyntax>())
        {
            if (semanticModel.GetDeclaredSymbol(declaration) is IMethodSymbol method)
            {
                yield return method;
            }
        }

        foreach (AccessorDeclarationSyntax accessor in root.DescendantNodes()
            .OfType<AccessorDeclarationSyntax>())
        {
            if (semanticModel.GetDeclaredSymbol(accessor) is IMethodSymbol method)
            {
                yield return method;
            }
        }

        foreach (PropertyDeclarationSyntax property in root.DescendantNodes()
            .OfType<PropertyDeclarationSyntax>()
            .Where(property => property.ExpressionBody is not null))
        {
            if ((semanticModel.GetDeclaredSymbol(property) as IPropertySymbol)?.GetMethod is { } getter)
            {
                yield return getter;
            }
        }

        foreach (IndexerDeclarationSyntax indexer in root.DescendantNodes()
            .OfType<IndexerDeclarationSyntax>()
            .Where(indexer => indexer.ExpressionBody is not null))
        {
            if ((semanticModel.GetDeclaredSymbol(indexer) as IPropertySymbol)?.GetMethod is { } getter)
            {
                yield return getter;
            }
        }
    }

    private static SemanticCallableImport? ProjectImport(
        IMethodSymbol method,
        ICollection<SemanticDiagnostic> diagnostics,
        SemanticCompilationUnit unit)
    {
        AttributeData? attribute = FindAttribute(method, DllImportAttributeName);
        if (attribute is null)
        {
            return null;
        }

        string? module = attribute.ConstructorArguments.FirstOrDefault().Value as string;
        string? name = GetNamedString(attribute, "EntryPoint") ?? method.Name;
        if (string.IsNullOrWhiteSpace(module) || string.IsNullOrWhiteSpace(name))
        {
            diagnostics.Add(CreateDiagnostic(
                "ASCS5001",
                "DllImport requires constant non-empty module and entry-point names.",
                method,
                unit));
            return null;
        }

        return new SemanticCallableImport(module, name);
    }

    private static SemanticCallableExport? ProjectExport(
        IMethodSymbol method,
        ICollection<SemanticDiagnostic> diagnostics,
        SemanticCompilationUnit unit)
    {
        AttributeData? unmanagedAttribute = FindAttribute(method, UnmanagedCallersOnlyAttributeName);
        AttributeData? avidAttribute = FindAttribute(method, AvidExportAttributeName);
        if (unmanagedAttribute is null && avidAttribute is null)
        {
            return null;
        }

        if (unmanagedAttribute is not null && avidAttribute is not null)
        {
            diagnostics.Add(CreateDiagnostic(
                "ASCS5005",
                "A callable must use either AvidExport or UnmanagedCallersOnly, not both.",
                method,
                unit));
            return null;
        }

        string? name = avidAttribute is not null
            ? avidAttribute.ConstructorArguments.FirstOrDefault().Value as string
            : GetNamedString(unmanagedAttribute!, "EntryPoint");
        if (string.IsNullOrWhiteSpace(name))
        {
            diagnostics.Add(CreateDiagnostic(
                "ASCS5002",
                "AvidScript exports require a constant non-empty entry-point name.",
                method,
                unit));
            return null;
        }

        return new SemanticCallableExport(name);
    }

    private static SemanticCallableOptimization? ProjectOptimization(
        IMethodSymbol method,
        SemanticCallableImport? import,
        bool hasBody,
        ICollection<SemanticDiagnostic> diagnostics,
        SemanticCompilationUnit unit)
    {
        AttributeData[] attributes = method.GetAttributes()
            .Where(attribute =>
                attribute.AttributeClass?.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat)
                    == DataLaneAttributeName)
            .ToArray();
        if (attributes.Length == 0)
        {
            return null;
        }

        AttributeData attribute = attributes[0];
        bool hasRequiredConstructor = attributes.Length == 1
            && attribute.ConstructorArguments.Length == 2
            && attribute.ConstructorArguments[0].Type?.SpecialType == SpecialType.System_String
            && attribute.ConstructorArguments[1].Type?.SpecialType == SpecialType.System_Int32
            && attribute.ConstructorArguments[0].Value is string
            && attribute.ConstructorArguments[1].Value is int;
        string? optimizationClass = hasRequiredConstructor
            ? (string?)attribute.ConstructorArguments[0].Value
            : null;
        int bindingOrdinal = hasRequiredConstructor
            ? (int)attribute.ConstructorArguments[1].Value!
            : -1;
        bool supportedClass = optimizationClass is
            "none" or "snapshot_read" or "buffered_write" or "fused_call";
        if (!hasRequiredConstructor
            || !supportedClass
            || (optimizationClass != "none" && bindingOrdinal < 0))
        {
            diagnostics.Add(CreateDiagnostic(
                "ASCS5004",
                "AvidScriptDataLane requires (string class, int ordinal), a supported class, " +
                "and a non-negative ordinal for non-'none' classes.",
                method,
                unit));
            return null;
        }

        bool directBufferedImport = !unit.IsPrimary
            && !hasBody
            && import is not null
            && string.Equals(import.Module, "avidscript", StringComparison.Ordinal);
        bool generatedPropertyWrapper = !unit.IsPrimary
            && hasBody
            && import is null
            && method.MethodKind == MethodKind.PropertySet;
        if (optimizationClass == "buffered_write"
            && !directBufferedImport
            && !generatedPropertyWrapper)
        {
            return null;
        }

        return new SemanticCallableOptimization(optimizationClass!, bindingOrdinal);
    }

    private static AttributeData? FindAttribute(IMethodSymbol method, string canonicalName)
    {
        return method.GetAttributes().SingleOrDefault(attribute =>
            attribute.AttributeClass?.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat) == canonicalName);
    }

    private static string? GetNamedString(AttributeData attribute, string name)
    {
        return attribute.NamedArguments
            .Where(argument => argument.Key == name)
            .Select(argument => argument.Value.Value as string)
            .SingleOrDefault();
    }

    private static string MapRefKind(RefKind refKind)
    {
        return refKind switch
        {
            RefKind.None => "none",
            RefKind.Ref => "ref",
            RefKind.Out => "out",
            RefKind.In => "in",
            _ => "roslyn:" + refKind.ToString().ToLowerInvariant(),
        };
    }

    private static SemanticDiagnostic CreateDiagnostic(
        string code,
        string message,
        IMethodSymbol method,
        SemanticCompilationUnit unit)
    {
        Location? location = method.Locations.FirstOrDefault(item =>
            item.IsInSource && item.SourceTree == unit.SyntaxTree);
        SemanticSpan span = location is null
            ? SemanticSpanFactory.Empty
            : SemanticSpanFactory.Create(unit.SourceText, location.SourceSpan);
        return new SemanticDiagnostic(code, "error", message, span);
    }
}
