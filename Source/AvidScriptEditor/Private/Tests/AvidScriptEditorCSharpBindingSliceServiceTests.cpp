#if WITH_DEV_AUTOMATION_TESTS

#include "CSharpBuild/AvidScriptEditorCSharpBindingSliceService.h"

#include "AvidScriptBindingDescriptor.h"
#include "AvidScriptBindingInvocation.h"
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
	TestTrue(TEXT("Runtime slice loads invocation package"), FAvidScriptBindingPackage::LoadDescriptor(SliceJson, LoadedSlice, LoadResult));
	if (LoadedSlice.IsValid())
	{
		TestEqual(TEXT("Runtime slice creates two VM imports"), LoadedSlice->GetVmPackage().Imports.Num(), 2);
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
