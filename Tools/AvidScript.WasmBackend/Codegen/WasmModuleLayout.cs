using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.GuestIr;

namespace AvidScript.WasmBackend;

internal sealed class WasmModuleLayout
{
    private WasmModuleLayout(
        IReadOnlyDictionary<string, GuestType> types,
        IReadOnlyList<WasmFunctionSignature> signatures,
        IReadOnlyDictionary<string, uint> typeIndices,
        IReadOnlyDictionary<string, uint> functionIndices,
        uint importedFunctionCount)
    {
        Types = types;
        Signatures = signatures;
        TypeIndices = typeIndices;
        FunctionIndices = functionIndices;
        ImportedFunctionCount = importedFunctionCount;
    }

    public IReadOnlyDictionary<string, GuestType> Types { get; }

    public IReadOnlyList<WasmFunctionSignature> Signatures { get; }

    public IReadOnlyDictionary<string, uint> TypeIndices { get; }

    public IReadOnlyDictionary<string, uint> FunctionIndices { get; }

    public uint ImportedFunctionCount { get; }

    public static WasmModuleLayout Create(GuestModule module)
    {
        Dictionary<string, GuestType> types = module.Types.ToDictionary(
            type => type.Id,
            StringComparer.Ordinal);
        List<WasmFunctionSignature> signatures = new();
        Dictionary<WasmFunctionSignature, uint> signatureIndices = new();
        Dictionary<string, uint> typeIndices = new(StringComparer.Ordinal);
        Dictionary<string, uint> functionIndices = new(StringComparer.Ordinal);

        uint functionIndex = 0;
        foreach (GuestImport import in module.Imports)
        {
            WasmFunctionSignature signature = CreateSignature(
                import.ParameterTypeIds,
                import.ReturnTypeId,
                types,
                false);
            typeIndices.Add(import.Id, AddSignature(signature, signatures, signatureIndices));
            functionIndices.Add(import.Id, functionIndex++);
        }

        uint importedFunctionCount = functionIndex;
        foreach (GuestFunction function in module.Functions)
        {
            WasmFunctionSignature signature = CreateSignature(
                function.Parameters.Select(parameter => parameter.TypeId),
                function.ReturnTypeId,
                types,
                IsMemoryType(types[function.ReturnTypeId]));
            typeIndices.Add(function.Id, AddSignature(signature, signatures, signatureIndices));
            functionIndices.Add(function.Id, functionIndex++);
        }

        return new WasmModuleLayout(
            types,
            signatures,
            typeIndices,
            functionIndices,
            importedFunctionCount);
    }

    public WasmValueType ResolveValueType(string typeId)
    {
        return ResolveValueType(Types[typeId]);
    }

    public bool UsesSRet(GuestFunction function)
    {
        return IsMemoryType(Types[function.ReturnTypeId]);
    }

    public bool IsMemoryType(string typeId)
    {
        return IsMemoryType(Types[typeId]);
    }

    private static uint AddSignature(
        WasmFunctionSignature signature,
        List<WasmFunctionSignature> signatures,
        Dictionary<WasmFunctionSignature, uint> signatureIndices)
    {
        if (signatureIndices.TryGetValue(signature, out uint existing))
        {
            return existing;
        }

        uint index = checked((uint)signatures.Count);
        signatures.Add(signature);
        signatureIndices.Add(signature, index);
        return index;
    }

    private static WasmFunctionSignature CreateSignature(
        IEnumerable<string> parameterTypeIds,
        string returnTypeId,
        IReadOnlyDictionary<string, GuestType> types,
        bool usesSRet)
    {
        List<WasmValueType> parameters = new();
        if (usesSRet)
        {
            parameters.Add(WasmValueType.I32);
        }

        parameters.AddRange(parameterTypeIds.Select(typeId => ResolveValueType(types[typeId])));
        GuestType returnType = types[returnTypeId];
        WasmValueType? result = usesSRet || string.Equals(returnType.Storage, "none", StringComparison.Ordinal)
            ? null
            : ResolveValueType(returnType);
        return new WasmFunctionSignature(parameters.ToArray(), result);
    }

    private static WasmValueType ResolveValueType(GuestType type)
    {
        return type.Storage switch
        {
            "i32" or "memory" => WasmValueType.I32,
            "i64" => WasmValueType.I64,
            "f32" => WasmValueType.F32,
            "f64" => WasmValueType.F64,
            _ => throw new NotSupportedException(
                $"Guest type '{type.Id}' storage '{type.Storage}' has no WASM value type."),
        };
    }

    private static bool IsMemoryType(GuestType type)
    {
        return string.Equals(type.Storage, "memory", StringComparison.Ordinal);
    }
}

internal sealed record WasmFunctionSignature(
    IReadOnlyList<WasmValueType> Parameters,
    WasmValueType? Result)
{
    public bool Equals(WasmFunctionSignature? other)
    {
        return other is not null
            && Nullable.Equals(Result, other.Result)
            && Parameters.SequenceEqual(other.Parameters);
    }

    public override int GetHashCode()
    {
        HashCode hash = new();
        foreach (WasmValueType parameter in Parameters)
        {
            hash.Add(parameter);
        }

        hash.Add(Result);
        return hash.ToHashCode();
    }
}
