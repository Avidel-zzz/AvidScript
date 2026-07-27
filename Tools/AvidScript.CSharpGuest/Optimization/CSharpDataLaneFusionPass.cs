using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal sealed record CSharpDataLaneFusionResult(
    bool Succeeded,
    IReadOnlyList<GuestType> Types,
    IReadOnlyList<GuestImport> Imports,
    IReadOnlyList<GuestFunction> Functions,
    IReadOnlyList<GuestDiagnostic> Diagnostics);

internal static class CSharpDataLaneFusionPass
{
    private const int MinimumGroupSize = 3;
    private const uint CommandMagic = 0x41564342u;

    public static CSharpDataLaneFusionResult Run(
        SemanticDocument document,
        IReadOnlyList<GuestType> types,
        IReadOnlyList<GuestImport> imports,
        IReadOnlyList<GuestFunction> functions)
    {
        ArgumentNullException.ThrowIfNull(document);
        ArgumentNullException.ThrowIfNull(types);
        ArgumentNullException.ThrowIfNull(imports);
        ArgumentNullException.ThrowIfNull(functions);

        Dictionary<string, GuestType> typeMap = types.ToDictionary(
            type => type.Id,
            StringComparer.Ordinal);
        Dictionary<string, SetterTarget> targets = BuildTargets(
            document,
            typeMap,
            imports,
            functions);
        List<FunctionPlan> plans = BuildPlans(functions, targets);
        if (plans.All(plan => plan.Blocks.Count == 0))
        {
            return Success(types, imports, functions);
        }

        List<GuestType> rawTypes = types.ToList();
        EnsureScalar(rawTypes, "type:uint16", "i32", 2, 2);
        EnsureScalar(rawTypes, "type:uint32", "i32", 4, 4);
        EnsureScalar(rawTypes, "type:uint64", "i64", 8, 8);

        UniqueIdAllocator typeIds = new(rawTypes.Select(type => type.Id));
        UniqueIdAllocator fieldIds = new(rawTypes.SelectMany(type =>
            type.Fields.Select(field => field.Id)));
        Dictionary<int, string> bufferTypeIds = new();
        foreach (int commandCount in plans
            .SelectMany(plan => plan.Blocks)
            .SelectMany(block => block.Groups)
            .Select(group => group.Calls.Count)
            .Distinct()
            .OrderBy(count => count))
        {
            string typeId = typeIds.Allocate(
                $"type:__avidscript_internal.data_lane_command_buffer.{commandCount}");
            rawTypes.Add(CreateCommandBufferType(typeId, commandCount, fieldIds));
            bufferTypeIds.Add(commandCount, typeId);
        }

        GuestTypeLayoutResult typeLayout = GuestDataLayout.ComputeTypes(rawTypes);
        if (!typeLayout.Succeeded)
        {
            return new CSharpDataLaneFusionResult(
                false,
                Array.Empty<GuestType>(),
                imports,
                functions,
                typeLayout.Diagnostics);
        }

        Dictionary<string, GuestType> laidOutTypes = typeLayout.Types.ToDictionary(
            type => type.Id,
            StringComparer.Ordinal);
        UniqueIdAllocator targetIds = new(
            imports.Select(import => import.Id).Concat(functions.Select(function => function.Id)));
        string epochImportId = targetIds.Allocate(
            "import:__avidscript_internal.avid_data_lane_epoch");
        string submitImportId = targetIds.Allocate(
            "import:__avidscript_internal.avid_data_lane_submit");
        GuestImport epochImport = new(
            epochImportId,
            "avidscript",
            "avid_data_lane_epoch",
            Array.Empty<string>(),
            "type:uint64",
            DispatchClass: "data_lane");
        GuestImport submitImport = new(
            submitImportId,
            "avidscript",
            "avid_data_lane_submit",
            new[] { "type:address", "type:int32" },
            "type:int32",
            DispatchClass: "data_lane");

        Dictionary<string, FunctionPlan> plansByFunction = plans.ToDictionary(
            plan => plan.Function.Id,
            StringComparer.Ordinal);
        GuestFunction[] transformedFunctions = functions
            .Select(function => plansByFunction[function.Id].Blocks.Count == 0
                ? function
                : TransformFunction(
                    plansByFunction[function.Id],
                    laidOutTypes,
                    bufferTypeIds,
                    epochImportId,
                    submitImportId))
            .ToArray();
        GuestImport[] transformedImports = imports
            .Concat(new[] { epochImport, submitImport })
            .ToArray();
        return Success(typeLayout.Types, transformedImports, transformedFunctions);
    }

