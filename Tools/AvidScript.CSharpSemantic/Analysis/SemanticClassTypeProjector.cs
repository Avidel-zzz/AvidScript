using System;
using System.Linq;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp.Syntax;

namespace AvidScript.CSharpSemantic;

internal static class SemanticClassTypeProjector
{
    public static SemanticClassType Project(INamedTypeSymbol type, string id, SemanticTypeRegistry registry)
    {
        ISymbol[] members = type.GetMembers().ToArray();
        TypeDeclarationSyntax[] declarations = type.DeclaringSyntaxReferences
            .Select(reference => reference.GetSyntax()).OfType<TypeDeclarationSyntax>().ToArray();
        return new(id, type.BaseType is { } parent ? registry.Register(parent) : null,
            type.Interfaces.Select(registry.Register).Distinct(StringComparer.Ordinal)
                .OrderBy(value => value, StringComparer.Ordinal).ToArray(),
            type.DeclaringSyntaxReferences.Length != 0, type.IsStatic, type.IsAbstract, type.IsSealed,
            type.IsGenericType, type.IsRecord, declarations.Any(declaration => declaration.ParameterList is not null),
            type.InstanceConstructors.Any(ctor => ctor.IsImplicitlyDeclared && ctor.Parameters.Length == 0),
            members.Any(member => !member.IsStatic && HasInitializer(member)),
            type.StaticConstructors.Length != 0 || members.Any(member => member.IsStatic
                && member is not IFieldSymbol { IsConst: true } && HasInitializer(member)),
            members.Any(member => !member.IsStatic && (member is IFieldSymbol { IsImplicitlyDeclared: true }
                || member is IEventSymbol && member.DeclaringSyntaxReferences.Any(reference => reference.GetSyntax() is VariableDeclaratorSyntax))),
            members.Any(member => member is IMethodSymbol or IPropertySymbol or IEventSymbol
                && !member.IsStatic && (member.IsVirtual || member.IsAbstract || member.IsOverride)),
            members.OfType<IMethodSymbol>().Any(method => method.MethodKind == MethodKind.Destructor));
    }

    private static bool HasInitializer(ISymbol member) => member.DeclaringSyntaxReferences.Any(reference =>
        reference.GetSyntax() is VariableDeclaratorSyntax { Initializer: not null }
            or PropertyDeclarationSyntax { Initializer: not null });
}
