using System;
using System.Collections.Generic;
using System.Linq;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.Operations;

namespace AvidScript.CSharpSemantic;

internal static class SemanticCancellationTokenProjector
{
    internal static bool IsFrameworkType(ITypeSymbol? type, Compilation compilation, string metadataName) =>
        type is INamedTypeSymbol { DeclaringSyntaxReferences.Length: 0 }
        && SymbolEqualityComparer.Default.Equals(type, compilation.GetTypeByMetadataName(metadataName))
        && SymbolEqualityComparer.Default.Equals(type.ContainingAssembly,
            compilation.GetSpecialType(SpecialType.System_Object).ContainingAssembly);

    internal static bool TryProject(IOperation operation, SemanticCompilationUnit unit,
        SemanticTypeRegistry types, Func<IOperation, SemanticOperation> project,
        ICollection<SemanticDiagnostic>? diagnostics, out SemanticOperation? result)
    {
        result = null;
        if (types.CancellationTokenContext is not { EnableCancellationTokens: true } context) return false;
        bool Token(ITypeSymbol? type) => IsFrameworkType(type, context.Compilation, SemanticCancellationTokens.MetadataName);
        SemanticSpan span = SemanticSpanFactory.Create(unit.SourceText, operation.Syntax.Span);
        SemanticOperation Node(string kind, IEnumerable<IOperation> children, string? comparison = null,
            string? symbol = null, bool supported = true) => new(kind, supported, comparison, false,
                false, false, false, operation.Type is { } type ? types.Register(type) : null,
                symbol, Array.Empty<string>(), null, null, null, null, null, span,
                children.Select(project).ToArray());
        switch (operation)
        {
            case IDefaultValueOperation when Token(operation.Type):
            case IObjectCreationOperation { Arguments.Length: 0, Initializer: null } when Token(operation.Type):
                result = Node(SemanticCancellationTokens.None, Array.Empty<IOperation>());
                return true;
            case IPropertyReferenceOperation { Instance: null, Arguments.Length: 0,
                Property: { Name: "None", IsStatic: true } property } when Token(property.ContainingType)
                    && Token(property.Type):
                result = Node(SemanticCancellationTokens.None, Array.Empty<IOperation>());
                return true;
            case IPropertyReferenceOperation { Instance: { } receiver, Arguments.Length: 0,
                Property: { Name: "CancellationToken", IsStatic: false } property }
                when IsFrameworkType(property.ContainingType, context.Compilation, "System.OperationCanceledException")
                    && Token(property.Type):
                result = Node(SemanticCancellationTokens.Read, new[] { receiver });
                return true;
            case IBinaryOperation { OperatorMethod: { } method, IsLifted: false } binary
                when Token(method.ContainingType) && Token(binary.LeftOperand.Type) && Token(binary.RightOperand.Type)
                    && method.Name is "op_Equality" or "op_Inequality":
                result = Node(SemanticCancellationTokens.Compare, new[] { binary.LeftOperand, binary.RightOperand },
                    method.Name == "op_Equality" ? "equals" : "not_equals");
                return true;
            case IInvocationOperation { Instance: { } receiver, Arguments.Length: 1,
                TargetMethod: { Name: "Equals", IsStatic: false, Parameters.Length: 1 } method } call
                when Token(method.ContainingType) && Token(method.Parameters[0].Type):
                result = Node(SemanticCancellationTokens.Compare, new[] { receiver, call.Arguments[0].Value }, "equals");
                return true;
            case IConversionOperation { OperatorMethod: { } method } conversion when Token(operation.Type):
                if (IsFacadeConversion(method, context))
                {
                    result = Node(SemanticCancellationTokens.FromAvid, new[] { conversion.Operand },
                        symbol: SemanticSymbolProjector.GetSymbolId(method));
                    return true;
                }
                break;
            default:
                // A local/reference/return of a token is ordinary C# value flow.
                // Unsupported framework methods must not fall through as external calls.
                ITypeSymbol? owner = operation switch
                {
                    IMemberReferenceOperation member => member.Member.ContainingType,
                    IInvocationOperation call => call.TargetMethod.ContainingType,
                    IObjectCreationOperation creation => creation.Constructor?.ContainingType,
                    IBinaryOperation binary => binary.OperatorMethod?.ContainingType,
                    _ => null,
                };
                ITypeSymbol? instance = operation switch
                {
                    IMemberReferenceOperation member => member.Instance?.Type,
                    IInvocationOperation call => call.Instance?.Type,
                    _ => null,
                };
                if (!Token(owner) && !Token(instance)) return false;
                break;
        }
        diagnostics?.Add(new(SemanticCancellationTokens.DiagnosticCode, "error",
            "Unsupported CancellationToken API or conversion. Supported operations are None/default, equality, typed Equals, and OperationCanceledException.CancellationToken.", span));
        result = Node("cancellation_token_unsupported", operation.ChildOperations, supported: false);
        return true;
    }

    private static bool IsFacadeConversion(IMethodSymbol method, SemanticCompilationContext context)
    {
        if (!method.IsStatic || !method.IsExtern || method.MethodKind != MethodKind.Conversion
            || method.Name != "op_Implicit" || method.Parameters.Length != 1
            || method.Parameters[0].RefKind != RefKind.None || method.ReturnsByRef || method.ReturnsByRefReadonly
            || method.ContainingType.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat) != "global::AvidScript.AvidCancellationToken"
            || !SymbolEqualityComparer.Default.Equals(method.Parameters[0].Type, method.ContainingType)
            || method.ContainingType is not { TypeKind: TypeKind.Struct, IsReadOnly: true }
            || method.DeclaringSyntaxReferences.Length != 1
            || !context.ProjectionUnits.Any(unit => !unit.IsPrimary
                && unit.SyntaxTree == method.DeclaringSyntaxReferences[0].SyntaxTree)) return false;
        IFieldSymbol[] fields = method.ContainingType.GetMembers().OfType<IFieldSymbol>().Where(field => !field.IsStatic).ToArray();
        return fields is [{ Name: "Value", IsReadOnly: true, Type.SpecialType: SpecialType.System_Int64 }]
            && method.GetAttributes().Any(attribute => IsFrameworkType(attribute.AttributeClass,
                context.Compilation, "System.Runtime.CompilerServices.MethodImplAttribute")
                && attribute.ConstructorArguments is [{ Value: int flags }]
                && (flags & (int)System.Runtime.CompilerServices.MethodImplOptions.InternalCall) != 0);
    }
}