    private static Dictionary<string, SetterTarget> BuildTargets(
        SemanticDocument document,
        IReadOnlyDictionary<string, GuestType> types,
        IReadOnlyList<GuestImport> imports,
        IReadOnlyList<GuestFunction> functions)
    {
        Dictionary<string, SetterTarget> targets = new(StringComparer.Ordinal);
        Dictionary<string, GuestImport> importsById = imports.ToDictionary(
            import => import.Id,
            StringComparer.Ordinal);
        Dictionary<string, GuestFunction> functionsById = functions.ToDictionary(
            function => function.Id,
            StringComparer.Ordinal);
        foreach (SemanticCallable callable in document.Callables.OrderBy(
            callable => callable.MethodSymbolId,
            StringComparer.Ordinal))
        {
            if (callable.Optimization is not
                    { OptimizationClass: "buffered_write", BindingOrdinal: >= 0 }
                || callable.ReturnTypeId != "type:void"
                || !TryGetSetterShape(callable, out string receiverTypeId)
                || !types.TryGetValue(receiverTypeId, out GuestType? receiverType)
                || receiverType.Kind != "struct")
            {
                continue;
            }

            GuestField[] slotFields = receiverType.Fields
                .Where(field => field.Name == "Slot" && field.TypeId == "type:int32")
                .ToArray();
            GuestField[] generationFields = receiverType.Fields
                .Where(field => field.Name == "Generation" && field.TypeId == "type:int32")
                .ToArray();
            if (slotFields.Length != 1 || generationFields.Length != 1)
            {
                continue;
            }

            SetterTarget target = new(
                callable.Optimization.BindingOrdinal,
                receiverType.Id,
                slotFields[0].Id,
                generationFields[0].Id);
            string targetId;
            if (!callable.HasBody
                && callable.Import is { Module: "avidscript" })
            {
                targetId = CSharpGuestIds.Import(callable.MethodSymbolId);
            }
            else
            {
                targetId = CSharpGuestIds.Function(callable.MethodSymbolId);
                if (callable.Import is not null
                    || callable.AssociatedSymbolId is null
                    || !functionsById.TryGetValue(targetId, out GuestFunction? function)
                    || !IsGeneratedPropertyForwarder(
                        function,
                        target,
                        importsById))
                {
                    continue;
                }
            }
            targets.TryAdd(targetId, target);
        }

        return targets;
    }

    private static bool IsGeneratedPropertyForwarder(
        GuestFunction function,
        SetterTarget target,
        IReadOnlyDictionary<string, GuestImport> imports)
    {
        if (function.Parameters.Count != 2
            || function.Parameters[0].TypeId != target.ReceiverTypeId
            || function.Parameters[1].TypeId != "type:int32")
        {
            return false;
        }

        GuestInstruction[] instructions = function.Blocks
            .SelectMany(block => block.Instructions)
            .ToArray();
        GuestInstruction[] calls = instructions
            .Where(instruction => instruction.Op == "call")
            .ToArray();
        if (instructions.Length != 4
            || calls.Length != 1
            || instructions.Any(instruction => instruction.Op is not
                ("field_load" or "local_load" or "call")))
        {
            return false;
        }

        GuestInstruction call = calls[0];
        if (call.TargetId is null
            || !imports.TryGetValue(call.TargetId, out GuestImport? import)
            || import.Module != "avidscript"
            || import.OptimizationClass != "buffered_write"
            || import.BindingOrdinal != target.BindingOrdinal
            || import.ReturnTypeId != "type:int32"
            || !import.ParameterTypeIds.SequenceEqual(
                new[] { "type:int32", "type:int32", "type:int32" })
            || call.OperandIds.Count != 3)
        {
            return false;
        }

        GuestInstruction[] fieldLoads = instructions
            .Where(instruction => instruction.Op == "field_load")
            .ToArray();
        GuestInstruction[] valueLoads = instructions
            .Where(instruction => instruction.Op == "local_load")
            .ToArray();
        if (fieldLoads.Length != 2
            || valueLoads.Length != 1
            || fieldLoads.Any(load => load.OperandIds.Count != 1
                || load.OperandIds[0] != function.Parameters[0].Id)
            || valueLoads[0].TargetId != function.Parameters[1].Id)
        {
            return false;
        }

        GuestInstruction? slotLoad = fieldLoads.SingleOrDefault(
            load => load.TargetId == target.SlotFieldId);
        GuestInstruction? generationLoad = fieldLoads.SingleOrDefault(
            load => load.TargetId == target.GenerationFieldId);
        if (slotLoad?.ResultId is null
            || generationLoad?.ResultId is null
            || valueLoads[0].ResultId is null
            || call.OperandIds[0] != slotLoad.ResultId
            || call.OperandIds[1] != generationLoad.ResultId
            || call.OperandIds[2] != valueLoads[0].ResultId)
        {
            return false;
        }

        return call.ResultId is null
            || (!instructions.Any(instruction =>
                    instruction != call
                    && instruction.OperandIds.Contains(call.ResultId))
                && function.Blocks.All(block =>
                    block.Terminator.ConditionValueId != call.ResultId
                    && block.Terminator.ReturnValueId != call.ResultId));
    }

