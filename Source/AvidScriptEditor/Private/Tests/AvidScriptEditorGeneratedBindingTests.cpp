#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptBindingDescriptor.h"
#include "AvidScriptEditorBindingDescriptorGenerator.h"
#include "AvidScriptEditorGeneratedBindingService.h"
#include "AvidScriptHash.h"
#include "BindingGeneration/AvidScriptEditorCSharpBindingRenderer.h"

#include "Algo/Reverse.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

namespace
{
FAvidScriptGeneratedBindingIr MakeGeneratedBinding(
	const TCHAR StableCharacter,
	const FString& ImportSuffix)
{
	FAvidScriptGeneratedBindingIr Binding;
	Binding.StableId = FString::ChrN(64, StableCharacter);
	Binding.OwnerModule = TEXT("Engine");
	Binding.OwnerHeader = TEXT("GameFramework/Actor.h");
	Binding.OwnerCppType = TEXT("AActor");
	Binding.FunctionName = TEXT("GeneratedPair");
	Binding.ImportModule = TEXT("avidscript");
	Binding.ImportName = TEXT("avid_s1_") + ImportSuffix;
	Binding.AbiSignature = TEXT("(iiii)i");
	Binding.Shape = EAvidScriptGeneratedBindingShape::I32PairToI32;
	Binding.ReceiverMode = EAvidScriptGeneratedReceiverMode::SelfBound;
	Binding.DescriptorIdentity =
		TEXT("/Script/Engine.Actor::GeneratedPair::") + ImportSuffix;
	return Binding;
}

bool ReadGeneratedArtifacts(
	const FString& Root,
	TArray<FString>& OutContents)
{
	static const TCHAR* Paths[] = {
		TEXT("Source/AvidScriptGeneratedBindings/AvidScriptGeneratedBindings.Build.cs"),
		TEXT("Source/AvidScriptGeneratedBindings/Public/AvidScriptGeneratedBindings.h"),
		TEXT("Source/AvidScriptGeneratedBindings/Private/AvidScriptGeneratedBindings.cpp"),
		TEXT("Source/AvidScriptGeneratedBindings/Private/generated-bindings.manifest.json")
	};
	OutContents.Reset();
	for (const TCHAR* RelativePath : Paths)
	{
		FString Contents;
		if (!FFileHelper::LoadFileToString(
				Contents,
				*(Root / RelativePath)))
		{
			return false;
		}
		OutContents.Add(MoveTemp(Contents));
	}
	return true;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorGeneratedBindingDeterminismTest,
	"AvidScript.Editor.GeneratedBindings.DeterministicProjectSource",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorGeneratedBindingDeterminismTest::RunTest(
	const FString& Parameters)
{
	const FString TestRoot = FPaths::ProjectSavedDir()
		/ TEXT("AvidScriptTests")
		/ (TEXT("GeneratedBindings-")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits));
	IFileManager::Get().MakeDirectory(*TestRoot, true);
	const FString ProjectFile = TestRoot / TEXT("GeneratedBindings.uproject");
	TestTrue(
		TEXT("Temporary project descriptor is written"),
		FFileHelper::SaveStringToFile(
			TEXT("{\n  \"FileVersion\": 3,\n  \"Modules\": []\n}\n"),
			*ProjectFile));

	FAvidScriptGeneratedBindingPackageIr Package;
	Package.PackageName = TEXT("avidscript.generated.tests");
	Package.PackageHash = FString::ChrN(64, TEXT('a'));
	Package.Bindings.Add(MakeGeneratedBinding(
		TEXT('2'),
		TEXT("2222222222222222")));
	Package.Bindings.Add(MakeGeneratedBinding(
		TEXT('1'),
		TEXT("1111111111111111")));
	FAvidScriptGeneratedBindingIr PropertyBinding = MakeGeneratedBinding(
		TEXT('3'),
		TEXT("3333333333333333"));
	PropertyBinding.Shape =
		EAvidScriptGeneratedBindingShape::PropertyI32GetSet;
	PropertyBinding.AbiSignature = TEXT("(iii)i");
	PropertyBinding.FunctionName = TEXT("GeneratedProperty");
	Package.Bindings.Add(PropertyBinding);
	FAvidScriptGeneratedBindingIr VectorBinding = MakeGeneratedBinding(
		TEXT('4'),
		TEXT("4444444444444444"));
	VectorBinding.Shape = EAvidScriptGeneratedBindingShape::VectorValue;
	VectorBinding.AbiSignature = TEXT("(iii)i");
	VectorBinding.FunctionName = TEXT("GeneratedVector");
	Package.Bindings.Add(VectorBinding);
	FAvidScriptGeneratedBindingIr ObjectBinding = MakeGeneratedBinding(
		TEXT('5'),
		TEXT("5555555555555555"));
	ObjectBinding.Shape =
		EAvidScriptGeneratedBindingShape::StableObjectRoundtrip;
	ObjectBinding.AbiSignature = TEXT("(iiiii)i");
	ObjectBinding.ReceiverMode =
		EAvidScriptGeneratedReceiverMode::StableBorrow;
	ObjectBinding.FunctionName = TEXT("GeneratedObject");
	Package.Bindings.Add(ObjectBinding);

	FAvidScriptEditorGeneratedBindingResult FirstResult;
	TestTrue(
		TEXT("First deterministic emission succeeds"),
		FAvidScriptEditorGeneratedBindingService::EmitProjectModule(
			ProjectFile,
			Package,
			FirstResult));
	TArray<FString> FirstContents;
	TestTrue(
		TEXT("First artifact set is readable"),
		ReadGeneratedArtifacts(TestRoot, FirstContents));

	Algo::Reverse(Package.Bindings);
	FAvidScriptEditorGeneratedBindingResult SecondResult;
	TestTrue(
		TEXT("Reordered emission succeeds"),
		FAvidScriptEditorGeneratedBindingService::EmitProjectModule(
			ProjectFile,
			Package,
			SecondResult));
	TestTrue(
		TEXT("Byte-identical generated module is reused"),
		SecondResult.bReusedExistingModule);
	TArray<FString> SecondContents;
	TestTrue(
		TEXT("Second artifact set is readable"),
		ReadGeneratedArtifacts(TestRoot, SecondContents));
	TestTrue(
		TEXT("Input ordering does not alter emitted bytes"),
		SecondContents == FirstContents);
	TestTrue(
		TEXT("Generated source contains lifecycle registration"),
		SecondContents[2].Contains(TEXT("RegisterPackage"))
			&& SecondContents[2].Contains(TEXT("UnregisterPackage"))
			&& SecondContents[2].Contains(TEXT("InvokeGenerated_0000"))
			&& SecondContents[2].Contains(
				TEXT("&InvokeGenerated_0000, nullptr, nullptr, nullptr"))
			&& SecondContents[2].Contains(
				TEXT("nullptr, &InvokeGenerated_0002, nullptr, nullptr"))
			&& SecondContents[2].Contains(
				TEXT("nullptr, nullptr, &InvokeGenerated_0003, nullptr"))
			&& SecondContents[2].Contains(
				TEXT("nullptr, nullptr, nullptr, &InvokeGenerated_0004")));
	TestFalse(
		TEXT("Generated call sites never use checked casts"),
		SecondContents[2].Contains(TEXT("CastChecked")));

	IFileManager::Get().DeleteDirectory(*TestRoot, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorGeneratedBindingIdentityAndInputTest,
	"AvidScript.Editor.GeneratedBindings.IdentityAndInputValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorGeneratedBindingIdentityAndInputTest::RunTest(
	const FString& Parameters)
{
	const FString First =
		FAvidScriptBindingDescriptorIdentity::MakeFunctionCanonicalIdentity(
			TEXT("owner::call"),
			TEXT("generated_native_s1"),
			TEXT("i32_pair_to_i32"),
			TEXT("self_bound"),
			TEXT("avid_s1_1111111111111111"));
	const FString Second =
		FAvidScriptBindingDescriptorIdentity::MakeFunctionCanonicalIdentity(
			TEXT("owner::call"),
			TEXT("generated_native_s1"),
			TEXT("vector_value"),
			TEXT("self_bound"),
			TEXT("avid_s1_1111111111111111"));
	TestNotEqual(
		TEXT("One generated shape field changes canonical SHA-256"),
		FAvidScriptHash::Sha256HexUtf8(First),
		FAvidScriptHash::Sha256HexUtf8(Second));

	const FString TestRoot = FPaths::ProjectSavedDir()
		/ TEXT("AvidScriptTests")
		/ (TEXT("GeneratedBindingsReject-")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits));
	IFileManager::Get().MakeDirectory(*TestRoot, true);
	const FString ProjectFile = TestRoot / TEXT("GeneratedBindings.uproject");
	FFileHelper::SaveStringToFile(
		TEXT("{\"FileVersion\":3,\"Modules\":[]}"),
		*ProjectFile);

	FAvidScriptGeneratedBindingPackageIr Package;
	Package.PackageName = TEXT("avidscript.generated.reject");
	Package.PackageHash = FString::ChrN(64, TEXT('b'));
	Package.Bindings.Add(MakeGeneratedBinding(
		TEXT('3'),
		TEXT("3333333333333333")));
	Package.Bindings[0].OwnerHeader = TEXT("../Private/Secret.h");
	FAvidScriptEditorGeneratedBindingResult Result;
	TestFalse(
		TEXT("Traversal include is rejected before source write"),
		FAvidScriptEditorGeneratedBindingService::EmitProjectModule(
			ProjectFile,
			Package,
			Result));
	TestEqual(
		TEXT("Traversal rejection category is stable"),
		Result.ErrorCategory,
		FString(TEXT("generated_binding_ir_invalid")));

	IFileManager::Get().DeleteDirectory(*TestRoot, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorGeneratedPropertyReachabilityTest,
	"AvidScript.Editor.GeneratedBindings.PropertyReachability",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorGeneratedPropertyReachabilityTest::RunTest(
	const FString& Parameters)
{
	FAvidScriptBindingSelectionProfile Profile;
	Profile.PackageName = TEXT("avidscript.generated.property.reachability");
	FAvidScriptReflectedClassSelection Rule;
	Rule.OwnerClassPath =
		TEXT("/Script/AvidScriptEditor.AvidScriptBindingRuntimeProcessEventTestActor");
	Rule.ExcludeFunctions.Add(TEXT("SetAlternateRoutedValue"));
	Rule.ExcludeFunctions.Add(TEXT("SetGeneratedSetterInt"));
	Rule.ExcludeFunctions.Add(TEXT("SetRoutedValue"));
	Rule.IncludeProperties.Add(TEXT("GeneratedPublicInt"));
	Rule.WritableProperties.Add(TEXT("GeneratedPublicInt"));
	Rule.GeneratedNativeProperties.Add(TEXT("GeneratedPublicInt"));
	Profile.Classes.Add(MoveTemp(Rule));

	FString DescriptorJson;
	FAvidScriptBindingSelectionResolveResult SelectionResult;
	FAvidScriptBindingDescriptorGenerateResult DescriptorResult;
	TestTrue(
		TEXT("Real profile reaches generated property descriptor"),
		FAvidScriptEditorBindingDescriptorGenerator::GenerateFromProfile(
			Profile,
			DescriptorJson,
			SelectionResult,
			DescriptorResult));

	FAvidScriptGeneratedBindingPackageIr Package;
	FAvidScriptEditorGeneratedBindingResult IrResult;
	TestTrue(
		TEXT("Generated property descriptor reaches IR"),
		FAvidScriptEditorGeneratedBindingService::BuildIr(
			DescriptorJson,
			Package,
			IrResult));
	TestEqual(
		TEXT("Generated property IR contains getter and setter"),
		Package.Bindings.Num(),
		2);
	for (const FAvidScriptGeneratedBindingIr& Binding : Package.Bindings)
	{
		TestFalse(
			TEXT("Generated cross-module include omits source-root prefixes"),
			Binding.OwnerHeader.StartsWith(TEXT("Public/"))
				|| Binding.OwnerHeader.StartsWith(TEXT("Classes/")));
		TestEqual(
			TEXT("IR uses the reflected field token"),
			Binding.FunctionName,
			FString(TEXT("GeneratedPublicInt")));
		TestEqual(
			TEXT("IR keeps the property shape"),
			Binding.Shape,
			EAvidScriptGeneratedBindingShape::PropertyI32GetSet);
	}

	FAvidScriptBindingPackageModel DescriptorPackage;
	FString ParseErrorCategory;
	FString ParseErrorSource;
	TestTrue(
		TEXT("Generated property descriptor parses for C# rendering"),
		FAvidScriptBindingDescriptorParser::Parse(
			DescriptorJson,
			DescriptorPackage,
			ParseErrorCategory,
			ParseErrorSource));
	const FAvidScriptBindingFunctionModel* Setter = DescriptorPackage.Bindings.FindByPredicate(
		[](const FAvidScriptBindingFunctionModel& Binding)
		{
			return Binding.BindingKind == TEXT("property_set");
		});
	FString CSharpSource;
	FString CSharpErrorCategory;
	FString CSharpErrorSource;
	TestNotNull(TEXT("Generated property setter model resolves"), Setter);
	TestTrue(
		TEXT("Generated property descriptor reaches the C# facade"),
		FAvidScriptEditorCSharpBindingRenderer::EmitReferenceSource(
			DescriptorPackage,
			FAvidScriptHash::Sha256HexUtf8(DescriptorJson),
			CSharpSource,
			CSharpErrorCategory,
			CSharpErrorSource));
	TestTrue(
		TEXT("C# facade declares the compile-time data lane attribute"),
		CSharpSource.Contains(TEXT("internal sealed class AvidScriptDataLaneAttribute")));
	if (Setter != nullptr)
	{
		TestTrue(
			TEXT("Generated int setter carries its real binding ordinal"),
			CSharpSource.Contains(FString::Printf(
				TEXT("[AvidScriptDataLane(\"buffered_write\", %d)]"),
				Setter->Ordinal)));
	}

	const FString TestRoot = FPaths::ProjectSavedDir()
		/ TEXT("AvidScriptTests")
		/ (TEXT("GeneratedProperty-")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits));
	IFileManager::Get().MakeDirectory(*TestRoot, true);
	const FString ProjectFile = TestRoot / TEXT("GeneratedProperty.uproject");
	TestTrue(
		TEXT("Generated property temporary project is written"),
		FFileHelper::SaveStringToFile(
			TEXT("{\"FileVersion\":3,\"Modules\":[]}"),
			*ProjectFile));
	FAvidScriptEditorGeneratedBindingResult EmitResult;
	TestTrue(
		TEXT("Generated property IR reaches project source"),
		FAvidScriptEditorGeneratedBindingService::EmitProjectModule(
			ProjectFile,
			Package,
			EmitResult));
	FString GeneratedSource;
	TestTrue(
		TEXT("Generated property source is readable"),
		FFileHelper::LoadFileToString(
			GeneratedSource,
			*(TestRoot
				/ TEXT("Source/AvidScriptGeneratedBindings/Private/AvidScriptGeneratedBindings.cpp"))));
	TestTrue(
		TEXT("Generated source directly accesses the public member"),
		GeneratedSource.Contains(
			TEXT("TypedReceiver->GeneratedPublicInt")));

	IFileManager::Get().DeleteDirectory(*TestRoot, false, true);
	return true;
}

#endif
