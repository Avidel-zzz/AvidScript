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

    public static SemanticCallableProjection Project(
        SemanticCompilationContext context,
        SemanticTypeRegistry typeRegistry)
    {
        SemanticModel semanticModel = context.Compilation.GetSemanticModel(
            context.SyntaxTree,
            ignoreAccessibility: false);
        SyntaxNode root = context.SyntaxTree.GetRoot();
        HashSet<string> bodyIds = SemanticExecutableBodyResolver.Resolve(context)
            .Select(body => SemanticSymbolProjector.GetSymbolId(body.Method))
            .ToHashSet(StringComparer.Ordinal);
        Dictionary<string, SemanticCallable> callables = new(StringComparer.Ordinal);
        Dictionary<string, IMethodSymbol> exportOwners = new(StringComparer.Ordinal);
        List<SemanticDiagnostic> diagnostics = new();

        foreach (IMethodSymbol method in EnumerateDeclaredMethods(root, semanticModel))
        {
            string methodId = SemanticSymbolProjector.GetSymbolId(method);
            SemanticCallableImport? import = ProjectImport(method, diagnostics, context);
            SemanticCallableExport? export = ProjectExport(method, diagnostics, context);
            if (export is not null && !exportOwners.TryAdd(export.Name, method))
            {
                diagnostics.Add(CreateDiagnostic(
                    "ASCS5003",
                    $"WASM export name '{export.Name}' is declared by more than one callable.",
                    method,
                    context));
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
                bodyIds.Contains(methodId),
                method.AssociatedSymbol is { } associated
                    ? SemanticSymbolProjector.GetSymbolId(associated)
                    : null,
                import,
                export);
            if (!callables.TryAdd(methodId, callable))
            {
                throw new InvalidOperationException($"Duplicate semantic callable id: {methodId}");
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
        SemanticCompilationContext context)
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
                context));
            return null;
        }

        return new SemanticCallableImport(module, name);
    }

    private static SemanticCallableExport? ProjectExport(
        IMethodSymbol method,
        ICollection<SemanticDiagnostic> diagnostics,
        SemanticCompilationContext context)
    {
        AttributeData? attribute = FindAttribute(method, UnmanagedCallersOnlyAttributeName);
        if (attribute is null)
        {
            return null;
        }

        string? name = GetNamedString(attribute, "EntryPoint");
        if (string.IsNullOrWhiteSpace(name))
        {
            diagnostics.Add(CreateDiagnostic(
                "ASCS5002",
                "UnmanagedCallersOnly requires a constant non-empty EntryPoint for AvidScript exports.",
                method,
                context));
            return null;
        }

        return new SemanticCallableExport(name);
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
        SemanticCompilationContext context)
    {
        Location? location = method.Locations.FirstOrDefault(item =>
            item.IsInSource && item.SourceTree == context.SyntaxTree);
        SemanticSpan span = location is null
            ? SemanticSpanFactory.Empty
            : SemanticSpanFactory.Create(context.SourceText, location.SourceSpan);
        return new SemanticDiagnostic(code, "error", message, span);
    }
}
