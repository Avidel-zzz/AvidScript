using System;
using System.Collections.Generic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

// Domain-local builder for the compiler-owned immutable delegate list helpers.
internal sealed class CSharpDelegateListBuilder
{
    public const string Int = CSharpGuestIds.Int32TypeId, Bool = "type:bool", Void = CSharpGuestIds.VoidTypeId;
    private readonly string signature, operation, returnType;
    private readonly IReadOnlyList<GuestRegister> parameters;
    private readonly List<GuestRegister> locals = new();
    private readonly List<GuestBasicBlock> blocks = new();
    private List<GuestInstruction> instructions = new();
    private string block = "entry";
    public CSharpDelegateListBuilder(string signature, string operation, string returnType, params GuestRegister[] parameters)
    { this.signature = signature; this.operation = operation; this.returnType = returnType; this.parameters = parameters; }
    public string Local(string type) { string id = "v" + locals.Count; locals.Add(new(id, type)); return id; }
    public void Emit(string op, string? result, string[] sources, string? target = null, string? kind = null, GuestConstant? constant = null)
        => instructions.Add(new(op, result, sources, target, kind, constant));
    public string Value(string type, string op, string[] sources, string? target = null, string? kind = null, GuestConstant? constant = null)
    { string result = Local(type); Emit(op, result, sources, target, kind, constant); return result; }
    public string Integer(int value) => Value(Int, "constant", Array.Empty<string>(), constant: new("int32", value.ToString(System.Globalization.CultureInfo.InvariantCulture)));
    public string Zero(string type, string kind = "zero") => Value(type, "constant", Array.Empty<string>(), constant: new(kind, null));
    public string Binary(string op, string left, string right, string type = Bool) => Value(type, "binary", new[] { left, right }, kind: op);
    public string Call(string operation, string type, params string[] arguments) => Value(type, "call", arguments, CSharpDelegateComposition.Function(signature, operation));
    public string Field(string owner, string field, string type, bool managed = false) => Value(type, managed ? "managed_get" : "field_load", new[] { owner }, field);
    public void Set(string owner, string field, string value, bool managed = false) => Emit(managed ? "managed_set" : "field_store", null, new[] { owner, value }, field);
    public void Copy(string target, string source) => Emit("local_store", null, new[] { source }, target);
    public string Target(string owner) => Field(owner, CSharpClosureLayout.TargetField, CSharpClosureLayout.FunctionType(signature));
    public string Context(string owner) => Field(owner, CSharpClosureLayout.ContextField, CSharpClosureLayout.ObjectType);
    public string Invoker() => Value(CSharpClosureLayout.FunctionType(signature), "function_ref", Array.Empty<string>(), CSharpDelegateComposition.Function(signature, "invoke"));
    public string Node(string context) => Value(CSharpClosureLayout.Reference(CSharpDelegateComposition.Node(signature)), "managed_cast", new[] { context });
    public void End(GuestTerminator terminator) { blocks.Add(new(block, instructions.ToArray(), terminator)); instructions = new(); }
    public void At(string name) { if (instructions.Count != 0) throw new InvalidOperationException("Unterminated delegate helper block."); block = name; }
    public void Jump(string target) => End(new("branch", null, target, null, null));
    public void Branch(string condition, string yes, string no) => End(new("branch_if", condition, yes, no, null));
    public void Return(string? value = null) => End(new("return", null, null, null, value));
    public void Trap() => End(new("trap", null, null, null, null));
    public GuestFunction Finish() => new(CSharpDelegateComposition.Function(signature, operation), parameters, locals, returnType, "entry", blocks);
}
