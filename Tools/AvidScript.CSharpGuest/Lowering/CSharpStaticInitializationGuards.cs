using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

public sealed record CSharpStaticInitializer(string TypeId, string BodyFunctionId, int SourceToken);

// Composes already lowered initializer bodies. Source triggers and closed-type
// specialization belong to the caller; this does not admit source plans by itself.
public static class CSharpStaticInitializationGuards
{
    public const string ExceptionType = "type:global::System.TypeInitializationException";
    public const string StateType = "type:$static:state_ref";
    private const string StatePayload = "type:$static:state";
    private const string Root = "type:language_error_root";
    private const string Payload = "type:language_error_payload";
    private const string I = "type:int32";
    private const string B = "type:bool";
    public static string FunctionId(string typeId) => "function:$static:ensure:" + typeId;
    public static string SlotId(string typeId) => "static:$initialization:" + typeId;

    public static bool TryCompose(GuestModule module, IReadOnlyList<CSharpStaticInitializer> initializers,
        out GuestModule? result, out string? error)
    {
        result = null;
        error = null;
        if (module is null || initializers is not { Count: > 0 and <= GuestManagedHeap.MaxStaticSlots }
            || initializers.Any(item => item is null || string.IsNullOrWhiteSpace(item.TypeId)
                || !item.TypeId.StartsWith("type:", StringComparison.Ordinal) || item.TypeId.Length > 1024
                || item.TypeId.Any(char.IsControl))
            || initializers.Select(item => item.TypeId).Distinct(StringComparer.Ordinal).Count() != initializers.Count)
            return Fail("Initializer types require unique bounded identities.", out error);
        if (GuestStaticStorage.IsVersion(module) != (module.StaticStorage is not null)
            || module.StaticStorage is { Slots: null })
            return Fail("Static storage must retain its original versioned envelope.", out error);
        GuestModule profile = GuestStaticStorage.ExecutionProfile(module);
        if (profile.SchemaVersion != 17 || profile.IrVersion != "1.16"
            || module.LanguageOutcomeTypes is not { Count: > 0 } outcomes
            || outcomes.Count(item => item.ValueTypeId is null) != 1
            || module.LanguageErrorCatalog is not { } catalog
            || catalog.Types.Count(item => item.TypeId == ExceptionType) != 1)
            return Fail("Static guards require the synchronous error profile and a preassigned initialization exception token.", out error);
        string outcome = outcomes.Single(item => item.ValueTypeId is null).TypeId;
        int exceptionToken = catalog.Types.Single(item => item.TypeId == ExceptionType).Token;
        foreach (var item in initializers)
            if (module.Functions.Count(function => function.Id == item.BodyFunctionId
                    && function.ReturnTypeId == outcome && function.Parameters.Count == 0) != 1
                || module.Functions.Any(function => function.Id == FunctionId(item.TypeId))
                || catalog.Sources.Count(source => source.Token == item.SourceToken) != 1)
                return Fail("An initializer needs one parameterless void-outcome body, an unused guard identity and a source token.", out error);
        var payloads = module.Types.Where(type => type.Id == Payload).ToArray();
        if (payloads.Length != 1 || payloads[0].Kind != "struct"
            || payloads[0].Fields.Count != 1 || payloads[0].Fields[0] is not { Id: "field:code", TypeId: I }
            || module.Types.Count(type => type.Id == Root && type.Kind == "managed_ref" && type.ElementTypeId == Payload) != 1
            || module.Types.Any(type => type.Id is StateType or StatePayload))
            return Fail("Static guards require an unmodified language error payload and unused state types.", out error);
        var slots = (module.StaticStorage?.Slots ?? Array.Empty<GuestStaticSlot>())
            .Concat(initializers.Select(item => new GuestStaticSlot(SlotId(item.TypeId), StateType))).ToArray();
        if (slots.Length > GuestManagedHeap.MaxStaticSlots || slots.Any(slot => slot is null)
            || slots.Select(slot => slot.Id).Distinct().Count() != slots.Length)
            return Fail("Initializer state slots exceed the budget or collide with existing static storage.", out error);
        var inputTypes = module.Types.Any(type => type.Id == B) ? module.Types
            : module.Types.Append(new GuestType(B, "scalar", "i32", Array.Empty<GuestField>(), null, null, 1, 1)).ToArray();
        var types = inputTypes.Select(type => type.Id != Payload ? type : type with
        {
            Fields = type.Fields.Concat(new[] {
                new GuestField("field:inner_root", "inner_root", Root, 0),
                new GuestField("field:inner_type", "inner_type", I, 0),
                new GuestField("field:inner_source", "inner_source", I, 0),
            }).ToArray(),
        }).Concat(new[] {
            new GuestType(StatePayload, "struct", "memory", new[] {
                new GuestField("field:state", "state", I, 0),
                new GuestField("field:failure", "failure", Root, 0),
            }, null, null, 0, 1),
            new GuestType(StateType, "managed_ref", "i64", Array.Empty<GuestField>(), StatePayload, null, 8, 8),
        }).ToArray();
        var computed = GuestDataLayout.ComputeTypes(types);
        if (!computed.Succeeded) return Fail("Initializer state layout could not be computed.", out error);
        var layout = GuestLayoutBuilder.Build(computed.Types, module.Globals, module.DataSegments, module.MemoryLayout.StateStart);
        if (!layout.Succeeded || layout.Layout is null) return Fail("Initializer module layout could not be computed.", out error);
        GuestModule candidate = module with
        {
            SchemaVersion = GuestStaticStorage.SchemaVersion, IrVersion = GuestStaticStorage.IrVersion,
            StaticStorage = new(profile.SchemaVersion, profile.IrVersion, slots),
            Types = computed.Types, MemoryLayout = layout.Layout, DataSegments = layout.DataSegments,
            Functions = module.Functions.Concat(initializers.Select(item => Build(item, outcome, exceptionToken))).ToArray(),
        };
        var validation = GuestModuleValidator.Validate(candidate);
        if (!validation.Succeeded) return Fail("Static initialization composition failed: "
            + string.Join(" | ", validation.Diagnostics.Select(item => item.Message)), out error);
        result = candidate;
        return true;
    }