    private static bool TryGetSetterShape(SemanticCallable callable, out string receiverTypeId)
    {
        receiverTypeId = string.Empty;
        if (callable.IsStatic)
        {
            if (callable.Parameters.Count != 2
                || callable.Parameters[0].RefKind != "none"
                || callable.Parameters[1].TypeId != "type:int32"
                || callable.Parameters[1].RefKind != "none")
            {
                return false;
            }

            receiverTypeId = callable.Parameters[0].TypeId;
            return true;
        }

        if (callable.Parameters.Count != 1
            || callable.Parameters[0].TypeId != "type:int32"
            || callable.Parameters[0].RefKind != "none")
        {
            return false;
        }

        receiverTypeId = callable.ContainingTypeId;
        return true;
    }

    private static List<FunctionPlan> BuildPlans(
        IReadOnlyList<GuestFunction> functions,
        IReadOnlyDictionary<string, SetterTarget> targets)
    {
        List<FunctionPlan> plans = new(functions.Count);
        foreach (GuestFunction function in functions)
        {
            Dictionary<string, GuestRegister> values = function.Parameters
                .Concat(function.Locals)
                .ToDictionary(value => value.Id, StringComparer.Ordinal);
            List<BlockPlan> blockPlans = new();
            int groupOrdinal = 0;
            foreach (GuestBasicBlock block in function.Blocks)
            {
                List<IReadOnlyList<CandidateCall>> callGroups = FindGroups(block, values, targets);
                if (callGroups.Count == 0)
                {
                    continue;
                }

                FusionGroup[] groups = callGroups
                    .Select(calls => new FusionGroup(groupOrdinal++, calls))
                    .ToArray();
                blockPlans.Add(new BlockPlan(block.Id, groups));
            }

            plans.Add(new FunctionPlan(function, blockPlans));
        }

        return plans;
    }

    private static List<IReadOnlyList<CandidateCall>> FindGroups(
        GuestBasicBlock block,
        IReadOnlyDictionary<string, GuestRegister> values,
        IReadOnlyDictionary<string, SetterTarget> targets)
    {
        Dictionary<string, GuestInstruction> definitions = block.Instructions
            .Where(instruction => instruction.ResultId is not null)
            .ToDictionary(instruction => instruction.ResultId!, StringComparer.Ordinal);
        List<IReadOnlyList<CandidateCall>> groups = new();
        List<CandidateCall> active = new();
        string? receiverId = null;

        void Flush()
        {
            if (active.Count >= MinimumGroupSize)
            {
                groups.Add(active.ToArray());
            }
            active.Clear();
            receiverId = null;
        }

        for (int index = 0; index < block.Instructions.Count; ++index)
        {
            GuestInstruction instruction = block.Instructions[index];
            CandidateCall? candidate = TryGetCandidate(
                instruction,
                index,
                values,
                definitions,
                targets);
            if (candidate is not null)
            {
                if (receiverId is not null
                    && !string.Equals(receiverId, candidate.ReceiverId, StringComparison.Ordinal))
                {
                    Flush();
                }

                receiverId = candidate.ReceiverId;
                active.Add(candidate);
                if (active.Count == CSharpDataLaneAbi.MaximumFusedCommands)
                {
                    Flush();
                }
                continue;
            }

            if (!IsSideEffectFreePreparation(instruction))
            {
                Flush();
            }
        }

        Flush();
        return groups;
    }

