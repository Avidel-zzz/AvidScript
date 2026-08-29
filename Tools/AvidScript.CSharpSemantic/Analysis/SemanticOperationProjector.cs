using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.FlowAnalysis;
using Microsoft.CodeAnalysis.Operations;

namespace AvidScript.CSharpSemantic;

internal sealed record SemanticOperationProjection(
    IReadOnlyList<SemanticMethodBody> Methods,
    IReadOnlyList<SemanticDiagnostic> Diagnostics);

internal static class SemanticOperationProjector
{
    public static SemanticOperationProjection Project(
        SemanticCompilationContext context,
        SemanticTypeRegistry typeRegistry)
    {
        List<SemanticMethodBody> methods = new();
        List<SemanticDiagnostic> diagnostics = new();

        foreach (SemanticExecutableBody body in SemanticExecutableBodyResolver.Resolve(context))
        {
            methods.Add(new SemanticMethodBody(
                SemanticSymbolProjector.GetSymbolId(body.Method),
                ProjectOperation(body.Operation, body.Unit, typeRegistry, diagnostics)));
        }

        return new SemanticOperationProjection(
            methods.OrderBy(method => method.MethodSymbolId, StringComparer.Ordinal).ToArray(),
            diagnostics
                .OrderBy(diagnostic => diagnostic.Span.Start)
                .ThenBy(diagnostic => diagnostic.Code, StringComparer.Ordinal)
                .ToArray());
    }

    internal static SemanticOperation ProjectControlFlowOperation(
        IOperation operation,
        SemanticCompilationUnit unit,
        SemanticTypeRegistry typeRegistry,
        SemanticCaptureRegistry captureRegistry)
    {
        return ProjectOperation(operation, unit, typeRegistry, diagnostics: null, captureRegistry);
    }

    internal static SemanticOperation ProjectAsyncStatementOperation(
        IOperation operation,
        SemanticCompilationUnit unit,
        SemanticTypeRegistry typeRegistry,
        ICollection<SemanticDiagnostic> diagnostics)
    {
        return ProjectOperation(operation, unit, typeRegistry, diagnostics);
    }

    private static SemanticOperation ProjectOperation(
        IOperation operation,
        SemanticCompilationUnit unit,
        SemanticTypeRegistry typeRegistry,
        ICollection<SemanticDiagnostic>? diagnostics,
        SemanticCaptureRegistry? captureRegistry = null)
    {
        (string kind, bool hasStableProjection) = DescribeOperation(operation);
        SemanticSupportDecision support = SemanticSupportPolicy.EvaluateOperation(
            operation,
            kind,
            hasStableProjection);
        ITypeSymbol? type = operation.Type;
        string? typeId = type is null ? null : typeRegistry.Register(type);
        string? symbolId = GetReferencedSymbol(operation) is { } symbol
            ? SemanticSymbolProjector.GetSymbolId(symbol)
            : null;
        SemanticSpan span = SemanticSpanFactory.Create(unit.SourceText, operation.Syntax.Span);
        if (!support.IsSupported && diagnostics is not null)
        {
            diagnostics.Add(new SemanticDiagnostic(
                support.DiagnosticCode!,
                "error",
                support.DiagnosticMessage!,
                span));
        }

        return new SemanticOperation(
            kind,
            support.IsSupported,
            GetOperatorKind(operation),
            GetIsChecked(operation),
            GetIsLifted(operation),
            GetIsPostfix(operation),
            GetIsTryCast(operation),
            typeId,
            symbolId,
            GetTypeArgumentIds(operation, typeRegistry),
            ProjectConstant(operation, type),
            ProjectConversion(operation),
            ProjectInputConversion(operation),
            ProjectOutputConversion(operation),
            GetCaptureId(operation, captureRegistry),
            span,
            operation.ChildOperations
                .Select(child => ProjectOperation(child, unit, typeRegistry, diagnostics, captureRegistry))
                .ToArray());
    }

