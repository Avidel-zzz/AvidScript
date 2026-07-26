using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal static class CSharpOperationLowerer
{
    public static GuestRegister? LowerValue(
        CSharpFunctionLoweringContext context,
        SemanticOperation operation,
        int blockOrdinal,
        List<GuestInstruction> instructions)
    {
        if (!operation.IsSupported)
        {
            context.Add("ASCG1004", $"Block {blockOrdinal} contains unsupported semantic operation '{operation.Kind}'.");
            return null;
        }

        return operation.Kind switch
        {
            "argument" or "expression_statement" or "parenthesized" => LowerWrapper(
                context, operation, blockOrdinal, instructions),
            "array_creation" => LowerConstantArray(context, operation, blockOrdinal, instructions),
            "assignment" => LowerAssignment(context, operation, blockOrdinal, instructions),
            "binary" => LowerBinary(context, operation, blockOrdinal, instructions),
            "compound_assignment" => LowerCompoundAssignment(context, operation, blockOrdinal, instructions),
            "conversion" => LowerConversion(context, operation, blockOrdinal, instructions),
            "declaration_expression" => LowerAddress(context, operation, blockOrdinal, instructions),
            "default_value" => LowerDefaultValue(context, operation, blockOrdinal, instructions),
            "field_reference" => LowerFieldLoad(context, operation, blockOrdinal, instructions),
            "flow_capture" => LowerFlowCapture(context, operation, blockOrdinal, instructions),
            "flow_capture_reference" => LowerFlowCaptureReference(context, operation, blockOrdinal, instructions),
            "increment_or_decrement" => LowerIncrementOrDecrement(
                context, operation, blockOrdinal, instructions),
            "instance_reference" => CSharpAggregateOperationLowerer.LowerInstance(
                context, operation, blockOrdinal),
            "invocation" => CSharpCallOperationLowerer.LowerInvocation(
                context, operation, blockOrdinal, instructions),
            "literal" => LowerLiteral(context, operation, blockOrdinal, instructions),
            "local_reference" or "parameter_reference" => LowerLocalLoad(
                context, operation, blockOrdinal, instructions),
            "object_creation" => CSharpCallOperationLowerer.LowerObjectCreation(
                context, operation, blockOrdinal, instructions),
            "property_reference" => CSharpCallOperationLowerer.LowerProperty(
                context, operation, blockOrdinal, instructions),
            "unary" => LowerUnary(context, operation, blockOrdinal, instructions),
            _ => Unsupported(context, operation, blockOrdinal),
        };
    }

    private static GuestRegister? LowerWrapper(
        CSharpFunctionLoweringContext context,
        SemanticOperation operation,
        int blockOrdinal,
        List<GuestInstruction> instructions)
    {
        if (operation.Children.Count != 1)
        {
            return Malformed(context, operation, blockOrdinal);
        }

        return LowerValue(context, operation.Children[0], blockOrdinal, instructions);
    }

    private static GuestRegister? LowerAssignment(
        CSharpFunctionLoweringContext context,
        SemanticOperation operation,
        int blockOrdinal,
        List<GuestInstruction> instructions)
    {
        if (operation.Children.Count != 2)
        {
            return Malformed(context, operation, blockOrdinal);
        }

        SemanticOperation target = operation.Children[0];
        if (target.Kind == "property_reference")
        {
            GuestRegister? receiver = CSharpCallOperationLowerer.LowerPropertyReceiver(
                context,
                target,
                blockOrdinal,
                instructions);
            GuestRegister? propertyValue = receiver is null
                ? null
                : LowerValue(context, operation.Children[1], blockOrdinal, instructions);
            if (receiver is null || propertyValue is null)
            {
                return null;
            }

            return CSharpCallOperationLowerer.LowerPropertySetter(
                context,
                target,
                receiver,
                propertyValue,
                blockOrdinal,
                instructions)
                ? propertyValue
                : null;
        }

        GuestRegister? value = LowerValue(context, operation.Children[1], blockOrdinal, instructions);
        if (value is null || target.Kind == "discard")
        {
            return value;
        }

        return StoreValue(context, target, value, blockOrdinal, instructions)
            ? value
            : null;
    }

    private static GuestRegister? LowerCompoundAssignment(
        CSharpFunctionLoweringContext context,
        SemanticOperation operation,
        int blockOrdinal,
        List<GuestInstruction> instructions)
    {
        if (operation.Children.Count != 2 || string.IsNullOrWhiteSpace(operation.OperatorKind))
        {
            return Malformed(context, operation, blockOrdinal);
        }

        SemanticOperation target = operation.Children[0];
        GuestRegister? propertyReceiver = null;
        GuestRegister? left;
        if (target.Kind == "property_reference")
        {
            propertyReceiver = CSharpCallOperationLowerer.LowerPropertyReceiver(
                context,
                target,
                blockOrdinal,
                instructions);
            if (propertyReceiver is null)
            {
                return null;
            }

            left = CSharpCallOperationLowerer.LowerProperty(
                context,
                target,
                propertyReceiver,
                blockOrdinal,
                instructions);
        }
        else
        {
            left = LowerValue(context, target, blockOrdinal, instructions);
        }

        GuestRegister? right = LowerValue(context, operation.Children[1], blockOrdinal, instructions);
        right = WidenShiftCount(context, operation.OperatorKind, left, right, blockOrdinal, instructions);
        GuestRegister? result = context.CreateTemporary(operation.TypeId, blockOrdinal);
        if (left is null || right is null || result is null)
        {
            return null;
        }

        instructions.Add(new GuestInstruction(
            "binary",
            result.Id,
            new[] { left.Id, right.Id },
            null,
            operation.OperatorKind,
            null));
        if (propertyReceiver is not null)
        {
            return CSharpCallOperationLowerer.LowerPropertySetter(
                context,
                target,
                propertyReceiver,
                result,
                blockOrdinal,
                instructions)
                ? result
                : null;
        }

        return StoreValue(context, target, result, blockOrdinal, instructions)
            ? result
            : null;
    }
    private static GuestRegister? LowerBinary(
        CSharpFunctionLoweringContext context,
        SemanticOperation operation,
        int blockOrdinal,
        List<GuestInstruction> instructions)
    {
        if (operation.Children.Count != 2 || string.IsNullOrWhiteSpace(operation.OperatorKind))
        {
            return Malformed(context, operation, blockOrdinal);
        }

        GuestRegister? left = LowerValue(context, operation.Children[0], blockOrdinal, instructions);
        GuestRegister? right = LowerValue(context, operation.Children[1], blockOrdinal, instructions);
        right = WidenShiftCount(context, operation.OperatorKind, left, right, blockOrdinal, instructions);
        if (left is null || right is null)
        {
            return null;
        }

        if (operation.SymbolId is not null)
        {
            if (!context.TryGetCallTarget(operation.SymbolId, out SemanticCallable callable, out string targetId))
            {
                context.Add("ASCG1005", $"Block {blockOrdinal} operator target '{operation.SymbolId}' is not a Guest function.");
                return null;
            }

            return EmitCall(
                context,
                callable,
                targetId,
                new[] { left.Id, right.Id },
                blockOrdinal,
                instructions);
        }

        GuestRegister? result = context.CreateTemporary(operation.TypeId, blockOrdinal);
        if (result is null)
        {
            return null;
        }

        instructions.Add(new GuestInstruction(
            "binary",
            result.Id,
            new[] { left.Id, right.Id },
            null,
            operation.OperatorKind,
            null));
        return result;
    }

    private static GuestRegister? LowerUnary(
        CSharpFunctionLoweringContext context,
        SemanticOperation operation,
        int blockOrdinal,
        List<GuestInstruction> instructions)
    {
        if (operation.Children.Count != 1 || string.IsNullOrWhiteSpace(operation.OperatorKind))
        {
            return Malformed(context, operation, blockOrdinal);
        }

        if (operation.Constant is not null)
        {
            return LowerLiteral(context, operation, blockOrdinal, instructions);
        }

        GuestRegister? operand = LowerValue(
            context, operation.Children[0], blockOrdinal, instructions);
        if (operand is null)
        {
            return null;
        }

        if (operation.SymbolId is not null)
        {
            if (!context.TryGetCallTarget(
                    operation.SymbolId,
                    out SemanticCallable callable,
                    out string targetId))
            {
                context.Add(
                    "ASCG1005",
                    $"Block {blockOrdinal} unary operator target '{operation.SymbolId}' is not a Guest function.");
                return null;
            }

            return EmitCall(
                context,
                callable,
                targetId,
                new[] { operand.Id },
                blockOrdinal,
                instructions);
        }

        if (operation.OperatorKind == "plus")
        {
            GuestRegister? result = context.CreateTemporary(operation.TypeId, blockOrdinal);
            if (result is not null)
            {
                instructions.Add(new GuestInstruction(
                    "copy", result.Id, new[] { operand.Id }, null, null, null));
            }
            return result;
        }

        GuestRegister? zero = EmitZero(
            context, operation.TypeId, blockOrdinal, instructions);
        if (zero is null)
        {
            return null;
        }

        return operation.OperatorKind switch
        {
            "negate" => EmitBinaryPrimitive(
                context, operation.TypeId, "subtract", zero, operand, blockOrdinal, instructions),
            "logical_not" => EmitBinaryPrimitive(
                context, operation.TypeId, "equals", operand, zero, blockOrdinal, instructions),
            "bitwise_not" => LowerBitwiseNot(
                context, operation.TypeId, operand, zero, blockOrdinal, instructions),
            _ => Unsupported(context, operation, blockOrdinal),
        };
    }

    private static GuestRegister? LowerBitwiseNot(
        CSharpFunctionLoweringContext context,
        string? typeId,
        GuestRegister operand,
        GuestRegister zero,
        int blockOrdinal,
        List<GuestInstruction> instructions)
    {
        GuestRegister? one = EmitOne(context, typeId, blockOrdinal, instructions);
        GuestRegister? allBits = one is null
            ? null
            : EmitBinaryPrimitive(
                context, typeId, "subtract", zero, one, blockOrdinal, instructions);
        return allBits is null
            ? null
            : EmitBinaryPrimitive(
                context, typeId, "bitwise_xor", operand, allBits, blockOrdinal, instructions);
    }

    private static GuestRegister? LowerIncrementOrDecrement(
        CSharpFunctionLoweringContext context,
        SemanticOperation operation,
        int blockOrdinal,
        List<GuestInstruction> instructions)
    {
        if (operation.Children.Count != 1
            || operation.OperatorKind is not ("increment" or "decrement"))
        {
            return Malformed(context, operation, blockOrdinal);
        }

        SemanticOperation target = operation.Children[0];
        GuestRegister? propertyReceiver = null;
        GuestRegister? previous;
        if (target.Kind == "property_reference")
        {
            propertyReceiver = CSharpCallOperationLowerer.LowerPropertyReceiver(
                context,
                target,
                blockOrdinal,
                instructions);
            previous = propertyReceiver is null
                ? null
                : CSharpCallOperationLowerer.LowerProperty(
                    context,
                    target,
                    propertyReceiver,
                    blockOrdinal,
                    instructions);
        }
        else
        {
            previous = LowerValue(context, target, blockOrdinal, instructions);
        }

        if (previous is null)
        {
            return null;
        }

        GuestRegister? next;
        if (operation.SymbolId is not null)
        {
            if (!context.TryGetCallTarget(
                    operation.SymbolId,
                    out SemanticCallable callable,
                    out string targetId))
            {
                context.Add(
                    "ASCG1005",
                    $"Block {blockOrdinal} increment/decrement target '{operation.SymbolId}' is not a Guest function.");
                return null;
            }

            next = EmitCall(
                context,
                callable,
                targetId,
                new[] { previous.Id },
                blockOrdinal,
                instructions);
        }
        else
        {
            GuestRegister? one = EmitOne(
                context, operation.TypeId, blockOrdinal, instructions);
            next = one is null
                ? null
                : EmitBinaryPrimitive(
                    context,
                    operation.TypeId,
                    operation.OperatorKind == "increment" ? "add" : "subtract",
                    previous,
                    one,
                    blockOrdinal,
                    instructions);
        }

        if (next is null)
        {
            return null;
        }

        bool stored = propertyReceiver is not null
            ? CSharpCallOperationLowerer.LowerPropertySetter(
                context,
                target,
                propertyReceiver,
                next,
                blockOrdinal,
                instructions)
            : StoreValue(context, target, next, blockOrdinal, instructions);
        return stored
            ? operation.IsPostfix ? previous : next
            : null;
    }

    private static GuestRegister? EmitZero(
        CSharpFunctionLoweringContext context,
        string? typeId,
        int blockOrdinal,
        List<GuestInstruction> instructions)
    {
        GuestRegister? result = context.CreateTemporary(typeId, blockOrdinal);
        if (result is not null)
        {
            instructions.Add(new GuestInstruction(
                "constant",
                result.Id,
                Array.Empty<string>(),
                null,
                null,
                new GuestConstant("zero", null)));
        }
        return result;
    }

    private static GuestRegister? EmitOne(
        CSharpFunctionLoweringContext context,
        string? typeId,
        int blockOrdinal,
        List<GuestInstruction> instructions)
    {
        SemanticType? semanticType = context.Document.Types.SingleOrDefault(
            item => string.Equals(item.Id, typeId, StringComparison.Ordinal));
        if (semanticType is null
            || !context.TryLowerConstant(
                typeId,
                new SemanticConstant(semanticType.CanonicalName, "1"),
                out GuestConstant constant))
        {
            context.Add("ASCG1004", $"Block {blockOrdinal} cannot encode a typed one constant for '{typeId}'.");
            return null;
        }

        GuestRegister? result = context.CreateTemporary(typeId, blockOrdinal);
        if (result is not null)
        {
            instructions.Add(new GuestInstruction(
                "constant",
                result.Id,
                Array.Empty<string>(),
                null,
                null,
                constant));
        }
        return result;
    }

    private static GuestRegister? EmitBinaryPrimitive(
        CSharpFunctionLoweringContext context,
        string? typeId,
        string operatorKind,
        GuestRegister left,
        GuestRegister right,
        int blockOrdinal,
        List<GuestInstruction> instructions)
    {
        GuestRegister? result = context.CreateTemporary(typeId, blockOrdinal);
        if (result is not null)
        {
            instructions.Add(new GuestInstruction(
                "binary",
                result.Id,
                new[] { left.Id, right.Id },
                null,
                operatorKind,
                null));
        }
        return result;
    }

    private static GuestRegister? WidenShiftCount(
        CSharpFunctionLoweringContext context,
        string operatorKind,
        GuestRegister? left,
        GuestRegister? right,
        int blockOrdinal,
        List<GuestInstruction> instructions)
    {
        if (left is null
            || right is null
            || string.Equals(left.TypeId, right.TypeId, StringComparison.Ordinal)
            || operatorKind is not ("left_shift" or "right_shift" or "unsigned_right_shift"))
        {
            return right;
        }

        GuestRegister? widened = context.CreateTemporary(left.TypeId, blockOrdinal);
        if (widened is null)
        {
            return null;
        }

        instructions.Add(new GuestInstruction(
            "convert", widened.Id, new[] { right.Id }, null, null, null));
        return widened;
    }

    private static GuestRegister? LowerDefaultValue(
        CSharpFunctionLoweringContext context,
        SemanticOperation operation,
        int blockOrdinal,
        List<GuestInstruction> instructions)
    {
        if (operation.Children.Count != 0
            || !context.TryGetGuestType(operation.TypeId, out GuestType type)
            || type.Kind is "void" or "class_ref" or "factory_ref" or "object_type_ref")
        {
            return Malformed(context, operation, blockOrdinal);
        }

        GuestRegister? result = context.CreateTemporary(operation.TypeId, blockOrdinal);
        if (result is not null)
        {
            instructions.Add(new GuestInstruction(
                "constant",
                result.Id,
                Array.Empty<string>(),
                null,
                null,
                new GuestConstant("zero", null)));
        }
        return result;
    }

    private static GuestRegister? LowerConversion(
        CSharpFunctionLoweringContext context,
        SemanticOperation operation,
        int blockOrdinal,
        List<GuestInstruction> instructions)
    {
        if (operation.Children.Count != 1 || operation.Conversion is null || !operation.Conversion.Exists)
        {
            return Malformed(context, operation, blockOrdinal);
        }

        GuestRegister? operand = LowerValue(context, operation.Children[0], blockOrdinal, instructions);
        if (operand is null)
        {
            return null;
        }

        if (operation.Conversion.IsUserDefined)
        {
            if (string.IsNullOrWhiteSpace(operation.Conversion.MethodSymbolId)
                || !context.TryGetCallTarget(
                    operation.Conversion.MethodSymbolId,
                    out SemanticCallable callable,
                    out string targetId))
            {
                context.Add("ASCG1004", $"Block {blockOrdinal} user-defined conversion target is missing, unreachable, or malformed.");
                return null;
            }

            if (CSharpClassReferencePolicy.IsIntrinsicUpcast(context.Document, callable)
                && string.Equals(callable.Parameters[0].TypeId, operand.TypeId, StringComparison.Ordinal)
                && string.Equals(callable.ReturnTypeId, operation.TypeId, StringComparison.Ordinal))
            {
                GuestRegister? upcast = context.CreateTemporary(operation.TypeId, blockOrdinal);
                if (upcast is null)
                {
                    return null;
                }

                instructions.Add(new GuestInstruction(
                    "convert",
                    upcast.Id,
                    new[] { operand.Id },
                    null,
                    "class_ref_upcast",
                    null));
                return upcast;
            }

            if (!callable.IsStatic
                || callable.IsConstructor
                || !callable.HasBody
                || callable.Import is not null
                || callable.Parameters.Count != 1
                || !string.Equals(callable.Parameters[0].RefKind, "none", StringComparison.Ordinal)
                || !string.Equals(
                    callable.Parameters[0].TypeId,
                    operation.Children[0].TypeId,
                    StringComparison.Ordinal)
                || !string.Equals(
                    callable.Parameters[0].TypeId,
                    operand.TypeId,
                    StringComparison.Ordinal)
                || !string.Equals(callable.ReturnTypeId, operation.TypeId, StringComparison.Ordinal))
            {
                context.Add("ASCG1004", $"Block {blockOrdinal} user-defined conversion target is missing, unreachable, or malformed.");
                return null;
            }

            return EmitCall(
                context,
                callable,
                targetId,
                new[] { operand.Id },
                blockOrdinal,
                instructions);
        }

        GuestRegister? result = context.CreateTemporary(operation.TypeId, blockOrdinal);
        if (result is null)
        {
            return null;
        }

        if (string.Equals(operand.TypeId, result.TypeId, StringComparison.Ordinal))
        {
            instructions.Add(new GuestInstruction(
                "copy", result.Id, new[] { operand.Id }, null, null, null));
            return result;
        }

        instructions.Add(new GuestInstruction(
            "convert", result.Id, new[] { operand.Id }, null, null, null));
        return result;
    }

    private static GuestRegister? LowerFieldLoad(
        CSharpFunctionLoweringContext context,
        SemanticOperation operation,
        int blockOrdinal,
        List<GuestInstruction> instructions)
    {
        if (operation.Constant is not null)
        {
            return LowerLiteral(context, operation, blockOrdinal, instructions);
        }

        if (operation.Children.Count != 0)
        {
            return CSharpAggregateOperationLowerer.LowerFieldLoad(
                context, operation, blockOrdinal, instructions);
        }

        if (!context.TryGetGlobal(operation.SymbolId, out string globalId))
        {
            context.Add("ASCG1004", $"Block {blockOrdinal} field '{operation.SymbolId}' is not projected state.");
            return null;
        }

        GuestRegister? result = context.CreateTemporary(operation.TypeId, blockOrdinal);
        if (result is null)
        {
            return null;
        }

        instructions.Add(new GuestInstruction(
            "global_load", result.Id, Array.Empty<string>(), globalId, null, null));
        return result;
    }
    public static GuestRegister? LowerAddress(
        CSharpFunctionLoweringContext context,
        SemanticOperation operation,
        int blockOrdinal,
        List<GuestInstruction> instructions)
    {
        SemanticOperation target = operation.Kind is "argument" or "declaration_expression"
            && operation.Children.Count == 1
                ? operation.Children[0]
                : operation;
        if (target.Kind == "flow_capture_reference"
            && context.TryGetCaptureAddressTarget(
                target.CaptureId,
                out GuestRegister capturedStorage,
                out bool capturedStorageIsAddress))
        {
            return EmitStorageAddress(
                context,
                capturedStorage,
                capturedStorageIsAddress,
                blockOrdinal,
                instructions);
        }

        if (target.Kind is not ("local_reference" or "parameter_reference")
            || !context.TryGetStorage(target.SymbolId, out GuestRegister storage))
        {
            context.Add("ASCG1004", $"Block {blockOrdinal} operation '{target.Kind}' is not addressable.");
            return null;
        }

        bool storageIsAddress = target.Kind == "parameter_reference"
            && context.TryGetParameter(target.SymbolId, out SemanticCallableParameter parameter)
            && parameter.RefKind != "none";
        return EmitStorageAddress(
            context,
            storage,
            storageIsAddress,
            blockOrdinal,
            instructions);
    }

    private static GuestRegister? EmitStorageAddress(
        CSharpFunctionLoweringContext context,
        GuestRegister storage,
        bool storageIsAddress,
        int blockOrdinal,
        List<GuestInstruction> instructions)
    {
        if (storageIsAddress)
        {
            return storage;
        }

        GuestRegister? address = context.CreateTemporary(CSharpGuestIds.AddressTypeId, blockOrdinal);
        if (address is null)
        {
            return null;
        }

        instructions.Add(new GuestInstruction(
            "address_of", address.Id, Array.Empty<string>(), storage.Id, null, null));
        return address;
    }
    public static GuestRegister? EmitCall(
        CSharpFunctionLoweringContext context,
        SemanticCallable callable,
        string targetId,
        IReadOnlyList<string> operands,
        int blockOrdinal,
        List<GuestInstruction> instructions)
    {
        if (!context.TryGetGuestType(callable.ReturnTypeId, out GuestType returnType))
        {
            context.Add("ASCG1004", $"Block {blockOrdinal} call target '{callable.MethodSymbolId}' has no Guest return type.");
            return null;
        }

        GuestRegister? result = returnType.Kind == "void"
            ? null
            : context.CreateTemporary(callable.ReturnTypeId, blockOrdinal);
        if (returnType.Kind != "void" && result is null)
        {
            return null;
        }

        instructions.Add(new GuestInstruction(
            "call", result?.Id, operands, targetId, null, null));
        return result;
    }
    private static GuestRegister? LowerConstantArray(
        CSharpFunctionLoweringContext context,
        SemanticOperation operation,
        int blockOrdinal,
        List<GuestInstruction> instructions)
    {
        SemanticOperation? initializer = operation.Children.Count == 1
            && operation.Children[0].Kind == "array_initializer"
                ? operation.Children[0]
                : null;
        if (initializer is null || operation.TypeId is null)
        {
            context.Add("ASCG1004", $"Block {blockOrdinal} supports only constant array initializers.");
            return null;
        }

        List<GuestConstant> elements = new();
        foreach (SemanticOperation element in initializer.Children)
        {
            if (element.Kind != "literal"
                || element.Constant is null
                || !context.TryLowerConstant(element.TypeId, element.Constant, out GuestConstant constant))
            {
                context.Add("ASCG1004", $"Block {blockOrdinal} array initializer contains a non-constant element.");
                return null;
            }

            elements.Add(constant);
        }

        string? segmentId = context.DataPool.AddConstantArray(
            operation.TypeId,
            elements,
            context.Diagnostics);
        GuestRegister? result = context.CreateTemporary(operation.TypeId, blockOrdinal);
        if (segmentId is null || result is null)
        {
            return null;
        }

        instructions.Add(new GuestInstruction(
            "data_address", result.Id, Array.Empty<string>(), segmentId, null, null));
        return result;
    }

    private static GuestRegister? LowerLiteral(
        CSharpFunctionLoweringContext context,
        SemanticOperation operation,
        int blockOrdinal,
        List<GuestInstruction> instructions)
    {
        if (operation.Constant is null)
        {
            return Malformed(context, operation, blockOrdinal);
        }

        GuestRegister? result = context.CreateTemporary(operation.TypeId, blockOrdinal);
        if (result is null)
        {
            return null;
        }

        if (operation.Constant.Kind == "string")
        {
            if (operation.TypeId is null || operation.Constant.Value is null)
            {
                return Malformed(context, operation, blockOrdinal);
            }

            string? segmentId = context.DataPool.AddUtf8String(
                operation.TypeId,
                operation.Constant.Value,
                context.Diagnostics);
            if (segmentId is null)
            {
                return null;
            }

            instructions.Add(new GuestInstruction(
                "data_address", result.Id, Array.Empty<string>(), segmentId, null, null));
            return result;
        }

        if (!context.TryLowerConstant(operation.TypeId, operation.Constant, out GuestConstant constant))
        {
            context.Add("ASCG1004", $"Block {blockOrdinal} literal has no canonical Guest encoding.");
            return null;
        }

        instructions.Add(new GuestInstruction(
            "constant",
            result.Id,
            Array.Empty<string>(),
            null,
            null,
            constant));
        return result;
    }
    private static GuestRegister? LowerLocalLoad(
        CSharpFunctionLoweringContext context,
        SemanticOperation operation,
        int blockOrdinal,
        List<GuestInstruction> instructions)
    {
        if (!context.TryGetStorage(operation.SymbolId, out GuestRegister storage))
        {
            context.Add("ASCG1004", $"Block {blockOrdinal} local '{operation.SymbolId}' has no storage slot.");
            return null;
        }

        GuestRegister? result = context.CreateTemporary(operation.TypeId, blockOrdinal);
        if (result is null)
        {
            return null;
        }

        bool indirect = operation.Kind == "parameter_reference"
            && context.TryGetParameter(operation.SymbolId, out SemanticCallableParameter parameter)
            && parameter.RefKind != "none";
        instructions.Add(indirect
            ? new GuestInstruction(
                "indirect_load", result.Id, new[] { storage.Id }, operation.TypeId, null, null)
            : new GuestInstruction(
                "local_load", result.Id, Array.Empty<string>(), storage.Id, null, null));
        return result;
    }

    private static GuestRegister? LowerFlowCapture(
        CSharpFunctionLoweringContext context,
        SemanticOperation operation,
        int blockOrdinal,
        List<GuestInstruction> instructions)
    {
        if (operation.Children.Count != 1)
        {
            return Malformed(context, operation, blockOrdinal);
        }

        context.TrackCaptureAddressTarget(operation.CaptureId, operation.Children[0], blockOrdinal);
        GuestRegister? value = LowerValue(context, operation.Children[0], blockOrdinal, instructions);
        GuestRegister? capture = context.GetOrCreateCapture(
            operation.CaptureId,
            value?.TypeId,
            blockOrdinal);
        if (value is null || capture is null)
        {
            return null;
        }

        instructions.Add(new GuestInstruction(
            "local_store", null, new[] { value.Id }, capture.Id, null, null));
        return value;
    }

    private static GuestRegister? LowerFlowCaptureReference(
        CSharpFunctionLoweringContext context,
        SemanticOperation operation,
        int blockOrdinal,
        List<GuestInstruction> instructions)
    {
        if (operation.Children.Count != 0)
        {
            return Malformed(context, operation, blockOrdinal);
        }

        GuestRegister? capture = context.GetOrCreateCapture(
            operation.CaptureId,
            operation.TypeId,
            blockOrdinal);
        GuestRegister? result = context.CreateTemporary(operation.TypeId, blockOrdinal);
        if (capture is null || result is null)
        {
            return null;
        }

        instructions.Add(new GuestInstruction(
            "local_load", result.Id, Array.Empty<string>(), capture.Id, null, null));
        return result;
    }

    private static bool StoreValue(
        CSharpFunctionLoweringContext context,
        SemanticOperation target,
        GuestRegister value,
        int blockOrdinal,
        List<GuestInstruction> instructions)
    {
        if (target.Kind is "local_reference" or "parameter_reference"
            && context.TryGetStorage(target.SymbolId, out GuestRegister storage))
        {
            bool indirect = target.Kind == "parameter_reference"
                && context.TryGetParameter(target.SymbolId, out SemanticCallableParameter parameter)
                && parameter.RefKind != "none";
            instructions.Add(indirect
                ? new GuestInstruction(
                    "indirect_store",
                    null,
                    new[] { storage.Id, value.Id },
                    target.TypeId,
                    null,
                    null)
                : new GuestInstruction(
                    "local_store", null, new[] { value.Id }, storage.Id, null, null));
            return true;
        }

        if (target.Kind == "field_reference")
        {
            if (target.Children.Count != 0)
            {
                return CSharpAggregateOperationLowerer.StoreField(
                    context, target, value, blockOrdinal, instructions);
            }

            if (context.TryGetGlobal(target.SymbolId, out string globalId))
            {
                instructions.Add(new GuestInstruction(
                    "global_store", null, new[] { value.Id }, globalId, null, null));
                return true;
            }
        }

        if (target.Kind == "flow_capture_reference")
        {
            GuestRegister? capture = context.GetOrCreateCapture(
                target.CaptureId,
                target.TypeId,
                blockOrdinal);
            if (capture is null
                || !string.Equals(capture.TypeId, value.TypeId, StringComparison.Ordinal))
            {
                return false;
            }

            instructions.Add(new GuestInstruction(
                "local_store", null, new[] { value.Id }, capture.Id, null, null));
            return true;
        }

        if (target.Kind == "property_reference")
        {
            return CSharpCallOperationLowerer.LowerPropertySetter(
                context,
                target,
                value,
                blockOrdinal,
                instructions);
        }

        context.Add("ASCG1004", $"Block {blockOrdinal} assignment target '{target.Kind}' is not writable.");
        return false;
    }
    private static GuestRegister? Malformed(
        CSharpFunctionLoweringContext context,
        SemanticOperation operation,
        int blockOrdinal)
    {
        context.Add("ASCG1004", $"Block {blockOrdinal} operation '{operation.Kind}' has malformed children or metadata.");
        return null;
    }

    private static GuestRegister? Unsupported(
        CSharpFunctionLoweringContext context,
        SemanticOperation operation,
        int blockOrdinal)
    {
        context.Add("ASCG1004", $"Block {blockOrdinal} operation '{operation.Kind}' has no Guest lowering rule.");
        return null;
    }
}