    private static CandidateCall? TryGetCandidate(
        GuestInstruction instruction,
        int instructionIndex,
        IReadOnlyDictionary<string, GuestRegister> values,
        IReadOnlyDictionary<string, GuestInstruction> definitions,
        IReadOnlyDictionary<string, SetterTarget> targets)
    {
        if (instruction.Op != "call"
            || instruction.ResultId is not null
            || instruction.TargetId is null
            || instruction.OperandIds.Count != 2
            || !targets.TryGetValue(instruction.TargetId, out SetterTarget? target)
            || !values.TryGetValue(instruction.OperandIds[0], out GuestRegister? receiver)
            || !values.TryGetValue(instruction.OperandIds[1], out GuestRegister? value)
            || receiver.TypeId != target.ReceiverTypeId
            || value.TypeId != "type:int32")
        {
            return null;
        }

        return new CandidateCall(
            instructionIndex,
            instruction,
            target,
            ResolveReceiverId(instruction.OperandIds[0], definitions, new HashSet<string>(StringComparer.Ordinal)));
    }

    private static string ResolveReceiverId(
        string valueId,
        IReadOnlyDictionary<string, GuestInstruction> definitions,
        HashSet<string> visited)
    {
        if (!visited.Add(valueId)
            || !definitions.TryGetValue(valueId, out GuestInstruction? definition))
        {
            return valueId;
        }

        return definition.Op switch
        {
            "copy" when definition.OperandIds.Count == 1 =>
                ResolveReceiverId(definition.OperandIds[0], definitions, visited),
            "local_load" when definition.TargetId is not null =>
                "local:" + definition.TargetId,
            "global_load" when definition.TargetId is not null =>
                "global:" + definition.TargetId,
            "field_load" when definition.TargetId is not null
                && definition.OperandIds.Count == 1 =>
                "field:" + ResolveReceiverId(definition.OperandIds[0], definitions, visited)
                    + ":" + definition.TargetId,
            _ => valueId,
        };
    }

    private static bool IsSideEffectFreePreparation(GuestInstruction instruction)
    {
        return instruction.Op is
            "constant" or "copy" or "local_load" or "global_load" or "field_load"
            or "address_of" or "data_address";
    }

    private static GuestFunction TransformFunction(
        FunctionPlan plan,
        IReadOnlyDictionary<string, GuestType> types,
        IReadOnlyDictionary<int, string> bufferTypeIds,
        string epochImportId,
        string submitImportId)
    {
        List<GuestRegister> locals = plan.Function.Locals.ToList();
        UniqueIdAllocator valueIds = new(
            plan.Function.Parameters.Select(value => value.Id)
                .Concat(plan.Function.Locals.Select(value => value.Id)));
        Dictionary<string, BlockPlan> blocksById = plan.Blocks.ToDictionary(
            block => block.BlockId,
            StringComparer.Ordinal);
        GuestBasicBlock[] blocks = plan.Function.Blocks.Select(block =>
        {
            if (!blocksById.TryGetValue(block.Id, out BlockPlan? blockPlan))
            {
                return block;
            }

            HashSet<int> removedCalls = blockPlan.Groups
                .SelectMany(group => group.Calls)
                .Select(call => call.InstructionIndex)
                .ToHashSet();
            Dictionary<int, FusionGroup> groupsByTail = blockPlan.Groups.ToDictionary(
                group => group.Calls[^1].InstructionIndex);
            List<GuestInstruction> instructions = new();
            for (int index = 0; index < block.Instructions.Count; ++index)
            {
                if (!removedCalls.Contains(index))
                {
                    instructions.Add(block.Instructions[index]);
                    continue;
                }

                if (groupsByTail.TryGetValue(index, out FusionGroup? group))
                {
                    EmitGroup(
                        plan.Function,
                        block,
                        group,
                        types[bufferTypeIds[group.Calls.Count]],
                        epochImportId,
                        submitImportId,
                        locals,
                        valueIds,
                        instructions);
                }
            }

            return block with { Instructions = instructions };
        }).ToArray();

        return plan.Function with { Locals = locals, Blocks = blocks };
    }