    private static (string Kind, bool IsSupported) DescribeOperation(IOperation operation)
    {
        return operation switch
        {
            ILockOperation => ("lock", false),
            IInvalidOperation => ("invalid", false),
            IMethodBodyOperation => ("method_body", true),
            IConstructorBodyOperation => ("constructor_body", true),
            IBlockOperation => ("block", true),
            IExpressionStatementOperation => ("expression_statement", true),
            IVariableDeclarationGroupOperation => ("variable_declaration_group", true),
            IVariableDeclarationOperation => ("variable_declaration", true),
            IVariableDeclaratorOperation => ("variable_declarator", true),
            IVariableInitializerOperation => ("variable_initializer", true),
            ICompoundAssignmentOperation compound => ("compound_assignment", IsKnownOperatorKind(compound)),
            ISimpleAssignmentOperation => ("assignment", true),
            IFieldReferenceOperation => ("field_reference", true),
            ILocalReferenceOperation => ("local_reference", true),
            IParameterReferenceOperation => ("parameter_reference", true),
            IPropertyReferenceOperation => ("property_reference", true),
            IEventReferenceOperation => ("event_reference", true),
            IMethodReferenceOperation => ("method_reference", true),
            IInvocationOperation => ("invocation", true),
            IAwaitOperation => ("await", true),
            IObjectCreationOperation => ("object_creation", true),
            IReturnOperation => ("return", true),
            IBinaryOperation binary => ("binary", IsKnownOperatorKind(binary)),
            IUnaryOperation unary => ("unary", IsKnownOperatorKind(unary)),
            IConversionOperation => ("conversion", true),
            ILiteralOperation => ("literal", true),
            IArgumentOperation => ("argument", true),
            IInstanceReferenceOperation => ("instance_reference", true),
            IIncrementOrDecrementOperation increment => ("increment_or_decrement", IsKnownOperatorKind(increment)),
            IConditionalOperation => ("conditional", true),
            ISwitchOperation => ("switch", true),
            ISwitchCaseOperation => ("switch_case", true),
            ICaseClauseOperation => ("case_clause", true),
            ICoalesceOperation => ("coalesce", false),
            IDefaultValueOperation => ("default_value", true),
            ITypeOfOperation => ("type_of", true),
            ISizeOfOperation => ("size_of", true),
            IArrayCreationOperation => ("array_creation", true),
            IArrayInitializerOperation => ("array_initializer", true),
            IArrayElementReferenceOperation => ("array_element_reference", true),
            IDeclarationExpressionOperation => ("declaration_expression", true),
            IDiscardOperation => ("discard", true),
            IParenthesizedOperation => ("parenthesized", true),
            IBranchOperation => ("branch", true),
            ILoopOperation => ("loop", true),
            IEmptyOperation => ("empty", true),
            IFlowCaptureOperation => ("flow_capture", true),
            IFlowCaptureReferenceOperation => ("flow_capture_reference", true),
            _ => ($"roslyn:{operation.Kind}", false),
        };
    }

    private static string? GetCaptureId(
        IOperation operation,
        SemanticCaptureRegistry? captureRegistry)
    {
        return operation switch
        {
            IFlowCaptureOperation capture => (captureRegistry ??
                throw new InvalidOperationException("Flow capture projection requires a capture registry."))
                .Register(capture.Id),
            IFlowCaptureReferenceOperation reference => (captureRegistry ??
                throw new InvalidOperationException("Flow capture projection requires a capture registry."))
                .Register(reference.Id),
            _ => null,
        };
    }

    private static bool IsKnownOperatorKind(IOperation operation)
    {
        return GetOperatorKind(operation) is { } kind &&
            !kind.StartsWith("roslyn:", StringComparison.Ordinal);
    }

