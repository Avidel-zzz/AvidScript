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
#include "Misc/ScopeExit.h"

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
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*TestRoot, false, true);
	};
	const FString ProjectFile = TestRoot / TEXT("GeneratedBindings.uproject");
	if (!TestTrue(
		TEXT("Temporary project descriptor is written"),
		FFileHelper::SaveStringToFile(
			TEXT("{\n  \"FileVersion\": 3,\n  \"Modules\": []\n}\n"),
			*ProjectFile)))
	{
		return false;
	}

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
	FAvidScriptGeneratedBindingIr PropertyGetterBinding = MakeGeneratedBinding(
		TEXT('6'),
		TEXT("6666666666666666"));
	PropertyGetterBinding.Shape =
		EAvidScriptGeneratedBindingShape::PropertyI32Get;
	PropertyGetterBinding.AbiSignature = TEXT("(ii)i");
	PropertyGetterBinding.FunctionName = TEXT("GeneratedPropertyGetter");
	Package.Bindings.Add(PropertyGetterBinding);
	FAvidScriptGeneratedBindingIr PropertySetterBinding = MakeGeneratedBinding(
		TEXT('7'),
		TEXT("7777777777777777"));
	PropertySetterBinding.Shape =
		EAvidScriptGeneratedBindingShape::PropertyI32Set;
	PropertySetterBinding.AbiSignature = TEXT("(iii)i");
	PropertySetterBinding.FunctionName = TEXT("GeneratedPropertySetter");
	Package.Bindings.Add(PropertySetterBinding);
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
	FAvidScriptGeneratedBindingIr UnaryBinding = MakeGeneratedBinding(
		TEXT('8'),
		TEXT("8888888888888888"));
	UnaryBinding.Shape = EAvidScriptGeneratedBindingShape::I32ToI32;
	UnaryBinding.AbiSignature = TEXT("(iii)i");
	UnaryBinding.FunctionName = TEXT("GeneratedUnary");
	Package.Bindings.Add(UnaryBinding);
	FAvidScriptGeneratedBindingIr VectorRefOutBinding = MakeGeneratedBinding(
		TEXT('9'),
		TEXT("9999999999999999"));
	VectorRefOutBinding.Shape =
		EAvidScriptGeneratedBindingShape::VectorRefOut;
	VectorRefOutBinding.AbiSignature = TEXT("(iii)i");
	VectorRefOutBinding.FunctionName = TEXT("GeneratedVectorRefOut");
	Package.Bindings.Add(VectorRefOutBinding);

	FAvidScriptEditorGeneratedBindingResult FirstResult;
	if (!TestTrue(
		TEXT("First deterministic emission succeeds"),
		FAvidScriptEditorGeneratedBindingService::EmitProjectModule(
			ProjectFile,
			Package,
			FirstResult)))
	{
		AddError(FirstResult.ErrorMessage);
		return false;
	}
	TArray<FString> FirstContents;
	if (!TestTrue(
		TEXT("First artifact set is readable"),
		ReadGeneratedArtifacts(TestRoot, FirstContents)))
	{
		return false;
	}

	Algo::Reverse(Package.Bindings);
	FAvidScriptEditorGeneratedBindingResult SecondResult;
	if (!TestTrue(
		TEXT("Reordered emission succeeds"),
		FAvidScriptEditorGeneratedBindingService::EmitProjectModule(
			ProjectFile,
			Package,
			SecondResult)))
	{
		AddError(SecondResult.ErrorMessage);
		return false;
	}
	TestTrue(
		TEXT("Byte-identical generated module is reused"),
		SecondResult.bReusedExistingModule);
	TArray<FString> SecondContents;
	if (!TestTrue(
		TEXT("Second artifact set is readable"),
		ReadGeneratedArtifacts(TestRoot, SecondContents)))
	{
		return false;
	}
	TestTrue(
		TEXT("Input ordering does not alter emitted bytes"),
		SecondContents == FirstContents);
	TestTrue(
		TEXT("Generated source contains lifecycle registration"),
		SecondContents[2].Contains(TEXT("RegisterPackage"))
			&& SecondContents[2].Contains(TEXT("UnregisterPackage"))
			&& SecondContents[2].Contains(TEXT("InvokeGenerated_0000"))
			&& SecondContents[2].Contains(
				TEXT("InvokePreparedGenerated_0000"))
			&& SecondContents[2].Contains(
				TEXT("&InvokeGenerated_0000, nullptr, nullptr, nullptr, nullptr, nullptr"))
			&& SecondContents[2].Contains(
				TEXT("nullptr, &InvokeGenerated_0002, nullptr, nullptr, nullptr, nullptr"))
			&& SecondContents[2].Contains(
				TEXT("nullptr, nullptr, nullptr, nullptr, &InvokeGenerated_0003, nullptr"))
			&& SecondContents[2].Contains(
				TEXT("nullptr, nullptr, nullptr, nullptr, nullptr, &InvokeGenerated_0004"))
			&& SecondContents[2].Contains(
				TEXT("nullptr, nullptr, &InvokeGenerated_0005, nullptr, nullptr, nullptr"))
			&& SecondContents[2].Contains(
				TEXT("nullptr, nullptr, nullptr, &InvokeGenerated_0006, nullptr, nullptr"))
			&& SecondContents[2].Contains(
				TEXT("int32& OutValue"))
			&& SecondContents[2].Contains(
				TEXT("int32 Value"))
			&& SecondContents[2].Contains(
				TEXT("OutValue = TypedReceiver->GeneratedUnary(Value);"))
			&& SecondContents[2].Contains(
				TEXT("TypedReceiver->GeneratedVectorRefOut(InOutValue, OutValue);"))
			&& SecondContents[2].Contains(
				TEXT("OutValue = TypedReceiver->GeneratedObject(InValue);"))
			&& SecondContents[2].Contains(
				TEXT("&InvokePreparedGenerated_0004"))
			&& SecondContents[2].Contains(
				TEXT("&InvokePreparedGenerated_0008"))
			&& SecondContents[2].Contains(TEXT("static_cast<")));
	TestFalse(
		TEXT("Generated call sites never use checked casts"),
		SecondContents[2].Contains(TEXT("CastChecked")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorGeneratedBindingPackedObjectFacadeTest,
	"AvidScript.Editor.GeneratedBindings.PackedObjectFacade",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorGeneratedBindingPackedObjectFacadeTest::RunTest(
	const FString& Parameters)
{
	const FString TestClassPath =
		TEXT("/Script/AvidScriptBindings.AvidScriptBindingsTestObject");
	FAvidScriptBindingSelectionProfile Profile;
	Profile.PackageName = TEXT("avidscript.generated.packed_object_facade");
	FAvidScriptReflectedClassSelection Rule;
	Rule.OwnerClassPath = TestClassPath;
	Rule.IncludeFunctions.Add(TEXT("FastPathObjectRoundtrip"));
	Rule.GeneratedNativeFunctions.Add(TEXT("FastPathObjectRoundtrip"));
	Profile.Classes.Add(MoveTemp(Rule));

	FString DescriptorJson;
	FAvidScriptBindingSelectionResolveResult SelectionResult;
	FAvidScriptBindingDescriptorGenerateResult GenerateResult;
	if (!TestTrue(
			TEXT("Generated object descriptor provides the facade fixture"),
			FAvidScriptEditorBindingDescriptorGenerator::GenerateFromProfile(
				Profile,
				DescriptorJson,
				SelectionResult,
				GenerateResult)))
	{
		AddError(GenerateResult.ErrorMessage);
		return false;
	}

	FAvidScriptBindingPackageModel Package;
	FString ErrorCategory;
	FString ErrorSource;
	if (!TestTrue(
			TEXT("Packed object descriptor satisfies the shared parser"),
			FAvidScriptBindingDescriptorParser::Parse(
				DescriptorJson,
				Package,
				ErrorCategory,
				ErrorSource))
		|| !TestEqual(
			TEXT("Packed object descriptor has one binding"),
			Package.Bindings.Num(),
			1))
	{
		AddError(ErrorCategory + TEXT(": ") + ErrorSource);
		return false;
	}
	TestEqual(
		TEXT("Generated object descriptor selects the packed ABI"),
		Package.Bindings[0].GeneratedShape,
		FString(TEXT("packed_stable_object_roundtrip")));
	TestEqual(
		TEXT("Generated object descriptor publishes the packed signature"),
		Package.Bindings[0].HostImport.Signature,
		FString(TEXT("(II)I")));

	FAvidScriptBindingPackageModel PackedPackage = Package;
	PackedPackage.Bindings[0].GeneratedShape =
		TEXT("packed_stable_object_roundtrip");
	PackedPackage.Bindings[0].HostImport.Signature = TEXT("(II)I");
	const FAvidScriptBindingFunctionModel& Binding =
		PackedPackage.Bindings[0];

	FString PackedSource;
	if (!TestTrue(
			TEXT("Packed object descriptor reaches the C# facade"),
			FAvidScriptEditorCSharpBindingRenderer::EmitReferenceSource(
				PackedPackage,
				FAvidScriptHash::Sha256HexUtf8(
					TEXT("packed:") + DescriptorJson),
				PackedSource,
				ErrorCategory,
				ErrorSource)))
	{
		AddError(ErrorCategory + TEXT(": ") + ErrorSource);
		return false;
	}

	const FString NativeMethod = FString::Printf(
		TEXT("Invoke%04d"),
		Binding.Ordinal);
	TestTrue(
		TEXT("Packed facade declares an i64 DllImport contract"),
		PackedSource.Contains(FString::Printf(
			TEXT("[DllImport(\"%s\", EntryPoint = \"%s\")]"),
			*Binding.HostImport.Module,
			*Binding.HostImport.Name))
			&& PackedSource.Contains(FString::Printf(
				TEXT("internal static extern long %s(long selfHandle, long p0Handle);"),
				*NativeMethod)));
	TestTrue(
		TEXT("Packed facade combines self and object handle cells"),
		PackedSource.Contains(
			TEXT("((long)(uint)this.Slot | ((long)(uint)this.Generation << 32))"))
			&& PackedSource.Contains(
				TEXT("((long)(uint)value.AvidScriptSlot | ((long)(uint)value.AvidScriptGeneration << 32))")));
	TestTrue(
		TEXT("Packed facade decodes the returned public object wrapper"),
		PackedSource.Contains(
			TEXT("public UObject FastPathObjectRoundtrip(UObject value)"))
			&& PackedSource.Contains(FString::Printf(
				TEXT("long __packedReturnValue = AvidScriptNative.%s("),
				*NativeMethod))
			&& PackedSource.Contains(
				TEXT("unchecked((int)(uint)__packedReturnValue)"))
			&& PackedSource.Contains(
				TEXT("unchecked((int)(uint)((ulong)__packedReturnValue >> 32))"))
			&& PackedSource.Contains(
				TEXT("return new UObject(__returnValue.Slot, __returnValue.Generation);")));

	FAvidScriptBindingPackageModel LegacyPackage = Package;
	LegacyPackage.Bindings[0].GeneratedShape =
		TEXT("stable_object_roundtrip");
	LegacyPackage.Bindings[0].HostImport.Signature = TEXT("(iiiii)i");
	FString LegacySource;
	TestTrue(
		TEXT("Legacy object descriptor remains renderable"),
		FAvidScriptEditorCSharpBindingRenderer::EmitReferenceSource(
			LegacyPackage,
			TEXT("legacy-stable-object-roundtrip"),
			LegacySource,
			ErrorCategory,
			ErrorSource));
	TestTrue(
		TEXT("Legacy object facade keeps split handles and out return storage"),
		LegacySource.Contains(FString::Printf(
			TEXT("internal static extern int %s(int selfSlot, int selfGeneration, int p0Slot, int p0Generation, out FAvidScriptObjectHandle returnValue);"),
			*NativeMethod))
			&& LegacySource.Contains(
				TEXT("this.Slot, this.Generation, value.AvidScriptSlot, value.AvidScriptGeneration, out __returnValue")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorGeneratedBindingTransactionPathTest,
	"AvidScript.Editor.GeneratedBindings.DeepProjectTransaction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorGeneratedBindingTransactionPathTest::RunTest(
	const FString& Parameters)
{
	FString TestRoot = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir() / TEXT("AvidScriptTests")
		/ FGuid::NewGuid().ToString(EGuidFormats::Digits));
	// The old staging name put Public/Private at the Win32 directory boundary.
	if (TestRoot.Len() < 167)
	{
		TestRoot += FString::ChrN(167 - TestRoot.Len(), TEXT('x'));
	}
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*TestRoot, false, true);
	};
	if (!TestTrue(TEXT("Deep project root is created"),
		IFileManager::Get().MakeDirectory(*TestRoot, true)))
	{
		return false;
	}
	const FString ProjectFile = TestRoot / TEXT("GeneratedBindings.uproject");
	const FString OriginalProject = TEXT("{\"FileVersion\":3,\"Modules\":[]}");
	if (!TestTrue(TEXT("Deep project descriptor is written"),
		FFileHelper::SaveStringToFile(OriginalProject, *ProjectFile)))
	{
		return false;
	}
	FAvidScriptGeneratedBindingPackageIr Package;
	Package.PackageName = TEXT("avidscript.generated.deep_project");
	Package.PackageHash = FString::ChrN(64, TEXT('a'));
	Package.Bindings.Add(MakeGeneratedBinding(TEXT('1'), TEXT("1111111111111111")));
	FAvidScriptEditorGeneratedBindingResult Result;
	if (!TestTrue(TEXT("Deep project initial emission succeeds"),
		FAvidScriptEditorGeneratedBindingService::EmitProjectModule(
			ProjectFile, Package, Result)))
	{
		AddError(Result.ErrorMessage);
		return false;
	}
	TArray<FString> FirstContents;
	if (!TestTrue(TEXT("Deep project initial artifacts are readable"),
		ReadGeneratedArtifacts(TestRoot, FirstContents)))
	{
		return false;
	}
	Package.PackageHash = FString::ChrN(64, TEXT('b'));
	Package.Bindings[0].FunctionName = TEXT("ReplacementPair");
	if (!TestTrue(TEXT("Deep project replacement emission succeeds"),
		FAvidScriptEditorGeneratedBindingService::EmitProjectModule(
			ProjectFile, Package, Result)))
	{
		AddError(Result.ErrorMessage);
		return false;
	}
	TestFalse(TEXT("Changed package is replaced instead of reused"),
		Result.bReusedExistingModule);
	TArray<FString> ReplacementContents;
	if (!TestTrue(TEXT("Deep project replacement artifacts are readable"),
		ReadGeneratedArtifacts(TestRoot, ReplacementContents)))
	{
		return false;
	}
	TestTrue(TEXT("Replacement publishes new source and manifest"),
		ReplacementContents[2].Contains(TEXT("ReplacementPair"))
			&& ReplacementContents[2] != FirstContents[2]
			&& ReplacementContents[3] != FirstContents[3]);
	TArray<FString> LeftoverTransactions;
	IFileManager::Get().FindFiles(LeftoverTransactions,
		*(TestRoot / TEXT("Source/.asgb-*")), false, true);
	TestEqual(TEXT("Published transaction leaves no stage or backup directory"),
		LeftoverTransactions.Num(), 0);

	const FString RejectedRoot = TestRoot / TEXT("Rejected");
	const FString RejectedProject = RejectedRoot / TEXT("Rejected.uproject");
	if (!TestTrue(TEXT("Rejected project fixture is created"),
		IFileManager::Get().MakeDirectory(*RejectedRoot, true)
			&& FFileHelper::SaveStringToFile(OriginalProject, *RejectedProject)
			&& FFileHelper::SaveStringToFile(TEXT("source-directory-blocker"),
				*(RejectedRoot / TEXT("Source")))))
	{
		return false;
	}
	TestFalse(TEXT("Unwritable stage fails without publishing"),
		FAvidScriptEditorGeneratedBindingService::EmitProjectModule(
			RejectedProject, Package, Result));
	TestEqual(TEXT("Stage failure keeps its stable category"),
		Result.ErrorCategory, FString(TEXT("generated_stage_write_failed")));
	TestTrue(TEXT("Stage failure identifies the failed directory"),
		Result.ErrorSource.StartsWith(RejectedRoot / TEXT("Source/")));
	FString PreservedProject;
	TestTrue(TEXT("Stage failure preserves the original project descriptor"),
		FFileHelper::LoadFileToString(PreservedProject, *RejectedProject)
			&& PreservedProject == OriginalProject);
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
	Rule.ExcludeFunctions.Add(TEXT("RecordBlueprintDeclaredCall"));
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
			TEXT("IR keeps the direction-specific property shape"),
			Binding.Shape,
			Binding.AbiSignature == TEXT("(ii)i")
				? EAvidScriptGeneratedBindingShape::PropertyI32Get
				: EAvidScriptGeneratedBindingShape::PropertyI32Set);
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
	const FAvidScriptBindingFunctionModel* Getter = DescriptorPackage.Bindings.FindByPredicate(
		[](const FAvidScriptBindingFunctionModel& Binding)
		{
			return Binding.BindingKind == TEXT("property_get");
		});
	FString CSharpSource;
	FString CSharpErrorCategory;
	FString CSharpErrorSource;
	TestNotNull(TEXT("Generated property setter model resolves"), Setter);
	TestNotNull(TEXT("Generated property getter model resolves"), Getter);
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
	if (Getter != nullptr)
	{
		TestTrue(
			TEXT("C# generated getter returns the native int directly"),
			CSharpSource.Contains(FString::Printf(
				TEXT("return AvidScriptNative.Invoke%04d(this.Slot, this.Generation);"),
				Getter->Ordinal))
				&& CSharpSource.Contains(FString::Printf(
					TEXT("internal static extern int Invoke%04d(int selfSlot, int selfGeneration);"),
					Getter->Ordinal))
				&& !CSharpSource.Contains(TEXT("out __returnValue")));
	}
	if (Setter != nullptr)
	{
		TestTrue(
			TEXT("Generated int setter carries its real binding ordinal"),
			CSharpSource.Contains(FString::Printf(
				TEXT("[AvidScriptDataLane(\"buffered_write\", %d)]"),
				Setter->Ordinal)));
		TestTrue(
			TEXT("C# generated setter passes one direct int value"),
			CSharpSource.Contains(FString::Printf(
				TEXT("internal static extern int Invoke%04d(int selfSlot, int selfGeneration, int value);"),
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
