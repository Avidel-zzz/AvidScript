#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptEditorBindingDescriptorGenerator.h"
#include "AvidScriptEditorBindingSelectionResolver.h"

#include "Misc/AutomationTest.h"

namespace
{
FString MakeBindingSelectionTestKey(const FAvidScriptReflectedFunctionSelection& Selection)
{
	return Selection.OwnerClassPath + TEXT(".") + Selection.FunctionName.ToString();
}

FString MakeBindingSelectionIssueTestKey(const FAvidScriptBindingSelectionIssue& Issue)
{
	return Issue.OwnerClassPath
		+ TEXT(".")
		+ Issue.FunctionName.ToString()
		+ TEXT(":")
		+ Issue.Category
		+ TEXT(":")
		+ Issue.Source;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBindingSelectionGameplayProfileTest,
	"AvidScript.Editor.BindingSelection.GameplayProfile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingSelectionGameplayProfileTest::RunTest(const FString& Parameters)
{
	const FAvidScriptBindingSelectionProfile Profile =
		FAvidScriptEditorBindingDescriptorGenerator::MakeEngineGameplayProfile();
	TArray<FAvidScriptReflectedFunctionSelection> FirstSelections;
	FAvidScriptBindingSelectionResolveResult FirstResult;
	TestTrue(
		TEXT("Engine gameplay profile resolves"),
		FAvidScriptEditorBindingSelectionResolver::Resolve(Profile, FirstSelections, FirstResult));
	AddInfo(FString::Printf(
		TEXT("P43.1 gameplay profile: candidates=%d accepted=%d rejected=%d"),
		FirstResult.CandidateFunctionCount,
		FirstResult.AcceptedFunctionCount,
		FirstResult.RejectedFunctionCount));
	TestTrue(TEXT("Gameplay profile expands beyond the eight-function baseline"), FirstSelections.Num() > 8);
	TestEqual(TEXT("Accepted count matches selection array"), FirstResult.AcceptedFunctionCount, FirstSelections.Num());
	TestTrue(TEXT("Gameplay profile records all reflected candidates"), FirstResult.CandidateFunctionCount >= FirstSelections.Num());
	TestTrue(
		TEXT("Actor location remains available"),
		FirstSelections.ContainsByPredicate([](const FAvidScriptReflectedFunctionSelection& Selection)
		{
			return Selection.OwnerClassPath == TEXT("/Script/Engine.Actor")
				&& Selection.FunctionName == TEXT("K2_GetActorLocation");
		}));
	TestTrue(
		TEXT("Actor scale write remains available"),
		FirstSelections.ContainsByPredicate([](const FAvidScriptReflectedFunctionSelection& Selection)
		{
			return Selection.OwnerClassPath == TEXT("/Script/Engine.Actor")
				&& Selection.FunctionName == TEXT("SetActorScale3D");
		}));

	TArray<FAvidScriptReflectedFunctionSelection> SecondSelections;
	FAvidScriptBindingSelectionResolveResult SecondResult;
	TestTrue(
		TEXT("Repeated gameplay profile resolution succeeds"),
		FAvidScriptEditorBindingSelectionResolver::Resolve(Profile, SecondSelections, SecondResult));
	TestEqual(TEXT("Repeated selection count is stable"), SecondSelections.Num(), FirstSelections.Num());
	TestEqual(TEXT("Repeated issue count is stable"), SecondResult.Issues.Num(), FirstResult.Issues.Num());
	TArray<FString> FirstSelectionKeys;
	TArray<FString> SecondSelectionKeys;
	for (const FAvidScriptReflectedFunctionSelection& Selection : FirstSelections)
	{
		FirstSelectionKeys.Add(MakeBindingSelectionTestKey(Selection));
	}
	for (const FAvidScriptReflectedFunctionSelection& Selection : SecondSelections)
	{
		SecondSelectionKeys.Add(MakeBindingSelectionTestKey(Selection));
	}
	TestEqual(
		TEXT("Resolved selection order is deterministic"),
		FString::Join(SecondSelectionKeys, TEXT("\n")),
		FString::Join(FirstSelectionKeys, TEXT("\n")));

	TArray<FString> FirstIssueKeys;
	TArray<FString> SecondIssueKeys;
	for (const FAvidScriptBindingSelectionIssue& Issue : FirstResult.Issues)
	{
		FirstIssueKeys.Add(MakeBindingSelectionIssueTestKey(Issue));
	}
	for (const FAvidScriptBindingSelectionIssue& Issue : SecondResult.Issues)
	{
		SecondIssueKeys.Add(MakeBindingSelectionIssueTestKey(Issue));
	}
	TestEqual(
		TEXT("Compatibility issue order is deterministic"),
		FString::Join(SecondIssueKeys, TEXT("\n")),
		FString::Join(FirstIssueKeys, TEXT("\n")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBindingSelectionFilteredCompatibilityTest,
	"AvidScript.Editor.BindingSelection.FilteredCompatibility",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingSelectionFilteredCompatibilityTest::RunTest(const FString& Parameters)
{
	FAvidScriptBindingSelectionProfile Profile;
	Profile.PackageName = TEXT("avidscript.engine.filtered");
	FAvidScriptReflectedClassSelection ActorRule;
	ActorRule.OwnerClassPath = TEXT("/Script/Engine.Actor");
	ActorRule.IncludeFunctions = { TEXT("K2_GetActorLocation"), TEXT("K2_SetActorLocation") };
	Profile.Classes.Add(MoveTemp(ActorRule));

	TArray<FAvidScriptReflectedFunctionSelection> Selections;
	FAvidScriptBindingSelectionResolveResult Result;
	TestTrue(
		TEXT("Class discovery skips unsupported functions without rejecting compatible peers"),
		FAvidScriptEditorBindingSelectionResolver::Resolve(Profile, Selections, Result));
	TestEqual(TEXT("Filtered profile observes two candidates"), Result.CandidateFunctionCount, 2);
	TestEqual(TEXT("Filtered profile accepts one function"), Selections.Num(), 1);
	TestEqual(TEXT("Filtered profile reports one rejected function"), Result.RejectedFunctionCount, 1);
	if (Selections.Num() == 1)
	{
		TestEqual(TEXT("Supported function is retained"), Selections[0].FunctionName, FName(TEXT("K2_GetActorLocation")));
	}
	if (Result.Issues.Num() == 1)
	{
		TestFalse(TEXT("Discovery compatibility issue is non-fatal"), Result.Issues[0].bFatal);
		TestEqual(TEXT("Unsupported signature has stable category"), Result.Issues[0].Category, FString(TEXT("unsupported_property")));
		TestEqual(TEXT("Rejected function is identified"), Result.Issues[0].FunctionName, FName(TEXT("K2_SetActorLocation")));
		TestTrue(TEXT("Unsupported FHitResult is identified"), Result.Issues[0].Source.Contains(TEXT("FHitResult")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBindingSelectionStrictExplicitFailureTest,
	"AvidScript.Editor.BindingSelection.StrictExplicitFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingSelectionStrictExplicitFailureTest::RunTest(const FString& Parameters)
{
	FAvidScriptBindingSelectionProfile Profile;
	Profile.PackageName = TEXT("avidscript.engine.strict");
	Profile.ExplicitFunctions.Add({
		TEXT("/Script/Engine.Actor"),
		TEXT("AvidScriptMissingFunction")
	});

	TArray<FAvidScriptReflectedFunctionSelection> Selections;
	FAvidScriptBindingSelectionResolveResult Result;
	TestFalse(
		TEXT("Missing explicit function fails closed"),
		FAvidScriptEditorBindingSelectionResolver::Resolve(Profile, Selections, Result));
	TestTrue(TEXT("Strict failure produces no partial selections"), Selections.IsEmpty());
	TestEqual(TEXT("Missing explicit function has stable category"), Result.ErrorCategory, FString(TEXT("function_missing")));
	TestEqual(TEXT("Strict failure records one fatal issue"), Result.Issues.Num(), 1);
	if (Result.Issues.Num() == 1)
	{
		TestTrue(TEXT("Explicit issue is fatal"), Result.Issues[0].bFatal);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBindingSelectionProfileDescriptorTest,
	"AvidScript.Editor.BindingSelection.ProfileDescriptor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingSelectionProfileDescriptorTest::RunTest(const FString& Parameters)
{
	const FAvidScriptBindingSelectionProfile Profile =
		FAvidScriptEditorBindingDescriptorGenerator::MakeEngineGameplayProfile();
	FString FirstJson;
	FAvidScriptBindingSelectionResolveResult FirstSelectionResult;
	FAvidScriptBindingDescriptorGenerateResult FirstDescriptorResult;
	TestTrue(
		TEXT("Gameplay profile generates a descriptor"),
		FAvidScriptEditorBindingDescriptorGenerator::GenerateFromProfile(
			Profile,
			FirstJson,
			FirstSelectionResult,
			FirstDescriptorResult));
	TestEqual(
		TEXT("Descriptor binding count matches accepted profile functions"),
		FirstDescriptorResult.BindingCount,
		FirstSelectionResult.AcceptedFunctionCount);

	FString SecondJson;
	FAvidScriptBindingSelectionResolveResult SecondSelectionResult;
	FAvidScriptBindingDescriptorGenerateResult SecondDescriptorResult;
	TestTrue(
		TEXT("Repeated gameplay profile descriptor generation succeeds"),
		FAvidScriptEditorBindingDescriptorGenerator::GenerateFromProfile(
			Profile,
			SecondJson,
			SecondSelectionResult,
			SecondDescriptorResult));
	TestEqual(TEXT("Profile descriptor bytes are deterministic"), SecondJson, FirstJson);
	TestEqual(
		TEXT("Profile package hash is deterministic"),
		SecondDescriptorResult.PackageHash,
		FirstDescriptorResult.PackageHash);

	return true;
}

#endif