    private static string? GetOperatorKind(IOperation operation)
    {
        return operation switch
        {
            ICompoundAssignmentOperation compound => MapBinaryOperator(compound.OperatorKind.ToString()),
            IBinaryOperation binary => MapBinaryOperator(binary.OperatorKind.ToString()),
            IUnaryOperation unary => MapUnaryOperator(unary.OperatorKind.ToString()),
            IIncrementOrDecrementOperation increment => increment.Kind.ToString() switch
            {
                "Increment" => "increment",
                "Decrement" => "decrement",
                string kind => "roslyn:" + kind,
            },
            _ => null,
        };
    }

    private static string MapBinaryOperator(string kind)
    {
        return kind switch
        {
            "Add" => "add",
            "Subtract" => "subtract",
            "Multiply" => "multiply",
            "Divide" => "divide",
            "Remainder" => "remainder",
            "LeftShift" => "left_shift",
            "RightShift" => "right_shift",
            "UnsignedRightShift" => "unsigned_right_shift",
            "And" => "bitwise_and",
            "Or" => "bitwise_or",
            "ExclusiveOr" => "bitwise_xor",
            "ConditionalAnd" => "logical_and",
            "ConditionalOr" => "logical_or",
            "Equals" => "equals",
            "NotEquals" => "not_equals",
            "LessThan" => "less_than",
            "LessThanOrEqual" => "less_than_or_equal",
            "GreaterThan" => "greater_than",
            "GreaterThanOrEqual" => "greater_than_or_equal",
            _ => "roslyn:" + kind,
        };
    }

    private static string MapUnaryOperator(string kind)
    {
        return kind switch
        {
            "Plus" => "plus",
            "Minus" => "negate",
            "Not" => "logical_not",
            "BitwiseNegation" => "bitwise_not",
            "True" => "true",
            "False" => "false",
            _ => "roslyn:" + kind,
        };
    }

    private static bool GetIsChecked(IOperation operation)
    {
        return operation switch
        {
            ICompoundAssignmentOperation compound => compound.IsChecked,
            IBinaryOperation binary => binary.IsChecked,
            IUnaryOperation unary => unary.IsChecked,
            IIncrementOrDecrementOperation increment => increment.IsChecked,
            IConversionOperation conversion => conversion.IsChecked,
            _ => false,
        };
    }

    private static bool GetIsLifted(IOperation operation)
    {
        return operation switch
        {
            ICompoundAssignmentOperation compound => compound.IsLifted,
            IBinaryOperation binary => binary.IsLifted,
            IUnaryOperation unary => unary.IsLifted,
            IIncrementOrDecrementOperation increment => increment.IsLifted,
            _ => false,
        };
    }

    private static bool GetIsPostfix(IOperation operation)
    {
        return operation is IIncrementOrDecrementOperation { IsPostfix: true };
    }
    private static bool GetIsTryCast(IOperation operation)
    {
        return operation is IConversionOperation { IsTryCast: true };
    }
    private static IReadOnlyList<string> GetTypeArgumentIds(
        IOperation operation,
        SemanticTypeRegistry typeRegistry)
    {
        IMethodSymbol? method = operation switch
        {
            IInvocationOperation invocation => invocation.TargetMethod,
            IMethodReferenceOperation methodReference => methodReference.Method,
            _ => null,
        };
        if (method is null || method.Arity == 0)
        {
            return Array.Empty<string>();
        }

        return method.TypeArguments.Select(typeRegistry.Register).ToArray();
    }
    private static ISymbol? GetReferencedSymbol(IOperation operation)
    {
        return operation switch
        {
            ICompoundAssignmentOperation compound => compound.OperatorMethod,
            IIncrementOrDecrementOperation increment => increment.OperatorMethod,
            IBinaryOperation binary => binary.OperatorMethod,
            IUnaryOperation unary => unary.OperatorMethod,
            IConversionOperation conversion => conversion.OperatorMethod,
            IFieldReferenceOperation field => field.Field,
            ILocalReferenceOperation local => local.Local,
            IParameterReferenceOperation parameter => parameter.Parameter,
            IPropertyReferenceOperation property => property.Property,
            IEventReferenceOperation eventReference => eventReference.Event,
            IMethodReferenceOperation method => method.Method,
            IInvocationOperation invocation => invocation.TargetMethod,
            IObjectCreationOperation creation => creation.Constructor,
            IArgumentOperation argument => argument.Parameter,
            IVariableDeclaratorOperation variable => variable.Symbol,
            IInstanceReferenceOperation instanceReference when instanceReference.Type is INamedTypeSymbol type => type,
            _ => null,
        };
    }

