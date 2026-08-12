using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal sealed record CSharpDelegateEventLoweringResult(
    IReadOnlyList<GuestFunction> Functions,
    IReadOnlyList<GuestExport> Exports);

internal static class CSharpDelegateEventLowerer
{
    private const int MaximumAbiCells = 8;

    private sealed record AbiValuePlan(
        GuestType Type,
        IReadOnlyList<(GuestField Field, AbiValuePlan Value)> Fields,
        int CellCount)
    {
        public bool IsLeaf => Fields.Count == 0 && Type.Kind is "scalar" or "enum";
    }

    public static CSharpDelegateEventLoweringResult? Lower(
        SemanticDocument document,
        IReadOnlyDictionary<string, GuestType> guestTypes,
        IReadOnlyList<GuestFunction> loweredFunctions,
        List<GuestDiagnostic> diagnostics)
    {
        if (document.DelegateEventCallbacks.Count == 0)
        {
            return null;
        }

        Dictionary<string, SemanticCallable> callablesById = document.Callables.ToDictionary(
            callable => callable.MethodSymbolId,
            StringComparer.Ordinal);
        HashSet<string> loweredFunctionIds = loweredFunctions
            .Select(function => function.Id)
            .ToHashSet(StringComparer.Ordinal);
        HashSet<string> explicitExports = document.Callables
            .Where(callable => callable.Export is not null)
            .Select(callable => callable.Export!.Name)
            .ToHashSet(StringComparer.Ordinal);
        HashSet<string> generatedExports = new(StringComparer.Ordinal);
        List<GuestFunction> functions = new();
        List<GuestExport> exports = new();

        foreach (SemanticDelegateEventCallback callback in document.DelegateEventCallbacks
            .OrderBy(callback => callback.SubscriptionId, StringComparer.Ordinal))
        {
            if (explicitExports.Contains(callback.ExportName)
                || !generatedExports.Add(callback.ExportName))
            {
                Add(diagnostics,
                    $"Delegate event '{callback.SubscriptionId}' conflicts with export '{callback.ExportName}'.");
                continue;
            }

            if (!callablesById.TryGetValue(callback.MethodSymbolId, out SemanticCallable? callable)
                || !loweredFunctionIds.Contains(CSharpGuestIds.Function(callback.MethodSymbolId))
                || !callable.IsStatic
                || !callable.HasBody
                || !string.Equals(callable.ReturnTypeId, "type:void", StringComparison.Ordinal)
                || callable.Parameters.Any(parameter => parameter.RefKind != "none"))
            {
                Add(diagnostics,
                    $"Delegate event handler '{callback.Name}' has no compatible lowered function.");
                continue;
            }

            List<AbiValuePlan> plans = new();
            bool valid = true;
            foreach (SemanticCallableParameter parameter in callable.Parameters.OrderBy(parameter => parameter.Ordinal))
            {
                if (!TryCreatePlan(
                    parameter.TypeId,
                    guestTypes,
                    new HashSet<string>(StringComparer.Ordinal),
                    out AbiValuePlan? plan,
                    out string error))
                {
                    Add(diagnostics,
                        $"Delegate event handler '{callback.Name}' parameter '{parameter.Name}' is unsupported: {error}");
                    valid = false;
                    break;
                }

                plans.Add(plan!);
            }

            int cellCount = valid ? plans.Sum(plan => plan.CellCount) : 0;
            if (valid && cellCount > MaximumAbiCells)
            {
                Add(diagnostics,
                    $"Delegate event handler '{callback.Name}' requires {cellCount} ABI cells; the limit is {MaximumAbiCells}.");
                valid = false;
            }

            if (!valid)
            {
                continue;
            }

            GuestFunction wrapper = BuildWrapper(callback, callable, plans);
            functions.Add(wrapper);
            exports.Add(new GuestExport(callback.ExportName, wrapper.Id));
        }

        return diagnostics.Count == 0
            ? new CSharpDelegateEventLoweringResult(functions, exports)
            : null;
    }

