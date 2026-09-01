using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal sealed record CSharpGuestDebugInstrumentationResult(
    IReadOnlyList<GuestType> Types,
    IReadOnlyList<GuestImport> Imports,
    IReadOnlyList<GuestFunction> Functions,
    IReadOnlyList<GuestExport> Exports,
    IReadOnlyList<GuestDiagnostic> Diagnostics,
    int ProbeCount);

internal static class CSharpGuestDebugInstrumenter
{
    public static CSharpGuestDebugInstrumentationResult Instrument(
        string moduleId,
        IReadOnlyList<GuestType> types,
        IReadOnlyList<GuestImport> imports,
        IReadOnlyList<GuestFunction> functions,
        IReadOnlyList<GuestExport> exports)
    {
        IReadOnlySet<string> resumableFunctionIds = exports
            .Select(export => export.FunctionId)
            .Where(functionId => functions.Any(function =>
                function.Id == functionId
                && function.ReturnTypeId == CSharpGuestIds.VoidTypeId))
            .ToHashSet(StringComparer.Ordinal);
        return Instrument(
            moduleId,
            types,
            imports,
            functions,
            exports,
            resumableFunctionIds);
    }

    public static CSharpGuestDebugInstrumentationResult Instrument(
        string moduleId,
        IReadOnlyList<GuestType> types,
        IReadOnlyList<GuestImport> imports,
        IReadOnlyList<GuestFunction> functions,
        IReadOnlyList<GuestExport> exports,
        IReadOnlySet<string> resumableFunctionIds)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(moduleId);
        ArgumentNullException.ThrowIfNull(types);
        ArgumentNullException.ThrowIfNull(imports);
        ArgumentNullException.ThrowIfNull(functions);
        ArgumentNullException.ThrowIfNull(exports);
        ArgumentNullException.ThrowIfNull(resumableFunctionIds);

        return CSharpGuestDebugResumableInstrumenter.Instrument(
            moduleId,
            types,
            imports,
            functions,
            exports,
            resumableFunctionIds);
    }
}
