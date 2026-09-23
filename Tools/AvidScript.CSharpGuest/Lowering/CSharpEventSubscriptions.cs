using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal static class CSharpEventSubscriptions
{
    public const string SubscribeImport = "import:$event:subscribe", ReadImport = "import:$event:read";
    public const string HandlerField = "$handler";
    public static string Box(SemanticEventSubscription entry) => "$event:" + entry.SubscriptionId;
    public static IReadOnlyList<SemanticEventSubscription> Active(SemanticDocument document) => document.EventSubscriptions
        .Where(entry => document.Reachability is null || document.Reachability.ReachableCallableIds.Contains(entry.MethodSymbolId)).ToArray();

    public static void AddTypes(SemanticDocument document, List<GuestType> types)
    {
        foreach (var entry in Active(document))
        {
            string payload = CSharpClosureLayout.Payload(Box(entry));
            types.Add(new(payload, "struct", "memory", new[] { new GuestField(HandlerField, HandlerField, entry.DelegateTypeId, 0) }, null, null, 0, 1));
            types.Add(new(CSharpClosureLayout.Reference(Box(entry)), "managed_ref", "i64", Array.Empty<GuestField>(), payload, null, 8, 8));
        }
    }

    public static IEnumerable<GuestImport> Imports(SemanticDocument document) => Active(document).Count == 0 ? Array.Empty<GuestImport>() : new[]
    {
        new GuestImport(SubscribeImport, GuestEventState.ImportModule, GuestEventState.SubscribeImport,
            new[] { "type:int32", "type:int32", "type:int32", "type:int32", "type:int64" }, "type:int64"),
        new GuestImport(ReadImport, GuestEventState.ImportModule, GuestEventState.ReadImport, new[] { "type:int32" }, "type:int64"),
    };

    public static IEnumerable<GuestFunction> Build(SemanticDocument document)
    {
        foreach (var entry in Active(document))
        {
            string functionType = CSharpClosureLayout.FunctionType(entry.DelegateTypeId);
            var locals = new[] { new GuestRegister("target", functionType), new("nil", functionType), new("empty", "type:bool"),
                new("zero", "type:int64"), new("ordinal", "type:int32"),
                new("box", CSharpClosureLayout.Reference(Box(entry))), new("token", "type:int64") };
            yield return new(CSharpGuestIds.Function(entry.MethodSymbolId),
                new[] { new GuestRegister("slot", "type:int32"), new("generation", "type:int32"), new("handler", entry.DelegateTypeId) },
                locals, "type:int64", "entry", new[]
                {
                    new GuestBasicBlock("entry", new GuestInstruction[] {
                        new("field_load", "target", new[] { "handler" }, CSharpClosureLayout.TargetField, null, null),
                        new("constant", "nil", Array.Empty<string>(), null, null, new("null", null)),
                        new("binary", "empty", new[] { "target", "nil" }, null, "equals", null) }, new("branch_if", "empty", "empty", "subscribe", null)),
                    new GuestBasicBlock("empty", new[] { new GuestInstruction("constant", "zero", Array.Empty<string>(), null, null, new("int64", "0")) },
                        new("return", null, null, null, "zero")),
                    new GuestBasicBlock("subscribe", new GuestInstruction[] {
                        new("constant", "ordinal", Array.Empty<string>(), null, null, new("int32", entry.EventOrdinal.ToString(CultureInfo.InvariantCulture))),
                        new("managed_new", "box", Array.Empty<string>(), null, null, null),
                        new("managed_set", null, new[] { "box", "handler" }, HandlerField, null, null),
                        new(GuestEventState.SubscribeOp, "token", new[] { "slot", "generation", "ordinal", "box" }, SubscribeImport, null, null) },
                        new("return", null, null, null, "token")),
                });
        }
    }

    public static void Invoke(SemanticEventSubscription entry, IReadOnlyList<string> arguments, string? result,
        List<GuestRegister> locals, List<GuestInstruction> instructions)
    {
        const string box = "$event:box", handler = "$event:handler", target = "$event:target", context = "$event:context";
        string functionType = CSharpClosureLayout.FunctionType(entry.DelegateTypeId);
        locals.Add(new(box, CSharpClosureLayout.Reference(Box(entry))));
        locals.Add(new(handler, entry.DelegateTypeId));
        locals.Add(new(target, functionType));
        locals.Add(new(context, CSharpClosureLayout.ObjectType));
        instructions.Add(new(GuestEventState.ReadOp, box, Array.Empty<string>(), ReadImport, null, null));
        instructions.Add(new("managed_get", handler, new[] { box }, HandlerField, null, null));
        instructions.Add(new("field_load", target, new[] { handler }, CSharpClosureLayout.TargetField, null, null));
        instructions.Add(new("field_load", context, new[] { handler }, CSharpClosureLayout.ContextField, null, null));
        instructions.Add(new("call_indirect", result, new[] { target, context }.Concat(arguments).ToArray(), functionType, null, null));
    }
}