    private static void EmitGroup(
        GuestFunction function,
        GuestBasicBlock block,
        FusionGroup group,
        GuestType bufferType,
        string epochImportId,
        string submitImportId,
        List<GuestRegister> locals,
        UniqueIdAllocator valueIds,
        List<GuestInstruction> instructions)
    {
        string prefix =
            $"value:__avidscript_data_lane:{function.Id}:{block.Id}:{group.Ordinal}";
        string Define(string typeId, string role)
        {
            string id = valueIds.Allocate($"{prefix}:{role}");
            locals.Add(new GuestRegister(id, typeId));
            return id;
        }

        string Constant(string typeId, string kind, uint value, string role)
        {
            string id = Define(typeId, role);
            instructions.Add(new GuestInstruction(
                "constant",
                id,
                Array.Empty<string>(),
                null,
                null,
                new GuestConstant(kind, value.ToString(CultureInfo.InvariantCulture))));
            return id;
        }

        int byteCount = checked(
            CSharpDataLaneAbi.HeaderBytes + (CSharpDataLaneAbi.CommandBytes * group.Calls.Count));
        string epochId = Define("type:uint64", "epoch");
        instructions.Add(new GuestInstruction(
            "call",
            epochId,
            Array.Empty<string>(),
            epochImportId,
            null,
            null));
        string bufferId = Define(bufferType.Id, "buffer");
        instructions.Add(new GuestInstruction(
            "stack_alloc",
            bufferId,
            Array.Empty<string>(),
            null,
            null,
            null));
        void Store(string fieldName, string valueId)
        {
            instructions.Add(new GuestInstruction(
                "field_store",
                null,
                new[] { bufferId, valueId },
                Field(bufferType, fieldName).Id,
                null,
                null));
        }

        string magicId = Constant("type:uint32", "uint32", CommandMagic, "magic");
        string schemaId = Constant("type:uint16", "uint16", 1, "schema");
        string countId = Constant(
            "type:uint16",
            "uint16",
            checked((uint)group.Calls.Count),
            "count");
        string byteCountU32Id = Constant(
            "type:uint32",
            "uint32",
            checked((uint)byteCount),
            "byte_count_u32");
        string zeroU32Id = Constant("type:uint32", "uint32", 0, "zero_u32");
        Store("Header.Magic", magicId);
        Store("Header.Schema", schemaId);
        Store("Header.Count", countId);
        Store("Header.ByteCount", byteCountU32Id);
        Store("Header.Reserved", zeroU32Id);
        Store("Header.CallbackEpoch", epochId);

        CandidateCall firstCall = group.Calls[0];
        string receiverId = firstCall.Instruction.OperandIds[0];
        string slotId = Define("type:int32", "self_slot");
        instructions.Add(new GuestInstruction(
            "field_load",
            slotId,
            new[] { receiverId },
            firstCall.Target.SlotFieldId,
            null,
            null));
        string generationId = Define("type:int32", "self_generation");
        instructions.Add(new GuestInstruction(
            "field_load",
            generationId,
            new[] { receiverId },
            firstCall.Target.GenerationFieldId,
            null,
            null));
        string opcodeId = Constant("type:uint16", "uint16", 1, "opcode");
        string zeroU16Id = Constant("type:uint16", "uint16", 0, "zero_u16");
        string recordBytesId = Constant(
            "type:uint32",
            "uint32",
            CSharpDataLaneAbi.CommandBytes,
            "record_bytes");
        string zeroI32Id = Constant("type:int32", "int32", 0, "zero_i32");

        for (int index = 0; index < group.Calls.Count; ++index)
        {
            CandidateCall call = group.Calls[index];
            string bindingOrdinalId = Constant(
                "type:uint32",
                "uint32",
                checked((uint)call.Target.BindingOrdinal),
                $"command_{index}_binding");
            Store($"Command[{index}].Opcode", opcodeId);
            Store($"Command[{index}].Flags", zeroU16Id);
            Store($"Command[{index}].RecordBytes", recordBytesId);
            Store($"Command[{index}].BindingOrdinal", bindingOrdinalId);
            Store($"Command[{index}].SelfSlot", slotId);
            Store($"Command[{index}].SelfGeneration", generationId);
            Store($"Command[{index}].Value", call.Instruction.OperandIds[1]);
            Store($"Command[{index}].Arg1", zeroI32Id);
            Store($"Command[{index}].Reserved", zeroU32Id);
        }

        string addressId = Define("type:address", "address");
        instructions.Add(new GuestInstruction(
            "address_of",
            addressId,
            Array.Empty<string>(),
            bufferId,
            null,
            null));
        string submitByteCountId = Constant(
            "type:int32",
            "int32",
            checked((uint)byteCount),
            "byte_count_i32");
        string statusId = Define("type:int32", "submit_status");
        instructions.Add(new GuestInstruction(
            "call",
            statusId,
            new[] { addressId, submitByteCountId },
            submitImportId,
            null,
            null));
    }

