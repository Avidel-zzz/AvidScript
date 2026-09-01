using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal static class CSharpGuestDebugResumableInstrumenter
{
    private const int MaximumFrameBytes = 4096;
    private const string ResumeExportName = "avid_on_debug_resume";
    private const string ResumeFunctionId =
        CSharpGuestIds.DelegateEventFunctionPrefix + "debug_resume:v1";
    private const string WrapperFunctionPrefix =
        CSharpGuestIds.DelegateEventFunctionPrefix + "debug_resumable_wrapper:v1:";

    public static CSharpGuestDebugInstrumentationResult Instrument(
        string moduleId,
        IReadOnlyList<GuestType> types,
        IReadOnlyList<GuestImport> imports,
        IReadOnlyList<GuestFunction> functions,
        IReadOnlyList<GuestExport> exports,
        IReadOnlySet<string> resumableFunctionIds)
    {
        Dictionary<string, GuestType> typesById = types.ToDictionary(
            type => type.Id,
            StringComparer.Ordinal);
        HashSet<string> guestCallTargets = functions
            .SelectMany(function => function.Blocks)
            .SelectMany(block => block.Instructions)
            .Where(instruction => instruction.Op == "call"
                && instruction.TargetId is not null)
            .Select(instruction => instruction.TargetId!)
            .ToHashSet(StringComparer.Ordinal);
        GuestFunction[] candidates = functions
            .Where(function => resumableFunctionIds.Contains(function.Id)
                && function.ReturnTypeId == CSharpGuestIds.VoidTypeId
                && !guestCallTargets.Contains(function.Id)
                && CountVisiblePoints(function) != 0)
            .OrderBy(function => function.Id, StringComparer.Ordinal)
            .ToArray();
        if (candidates.Length == 0)
        {
            return new CSharpGuestDebugInstrumentationResult(
                types,
                imports,
                functions,
                exports,
                Array.Empty<GuestDiagnostic>(),
                0);
        }

        ValidateReservedIdentities(types, imports, functions, exports, candidates);

        Dictionary<string, string> wrapperIds = candidates.ToDictionary(
            function => function.Id,
            function => WrapperFunctionPrefix + function.Id,
            StringComparer.Ordinal);
        Dictionary<string, GuestFunction> rewrittenCandidates = candidates.ToDictionary(
            function => function.Id,
            function => RewriteCalls(function, wrapperIds),
            StringComparer.Ordinal);

        List<ResumablePlan> plans = new(candidates.Length);
        int nextRoute = 1;
        foreach (GuestFunction candidate in candidates)
        {
            GuestFunction rewritten = rewrittenCandidates[candidate.Id];
            int probeCount = CountVisiblePoints(rewritten);
            int[] routes = Enumerable.Range(nextRoute, probeCount).ToArray();
            nextRoute = checked(nextRoute + probeCount);
            plans.Add(CreatePlan(rewritten, wrapperIds[rewritten.Id], routes, typesById));
        }

        Dictionary<string, ResumablePlan> plansByFunction = plans.ToDictionary(
            plan => plan.Original.Id,
            StringComparer.Ordinal);
        List<GuestFunction> resultFunctions = new(functions.Count + plans.Count + 1);
        foreach (GuestFunction function in functions)
        {
            GuestFunction rewritten = RewriteCalls(function, wrapperIds);
            resultFunctions.Add(plansByFunction.TryGetValue(function.Id, out ResumablePlan? plan)
                ? BuildMachine(plan with { Original = rewritten }, moduleId)
                : rewritten);
        }

        foreach (ResumablePlan plan in plans)
        {
            resultFunctions.Add(BuildWrapper(plan));
        }
        resultFunctions.Add(BuildResumeRouter(plans, typesById));

        List<GuestType> resultTypes = EnsureAbiTypes(types);
        resultTypes.AddRange(plans.Select(plan => plan.Frame.Type));

        List<GuestImport> resultImports = imports.ToList();
        resultImports.AddRange(CreateDebugImports());

        GuestExport[] resultExports = exports
            .Select(export => wrapperIds.TryGetValue(export.FunctionId, out string? wrapperId)
                ? export with { FunctionId = wrapperId }
                : export)
            .Append(new GuestExport(ResumeExportName, ResumeFunctionId))
            .OrderBy(export => export.Name, StringComparer.Ordinal)
            .ToArray();

        return new CSharpGuestDebugInstrumentationResult(
            resultTypes,
            resultImports,
            resultFunctions,
            resultExports,
            Array.Empty<GuestDiagnostic>(),
            nextRoute - 1);
    }

    private static void ValidateReservedIdentities(
        IReadOnlyList<GuestType> types,
        IReadOnlyList<GuestImport> imports,
        IReadOnlyList<GuestFunction> functions,
        IReadOnlyList<GuestExport> exports,
        IReadOnlyList<GuestFunction> candidates)
    {
        (string Id, string Name)[] reservedImports =
        {
            (CSharpGuestDebugProbeAbi.ImportId, CSharpGuestDebugProbeAbi.ImportName),
            (CSharpGuestDebugProbeAbi.SuspendImportId, CSharpGuestDebugProbeAbi.SuspendImportName),
            (CSharpGuestDebugProbeAbi.FrameReadImportId, CSharpGuestDebugProbeAbi.FrameReadImportName),
        };
        foreach ((string id, string name) in reservedImports)
        {
            if (imports.Any(item => item.Id == id
                    || (item.Module == CSharpGuestDebugProbeAbi.ModuleName && item.Name == name)))
            {
                throw new InvalidOperationException(
                    $"ASDEBUG1101: Debug import identity '{id}' is reserved.");
            }
        }

        HashSet<string> functionIds = functions
            .Select(function => function.Id)
            .ToHashSet(StringComparer.Ordinal);
        if (functionIds.Contains(ResumeFunctionId)
            || exports.Any(export => export.Name == ResumeExportName))
        {
            throw new InvalidOperationException(
                "ASDEBUG1101: Debug resume export identity is reserved.");
        }

        HashSet<string> typeIds = types.Select(type => type.Id).ToHashSet(StringComparer.Ordinal);
        foreach (GuestFunction candidate in candidates)
        {
            string wrapperId = WrapperFunctionPrefix + candidate.Id;
            string frameTypeId = CSharpGuestDebugProbeAbi.FrameTypePrefix + candidate.Id;
            if (functionIds.Contains(wrapperId) || typeIds.Contains(frameTypeId))
            {
                throw new InvalidOperationException(
                    $"ASDEBUG1101: Debug resumable identity for '{candidate.Id}' is reserved.");
            }
        }
    }

    private static List<GuestType> EnsureAbiTypes(IReadOnlyList<GuestType> types)
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
        if (!types.Any(item => item.Id == typeId))
        {
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
    }

    private static IReadOnlyList<GuestImport> CreateDebugImports()
    {
        return new[]
        {
            new GuestImport(
                CSharpGuestDebugProbeAbi.ImportId,
                CSharpGuestDebugProbeAbi.ModuleName,
                CSharpGuestDebugProbeAbi.ImportName,
                new[] { CSharpGuestDebugProbeAbi.ProbeIdTypeId },
                CSharpGuestDebugProbeAbi.ActionTypeId,
                "debug"),
            new GuestImport(
                CSharpGuestDebugProbeAbi.SuspendImportId,
                CSharpGuestDebugProbeAbi.ModuleName,
                CSharpGuestDebugProbeAbi.SuspendImportName,
                new[]
                {
                    CSharpGuestDebugProbeAbi.ProbeIdTypeId,
                    CSharpGuestDebugProbeAbi.ActionTypeId,
                    CSharpGuestIds.AddressTypeId,
                    CSharpGuestDebugProbeAbi.ActionTypeId,
                },
                CSharpGuestDebugProbeAbi.ProbeIdTypeId,
                "debug"),
            new GuestImport(
                CSharpGuestDebugProbeAbi.FrameReadImportId,
                CSharpGuestDebugProbeAbi.ModuleName,
                CSharpGuestDebugProbeAbi.FrameReadImportName,
                new[]
                {
                    CSharpGuestDebugProbeAbi.ProbeIdTypeId,
                    CSharpGuestIds.AddressTypeId,
                    CSharpGuestDebugProbeAbi.ActionTypeId,
                },
                CSharpGuestDebugProbeAbi.ActionTypeId,
                "debug"),
        };
    }

    private static ResumablePlan CreatePlan(
        GuestFunction function,
        string wrapperId,
        IReadOnlyList<int> routes,
        IReadOnlyDictionary<string, GuestType> types)
    {
        List<GuestField> fields = new();
        int offset = 0;
        int alignment = 1;

        GuestType routeType = types[CSharpGuestDebugProbeAbi.ActionTypeId];
        GuestField routeField = AddField(CSharpGuestDebugProbeAbi.FrameRouteFieldName, routeType);
        List<SpillSlot> slots = new(function.Parameters.Count + function.Locals.Count);
        foreach (GuestRegister register in function.Parameters.Concat(function.Locals))
        {
            GuestType type = types[register.TypeId];
            GuestField field = AddField(
                $"slot_{slots.Count.ToString(CultureInfo.InvariantCulture)}",
                type);
            slots.Add(new SpillSlot(register, field));
        }

        int frameSize = AlignUp(offset, alignment);
        if (frameSize <= 0 || frameSize > MaximumFrameBytes)
        {
            throw new InvalidOperationException(
                $"ASDEBUG1102: Debug frame for '{function.Id}' is {frameSize} bytes; maximum is {MaximumFrameBytes}.");
        }

        GuestType frameType = new(
            CSharpGuestDebugProbeAbi.FrameTypePrefix + function.Id,
            "struct",
            "memory",
            fields,
            null,
            null,
            frameSize,
            alignment);
        return new ResumablePlan(
            function,
            wrapperId,
            new FramePlan(frameType, routeField, slots),
            routes);

        GuestField AddField(string name, GuestType type)
        {
            if (type.Kind == "void" || type.Size <= 0)
            {
                throw new InvalidOperationException(
                    $"ASDEBUG1102: Debug frame for '{function.Id}' contains unsupported type '{type.Id}'.");
            }

            offset = AlignUp(offset, type.Alignment);
            GuestField field = new(
                $"field:synthetic:debug_resumable:{function.Id}:{name}",
                name,
                type.Id,
                offset);
            fields.Add(field);
            offset = checked(offset + type.Size);
            alignment = Math.Max(alignment, type.Alignment);
            return field;
        }
    }

    private static int AlignUp(int value, int alignment)
    {
        if (value < 0
            || alignment <= 0
            || alignment > 16
            || (alignment & (alignment - 1)) != 0)
        {
            throw new OverflowException("Invalid debug frame layout.");
        }
        return checked((value + alignment - 1) & -alignment);
    }

    private static GuestFunction RewriteCalls(
        GuestFunction function,
        IReadOnlyDictionary<string, string> wrapperIds)
    {
        GuestBasicBlock[] blocks = function.Blocks.Select(block => block with
        {
            Instructions = block.Instructions.Select(instruction =>
                instruction.Op == "call"
                    && instruction.TargetId is not null
                    && wrapperIds.TryGetValue(instruction.TargetId, out string? wrapperId)
                    ? instruction with { TargetId = wrapperId }
                    : instruction).ToArray(),
        }).ToArray();
        return function with { Blocks = blocks };
    }

    private static GuestFunction BuildMachine(ResumablePlan plan, string moduleId)
    {
        GuestFunction function = plan.Original;
        List<GuestRegister> parameters = function.Parameters.ToList();
        GuestRegister tokenParameter = new(
            $"value:debug_resumable:{function.Id}:parameter:token",
            CSharpGuestDebugProbeAbi.ProbeIdTypeId);
        GuestRegister routeParameter = new(
            $"value:debug_resumable:{function.Id}:parameter:route",
            CSharpGuestDebugProbeAbi.ActionTypeId);
        parameters.Add(tokenParameter);
        parameters.Add(routeParameter);

        List<GuestRegister> locals = function.Locals.ToList();
        HashSet<string> valueIds = parameters
            .Concat(locals)
            .Select(register => register.Id)
            .ToHashSet(StringComparer.Ordinal);
        GuestRegister AddLocal(string name, string typeId)
        {
            GuestRegister result = new(
                $"value:debug_resumable:{function.Id}:{name}",
                typeId);
            if (!valueIds.Add(result.Id))
            {
                throw new InvalidOperationException(
                    $"ASDEBUG1101: Debug local identity '{result.Id}' is reserved.");
            }
            locals.Add(result);
            return result;
        }

        GuestRegister frame = AddLocal("frame", plan.Frame.Type.Id);
        GuestRegister frameAddress = AddLocal("frame_address", CSharpGuestIds.AddressTypeId);
        GuestRegister frameBytes = AddLocal("frame_bytes", CSharpGuestDebugProbeAbi.ActionTypeId);
        GuestRegister zeroRoute = AddLocal("fresh_route", CSharpGuestDebugProbeAbi.ActionTypeId);
        GuestRegister isFresh = AddLocal("is_fresh", CSharpGuestDebugProbeAbi.ActionTypeId);
        GuestRegister readStatus = AddLocal("frame_read_status", CSharpGuestDebugProbeAbi.ActionTypeId);
        GuestRegister storedRoute = AddLocal("stored_route", CSharpGuestDebugProbeAbi.ActionTypeId);
        GuestRegister routeMatchesFrame = AddLocal(
            "route_matches_frame",
            CSharpGuestDebugProbeAbi.ActionTypeId);

        string blockPrefix = $"block:synthetic:debug_resumable:{function.Id}:";
        string entryBlockId = blockPrefix + "entry";
        string readBlockId = blockPrefix + "read_frame";
        string verifyBlockId = blockPrefix + "verify_frame_route";
        string restoreBlockId = blockPrefix + "restore_frame";
        string trapBlockId = blockPrefix + "trap";

        List<GuestBasicBlock> blocks = new();
        blocks.Add(new GuestBasicBlock(
            entryBlockId,
            new GuestInstruction[]
            {
                StackAlloc(frame),
                AddressOf(frameAddress, frame.Id),
                Constant(frameBytes, "int32", plan.Frame.Type.Size),
                Constant(zeroRoute, "int32", 0),
                Binary(isFresh, routeParameter.Id, zeroRoute.Id, "equals"),
            },
            BranchIf(isFresh.Id, function.EntryBlockId, blockPrefix + "validate_route:0")));

        for (int index = 0; index < plan.Routes.Count; ++index)
        {
            int route = plan.Routes[index];
            GuestRegister expectedRoute = AddLocal(
                $"validate_route:{index}:expected",
                CSharpGuestDebugProbeAbi.ActionTypeId);
            GuestRegister routeMatches = AddLocal(
                $"validate_route:{index}:matches",
                CSharpGuestDebugProbeAbi.ActionTypeId);
            string falseTarget = index + 1 < plan.Routes.Count
                ? blockPrefix + $"validate_route:{index + 1}"
                : trapBlockId;
            blocks.Add(new GuestBasicBlock(
                blockPrefix + $"validate_route:{index}",
                new GuestInstruction[]
                {
                    Constant(expectedRoute, "int32", route),
                    Binary(routeMatches, routeParameter.Id, expectedRoute.Id, "equals"),
                },
                BranchIf(routeMatches.Id, readBlockId, falseTarget)));
        }

        blocks.Add(new GuestBasicBlock(
            readBlockId,
            new[]
            {
                new GuestInstruction(
                    "call",
                    readStatus.Id,
                    new[] { tokenParameter.Id, frameAddress.Id, frameBytes.Id },
                    CSharpGuestDebugProbeAbi.FrameReadImportId,
                    null,
                    null),
            },
            BranchIf(readStatus.Id, verifyBlockId, trapBlockId)));
        blocks.Add(new GuestBasicBlock(
            verifyBlockId,
            new GuestInstruction[]
            {
                FieldLoad(storedRoute, frame.Id, plan.Frame.RouteField.Id),
                Binary(routeMatchesFrame, routeParameter.Id, storedRoute.Id, "equals"),
            },
            BranchIf(routeMatchesFrame.Id, restoreBlockId, trapBlockId)));

        List<GuestInstruction> restoreInstructions = new();
        for (int index = 0; index < plan.Frame.Slots.Count; ++index)
        {
            SpillSlot slot = plan.Frame.Slots[index];
            GuestRegister restored = AddLocal($"restore:{index}", slot.Register.TypeId);
            restoreInstructions.Add(FieldLoad(restored, frame.Id, slot.Field.Id));
            restoreInstructions.Add(LocalStore(slot.Register.Id, restored.Id));
        }
        AppendAddressReconstruction(
            function,
            plan.Frame.Slots,
            restoreInstructions,
            AddLocal);
        blocks.Add(new GuestBasicBlock(
            restoreBlockId,
            restoreInstructions,
            Branch(blockPrefix + "dispatch_route:0")));

        MachineBody body = TransformBody(
            plan,
            moduleId,
            frame,
            frameAddress,
            frameBytes,
            trapBlockId,
            locals,
            valueIds);
        for (int index = 0; index < plan.Routes.Count; ++index)
        {
            GuestRegister expectedRoute = AddLocal(
                $"dispatch_route:{index}:expected",
                CSharpGuestDebugProbeAbi.ActionTypeId);
            GuestRegister routeMatches = AddLocal(
                $"dispatch_route:{index}:matches",
                CSharpGuestDebugProbeAbi.ActionTypeId);
            string falseTarget = index + 1 < plan.Routes.Count
                ? blockPrefix + $"dispatch_route:{index + 1}"
                : trapBlockId;
            blocks.Add(new GuestBasicBlock(
                blockPrefix + $"dispatch_route:{index}",
                new GuestInstruction[]
                {
                    Constant(expectedRoute, "int32", plan.Routes[index]),
                    Binary(routeMatches, routeParameter.Id, expectedRoute.Id, "equals"),
                },
                BranchIf(routeMatches.Id, body.ContinueBlocks[index], falseTarget)));
        }

        blocks.Add(new GuestBasicBlock(
            trapBlockId,
            Array.Empty<GuestInstruction>(),
            Trap()));
        blocks.AddRange(body.Blocks);

        return function with
        {
            Parameters = parameters,
            Locals = locals,
            EntryBlockId = entryBlockId,
            Blocks = blocks,
        };
    }

    private static void AppendAddressReconstruction(
        GuestFunction function,
        IReadOnlyList<SpillSlot> slots,
        ICollection<GuestInstruction> instructions,
        Func<string, string, GuestRegister> addLocal)
    {
        Dictionary<string, GuestInstruction> definitions = function.Blocks
            .SelectMany(block => block.Instructions)
            .Where(instruction => instruction.ResultId is not null)
            .ToDictionary(instruction => instruction.ResultId!, StringComparer.Ordinal);
        int ordinal = 0;
        foreach (SpillSlot slot in slots)
        {
            if (slot.Register.TypeId != CSharpGuestIds.AddressTypeId
                || !definitions.TryGetValue(slot.Register.Id, out GuestInstruction? definition)
                || definition.Op != "address_of"
                || definition.TargetId is null)
            {
                continue;
            }

            GuestRegister reconstructed = addLocal(
                $"restore_address:{ordinal++}",
                CSharpGuestIds.AddressTypeId);
            instructions.Add(AddressOf(reconstructed, definition.TargetId));
            instructions.Add(LocalStore(slot.Register.Id, reconstructed.Id));
        }
    }

    private static MachineBody TransformBody(
        ResumablePlan plan,
        string moduleId,
        GuestRegister frame,
        GuestRegister frameAddress,
        GuestRegister frameBytes,
        string trapBlockId,
        ICollection<GuestRegister> locals,
        ISet<string> valueIds)
    {
        List<GuestBasicBlock> blocks = new();
        List<string> continueBlocks = new(plan.Routes.Count);
        int probeOrdinal = 0;
        foreach (GuestBasicBlock originalBlock in plan.Original.Blocks)
        {
            string currentBlockId = originalBlock.Id;
            List<GuestInstruction> currentInstructions = new();
            foreach (GuestInstruction originalInstruction in originalBlock.Instructions)
            {
                if (originalInstruction.DebugLocation is not { Hidden: false } debugLocation)
                {
                    currentInstructions.Add(originalInstruction);
                    continue;
                }

                ProbeBlocks probe = BuildProbeBlocks(
                    plan,
                    moduleId,
                    probeOrdinal,
                    debugLocation,
                    frame,
                    frameAddress,
                    frameBytes,
                    trapBlockId,
                    locals,
                    valueIds);
                blocks.Add(new GuestBasicBlock(
                    currentBlockId,
                    currentInstructions,
                    Branch(probe.ProbeBlockId)));
                blocks.AddRange(probe.Blocks);
                continueBlocks.Add(probe.ContinueBlockId);
                currentBlockId = probe.ContinueBlockId;
                currentInstructions = new List<GuestInstruction>
                {
                    originalInstruction with { DebugLocation = null },
                };
                ++probeOrdinal;
            }

            if (originalBlock.Terminator.DebugLocation is { Hidden: false } terminatorLocation)
            {
                ProbeBlocks probe = BuildProbeBlocks(
                    plan,
                    moduleId,
                    probeOrdinal,
                    terminatorLocation,
                    frame,
                    frameAddress,
                    frameBytes,
                    trapBlockId,
                    locals,
                    valueIds);
                blocks.Add(new GuestBasicBlock(
                    currentBlockId,
                    currentInstructions,
                    Branch(probe.ProbeBlockId)));
                blocks.AddRange(probe.Blocks);
                continueBlocks.Add(probe.ContinueBlockId);
                blocks.Add(new GuestBasicBlock(
                    probe.ContinueBlockId,
                    Array.Empty<GuestInstruction>(),
                    originalBlock.Terminator with { DebugLocation = null }));
                ++probeOrdinal;
            }
            else
            {
                blocks.Add(new GuestBasicBlock(
                    currentBlockId,
                    currentInstructions,
                    originalBlock.Terminator));
            }
        }

        if (probeOrdinal != plan.Routes.Count)
        {
            throw new InvalidOperationException(
                $"ASDEBUG1102: Debug point count changed while instrumenting '{plan.Original.Id}'.");
        }
        return new MachineBody(blocks, continueBlocks);
    }

    private static ProbeBlocks BuildProbeBlocks(
        ResumablePlan plan,
        string moduleId,
        int probeOrdinal,
        GuestDebugLocation debugLocation,
        GuestRegister frame,
        GuestRegister frameAddress,
        GuestRegister frameBytes,
        string trapBlockId,
        ICollection<GuestRegister> locals,
        ISet<string> valueIds)
    {
        string functionId = plan.Original.Id;
        int route = plan.Routes[probeOrdinal];
        string valuePrefix = $"value:debug_resumable:{functionId}:probe:{probeOrdinal}";
        GuestRegister AddLocal(string name, string typeId)
        {
            GuestRegister result = new(valuePrefix + ":" + name, typeId);
            if (!valueIds.Add(result.Id))
            {
                throw new InvalidOperationException(
                    $"ASDEBUG1101: Debug probe local identity '{result.Id}' is reserved.");
            }
            locals.Add(result);
            return result;
        }

        GuestRegister probeId = AddLocal("id", CSharpGuestDebugProbeAbi.ProbeIdTypeId);
        GuestRegister action = AddLocal("action", CSharpGuestDebugProbeAbi.ActionTypeId);
        GuestRegister pauseAction = AddLocal("pause", CSharpGuestDebugProbeAbi.ActionTypeId);
        GuestRegister isPause = AddLocal("is_pause", CSharpGuestDebugProbeAbi.ActionTypeId);
        GuestRegister continueAction = AddLocal("continue", CSharpGuestDebugProbeAbi.ActionTypeId);
        GuestRegister isContinue = AddLocal("is_continue", CSharpGuestDebugProbeAbi.ActionTypeId);
        GuestRegister routeValue = AddLocal("route", CSharpGuestDebugProbeAbi.ActionTypeId);
        GuestRegister suspendToken = AddLocal("suspend_token", CSharpGuestDebugProbeAbi.ProbeIdTypeId);
        GuestRegister zeroToken = AddLocal("zero_token", CSharpGuestDebugProbeAbi.ProbeIdTypeId);
        GuestRegister tokenPositive = AddLocal("token_positive", CSharpGuestDebugProbeAbi.ActionTypeId);

        string probeIdHex = CSharpGuestDebugProbeIdentity.Create(
            moduleId,
            functionId,
            debugLocation.SemanticOperationId);
        long probeBits = unchecked((long)ulong.Parse(
            probeIdHex,
            NumberStyles.AllowHexSpecifier,
            CultureInfo.InvariantCulture));

        string blockPrefix = $"block:synthetic:debug_resumable:{functionId}:probe:{probeOrdinal}:";
        string probeBlockId = blockPrefix + "check";
        string actionBlockId = blockPrefix + "action";
        string pauseBlockId = blockPrefix + "pause";
        string returnBlockId = blockPrefix + "return";
        string continueBlockId = blockPrefix + "continue";
        GuestBasicBlock probeBlock = new(
            probeBlockId,
            new GuestInstruction[]
            {
                Constant(probeId, "int64", probeBits),
                new GuestInstruction(
                    "call",
                    action.Id,
                    new[] { probeId.Id },
                    CSharpGuestDebugProbeAbi.ImportId,
                    null,
                    null,
                    debugLocation),
                Constant(pauseAction, "int32", CSharpGuestDebugProbeAbi.PauseAction),
                Binary(isPause, action.Id, pauseAction.Id, "equals"),
                Constant(routeValue, "int32", route),
            },
            BranchIf(isPause.Id, pauseBlockId, actionBlockId));
        GuestBasicBlock actionBlock = new(
            actionBlockId,
            new GuestInstruction[]
            {
                Constant(continueAction, "int32", CSharpGuestDebugProbeAbi.ContinueAction),
                Binary(isContinue, action.Id, continueAction.Id, "equals"),
            },
            BranchIf(isContinue.Id, continueBlockId, trapBlockId));

        List<GuestInstruction> pauseInstructions = new()
        {
            FieldStore(frame.Id, routeValue.Id, plan.Frame.RouteField.Id),
        };
        foreach (SpillSlot slot in plan.Frame.Slots)
        {
            pauseInstructions.Add(FieldStore(frame.Id, slot.Register.Id, slot.Field.Id));
        }
        pauseInstructions.Add(new GuestInstruction(
            "call",
            suspendToken.Id,
            new[] { probeId.Id, routeValue.Id, frameAddress.Id, frameBytes.Id },
            CSharpGuestDebugProbeAbi.SuspendImportId,
            null,
            null));
        pauseInstructions.Add(Constant(zeroToken, "int64", 0L));
        pauseInstructions.Add(Binary(
            tokenPositive,
            suspendToken.Id,
            zeroToken.Id,
            "greater_than"));
        GuestBasicBlock pauseBlock = new(
            pauseBlockId,
            pauseInstructions,
            BranchIf(tokenPositive.Id, returnBlockId, trapBlockId));
        GuestBasicBlock returnBlock = new(
            returnBlockId,
            Array.Empty<GuestInstruction>(),
            Return());
        return new ProbeBlocks(
            probeBlockId,
            continueBlockId,
            new[] { probeBlock, actionBlock, pauseBlock, returnBlock });
    }

    private static GuestFunction BuildWrapper(ResumablePlan plan)
    {
        GuestRegister token = new(
            $"value:debug_resumable_wrapper:{plan.Original.Id}:token",
            CSharpGuestDebugProbeAbi.ProbeIdTypeId);
        GuestRegister route = new(
            $"value:debug_resumable_wrapper:{plan.Original.Id}:route",
            CSharpGuestDebugProbeAbi.ActionTypeId);
        List<string> operands = plan.Original.Parameters
            .Select(parameter => parameter.Id)
            .ToList();
        operands.Add(token.Id);
        operands.Add(route.Id);
        return new GuestFunction(
            plan.WrapperId,
            plan.Original.Parameters,
            new[] { token, route },
            CSharpGuestIds.VoidTypeId,
            $"block:synthetic:debug_resumable_wrapper:{plan.Original.Id}",
            new[]
            {
                new GuestBasicBlock(
                    $"block:synthetic:debug_resumable_wrapper:{plan.Original.Id}",
                    new GuestInstruction[]
                    {
                        Constant(token, "int64", 0L),
                        Constant(route, "int32", 0),
                        new GuestInstruction(
                            "call",
                            null,
                            operands,
                            plan.Original.Id,
                            null,
                            null),
                    },
                    Return()),
            });
    }

    private static GuestFunction BuildResumeRouter(
        IReadOnlyList<ResumablePlan> plans,
        IReadOnlyDictionary<string, GuestType> types)
    {
        GuestRegister token = new(
            "value:debug_resume:parameter:token",
            CSharpGuestDebugProbeAbi.ProbeIdTypeId);
        GuestRegister route = new(
            "value:debug_resume:parameter:route",
            CSharpGuestDebugProbeAbi.ActionTypeId);
        List<GuestRegister> locals = new();
        List<GuestBasicBlock> blocks = new();
        List<(int Route, string CallBlock)> routeTargets = plans
            .SelectMany((plan, planIndex) => plan.Routes.Select(value =>
                (Route: value, CallBlock: $"block:synthetic:debug_resume:call:{planIndex}")))
            .ToList();
        string trapBlockId = "block:synthetic:debug_resume:trap";
        for (int index = 0; index < routeTargets.Count; ++index)
        {
            GuestRegister expected = new(
                $"value:debug_resume:route:{index}:expected",
                CSharpGuestDebugProbeAbi.ActionTypeId);
            GuestRegister matches = new(
                $"value:debug_resume:route:{index}:matches",
                CSharpGuestDebugProbeAbi.ActionTypeId);
            locals.Add(expected);
            locals.Add(matches);
            string next = index + 1 < routeTargets.Count
                ? $"block:synthetic:debug_resume:check:{index + 1}"
                : trapBlockId;
            blocks.Add(new GuestBasicBlock(
                $"block:synthetic:debug_resume:check:{index}",
                new GuestInstruction[]
                {
                    Constant(expected, "int32", routeTargets[index].Route),
                    Binary(matches, route.Id, expected.Id, "equals"),
                },
                BranchIf(matches.Id, routeTargets[index].CallBlock, next)));
        }

        for (int planIndex = 0; planIndex < plans.Count; ++planIndex)
        {
            ResumablePlan plan = plans[planIndex];
            List<GuestInstruction> instructions = new();
            List<string> operands = new();
            for (int parameterIndex = 0;
                parameterIndex < plan.Original.Parameters.Count;
                ++parameterIndex)
            {
                GuestRegister parameter = plan.Original.Parameters[parameterIndex];
                GuestRegister dummy = new(
                    $"value:debug_resume:machine:{planIndex}:dummy:{parameterIndex}",
                    parameter.TypeId);
                locals.Add(dummy);
                GuestType parameterType = types[parameter.TypeId];
                instructions.Add(parameterType.Storage == "memory"
                    ? StackAlloc(dummy)
                    : new GuestInstruction(
                        "constant",
                        dummy.Id,
                        Array.Empty<string>(),
                        null,
                        null,
                        ZeroConstant(parameterType)));
                operands.Add(dummy.Id);
            }
            operands.Add(token.Id);
            operands.Add(route.Id);
            instructions.Add(new GuestInstruction(
                "call",
                null,
                operands,
                plan.Original.Id,
                null,
                null));
            blocks.Add(new GuestBasicBlock(
                $"block:synthetic:debug_resume:call:{planIndex}",
                instructions,
                Return()));
        }
        blocks.Add(new GuestBasicBlock(
            trapBlockId,
            Array.Empty<GuestInstruction>(),
            Trap()));

        return new GuestFunction(
            ResumeFunctionId,
            new[] { token, route },
            locals,
            CSharpGuestIds.VoidTypeId,
            "block:synthetic:debug_resume:check:0",
            blocks);
    }

    private static GuestConstant ZeroConstant(GuestType type)
    {
        return type.Kind switch
        {
            "class_ref" => new GuestConstant("class_ref", "0"),
            "factory_ref" => new GuestConstant("factory_ref", "0"),
            "object_type_ref" => new GuestConstant("object_type_ref", "0"),
            _ => new GuestConstant("zero", null),
        };
    }

    private static int CountVisiblePoints(GuestFunction function)
    {
        return function.Blocks.Sum(block =>
            block.Instructions.Count(instruction => instruction.DebugLocation is { Hidden: false })
            + (block.Terminator.DebugLocation is { Hidden: false } ? 1 : 0));
    }

    private static GuestInstruction Constant(GuestRegister result, string kind, long value)
    {
        return new GuestInstruction(
            "constant",
            result.Id,
            Array.Empty<string>(),
            null,
            null,
            new GuestConstant(kind, value.ToString(CultureInfo.InvariantCulture)));
    }

    private static GuestInstruction Binary(
        GuestRegister result,
        string left,
        string right,
        string operatorKind)
    {
        return new GuestInstruction(
            "binary",
            result.Id,
            new[] { left, right },
            null,
            operatorKind,
            null);
    }

    private static GuestInstruction StackAlloc(GuestRegister result)
    {
        return new GuestInstruction(
            "stack_alloc",
            result.Id,
            Array.Empty<string>(),
            null,
            null,
            null);
    }

    private static GuestInstruction AddressOf(GuestRegister result, string targetId)
    {
        return new GuestInstruction(
            "address_of",
            result.Id,
            Array.Empty<string>(),
            targetId,
            null,
            null);
    }

    private static GuestInstruction FieldLoad(
        GuestRegister result,
        string aggregateId,
        string fieldId)
    {
        return new GuestInstruction(
            "field_load",
            result.Id,
            new[] { aggregateId },
            fieldId,
            null,
            null);
    }

    private static GuestInstruction FieldStore(
        string aggregateId,
        string valueId,
        string fieldId)
    {
        return new GuestInstruction(
            "field_store",
            null,
            new[] { aggregateId, valueId },
            fieldId,
            null,
            null);
    }

    private static GuestInstruction LocalStore(string targetId, string valueId)
    {
        return new GuestInstruction(
            "local_store",
            null,
            new[] { valueId },
            targetId,
            null,
            null);
    }

    private static GuestTerminator Branch(string target) =>
        new("branch", null, target, null, null);

    private static GuestTerminator BranchIf(string condition, string target, string falseTarget) =>
        new("branch_if", condition, target, falseTarget, null);

    private static GuestTerminator Return() =>
        new("return", null, null, null, null);

    private static GuestTerminator Trap() =>
        new("trap", null, null, null, null);

    private sealed record SpillSlot(GuestRegister Register, GuestField Field);

    private sealed record FramePlan(
        GuestType Type,
        GuestField RouteField,
        IReadOnlyList<SpillSlot> Slots);

    private sealed record ResumablePlan(
        GuestFunction Original,
        string WrapperId,
        FramePlan Frame,
        IReadOnlyList<int> Routes);

    private sealed record ProbeBlocks(
        string ProbeBlockId,
        string ContinueBlockId,
        IReadOnlyList<GuestBasicBlock> Blocks);

    private sealed record MachineBody(
        IReadOnlyList<GuestBasicBlock> Blocks,
        IReadOnlyList<string> ContinueBlocks);
}