    private static SemanticConstant? ProjectConstant(IOperation operation, ITypeSymbol? type)
    {
        if (!operation.ConstantValue.HasValue)
        {
            return null;
        }

        object? value = operation.ConstantValue.Value;
        string kind = value is null
            ? "null"
            : type is null
                ? GetRuntimeConstantKind(value)
                : SemanticTypeRegistry.GetCanonicalName(type);
        return new SemanticConstant(kind, FormatConstant(value));
    }

    private static string GetRuntimeConstantKind(object value)
    {
        return value switch
        {
            bool => "bool",
            byte => "uint8",
            sbyte => "int8",
            short => "int16",
            ushort => "uint16",
            int => "int32",
            uint => "uint32",
            long => "int64",
            ulong => "uint64",
            float => "float32",
            double => "float64",
            char => "char16",
            string => "string",
            _ => "unknown",
        };
    }

    private static string? FormatConstant(object? value)
    {
        return value switch
        {
            null => null,
            bool boolean => boolean ? "true" : "false",
            float single => single.ToString("R", CultureInfo.InvariantCulture),
            double doubleValue => doubleValue.ToString("R", CultureInfo.InvariantCulture),
            decimal decimalValue => decimalValue.ToString(CultureInfo.InvariantCulture),
            IFormattable formattable => formattable.ToString(null, CultureInfo.InvariantCulture),
            _ => value.ToString(),
        };
    }

    private static SemanticConversion? ProjectConversion(IOperation operation)
    {
        if (operation is not IConversionOperation conversionOperation)
        {
            return null;
        }

        return ProjectCommonConversion(conversionOperation.Conversion, operation.IsImplicit);
    }

    private static SemanticConversion? ProjectInputConversion(IOperation operation)
    {
        return operation switch
        {
            ICompoundAssignmentOperation compound => ProjectCommonConversion(compound.InConversion, isImplicit: true),
            IArgumentOperation argument => ProjectCommonConversion(argument.InConversion, isImplicit: true),
            _ => null,
        };
    }

    private static SemanticConversion? ProjectOutputConversion(IOperation operation)
    {
        return operation switch
        {
            ICompoundAssignmentOperation compound => ProjectCommonConversion(compound.OutConversion, isImplicit: true),
            IArgumentOperation argument => ProjectCommonConversion(argument.OutConversion, isImplicit: true),
            _ => null,
        };
    }

    private static SemanticConversion ProjectCommonConversion(CommonConversion conversion, bool isImplicit)
    {
        return new SemanticConversion(
            GetConversionKind(conversion),
            conversion.Exists,
            conversion.IsIdentity,
            isImplicit,
            conversion.IsNumeric,
            conversion.IsReference,
            conversion.IsNullable,
            conversion.IsUserDefined,
            conversion.MethodSymbol is { } method ? SemanticSymbolProjector.GetSymbolId(method) : null);
    }

    private static string GetConversionKind(CommonConversion conversion)
    {
        if (!conversion.Exists)
        {
            return "none";
        }

        if (conversion.IsIdentity)
        {
            return "identity";
        }

        if (conversion.IsUserDefined)
        {
            return "user_defined";
        }

        if (conversion.IsNumeric)
        {
            return "numeric";
        }

        if (conversion.IsReference)
        {
            return "reference";
        }

        if (conversion.IsNullable)
        {
            return "nullable";
        }

        return "other";
    }
}
