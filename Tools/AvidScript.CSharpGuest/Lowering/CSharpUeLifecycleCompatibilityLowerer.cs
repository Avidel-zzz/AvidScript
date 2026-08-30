using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal sealed record CSharpUeLifecycleCompatibilityLoweringResult(
    IReadOnlyList<GuestFunction> Functions,
    IReadOnlyList<GuestExport> Exports);

internal static class CSharpUeLifecycleCompatibilityLowerer
{
    private sealed record CompatibilityEntry(
        string ExportName,
        string FunctionId,
        string BlockId,
        string? ParameterTypeId);

    private static readonly CompatibilityEntry[] Entries =
    {
        new(
            CSharpGuestIds.UeBeginPlayCompatibilityExportName,
            CSharpGuestIds.UeBeginPlayCompatibilityFunctionId,
            "block:synthetic:ue_lifecycle:begin_play",
            null),
        new(
            CSharpGuestIds.UeTickCompatibilityExportName,
            CSharpGuestIds.UeTickCompatibilityFunctionId,
            "block:synthetic:ue_lifecycle:tick",
            CSharpGuestIds.Float32TypeId),
        new(
            CSharpGuestIds.UeEndPlayCompatibilityExportName,
            CSharpGuestIds.UeEndPlayCompatibilityFunctionId,
            "block:synthetic:ue_lifecycle:end_play",
            null),
    };

    public static CSharpUeLifecycleCompatibilityLoweringResult? Lower(
        SemanticDocument document,
        IReadOnlyList<GuestExport> existingExports,
        List<GuestDiagnostic> diagnostics)
    {
        if (document.UeTypeDeclarations.Count == 0)
        {
            return null;
        }

        HashSet<string> lifecycleMethodIds = document.UeTypeDeclarations
            .SelectMany(type => type.Functions)
            .Where(function => function.Flags.Contains("lifecycle"))
            .Select(function => function.MethodSymbolId)
            .ToHashSet(StringComparer.Ordinal);
        HashSet<string> compatibilityNames = Entries
            .Select(entry => entry.ExportName)
            .ToHashSet(StringComparer.Ordinal);
        foreach (SemanticCallable callable in document.Callables.Where(callable =>
            lifecycleMethodIds.Contains(callable.MethodSymbolId)
            && callable.Export is not null
            && compatibilityNames.Contains(callable.Export.Name)))
        {
            diagnostics.Add(new GuestDiagnostic(
                "ASCG1016",
                "error",
                $"UE lifecycle method '{callable.MethodSymbolId}' cannot also export "
                    + $"'{callable.Export!.Name}'; generated shells own instance lifecycle dispatch.",
                null));
        }
        if (diagnostics.Count != 0)
        {
            return null;
        }

        HashSet<string> existingNames = existingExports
            .Select(export => export.Name)
            .ToHashSet(StringComparer.Ordinal);
        List<GuestFunction> functions = new();
        List<GuestExport> exports = new();
        foreach (CompatibilityEntry entry in Entries.Where(entry =>
            !existingNames.Contains(entry.ExportName)))
        {
            GuestRegister[] parameters = entry.ParameterTypeId is null
                ? Array.Empty<GuestRegister>()
                : new[]
                {
                    new GuestRegister(
                        $"value:ue_lifecycle:parameter:{entry.ExportName}",
                        entry.ParameterTypeId),
                };
            functions.Add(new GuestFunction(
                entry.FunctionId,
                parameters,
                Array.Empty<GuestRegister>(),
                CSharpGuestIds.VoidTypeId,
                entry.BlockId,
                new[]
                {
                    new GuestBasicBlock(
                        entry.BlockId,
                        Array.Empty<GuestInstruction>(),
                        new GuestTerminator("return", null, null, null, null)),
                }));
            exports.Add(new GuestExport(entry.ExportName, entry.FunctionId));
        }

        return new CSharpUeLifecycleCompatibilityLoweringResult(functions, exports);
    }
}
