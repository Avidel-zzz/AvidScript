#if WITH_DEV_AUTOMATION_TESTS

#include "CSharpBuild/AvidScriptEditorCSharpBindingSliceService.h"

#include "AvidScriptBindingDescriptor.h"
#include "AvidScriptBindingInvocation.h"
#include "AvidScriptEditorBindingDescriptorGenerator.h"
#include "AvidScriptEditorCSharpBindingEmitter.h"
#include "AvidScriptFrontendReport.h"

#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
FAvidScriptFrontendBindingImport MakeAvidScriptBindingSliceTestImport(
	const FAvidScriptBindingFunctionModel& Binding)
{
	FAvidScriptFrontendBindingImport Import;
	Import.StableId = Binding.StableId;
	Import.Ordinal = Binding.Ordinal;
	Import.Module = Binding.HostImport.Module;
	Import.Name = Binding.HostImport.Name;
	Import.Signature = Binding.HostImport.Signature;
	return Import;
}

FAvidScriptFrontendBindingPackage MakeAvidScriptBindingSliceTestProvenance(
	const FAvidScriptCSharpBindingEmitResult& AuthorizationPackage,
	const TArray<FAvidScriptFrontendBindingImport>& UsedImports)
{
	FAvidScriptFrontendBindingPackage Provenance;
	Provenance.bPresent = true;
	Provenance.bRequired = true;
	Provenance.PackageManifest = AuthorizationPackage.ManifestPath;
	Provenance.PackageName = AuthorizationPackage.PackageName;
	Provenance.PackageHash = AuthorizationPackage.PackageHash;
	Provenance.DescriptorFile = AuthorizationPackage.DescriptorPath;
	Provenance.DescriptorSha256 = AuthorizationPackage.DescriptorHash;
	Provenance.ReferenceSourceFile = AuthorizationPackage.ReferenceSourcePath;
	Provenance.ReferenceSourceSha256 = AuthorizationPackage.SourceHash;
	Provenance.ProfileImportCount = AuthorizationPackage.BindingCount;
	Provenance.UsedImportCount = UsedImports.Num();
	Provenance.UsedImports = UsedImports;
	return Provenance;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpBindingSliceServiceContractsTest,
	"AvidScript.Editor.CSharpBindingSlice.Contracts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpBindingSliceServiceContractsTest::RunTest(const FString& Parameters)
{
	const FString OutputRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptGeneratedBindingsTests"),
		TEXT("P43_3B_Slices")));

	FAvidScriptCSharpBindingEmitResult AuthorizationPackage;
	if (!TestTrue(
		TEXT("Complete gameplay authorization package publishes"),
		FAvidScriptEditorCSharpBindingEmitter::PublishEngineGameplay(OutputRoot, AuthorizationPackage)))
	{
		AddError(AuthorizationPackage.ErrorMessage);
		return false;
	}

	FString AuthorizationJson;
	FAvidScriptBindingPackageModel AuthorizationModel;
	FString ParseCategory;
	FString ParseSource;
	if (!TestTrue(
		TEXT("Complete gameplay descriptor reads"),
		FFileHelper::LoadFileToString(AuthorizationJson, *AuthorizationPackage.DescriptorPath))
		|| !TestTrue(
			TEXT("Complete gameplay descriptor parses"),
			FAvidScriptBindingDescriptorParser::Parse(
				AuthorizationJson,
				AuthorizationModel,
				ParseCategory,
				ParseSource)))
	{
		return false;
	}

	const FAvidScriptBindingFunctionModel* GetScale = AuthorizationModel.Bindings.FindByPredicate(
		[](const FAvidScriptBindingFunctionModel& Binding)
		{
			return Binding.UeFunction == TEXT("GetActorScale3D");
		});
	const FAvidScriptBindingFunctionModel* SetScale = AuthorizationModel.Bindings.FindByPredicate(
		[](const FAvidScriptBindingFunctionModel& Binding)
		{
			return Binding.UeFunction == TEXT("SetActorScale3D");
		});
	if (!TestNotNull(TEXT("Gameplay descriptor contains scale getter"), GetScale)
		|| !TestNotNull(TEXT("Gameplay descriptor contains scale setter"), SetScale))
	{
		return false;
	}
	TestEqual(TEXT("Authorization descriptor is schema v4"), AuthorizationModel.SchemaVersion, 4);
	TestEqual(TEXT("Authorization getter has no reload effect"), GetScale->ReloadEffect, EAvidScriptBindingReloadEffect::None);
	TestEqual(TEXT("Authorization setter has actor transform effect"), SetScale->ReloadEffect, EAvidScriptBindingReloadEffect::ActorTransform);

	const TArray<FAvidScriptFrontendBindingImport> UsedImports = {
		MakeAvidScriptBindingSliceTestImport(*GetScale),
		MakeAvidScriptBindingSliceTestImport(*SetScale)
	};
	const FAvidScriptFrontendBindingPackage Provenance =
		MakeAvidScriptBindingSliceTestProvenance(AuthorizationPackage, UsedImports);
	FAvidScriptCSharpBindingEmitResult SlicePackage;
	FAvidScriptEditorCSharpBindingSliceResult SliceResult;
	if (!TestTrue(
		TEXT("Two-binding runtime slice publishes"),
		FAvidScriptEditorCSharpBindingSliceService::Publish(
			AuthorizationPackage.DescriptorPath,
			Provenance,
			OutputRoot,
			SlicePackage,
			SliceResult)))
	{
		AddError(SliceResult.ErrorMessage);
		return false;
	}

	TestEqual(TEXT("Runtime slice reports two requested bindings"), SliceResult.RequestedBindingCount, 2);
	TestEqual(TEXT("Runtime slice emits two bindings"), SlicePackage.BindingCount, 2);
	TestEqual(TEXT("Runtime slice keeps package capability name"), SlicePackage.PackageName, AuthorizationPackage.PackageName);
	TestNotEqual(TEXT("Runtime slice has a distinct package hash"), SlicePackage.PackageHash, AuthorizationPackage.PackageHash);

	FString SliceJson;
	FAvidScriptBindingPackageModel SliceModel;
	TestTrue(TEXT("Runtime slice descriptor reads"), FFileHelper::LoadFileToString(SliceJson, *SlicePackage.DescriptorPath));
	TestTrue(
		TEXT("Runtime slice descriptor parses"),
		FAvidScriptBindingDescriptorParser::Parse(SliceJson, SliceModel, ParseCategory, ParseSource));
	TSet<FString> ExpectedStableIds = { GetScale->StableId, SetScale->StableId };
	TSet<FString> ActualStableIds;
	for (const FAvidScriptBindingFunctionModel& Binding : SliceModel.Bindings)
	{
		ActualStableIds.Add(Binding.StableId);
		if (Binding.UeFunction == TEXT("GetActorScale3D"))
		{
			TestEqual(TEXT("Slice preserves getter reload effect"), Binding.ReloadEffect, EAvidScriptBindingReloadEffect::None);
		}
		else if (Binding.UeFunction == TEXT("SetActorScale3D"))
		{
			TestEqual(TEXT("Slice preserves setter reload effect"), Binding.ReloadEffect, EAvidScriptBindingReloadEffect::ActorTransform);
		}
	}
	bool bStableIdsMatch = ActualStableIds.Num() == ExpectedStableIds.Num();
	for (const FString& StableId : ExpectedStableIds)
	{
		if (!ActualStableIds.Contains(StableId))
		{
			bStableIdsMatch = false;
			break;
		}
	}
	TestTrue(TEXT("Runtime slice stable IDs exactly match request"), bStableIdsMatch);

	TSharedPtr<const FAvidScriptBindingPackage> LoadedSlice;
	FAvidScriptBindingPackageLoadResult LoadResult;
	const bool bSliceLoaded = FAvidScriptBindingPackage::LoadDescriptor(SliceJson, LoadedSlice, LoadResult);
	TestTrue(TEXT("Runtime slice loads invocation package"), bSliceLoaded);
	if (!bSliceLoaded)
	{
		AddError(FString::Printf(
			TEXT("Slice load failed | category=%s | source=%s | details=%s"),
			*LoadResult.ErrorCategory,
			*LoadResult.ErrorSource,
			*LoadResult.ErrorDetails));
	}
	if (LoadedSlice.IsValid())
	{
		TestEqual(TEXT("Runtime slice creates two VM imports"), LoadedSlice->GetVmPackage().Imports.Num(), 2);
	}

	const TArray<FAvidScriptReflectedPropertySelection> PropertySelections = {
		{ TEXT("/Script/Engine.Actor"), TEXT("CustomTimeDilation") }
	};
	FString PropertyAuthorizationJson;
	FAvidScriptBindingDescriptorGenerateResult PropertyDescriptorResult;
	if (!TestTrue(
		TEXT("Property authorization descriptor generates"),
		FAvidScriptEditorBindingDescriptorGenerator::GenerateWithReadableProperties(
			TEXT("avidscript.engine.property_slice"),
			{},
			PropertySelections,
			PropertyAuthorizationJson,
			PropertyDescriptorResult)))
	{
		return false;
	}

	FAvidScriptCSharpBindingEmitResult PropertyAuthorizationPackage;
	if (!TestTrue(
		TEXT("Property authorization package publishes"),
		FAvidScriptEditorCSharpBindingEmitter::PublishDescriptor(
			PropertyAuthorizationJson,
			OutputRoot,
			PropertyAuthorizationPackage)))
	{
		AddError(PropertyAuthorizationPackage.ErrorMessage);
		return false;
	}

	FAvidScriptBindingPackageModel PropertyAuthorizationModel;
	if (!TestTrue(
		TEXT("Property authorization descriptor parses"),
		FAvidScriptBindingDescriptorParser::Parse(
			PropertyAuthorizationJson,
			PropertyAuthorizationModel,
			ParseCategory,
			ParseSource)))
	{
		return false;
	}
	TestEqual(TEXT("Property authorization uses schema v4"), PropertyAuthorizationModel.SchemaVersion, 4);
	TestEqual(TEXT("Property authorization contains one binding"), PropertyAuthorizationModel.Bindings.Num(), 1);
	if (PropertyAuthorizationModel.Bindings.Num() != 1)
	{
		return false;
	}
	const FAvidScriptBindingFunctionModel& PropertyBinding = PropertyAuthorizationModel.Bindings[0];
	TestEqual(TEXT("Authorization binding remains a property getter"), PropertyBinding.BindingKind, FString(TEXT("property_get")));

	const FAvidScriptFrontendBindingPackage PropertyProvenance = MakeAvidScriptBindingSliceTestProvenance(
		PropertyAuthorizationPackage,
		{ MakeAvidScriptBindingSliceTestImport(PropertyBinding) });
	FAvidScriptCSharpBindingEmitResult PropertySlicePackage;
	FAvidScriptEditorCSharpBindingSliceResult PropertySliceResult;
	if (!TestTrue(
		TEXT("Used property import publishes a minimal runtime slice"),
		FAvidScriptEditorCSharpBindingSliceService::Publish(
			PropertyAuthorizationPackage.DescriptorPath,
			PropertyProvenance,
			OutputRoot,
			PropertySlicePackage,
			PropertySliceResult)))
	{
		AddError(PropertySliceResult.ErrorMessage);
		return false;
	}
	TestEqual(TEXT("Property runtime slice contains one binding"), PropertySlicePackage.BindingCount, 1);

	FString PropertySliceJson;
	FAvidScriptBindingPackageModel PropertySliceModel;
	if (!TestTrue(
		TEXT("Property runtime slice descriptor reads"),
		FFileHelper::LoadFileToString(PropertySliceJson, *PropertySlicePackage.DescriptorPath))
		|| !TestTrue(
			TEXT("Property runtime slice descriptor parses"),
			FAvidScriptBindingDescriptorParser::Parse(
				PropertySliceJson,
				PropertySliceModel,
				ParseCategory,
				ParseSource)))
	{
		return false;
	}
	TestEqual(TEXT("Property runtime slice preserves schema v4"), PropertySliceModel.SchemaVersion, 4);
	TestEqual(TEXT("Property runtime slice preserves one binding"), PropertySliceModel.Bindings.Num(), 1);
	if (PropertySliceModel.Bindings.Num() == 1)
	{
		TestEqual(TEXT("Property runtime slice preserves member kind"), PropertySliceModel.Bindings[0].BindingKind, FString(TEXT("property_get")));
		TestEqual(TEXT("Property runtime slice preserves stable id"), PropertySliceModel.Bindings[0].StableId, PropertyBinding.StableId);
	}
	TSharedPtr<const FAvidScriptBindingPackage> LoadedPropertySlice;
	TestTrue(
		TEXT("Runtime loads the minimal property slice"),
		FAvidScriptBindingPackage::LoadDescriptor(PropertySliceJson, LoadedPropertySlice, LoadResult));
	if (LoadedPropertySlice.IsValid())
	{
		TestEqual(TEXT("Property runtime slice creates one VM import"), LoadedPropertySlice->GetVmPackage().Imports.Num(), 1);
	}

	FAvidScriptFrontendBindingPackage DuplicateProvenance =
		MakeAvidScriptBindingSliceTestProvenance(AuthorizationPackage, { UsedImports[0], UsedImports[0] });
	TestFalse(
		TEXT("Duplicate stable IDs fail closed"),
		FAvidScriptEditorCSharpBindingSliceService::Publish(
			AuthorizationPackage.DescriptorPath,
			DuplicateProvenance,
			OutputRoot,
			SlicePackage,
			SliceResult));
	TestEqual(TEXT("Duplicate stable ID category"), SliceResult.ErrorCategory, FString(TEXT("slice_duplicate_stable_id")));

	FAvidScriptFrontendBindingImport InvalidImport = UsedImports[0];
	InvalidImport.StableId = TEXT("not-a-stable-id");
	FAvidScriptFrontendBindingPackage InvalidProvenance =
		MakeAvidScriptBindingSliceTestProvenance(AuthorizationPackage, { InvalidImport });
	TestFalse(
		TEXT("Invalid stable ID fails closed"),
		FAvidScriptEditorCSharpBindingSliceService::Publish(
			AuthorizationPackage.DescriptorPath,
			InvalidProvenance,
			OutputRoot,
			SlicePackage,
			SliceResult));
	TestEqual(TEXT("Invalid stable ID category"), SliceResult.ErrorCategory, FString(TEXT("slice_stable_id_invalid")));

	FAvidScriptFrontendBindingImport MissingImport = UsedImports[0];
	MissingImport.StableId = FString::ChrN(64, TEXT('f'));
	MissingImport.Name = TEXT("avid_ue_ffffffffffffffff");
	FAvidScriptFrontendBindingPackage MissingProvenance =
		MakeAvidScriptBindingSliceTestProvenance(AuthorizationPackage, { MissingImport });
	TestFalse(
		TEXT("Unknown stable ID fails closed"),
		FAvidScriptEditorCSharpBindingSliceService::Publish(
			AuthorizationPackage.DescriptorPath,
			MissingProvenance,
			OutputRoot,
			SlicePackage,
			SliceResult));
	TestEqual(TEXT("Unknown stable ID category"), SliceResult.ErrorCategory, FString(TEXT("slice_binding_missing")));

	FAvidScriptFrontendBindingPackage CountMismatch = Provenance;
	CountMismatch.UsedImportCount = 3;
	TestFalse(
		TEXT("Used import count mismatch fails closed"),
		FAvidScriptEditorCSharpBindingSliceService::Publish(
			AuthorizationPackage.DescriptorPath,
			CountMismatch,
			OutputRoot,
			SlicePackage,
			SliceResult));
	TestEqual(TEXT("Count mismatch category"), SliceResult.ErrorCategory, FString(TEXT("slice_provenance_invalid")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
