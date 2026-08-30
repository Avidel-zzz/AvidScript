using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;

namespace AvidScript.CSharpGuest;

internal sealed record CSharpUePropertyAccessPlan(
    SemanticUePropertyRuntimePlan RuntimePlan,
    SemanticCallable? Getter,
    SemanticCallable? Setter)
{
    public static IReadOnlyDictionary<string, CSharpUePropertyAccessPlan> Build(
        SemanticDocument document)
    {
        return SemanticUeTypeRuntimeContract.BuildPropertyPlans(document)
            .Select(plan => new CSharpUePropertyAccessPlan(
                plan,
                plan.GetterImportName.Length == 0 ? null : MakeGetter(plan),
                plan.SetterImportName.Length == 0 ? null : MakeSetter(plan)))
            .ToDictionary(plan => plan.RuntimePlan.PropertySymbolId, StringComparer.Ordinal);
    }

    private static SemanticCallable MakeGetter(SemanticUePropertyRuntimePlan plan)
    {
        string methodId = MethodId(plan, "get");
        return new SemanticCallable(
            methodId,
            plan.OwnerTypeId,
            plan.ValueTypeId,
            Array.Empty<SemanticCallableParameter>(),
            IsStatic: false,
            IsConstructor: false,
            HasBody: false,
            plan.PropertySymbolId,
            new SemanticCallableImport(
                SemanticUeTypeRuntimeContract.HostModule,
                plan.GetterImportName),
            Export: null);
    }

    private static SemanticCallable MakeSetter(SemanticUePropertyRuntimePlan plan)
    {
        string methodId = MethodId(plan, "set");
        return new SemanticCallable(
            methodId,
            plan.OwnerTypeId,
            "type:void",
            new[]
            {
                new SemanticCallableParameter(
                    0,
                    $"parameter:synthetic:{methodId}:value",
                    "value",
                    plan.ValueTypeId,
                    "none"),
            },
            IsStatic: false,
            IsConstructor: false,
            HasBody: false,
            plan.PropertySymbolId,
            new SemanticCallableImport(
                SemanticUeTypeRuntimeContract.HostModule,
                plan.SetterImportName),
            Export: null);
    }

    private static string MethodId(SemanticUePropertyRuntimePlan plan, string access)
    {
        return $"method:synthetic:ue_property:{plan.TypeOrdinal}:{plan.MemberOrdinal}:{access}";
    }
}