    private static bool TryCreatePlan(
        string typeId,
        IReadOnlyDictionary<string, GuestType> guestTypes,
        HashSet<string> visiting,
        out AbiValuePlan? plan,
        out string error)
    {
        plan = null;
        if (!guestTypes.TryGetValue(typeId, out GuestType? type))
        {
            error = $"Guest type '{typeId}' is missing.";
            return false;
        }

        if (type.Kind is "scalar" or "enum")
        {
            int cells = type.Storage switch
            {
                "i32" or "f32" => 1,
                "i64" or "f64" => 2,
                _ => 0,
            };
            if (cells == 0)
            {
                error = $"leaf type '{type.Id}' has unsupported storage '{type.Storage}'.";
                return false;
            }

            plan = new AbiValuePlan(type, Array.Empty<(GuestField, AbiValuePlan)>(), cells);
            error = string.Empty;
            return true;
        }

        if (type.Kind != "struct" || !string.Equals(type.Storage, "memory", StringComparison.Ordinal))
        {
            error = $"type '{type.Id}' is not a scalar, enum, or fixed-layout struct.";
            return false;
        }

        if (!visiting.Add(type.Id))
        {
            error = $"struct type '{type.Id}' is recursively defined.";
            return false;
        }

        List<(GuestField Field, AbiValuePlan Value)> fields = new();
        int cellCount = 0;
        foreach (GuestField field in type.Fields
            .OrderBy(field => field.Offset)
            .ThenBy(field => field.Id, StringComparer.Ordinal))
        {
            if (!TryCreatePlan(field.TypeId, guestTypes, visiting, out AbiValuePlan? fieldPlan, out error))
            {
                visiting.Remove(type.Id);
                return false;
            }

            fields.Add((field, fieldPlan!));
            cellCount = checked(cellCount + fieldPlan!.CellCount);
        }

        visiting.Remove(type.Id);
        plan = new AbiValuePlan(type, fields, cellCount);
        error = string.Empty;
        return true;
    }

    private static GuestFunction BuildWrapper(
        SemanticDelegateEventCallback callback,
        SemanticCallable callable,
        IReadOnlyList<AbiValuePlan> plans)
    {
        List<GuestRegister> parameters = new();
        foreach (AbiValuePlan plan in plans)
        {
            AddLeafParameters(callback, plan, parameters);
        }

        List<GuestRegister> locals = new();
        List<GuestInstruction> instructions = new();
        List<string> arguments = new();
        int leafOrdinal = 0;
        int aggregateOrdinal = 0;
        foreach (AbiValuePlan plan in plans)
        {
            arguments.Add(BuildValue(
                callback,
                plan,
                parameters,
                ref leafOrdinal,
                ref aggregateOrdinal,
                locals,
                instructions).Id);
        }

        instructions.Add(new GuestInstruction(
            "call",
            null,
            arguments,
            CSharpGuestIds.Function(callable.MethodSymbolId),
            null,
            null));
        string blockId = CSharpGuestIds.DelegateEventBlock(callback.SubscriptionId);
        return new GuestFunction(
            CSharpGuestIds.DelegateEventFunction(callback.SubscriptionId),
            parameters,
            locals,
            "type:void",
            blockId,
            new[]
            {
                new GuestBasicBlock(
                    blockId,
                    instructions,
                    new GuestTerminator("return", null, null, null, null)),
            });
    }

    private static void AddLeafParameters(
        SemanticDelegateEventCallback callback,
        AbiValuePlan plan,
        List<GuestRegister> parameters)
    {
        if (plan.IsLeaf)
        {
            parameters.Add(new GuestRegister(
                CSharpGuestIds.DelegateEventParameter(callback.SubscriptionId, parameters.Count),
                plan.Type.Id));
            return;
        }

        foreach ((GuestField Field, AbiValuePlan Value) field in plan.Fields)
        {
            AddLeafParameters(callback, field.Value, parameters);
        }
    }

    private static GuestRegister BuildValue(
        SemanticDelegateEventCallback callback,
        AbiValuePlan plan,
        IReadOnlyList<GuestRegister> parameters,
        ref int leafOrdinal,
        ref int aggregateOrdinal,
        List<GuestRegister> locals,
        List<GuestInstruction> instructions)
    {
        if (plan.IsLeaf)
        {
            return parameters[leafOrdinal++];
        }

        List<(GuestField Field, GuestRegister Value)> fieldValues = new();
        foreach ((GuestField field, AbiValuePlan fieldPlan) in plan.Fields)
        {
            fieldValues.Add((field, BuildValue(
                callback,
                fieldPlan,
                parameters,
                ref leafOrdinal,
                ref aggregateOrdinal,
                locals,
                instructions)));
        }

        GuestRegister aggregate = new(
            CSharpGuestIds.DelegateEventAggregate(callback.SubscriptionId, aggregateOrdinal++),
            plan.Type.Id);
        locals.Add(aggregate);
        instructions.Add(new GuestInstruction(
            "stack_alloc",
            aggregate.Id,
            Array.Empty<string>(),
            null,
            null,
            null));
        foreach ((GuestField field, GuestRegister value) in fieldValues)
        {
            instructions.Add(new GuestInstruction(
                "field_store",
                null,
                new[] { aggregate.Id, value.Id },
                field.Id,
                null,
                null));
        }

        return aggregate;
    }

    private static void Add(List<GuestDiagnostic> diagnostics, string message)
    {
        diagnostics.Add(new GuestDiagnostic("ASCG1008", "error", message, null));
    }
}