    private static GuestFunction Build(CSharpStaticInitializer initializer, string outcome, int exceptionToken)
    {
        var locals = new[] {
            R("existing", StateType), R("empty", StateType), R("created", StateType),
            R("is_new", B), R("state", I), R("ready", B), R("reentrant", B), R("faulted", B),
            R("one", I), R("two", I), R("three", I), R("zero", I), R("exception", I), R("source", I),
            R("initialized", outcome), R("status", I), R("inner_root", Root), R("inner_type", I), R("inner_source", I),
            R("wrapper", Root), R("cached", Root), R("success", outcome), R("failure", outcome), R("repeat", outcome),
        };
        return new(FunctionId(initializer.TypeId), Array.Empty<GuestRegister>(), locals, outcome, "entry", new[] {
            Block("entry", new[] {
                Constant("zero", 0), Constant("one", 1), Constant("two", 2), Constant("three", 3),
                Constant("exception", exceptionToken), Constant("source", initializer.SourceToken),
                Op(GuestStaticStorage.GetOp, "existing", target: SlotId(initializer.TypeId)),
                new GuestInstruction("constant", "empty", Array.Empty<string>(), null, null, new("null", null)),
                Equal("is_new", "existing", "empty"),
            }, Branch("is_new", "start", "inspect")),
            Block("inspect", new[] { Get("state", "existing", "state"), Equal("ready", "state", "two") }, Branch("ready", "ok", "reentry")),
            Block("reentry", new[] { Equal("reentrant", "state", "one") }, Branch("reentrant", "ok", "fault")),
            Block("fault", new[] { Equal("faulted", "state", "three") }, Branch("faulted", "cached", "invalid")),
            Block("invalid", Array.Empty<GuestInstruction>(), new("trap", null, null, null, null)),
            Block("start", new[] {
                Op("managed_new", "created"), Set("created", "state", "one"),
                Op(GuestStaticStorage.SetOp, operands: new[] { "created" }, target: SlotId(initializer.TypeId)),
                Op("call", "initialized", target: initializer.BodyFunctionId),
                Load("status", "initialized", "status"),
            }, Branch("status", "failed", "initialized")),
            Block("initialized", new[] { Set("created", "state", "two") }, new("branch", null, "ok", null, null)),
            Block("ok", new[] { Op("stack_alloc", "success"), Store("success", "status", "zero") }, Return("success")),
            Block("failed", new[] {
                Load("inner_root", "initialized", "error_root"), Load("inner_type", "initialized", "error_type"), Load("inner_source", "initialized", "source"),
                Op("managed_new", "wrapper"), Set("wrapper", "code", "exception"),
                Set("wrapper", "inner_root", "inner_root"), Set("wrapper", "inner_type", "inner_type"), Set("wrapper", "inner_source", "inner_source"),
                Set("created", "failure", "wrapper"), Set("created", "state", "three"),
            }.Concat(Failure("failure", "wrapper")).ToArray(), Return("failure")),
            Block("cached", new[] { Get("cached", "existing", "failure") }.Concat(Failure("repeat", "cached")).ToArray(), Return("repeat")),
        });
    }

    private static GuestInstruction[] Failure(string result, string root) => new[] {
        Op("stack_alloc", result), Store(result, "status", "one"), Store(result, "error_type", "exception"),
        Store(result, "source", "source"), Store(result, "error_root", root),
    };
    private static GuestRegister R(string id, string type) => new(id, type);
    private static GuestBasicBlock Block(string id, GuestInstruction[] instructions, GuestTerminator end) => new(id, instructions, end);
    private static GuestTerminator Branch(string value, string yes, string no) => new("branch_if", value, yes, no, null);
    private static GuestTerminator Return(string value) => new("return", null, null, null, value);
    private static GuestInstruction Op(string op, string? result = null, string[]? operands = null, string? target = null) =>
        new(op, result, operands ?? Array.Empty<string>(), target, null, null);
    private static GuestInstruction Constant(string id, int value) => new("constant", id, Array.Empty<string>(), null, null, new("int32", value.ToString(CultureInfo.InvariantCulture)));
    private static GuestInstruction Equal(string id, string a, string b) => new("binary", id, new[] { a, b }, null, "equals", null);
    private static GuestInstruction Get(string id, string owner, string field) => Op("managed_get", id, new[] { owner }, "field:" + field);
    private static GuestInstruction Set(string owner, string field, string value) => Op("managed_set", operands: new[] { owner, value }, target: "field:" + field);
    private static GuestInstruction Load(string id, string owner, string field) => Op("field_load", id, new[] { owner }, "field:" + field);
    private static GuestInstruction Store(string owner, string field, string value) => Op("field_store", operands: new[] { owner, value }, target: "field:" + field);
    private static bool Fail(string message, out string? error) { error = message; return false; }
}