    private static GuestType CreateCommandBufferType(
        string typeId,
        int commandCount,
        UniqueIdAllocator fieldIds)
    {
        List<GuestField> fields = new();
        void Add(string name, string fieldTypeId)
        {
            fields.Add(new GuestField(
                fieldIds.Allocate($"field:{typeId}:{fields.Count}"),
                name,
                fieldTypeId,
                0));
        }

        Add("Header.Magic", "type:uint32");
        Add("Header.Schema", "type:uint16");
        Add("Header.Count", "type:uint16");
        Add("Header.ByteCount", "type:uint32");
        Add("Header.Reserved", "type:uint32");
        Add("Header.CallbackEpoch", "type:uint64");
        for (int index = 0; index < commandCount; ++index)
        {
            Add($"Command[{index}].Opcode", "type:uint16");
            Add($"Command[{index}].Flags", "type:uint16");
            Add($"Command[{index}].RecordBytes", "type:uint32");
            Add($"Command[{index}].BindingOrdinal", "type:uint32");
            Add($"Command[{index}].SelfSlot", "type:int32");
            Add($"Command[{index}].SelfGeneration", "type:int32");
            Add($"Command[{index}].Value", "type:int32");
            Add($"Command[{index}].Arg1", "type:int32");
            Add($"Command[{index}].Reserved", "type:uint32");
        }

        return new GuestType(
            typeId,
            "struct",
            "memory",
            fields,
            null,
            null,
            0,
            1);
    }

    private static GuestField Field(GuestType type, string name)
    {
        return type.Fields.Single(field => field.Name == name);
    }

    private static void EnsureScalar(
        List<GuestType> types,
        string id,
        string storage,
        int size,
        int alignment)
    {
        if (types.Any(type => type.Id == id))
        {
            return;
        }

        types.Add(new GuestType(
            id,
            "scalar",
            storage,
            Array.Empty<GuestField>(),
            null,
            null,
            size,
            alignment));
    }

    private static CSharpDataLaneFusionResult Success(
        IReadOnlyList<GuestType> types,
        IReadOnlyList<GuestImport> imports,
        IReadOnlyList<GuestFunction> functions)
    {
        return new CSharpDataLaneFusionResult(
            true,
            types,
            imports,
            functions,
            Array.Empty<GuestDiagnostic>());
    }

    private sealed record SetterTarget(
        int BindingOrdinal,
        string ReceiverTypeId,
        string SlotFieldId,
        string GenerationFieldId);

    private sealed record CandidateCall(
        int InstructionIndex,
        GuestInstruction Instruction,
        SetterTarget Target,
        string ReceiverId);

    private sealed record FusionGroup(
        int Ordinal,
        IReadOnlyList<CandidateCall> Calls);

    private sealed record BlockPlan(
        string BlockId,
        IReadOnlyList<FusionGroup> Groups);

    private sealed record FunctionPlan(
        GuestFunction Function,
        IReadOnlyList<BlockPlan> Blocks);

    private sealed class UniqueIdAllocator
    {
        private readonly HashSet<string> used;

        public UniqueIdAllocator(IEnumerable<string> ids)
        {
            used = ids.ToHashSet(StringComparer.Ordinal);
        }

        public string Allocate(string baseId)
        {
            if (used.Add(baseId))
            {
                return baseId;
            }

            for (int suffix = 1; ; ++suffix)
            {
                string candidate = $"{baseId}.{suffix}";
                if (used.Add(candidate))
                {
                    return candidate;
                }
            }
        }
    }
}
