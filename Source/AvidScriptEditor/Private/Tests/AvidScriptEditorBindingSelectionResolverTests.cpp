#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptEditorBindingDescriptorGenerator.h"
#include "AvidScriptEditorBindingPropertySelectionResolver.h"
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
	TestEqual(TEXT("Gameplay profile declares exactly six facade classes"), Profile.Classes.Num(), 6);
	const TArray<FString> ExpectedClassPaths = {
		TEXT("/Script/Engine.Actor"),
		TEXT("/Script/Engine.ActorComponent"),
		TEXT("/Script/Engine.SceneComponent"),
		TEXT("/Script/Engine.PrimitiveComponent"),
		TEXT("/Script/Engine.Pawn"),
		TEXT("/Script/Engine.Controller")
	};
	for (const FString& ExpectedClassPath : ExpectedClassPaths)
	{
		TestTrue(
			FString::Printf(TEXT("Gameplay profile declares %s"), *ExpectedClassPath),
			Profile.Classes.ContainsByPredicate([&ExpectedClassPath](const FAvidScriptReflectedClassSelection& Rule)
			{
				return Rule.OwnerClassPath == ExpectedClassPath;
			}));
	}
	TArray<FAvidScriptReflectedFunctionSelection> FirstSelections;
	FAvidScriptBindingSelectionResolveResult FirstResult;
	TestTrue(
		TEXT("Engine gameplay profile resolves"),
		FAvidScriptEditorBindingSelectionResolver::Resolve(Profile, FirstSelections, FirstResult));
	AddInfo(FString::Printf(
		TEXT("P48.1 gameplay profile: candidates=%d accepted=%d rejected=%d"),
		FirstResult.CandidateFunctionCount,
		FirstResult.AcceptedFunctionCount,
		FirstResult.RejectedFunctionCount));
	for (const FAvidScriptReflectedClassSelection& Rule : Profile.Classes)
	{
		FAvidScriptBindingSelectionProfile ClassProfile;
		ClassProfile.PackageName = Profile.PackageName;
		ClassProfile.Classes.Add(Rule);
		TArray<FAvidScriptReflectedFunctionSelection> ClassSelections;
		FAvidScriptBindingSelectionResolveResult ClassResult;
		if (TestTrue(
			FString::Printf(TEXT("Gameplay profile class %s resolves"), *Rule.OwnerClassPath),
			FAvidScriptEditorBindingSelectionResolver::Resolve(ClassProfile, ClassSelections, ClassResult)))
		{
			AddInfo(FString::Printf(
				TEXT("P48.1 class=%s candidates=%d accepted=%d rejected=%d"),
				*Rule.OwnerClassPath,
				ClassResult.CandidateFunctionCount,
				ClassResult.AcceptedFunctionCount,
				ClassResult.RejectedFunctionCount));
		}
	}
	TMap<FString, int32> RejectedTypeCounts;
	for (const FAvidScriptBindingSelectionIssue& Issue : FirstResult.Issues)
	{
		if (Issue.Category == TEXT("unsupported_property") && !Issue.Source.IsEmpty())
		{
			++RejectedTypeCounts.FindOrAdd(Issue.Source);
		}
	}
	TArray<TPair<FString, int32>> RejectedTypes;
	for (const TPair<FString, int32>& RejectedType : RejectedTypeCounts)
	{
		RejectedTypes.Add(RejectedType);
	}
	RejectedTypes.Sort([](const TPair<FString, int32>& Left, const TPair<FString, int32>& Right)
	{
		return Left.Value == Right.Value
			? Left.Key.Compare(Right.Key, ESearchCase::CaseSensitive) < 0
			: Left.Value > Right.Value;
	});
	for (int32 Index = 0; Index < FMath::Min(RejectedTypes.Num(), 5); ++Index)
	{
		AddInfo(FString::Printf(
			TEXT("P48.1 rejected_type=%s count=%d"),
			*RejectedTypes[Index].Key,
			RejectedTypes[Index].Value));
	}
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
	TestTrue(
		TEXT("Pawn facade retains Pawn-declared movement input"),
		FirstSelections.ContainsByPredicate([](const FAvidScriptReflectedFunctionSelection& Selection)
		{
			return Selection.OwnerClassPath == TEXT("/Script/Engine.Pawn")
				&& Selection.FunctionName == TEXT("AddMovementInput");
		}));
	TestFalse(
		TEXT("Pawn facade does not copy Actor-declared location query"),
		FirstSelections.ContainsByPredicate([](const FAvidScriptReflectedFunctionSelection& Selection)
		{
			return Selection.OwnerClassPath == TEXT("/Script/Engine.Pawn")
				&& Selection.FunctionName == TEXT("K2_GetActorLocation");
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
	FAvidScriptEditorBindingSelectionFNameExplicitTest,
	"AvidScript.Editor.BindingSelection.FNameExplicit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingSelectionFNameExplicitTest::RunTest(const FString& Parameters)
{
	FAvidScriptBindingSelectionProfile Profile;
	Profile.PackageName = TEXT("avidscript.engine.fname");
	Profile.ExplicitFunctions.Add({
		TEXT("/Script/Engine.Actor"),
		TEXT("ActorHasTag")
	});

	TArray<FAvidScriptReflectedFunctionSelection> Selections;
	FAvidScriptBindingSelectionResolveResult Result;
	TestTrue(
		TEXT("Explicit ActorHasTag FName input is supported"),
		FAvidScriptEditorBindingSelectionResolver::Resolve(Profile, Selections, Result));
	TestEqual(TEXT("FName explicit selection produces one function"), Selections.Num(), 1);
	if (Selections.Num() == 1)
	{
		TestEqual(TEXT("FName explicit selection keeps ActorHasTag"), Selections[0].FunctionName, FName(TEXT("ActorHasTag")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBindingPropertySelectionCompatibilityTest,
	"AvidScript.Editor.BindingSelection.PropertyCompatibility",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingPropertySelectionCompatibilityTest::RunTest(const FString& Parameters)
{
	FAvidScriptBindingSelectionProfile Profile;
	Profile.PackageName = TEXT("avidscript.engine.property_compatibility");
	FAvidScriptReflectedClassSelection ActorRule;
	ActorRule.OwnerClassPath = TEXT("/Script/Engine.Actor");
	ActorRule.IncludeProperties = {
		TEXT("CustomTimeDilation"),
		TEXT("InitialLifeSpan"),
		TEXT("Tags")
	};
	Profile.Classes.Add(MoveTemp(ActorRule));

	TArray<FAvidScriptReflectedPropertySelection> FirstSelections;
	FAvidScriptBindingSelectionResolveResult FirstResult;
	TestTrue(
		TEXT("Compatible properties survive an unsupported peer"),
		FAvidScriptEditorBindingPropertySelectionResolver::ResolveReadable(
			Profile,
			FirstSelections,
			FirstResult));
	TestEqual(TEXT("Three reflected properties are considered"), FirstResult.CandidatePropertyCount, 3);
	TestEqual(TEXT("Two scalar properties are accepted"), FirstResult.AcceptedPropertyCount, 2);
	TestEqual(TEXT("One array property is rejected"), FirstResult.RejectedPropertyCount, 1);
	TestEqual(TEXT("Selections are deterministically ordered"), FirstSelections.Num(), 2);
	if (FirstSelections.Num() == 2)
	{
		TestEqual(TEXT("CustomTimeDilation sorts first"), FirstSelections[0].PropertyName, FName(TEXT("CustomTimeDilation")));
		TestEqual(TEXT("InitialLifeSpan sorts second"), FirstSelections[1].PropertyName, FName(TEXT("InitialLifeSpan")));
	}
	if (FirstResult.Issues.Num() == 1)
	{
		TestFalse(TEXT("Discovery rejection is non-fatal"), FirstResult.Issues[0].bFatal);
		TestEqual(TEXT("Issue identifies a property"), FirstResult.Issues[0].MemberKind, FString(TEXT("property")));
		TestEqual(TEXT("Array rejection identifies Tags"), FirstResult.Issues[0].PropertyName, FName(TEXT("Tags")));
		TestEqual(
			TEXT("Unsupported type uses a stable category"),
			FirstResult.Issues[0].Category,
			FString(TEXT("unsupported_property_type")));
	}

	TArray<FAvidScriptReflectedPropertySelection> SecondSelections;
	FAvidScriptBindingSelectionResolveResult SecondResult;
	TestTrue(
		TEXT("Repeated property resolution succeeds"),
		FAvidScriptEditorBindingPropertySelectionResolver::ResolveReadable(
			Profile,
			SecondSelections,
			SecondResult));
	TestEqual(TEXT("Repeated accepted count is stable"), SecondSelections.Num(), FirstSelections.Num());
	TestEqual(TEXT("Repeated issue count is stable"), SecondResult.Issues.Num(), FirstResult.Issues.Num());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBindingPropertySelectionStrictFailureTest,
	"AvidScript.Editor.BindingSelection.PropertyStrictFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingPropertySelectionStrictFailureTest::RunTest(const FString& Parameters)
{
	FAvidScriptBindingSelectionProfile Profile;
	Profile.PackageName = TEXT("avidscript.engine.property_strict");
	Profile.ExplicitProperties.Add({
		TEXT("/Script/Engine.Actor"),
		TEXT("AvidScriptMissingProperty")
	});

	TArray<FAvidScriptReflectedPropertySelection> Selections;
	FAvidScriptBindingSelectionResolveResult Result;
	TestFalse(
		TEXT("Missing explicit property fails closed"),
		FAvidScriptEditorBindingPropertySelectionResolver::ResolveReadable(
			Profile,
			Selections,
			Result));
	TestTrue(TEXT("Strict property failure produces no partial selection"), Selections.IsEmpty());
	TestEqual(TEXT("Missing property has stable category"), Result.ErrorCategory, FString(TEXT("property_missing")));
	TestEqual(TEXT("Strict property failure records one issue"), Result.Issues.Num(), 1);
	if (Result.Issues.Num() == 1)
	{
		TestTrue(TEXT("Explicit property issue is fatal"), Result.Issues[0].bFatal);
		TestEqual(TEXT("Issue member kind is property"), Result.Issues[0].MemberKind, FString(TEXT("property")));
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
	TestEqual(TEXT("Gameplay profile accepts 340 functions including FName inputs"), FirstSelectionResult.AcceptedFunctionCount, 340);
	TestEqual(TEXT("Gameplay profile accepts two readable properties"), FirstSelectionResult.AcceptedPropertyCount, 2);
	TestEqual(
		TEXT("Descriptor binding count matches all accepted reflected members"),
		FirstDescriptorResult.BindingCount,
		FirstSelectionResult.AcceptedFunctionCount + FirstSelectionResult.AcceptedPropertyCount);

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
