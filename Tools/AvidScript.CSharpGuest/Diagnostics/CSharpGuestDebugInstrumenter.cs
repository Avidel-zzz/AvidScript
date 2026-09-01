using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal sealed record CSharpGuestDebugInstrumentationResult(
    IReadOnlyList<GuestType> Types,
    IReadOnlyList<GuestImport> Imports,
    IReadOnlyList<GuestFunction> Functions,
    int ProbeCount);

internal static class CSharpGuestDebugInstrumenter
{
    public static CSharpGuestDebugInstrumentationResult Instrument(
        string moduleId,
        IReadOnlyList<GuestType> types,
        IReadOnlyList<GuestImport> imports,
        IReadOnlyList<GuestFunction> functions)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(moduleId);
        ArgumentNullException.ThrowIfNull(types);
        ArgumentNullException.ThrowIfNull(imports);
        ArgumentNullException.ThrowIfNull(functions);

        if (imports.Any(item => item.Id == CSharpGuestDebugProbeAbi.ImportId
                || (item.Module == CSharpGuestDebugProbeAbi.ModuleName
                    && item.Name == CSharpGuestDebugProbeAbi.ImportName)))
        {
            throw new InvalidOperationException("ASDEBUG1101: Debug probe import identity is reserved.");
        }

        int probeCount = 0;
        GuestFunction[] instrumentedFunctions = new GuestFunction[functions.Count];
        for (int index = 0; index < functions.Count; ++index)
        {
            instrumentedFunctions[index] = InstrumentFunction(
                moduleId,
                functions[index],
                ref probeCount);
        }
        if (probeCount == 0)
        {
            return new CSharpGuestDebugInstrumentationResult(types, imports, functions, 0);
        }

        IReadOnlyList<GuestType> instrumentedTypes = EnsureAbiTypes(types);
        GuestImport probeImport = new(
            CSharpGuestDebugProbeAbi.ImportId,
            CSharpGuestDebugProbeAbi.ModuleName,
            CSharpGuestDebugProbeAbi.ImportName,
            new[] { CSharpGuestDebugProbeAbi.ProbeIdTypeId },
            CSharpGuestDebugProbeAbi.ActionTypeId,
            "debug");
        return new CSharpGuestDebugInstrumentationResult(
            instrumentedTypes,
            imports.Concat(new[] { probeImport }).ToArray(),
            instrumentedFunctions,
            probeCount);
    }

    private static IReadOnlyList<GuestType> EnsureAbiTypes(IReadOnlyList<GuestType> types)
    {
        List<GuestType> result = types.ToList();
        AddScalarIfMissing(result, CSharpGuestDebugProbeAbi.ProbeIdTypeId, "i64", 8);
        AddScalarIfMissing(result, CSharpGuestDebugProbeAbi.ActionTypeId, "i32", 4);
        return result;
    }

    private static void AddScalarIfMissing(
        ICollection<GuestType> types,
        string typeId,
        string storage,
        int size)
    {
        if (types.Any(item => item.Id == typeId))
        {
            return;
        }
        types.Add(new GuestType(
            typeId,
            "scalar",
            storage,
            Array.Empty<GuestField>(),
            null,
            null,
            size,
            size));
    }

    private static GuestFunction InstrumentFunction(
        string moduleId,
        GuestFunction function,
        ref int moduleProbeCount)
    {
        List<GuestRegister> locals = function.Locals.ToList();
        List<GuestBasicBlock> blocks = new(function.Blocks.Count);
        int functionProbeOrdinal = 0;
        foreach (GuestBasicBlock block in function.Blocks)
        {
            List<GuestInstruction> instructions = new(block.Instructions.Count);
            foreach (GuestInstruction instruction in block.Instructions)
            {
                GuestInstruction emitted = instruction;
                if (instruction.DebugLocation is { Hidden: false } debugLocation)
                {
                    AppendProbe(
                        moduleId,
                        function.Id,
                        functionProbeOrdinal++,
                        debugLocation,
                        locals,
                        instructions);
                    emitted = instruction with { DebugLocation = null };
                    ++moduleProbeCount;
                }
                instructions.Add(emitted);
            }

            GuestTerminator terminator = block.Terminator;
            if (terminator.DebugLocation is { Hidden: false } terminatorLocation)
            {
                AppendProbe(
                    moduleId,
                    function.Id,
                    functionProbeOrdinal++,
                    terminatorLocation,
                    locals,
                    instructions);
                terminator = terminator with { DebugLocation = null };
                ++moduleProbeCount;
            }
            blocks.Add(block with { Instructions = instructions, Terminator = terminator });
        }

        return functionProbeOrdinal == 0
            ? function
            : function with { Locals = locals, Blocks = blocks };
    }

    private static void AppendProbe(
        string moduleId,
        string functionId,
        int ordinal,
        GuestDebugLocation debugLocation,
        ICollection<GuestRegister> locals,
        ICollection<GuestInstruction> instructions)
    {
        string probeId = CSharpGuestDebugProbeIdentity.Create(
            moduleId,
            functionId,
            debugLocation.SemanticOperationId);
        long probeBits = unchecked((long)ulong.Parse(
            probeId,
            NumberStyles.AllowHexSpecifier,
            CultureInfo.InvariantCulture));
        string valuePrefix = $"value:debug_probe:{functionId}:{ordinal}";
        string probeValueId = valuePrefix + ":id";
        string actionValueId = valuePrefix + ":action";
        locals.Add(new GuestRegister(probeValueId, CSharpGuestDebugProbeAbi.ProbeIdTypeId));
        locals.Add(new GuestRegister(actionValueId, CSharpGuestDebugProbeAbi.ActionTypeId));
        instructions.Add(new GuestInstruction(
            "constant",
            probeValueId,
            Array.Empty<string>(),
            null,
            null,
            new GuestConstant("int64", probeBits.ToString(CultureInfo.InvariantCulture))));
        instructions.Add(new GuestInstruction(
            "call",
            actionValueId,
            new[] { probeValueId },
            CSharpGuestDebugProbeAbi.ImportId,
            null,
            null,
            debugLocation));
    }
}
