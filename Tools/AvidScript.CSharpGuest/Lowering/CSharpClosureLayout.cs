using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal static class CSharpClosureLayout
{
    public const string ObjectType = "type:$closure:object";
    public const string HeapImport = "import:$closure:heap";
    public const string TargetField = "$target", ContextField = "$context";
    public static string Reference(string environment) => "type:$closure:ref:" + environment;
    public static string Payload(string environment) => "type:$closure:payload:" + environment;
    public static string FunctionType(string signature) => signature + ":$closure:function";
    public static string Binding(string method) => "$closure:binding:" + method;
    public static string Thunk(string method, string signature) => CSharpGuestIds.Function(method) + ":$closure:thunk:" + signature;
    public static bool UsesManagedDelegates(SemanticDocument document) => document.ClosureEnvironments.Count != 0
        || CSharpDelegateComposition.Signatures(document).Count != 0;

    public static void AddTypes(SemanticDocument document, List<GuestType> types)
    {
        if (!UsesManagedDelegates(document)) return;
        types.Add(new(ObjectType, "managed_ref", "i64", Array.Empty<GuestField>(), null, null, 8, 8));
        foreach (SemanticDelegateType signature in document.DelegateTypes)
        {
            int index = types.FindIndex(type => type.Id == signature.TypeId);
            if (index < 0) continue;
            types[index] = Struct(signature.TypeId, new[] { Field(TargetField, FunctionType(signature.TypeId)), Field(ContextField, ObjectType) });
            types.Add(new(FunctionType(signature.TypeId), "function_ref", "i32", Array.Empty<GuestField>(), null, null, 4, 4));
        }
        foreach (SemanticClosureEnvironment environment in document.ClosureEnvironments)
            AddEnvironment(environment.Id, environment.Cells.Select(cell => Field(cell.SymbolId, cell.TypeId)).ToArray());
        foreach (SemanticClosureBinding binding in document.ClosureBindings.Where(binding => binding.IsDelegateTarget))
            AddEnvironment(Binding(binding.MethodSymbolId), Environments(document, binding.MethodSymbolId)
                .Select(environment => Field(environment.Id, Reference(environment.Id))).ToArray());
        CSharpDelegateComposition.AddTypes(document, types);
        void AddEnvironment(string id, GuestField[] fields)
        {
            types.Add(Struct(Payload(id), fields));
            types.Add(new(Reference(id), "managed_ref", "i64", Array.Empty<GuestField>(), Payload(id), null, 8, 8));
        }
    }

    public static IReadOnlyList<SemanticClosureEnvironment> Environments(SemanticDocument document, string method)
    {
        SemanticClosureBinding? binding = document.ClosureBindings.SingleOrDefault(item => item.MethodSymbolId == method);
        return binding is null ? Array.Empty<SemanticClosureEnvironment>() : document.ClosureEnvironments
            .Where(environment => environment.Cells.Any(cell => binding.CellSymbolIds.Contains(cell.SymbolId)))
            .OrderBy(environment => environment.Id, StringComparer.Ordinal).ToArray();
    }
    public static (SemanticClosureEnvironment Environment, SemanticClosureCell Cell)? CapturedParameter(SemanticDocument document, string symbol)
    {
        foreach (SemanticClosureBinding binding in document.ClosureBindings)
        {
            string prefix = $"symbol:parameter:{binding.MethodSymbolId}:capture:";
            if (!symbol.StartsWith(prefix, StringComparison.Ordinal)) continue;
            string original = symbol[prefix.Length..];
            foreach (SemanticClosureEnvironment environment in document.ClosureEnvironments)
                if (environment.Cells.SingleOrDefault(cell => cell.SymbolId == original) is { } cell) return (environment, cell);
        }
        return null;
    }
    public static string ParameterType(SemanticDocument document, SemanticCallableParameter parameter) =>
        CapturedParameter(document, parameter.SymbolId) is { } capture ? Reference(capture.Environment.Id)
            : CSharpBorrowedReferences.Parameter(document, parameter.TypeId, parameter.RefKind);
    private static GuestType Struct(string id, GuestField[] fields) => new(id, "struct", "memory", fields, null, null, 0, 1);
    private static GuestField Field(string id, string type) => new(id, id, type, 0);
}
