using System;
using System.Collections.Generic;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

// Language adapters normalize only the hidden receiver. The body retains its
// nominal receiver and authority guard; a frame signature is not an object permit.
internal static class CSharpUeMethodFrames
{
    public static IReadOnlyList<GuestFramedExport> Lower(SemanticDocument document,
        List<GuestFunction> functions, List<GuestDiagnostic> diagnostics)
    {
        if (document.UeMethodCatalog is null) return Array.Empty<GuestFramedExport>();
        var bodies = functions.ToDictionary(function => function.Id, StringComparer.Ordinal);
        var called = functions.SelectMany(function => function.Blocks).SelectMany(block => block.Instructions)
            .Where(instruction => instruction.Op == "call").Select(instruction => instruction.TargetId)
            .ToHashSet(StringComparer.Ordinal);
        Dictionary<string, GuestFramedExport> exports = new(StringComparer.Ordinal);
        List<GuestFunction> adapters = new();
        foreach (SemanticUeMethodEntry method in document.UeMethodCatalog.Methods.OrderBy(method => method.MethodSymbolId, StringComparer.Ordinal))
        {
            string bodyId = CSharpGuestIds.Function(method.MethodSymbolId);
            // Export-only lifecycle entries keep their existing host ABI and debug
            // resumability. Async methods require a persistent invocation contract.
            if (!method.HasGuestBody || !CSharpUeReceivers.IsType(document, method.ContainingTypeId)
                || !called.Contains(bodyId) || !bodies.TryGetValue(bodyId, out GuestFunction? body)) continue;
            if (method.ReturnRefKind != "none" || body.Parameters.Count != method.Parameters.Count + 1
                || body.Parameters[0].TypeId != method.ContainingTypeId || body.ReturnTypeId != method.ReturnTypeId
                || !body.Parameters.Skip(1).Select(parameter => parameter.TypeId).SequenceEqual(method.Parameters.Select(parameter =>
                    CSharpBorrowedReferences.Parameter(document, parameter.TypeId, parameter.RefKind))))
            {
                diagnostics.Add(new("ASCG1024", "error", $"UE method '{method.MethodSymbolId}' cannot preserve its catalog signature in a synchronous frame.", null));
                continue;
            }
            string identity = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(method.MethodSymbolId))).ToLowerInvariant();
            string adapterId = "$ue:method:frame:v1:" + identity;
            GuestRegister[] parameters = body.Parameters.Select((parameter, index) =>
                new GuestRegister("$argument:" + index, index == 0 ? "type:uint64" : parameter.TypeId)).ToArray();
            GuestRegister receiver = new("$receiver", method.ContainingTypeId);
            GuestRegister? result = body.ReturnTypeId == CSharpGuestIds.VoidTypeId ? null : new("$result", body.ReturnTypeId);
            var adapter = new GuestFunction(adapterId, parameters,
                result is null ? new[] { receiver } : new[] { receiver, result }, body.ReturnTypeId, "$entry",
                new[] { new GuestBasicBlock("$entry", new GuestInstruction[] {
                    new("convert", receiver.Id, new[] { parameters[0].Id }, null, null, null),
                    new("call", result?.Id, new[] { receiver.Id }.Concat(parameters.Skip(1).Select(parameter => parameter.Id)).ToArray(), body.Id, null, null),
                }, new GuestTerminator("return", null, null, null, result?.Id)) });
            adapters.Add(adapter);
            exports.Add(bodyId, new("avid_ue_method_" + identity + "_frame_v1", adapterId,
                new[] { "value" }.Concat(method.Parameters.Select(parameter => parameter.RefKind switch {
                    "none" => "value", "ref_readonly" => "in", _ => parameter.RefKind,
                })).ToArray()));
        }
        if (diagnostics.Count != 0) return Array.Empty<GuestFramedExport>();
        // Run after closure thunks and property accessors exist, so all ordinary
        // direct calls to these bodies use the same ABI, including method groups.
        for (int index = 0; index < functions.Count; ++index)
        {
            GuestFunction function = functions[index];
            List<GuestRegister> locals = function.Locals.ToList();
            HashSet<string> used = function.Parameters.Concat(locals).Select(register => register.Id).ToHashSet(StringComparer.Ordinal);
            functions[index] = function with { Blocks = function.Blocks.Select(block => block with {
                Instructions = block.Instructions.SelectMany(instruction => {
                    if (instruction.Op != "call" || instruction.TargetId is null
                        || !exports.TryGetValue(instruction.TargetId, out GuestFramedExport? export)) return new[] { instruction };
                    int ordinal = locals.Count;
                    string id;
                    do { id = "$ue:frame:receiver:" + ordinal++; } while (!used.Add(id));
                    locals.Add(new(id, "type:uint64"));
                    return new[] {
                        new GuestInstruction("convert", id, new[] { instruction.OperandIds[0] }, null, null, null),
                        instruction with { Op = "call_framed", TargetId = export.Name,
                            OperandIds = new[] { id }.Concat(instruction.OperandIds.Skip(1)).ToArray() },
                    };
                }).ToArray(),
            }).ToArray(), Locals = locals };
        }
        functions.AddRange(adapters);
        return exports.Values.ToArray();
    }
}
