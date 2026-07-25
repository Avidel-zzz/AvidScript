#if WITH_DEV_AUTOMATION_TESTS

#include "CSharpBuild/AvidScriptEditorCSharpBindingSliceService.h"

#include "AvidScriptBindingDescriptor.h"
#include "AvidScriptBindingInvocation.h"
#include "AvidScriptObjectLifecycleBinding.h"
#include "AvidScriptEditorBindingDescriptorGenerator.h"
#include "AvidScriptEditorCSharpBindingEmitter.h"
#include "AvidScriptFrontendReport.h"

#include "Dom/JsonObject.h"
#include "Engine/StaticMeshActor.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

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

FAvidScriptFrontendBindingImport MakeAvidScriptBindingSliceTestLifecycleImport(
	const FAvidScriptObjectLifecycleBindingSpec& Spec,
	const int32 Ordinal)
{
	FAvidScriptFrontendBindingImport Import;
	Import.StableId = Spec.StableId;
	Import.Ordinal = Ordinal;
	Import.Module = Spec.ModuleName;
	Import.Name = Spec.ImportName;
	Import.Signature = Spec.Signature;
	return Import;
}

bool ReadAvidScriptBindingSliceTestManifestImports(
	const FString& ManifestPath,
	TArray<FAvidScriptFrontendBindingImport>& OutImports)
{
	OutImports.Reset();
	FString ManifestJson;
	if (!FFileHelper::LoadFileToString(ManifestJson, *ManifestPath))
	{
		return false;
	}

	TSharedPtr<FJsonObject> ManifestObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ManifestJson);
	if (!FJsonSerializer::Deserialize(Reader, ManifestObject) || !ManifestObject.IsValid())
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* RequiredImports = nullptr;
	if (!ManifestObject->TryGetArrayField(TEXT("required_imports"), RequiredImports)
		|| RequiredImports == nullptr)
	{
		return false;
	}

	OutImports.Reserve(RequiredImports->Num());
	for (const TSharedPtr<FJsonValue>& ImportValue : *RequiredImports)
	{
		const TSharedPtr<FJsonObject> ImportObject = ImportValue.IsValid()
			? ImportValue->AsObject()
			: nullptr;
		FAvidScriptFrontendBindingImport Import;
		if (!ImportObject.IsValid()
			|| !ImportObject->TryGetStringField(TEXT("stable_id"), Import.StableId)
			|| !ImportObject->TryGetNumberField(TEXT("ordinal"), Import.Ordinal)
			|| !ImportObject->TryGetStringField(TEXT("module"), Import.Module)
			|| !ImportObject->TryGetStringField(TEXT("name"), Import.Name)
			|| !ImportObject->TryGetStringField(TEXT("signature"), Import.Signature))
		{
			OutImports.Reset();
			return false;
		}
		OutImports.Add(MoveTemp(Import));
	}
	return true;
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
	TArray<FAvidScriptFrontendBindingImport> ManifestImports;
	Provenance.ProfileImportCount =
		ReadAvidScriptBindingSliceTestManifestImports(
			AuthorizationPackage.ManifestPath,
			ManifestImports)
		? ManifestImports.Num()
		: 0;
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

	FAvidScriptBindingSelectionProfile AuthorizationProfile =
		FAvidScriptEditorBindingDescriptorGenerator::MakeEngineGameplayProfile();
	AuthorizationProfile.SelfClassPath = TEXT("/Script/Engine.StaticMeshActor");
	FAvidScriptCSharpBindingEmitResult AuthorizationPackage;
	if (!TestTrue(
		TEXT("Complete gameplay authorization package with a class table publishes"),
		FAvidScriptEditorCSharpBindingEmitter::PublishProfile(
			AuthorizationProfile,
			{
				{ TEXT("ProjectileClass"), TEXT("/Script/Engine.StaticMeshActor"), TEXT("/Script/Engine.Actor"), TEXT("EditorLoad") }
			},
			OutputRoot,
			AuthorizationPackage)))
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
	TestEqual(TEXT("Authorization descriptor is schema v6"), AuthorizationModel.SchemaVersion, 6);
	TestEqual(TEXT("Authorization descriptor contains one class reference"), AuthorizationModel.ClassReferences.Num(), 1);
	TestEqual(TEXT("Authorization getter has no reload effect"), GetScale->ReloadEffect, EAvidScriptBindingReloadEffect::None);
	TestEqual(TEXT("Authorization setter has actor transform effect"), SetScale->ReloadEffect, EAvidScriptBindingReloadEffect::ActorTransform);

	const TConstArrayView<FAvidScriptObjectLifecycleBindingSpec> LifecycleSpecs =
		FAvidScriptObjectLifecycleBindings::GetSpecs();
	if (!TestEqual(TEXT("Lifecycle contract publishes three stable capabilities"), LifecycleSpecs.Num(), 3))
	{
		return false;
	}
	TArray<FAvidScriptFrontendBindingImport> UsedImports = {
		MakeAvidScriptBindingSliceTestImport(*GetScale),
		MakeAvidScriptBindingSliceTestImport(*SetScale)
	};
	for (int32 SpecIndex = 0; SpecIndex < LifecycleSpecs.Num(); ++SpecIndex)
	{
		UsedImports.Add(MakeAvidScriptBindingSliceTestLifecycleImport(
			LifecycleSpecs[SpecIndex],
			AuthorizationModel.Bindings.Num() + SpecIndex));
	}
	TArray<FAvidScriptFrontendBindingImport> AuthorizationManifestImports;
	if (!TestTrue(
			TEXT("Authorization manifest imports read"),
			ReadAvidScriptBindingSliceTestManifestImports(
				AuthorizationPackage.ManifestPath,
				AuthorizationManifestImports)))
	{
		return false;
	}
	const FAvidScriptFrontendBindingImport* ObjectTypeImport =
		AuthorizationManifestImports.FindByPredicate(
			[](const FAvidScriptFrontendBindingImport& Import)
			{
				return Import.Name == TEXT("avid_object_type_is_a");
			});
	const FAvidScriptFrontendBindingImport* PackedOwnerImport =
		AuthorizationManifestImports.FindByPredicate(
			[](const FAvidScriptFrontendBindingImport& Import)
			{
				return Import.Name == TEXT("avid_owner_get_handle");
			});
	if (!TestNotNull(TEXT("Authorization manifest exposes object-type support"), ObjectTypeImport)
		|| !TestNotNull(TEXT("Authorization manifest exposes packed owner access"), PackedOwnerImport))
	{
		return false;
	}
	UsedImports.Add(*ObjectTypeImport);
	UsedImports.Add(*PackedOwnerImport);
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

	TestEqual(TEXT("Runtime slice reports every used import"), SliceResult.RequestedBindingCount, 2 + LifecycleSpecs.Num() + 2);
	TestEqual(TEXT("Runtime slice emits two bindings"), SlicePackage.BindingCount, 2);
	TestEqual(TEXT("Runtime slice preserves one class reference"), SlicePackage.ClassReferenceCount, 1);
	TestEqual(TEXT("Runtime slice keeps package capability name"), SlicePackage.PackageName, AuthorizationPackage.PackageName);
	TestNotEqual(TEXT("Runtime slice has a distinct package hash"), SlicePackage.PackageHash, AuthorizationPackage.PackageHash);

	FString SliceJson;
	FAvidScriptBindingPackageModel SliceModel;
	TestTrue(TEXT("Runtime slice descriptor reads"), FFileHelper::LoadFileToString(SliceJson, *SlicePackage.DescriptorPath));
	TestTrue(
		TEXT("Runtime slice descriptor parses"),
		FAvidScriptBindingDescriptorParser::Parse(SliceJson, SliceModel, ParseCategory, ParseSource));
	TestTrue(
		TEXT("Runtime slice declares an explicit object-type activation set"),
		SliceModel.bHasActiveObjectTypeOrdinals);
	TestEqual(
		TEXT("Fixture without reachable type tokens declares an empty activation set"),
		SliceModel.ActiveObjectTypeOrdinals.Num(),
		0);
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
	TestEqual(TEXT("Slice descriptor preserves the complete class table"), SliceModel.ClassReferences.Num(), 1);
	TestEqual(TEXT("Slice preserves the authorized Self type"), SliceModel.SelfTypeId, AuthorizationModel.SelfTypeId);
	if (SliceModel.ClassReferences.Num() == 1 && AuthorizationModel.ClassReferences.Num() == 1)
	{
		TestEqual(
			TEXT("Slice preserves class-reference result identity"),
			SliceModel.ClassReferences[0].ResultTypeId,
			AuthorizationModel.ClassReferences[0].ResultTypeId);
	}

	TMap<FString, const FAvidScriptBindingTypeModel*> AuthorizationTypesById;
	TSet<FString> SliceTypeIds;
	for (const FAvidScriptBindingTypeModel& Type : AuthorizationModel.Types)
	{
		AuthorizationTypesById.Add(Type.StableId, &Type);
	}
	for (const FAvidScriptBindingTypeModel& Type : SliceModel.Types)
	{
		SliceTypeIds.Add(Type.StableId);
	}
	TSet<FString> RequiredTypeIds;
	RequiredTypeIds.Add(AuthorizationModel.SelfTypeId);
	for (const FAvidScriptBindingClassReferenceModel& Reference : AuthorizationModel.ClassReferences)
	{
		RequiredTypeIds.Add(Reference.ResultTypeId);
	}
	for (const FAvidScriptBindingFunctionModel& Binding : SliceModel.Bindings)
	{
		RequiredTypeIds.Add(Binding.ReturnValue.TypeId);
		for (const FAvidScriptBindingValueModel& Parameter : Binding.Parameters)
		{
			RequiredTypeIds.Add(Parameter.TypeId);
		}
		if (!Binding.bStatic)
		{
			RequiredTypeIds.Add(FAvidScriptBindingDescriptorIdentity::MakeTypeStableId(
				TEXT("object:") + Binding.OwnerClass,
				{}));
		}
	}
	RequiredTypeIds.Remove(FAvidScriptBindingDescriptorIdentity::MakeTypeStableId(TEXT("void"), {}));
	TArray<FString> PendingTypeIds = RequiredTypeIds.Array();
	while (!PendingTypeIds.IsEmpty())
	{
		const FString TypeId = PendingTypeIds.Pop(EAllowShrinking::No);
		const FAvidScriptBindingTypeModel* Type = AuthorizationTypesById.FindRef(TypeId);
		if (Type != nullptr && !Type->BaseTypeId.IsEmpty() && !RequiredTypeIds.Contains(Type->BaseTypeId))
		{
			RequiredTypeIds.Add(Type->BaseTypeId);
			PendingTypeIds.Add(Type->BaseTypeId);
		}
	}
	for (const FString& TypeId : RequiredTypeIds)
	{
		TestTrue(TEXT("Slice keeps every referenced type and graph ancestor"), SliceTypeIds.Contains(TypeId));
	}
	int32 SliceObjectTypeCount = 0;
	int32 AuthorizationObjectTypeCount = 0;
	for (const FAvidScriptBindingTypeModel& Type : AuthorizationModel.Types)
	{
		AuthorizationObjectTypeCount += Type.ObjectTypeOrdinal != INDEX_NONE ? 1 : 0;
	}
	for (const FAvidScriptBindingTypeModel& Type : SliceModel.Types)
	{
		if (Type.ObjectTypeOrdinal == INDEX_NONE)
		{
			continue;
		}
		const FAvidScriptBindingTypeModel* AuthorizationType =
			AuthorizationTypesById.FindRef(Type.StableId);
		if (TestNotNull(
			TEXT("Slice object type comes from the authorization graph"),
			AuthorizationType))
		{
			TestEqual(
				TEXT("Slice object-type ordinal preserves prepared C# constants"),
				Type.ObjectTypeOrdinal,
				AuthorizationType->ObjectTypeOrdinal);
		}
		++SliceObjectTypeCount;
	}
	TestEqual(
		TEXT("Slice preserves the complete stable object-type ordinal table"),
		SliceObjectTypeCount,
		AuthorizationObjectTypeCount);
	TestTrue(
		TEXT("Slice drops unrelated authorization types instead of copying the full package"),
		SliceModel.Types.Num() < AuthorizationModel.Types.Num());
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

	TArray<FAvidScriptFrontendBindingImport> SliceManifestImports;
	if (TestTrue(
		TEXT("Runtime slice manifest imports read"),
		ReadAvidScriptBindingSliceTestManifestImports(SlicePackage.ManifestPath, SliceManifestImports)))
	{
		TestEqual(
			TEXT("Runtime slice manifest republishes reflected selections and every lifecycle capability"),
			SliceManifestImports.Num(),
			SlicePackage.BindingCount + LifecycleSpecs.Num() + 2);
		for (int32 SpecIndex = 0; SpecIndex < LifecycleSpecs.Num(); ++SpecIndex)
		{
			const FAvidScriptObjectLifecycleBindingSpec& Spec = LifecycleSpecs[SpecIndex];
			const int32 ExpectedOrdinal = SlicePackage.BindingCount + SpecIndex;
			const bool bFoundExactLifecycleImport = SliceManifestImports.ContainsByPredicate(
				[&Spec, ExpectedOrdinal](const FAvidScriptFrontendBindingImport& Import)
				{
					return Import.StableId == Spec.StableId
						&& Import.Ordinal == ExpectedOrdinal
						&& Import.Module == Spec.ModuleName
						&& Import.Name == Spec.ImportName
						&& Import.Signature == Spec.Signature;
				});
			TestTrue(
				*FString::Printf(TEXT("Runtime slice republishes lifecycle capability %d exactly"), SpecIndex),
				bFoundExactLifecycleImport);
		}
	}

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
		TestEqual(
			TEXT("Runtime slice creates reflected and lifecycle VM imports"),
			LoadedSlice->GetVmPackage().Imports.Num(),
			2 + LifecycleSpecs.Num() + 1);
		TestEqual(TEXT("Runtime slice creates one cached class plan"), LoadedSlice->GetClassReferenceCount(), 1);
		int32 ActiveObjectTypeCount = 0;
		int32 InactiveObjectTypeCount = 0;
		for (const FAvidScriptBindingTypeModel& Type : SliceModel.Types)
		{
			if (Type.ObjectTypeOrdinal == INDEX_NONE)
			{
				continue;
			}
			UClass* ResolvedClass = nullptr;
			const bool bResolved = LoadedSlice->TryResolveObjectType(
				static_cast<uint32>(Type.ObjectTypeOrdinal),
				ResolvedClass);
			const bool bRequired = RequiredTypeIds.Contains(Type.StableId);
			TestEqual(
				TEXT("Runtime activates exactly the selected object-type closure"),
				bResolved,
				bRequired);
			ActiveObjectTypeCount += bResolved ? 1 : 0;
			InactiveObjectTypeCount += bResolved ? 0 : 1;
			if (!bResolved && InactiveObjectType == nullptr)
			{
				InactiveObjectType = &Type;
			}
		}
		TestEqual(
			TEXT("Runtime slice keeps authorization ordinal capacity"),
			LoadedSlice->GetObjectTypeCount(),
			AuthorizationObjectTypeCount);
		TestTrue(
			TEXT("Runtime slice leaves unrelated authorization object types inactive"),
			InactiveObjectTypeCount > 0);
		TestEqual(
			TEXT("Runtime loads and anchors only active object-type classes"),
			LoadedSlice->GetInstrumentation().ClassLoadCount,
			ActiveObjectTypeCount);
		TestEqual(
			TEXT("Runtime slice retains custom Self class identity"),
			LoadedSlice->GetExpectedSelfClass(),
			AStaticMeshActor::StaticClass());
	}
	if (TestNotNull(
		TEXT("Authorization graph retains an inactive object type for provenance coverage"),
		InactiveObjectType))
	{
		FAvidScriptFrontendBindingPackage TypedProvenance = Provenance;
		TypedProvenance.UsedObjectTypeCount = 1;
		TypedProvenance.UsedObjectTypeOrdinals = {
			InactiveObjectType->ObjectTypeOrdinal
		};
		FAvidScriptCSharpBindingEmitResult TypedSlicePackage;
		FAvidScriptEditorCSharpBindingSliceResult TypedSliceResult;
		if (TestTrue(
			TEXT("Object-type provenance runtime slice publishes"),
			FAvidScriptEditorCSharpBindingSliceService::Publish(
				AuthorizationPackage.DescriptorPath,
				TypedProvenance,
				OutputRoot,
				TypedSlicePackage,
				TypedSliceResult)))
		{
			FString TypedSliceJson;
			FAvidScriptBindingPackageModel TypedSliceModel;
			TestTrue(
				TEXT("Object-type provenance descriptor reads"),
				FFileHelper::LoadFileToString(
					TypedSliceJson,
					*TypedSlicePackage.DescriptorPath));
			TestTrue(
				TEXT("Object-type provenance descriptor parses"),
				FAvidScriptBindingDescriptorParser::Parse(
					TypedSliceJson,
					TypedSliceModel,
					ParseCategory,
					ParseSource));
			TestTrue(
				TEXT("Runtime slice preserves the reachable Guest type ordinal"),
				TypedSliceModel.ActiveObjectTypeOrdinals
					== TypedProvenance.UsedObjectTypeOrdinals);

			TSharedPtr<const FAvidScriptBindingPackage> TypedLoadedSlice;
			FAvidScriptBindingPackageLoadResult TypedLoadResult;
			if (TestTrue(
				TEXT("Object-type provenance runtime slice loads"),
				FAvidScriptBindingPackage::LoadDescriptor(
					TypedSliceJson,
					TypedLoadedSlice,
					TypedLoadResult))
				&& TypedLoadedSlice.IsValid())
			{
				UClass* ResolvedClass = nullptr;
				TestTrue(
					TEXT("Reachable Guest object-type ordinal activates its immutable class plan"),
					TypedLoadedSlice->TryResolveObjectType(
						static_cast<uint32>(InactiveObjectType->ObjectTypeOrdinal),
						ResolvedClass));
			}
		}
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
	TestEqual(TEXT("Property authorization uses schema v6"), PropertyAuthorizationModel.SchemaVersion, 6);
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
	TestEqual(TEXT("Property runtime slice preserves schema v6"), PropertySliceModel.SchemaVersion, 6);
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
		TestEqual(TEXT("Property runtime slice creates its reflected and object-type imports"), LoadedPropertySlice->GetVmPackage().Imports.Num(), 2);
	}

	const FAvidScriptFrontendBindingPackage UnauthorizedLifecycleProvenance =
		MakeAvidScriptBindingSliceTestProvenance(
			PropertyAuthorizationPackage,
			{
				MakeAvidScriptBindingSliceTestLifecycleImport(
					LifecycleSpecs[0],
					PropertyAuthorizationModel.Bindings.Num())
			});
	TestFalse(
		TEXT("Package without class references cannot claim lifecycle imports"),
		FAvidScriptEditorCSharpBindingSliceService::Publish(
			PropertyAuthorizationPackage.DescriptorPath,
			UnauthorizedLifecycleProvenance,
			OutputRoot,
			PropertySlicePackage,
			PropertySliceResult));
	TestEqual(
		TEXT("Unauthorized lifecycle import category"),
		PropertySliceResult.ErrorCategory,
		FString(TEXT("slice_import_identity_mismatch")));

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

	FAvidScriptFrontendBindingImport ForgedLifecycleOrdinal = UsedImports[2];
	ForgedLifecycleOrdinal.Ordinal = MIN_int32;
	FAvidScriptFrontendBindingPackage ForgedLifecycleOrdinalProvenance =
		MakeAvidScriptBindingSliceTestProvenance(AuthorizationPackage, { ForgedLifecycleOrdinal });
	TestFalse(
		TEXT("Forged lifecycle ordinal fails closed"),
		FAvidScriptEditorCSharpBindingSliceService::Publish(
			AuthorizationPackage.DescriptorPath,
			ForgedLifecycleOrdinalProvenance,
			OutputRoot,
			SlicePackage,
			SliceResult));
	TestEqual(
		TEXT("Forged lifecycle ordinal category"),
		SliceResult.ErrorCategory,
		FString(TEXT("slice_binding_missing")));

	FAvidScriptFrontendBindingImport ForgedLifecycleSignature = UsedImports[2];
	ForgedLifecycleSignature.Signature += TEXT("_forged");
	FAvidScriptFrontendBindingPackage ForgedLifecycleSignatureProvenance =
		MakeAvidScriptBindingSliceTestProvenance(AuthorizationPackage, { ForgedLifecycleSignature });
	TestFalse(
		TEXT("Forged lifecycle signature fails closed"),
		FAvidScriptEditorCSharpBindingSliceService::Publish(
			AuthorizationPackage.DescriptorPath,
			ForgedLifecycleSignatureProvenance,
			OutputRoot,
			SlicePackage,
			SliceResult));
	TestEqual(
		TEXT("Forged lifecycle signature category"),
		SliceResult.ErrorCategory,
		FString(TEXT("slice_import_identity_mismatch")));

	FAvidScriptFrontendBindingImport ForgedObjectTypeSignature = *ObjectTypeImport;
	ForgedObjectTypeSignature.Signature += TEXT("_forged");
	FAvidScriptFrontendBindingPackage ForgedObjectTypeProvenance =
		MakeAvidScriptBindingSliceTestProvenance(
			AuthorizationPackage,
			{ ForgedObjectTypeSignature });
	TestFalse(
		TEXT("Forged object-type signature fails closed"),
		FAvidScriptEditorCSharpBindingSliceService::Publish(
			AuthorizationPackage.DescriptorPath,
			ForgedObjectTypeProvenance,
			OutputRoot,
			SlicePackage,
			SliceResult));
	TestEqual(
		TEXT("Forged object-type signature category"),
		SliceResult.ErrorCategory,
		FString(TEXT("slice_import_identity_mismatch")));

	FAvidScriptFrontendBindingImport ForgedOwnerSignature = *PackedOwnerImport;
	ForgedOwnerSignature.Signature += TEXT("_forged");
	FAvidScriptFrontendBindingPackage ForgedOwnerProvenance =
		MakeAvidScriptBindingSliceTestProvenance(
			AuthorizationPackage,
			{ ForgedOwnerSignature });
	TestFalse(
		TEXT("Forged packed owner signature fails closed"),
		FAvidScriptEditorCSharpBindingSliceService::Publish(
			AuthorizationPackage.DescriptorPath,
			ForgedOwnerProvenance,
			OutputRoot,
			SlicePackage,
			SliceResult));
	TestEqual(
		TEXT("Forged packed owner signature category"),
		SliceResult.ErrorCategory,
		FString(TEXT("slice_import_identity_mismatch")));

	FAvidScriptFrontendBindingPackage ProfileCountMismatch = Provenance;
	ProfileCountMismatch.ProfileImportCount = AuthorizationPackage.BindingCount;
	TestFalse(
		TEXT("Class-reference package without lifecycle profile count fails closed"),
		FAvidScriptEditorCSharpBindingSliceService::Publish(
			AuthorizationPackage.DescriptorPath,
			ProfileCountMismatch,
			OutputRoot,
			SlicePackage,
			SliceResult));
	TestEqual(
		TEXT("Lifecycle profile count mismatch category"),
		SliceResult.ErrorCategory,
		FString(TEXT("slice_package_identity_mismatch")));

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpBindingSliceServiceObjectFactoryTest,
	"AvidScript.Editor.CSharpBindingSlice.ObjectFactory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpBindingSliceServiceObjectFactoryTest::RunTest(
	const FString& Parameters)
{
	const FString OutputRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptGeneratedBindingsTests"),
		TEXT("P51_1_FactorySlices")));
	FAvidScriptBindingSelectionProfile Profile;
	Profile.PackageName = TEXT("avidscript.project.factory_slice");
	Profile.SelfClassPath = TEXT("/Script/Engine.Actor");
	const TArray<FAvidScriptProjectBindingClassSpec> ClassReferences = {
		{
			TEXT("InventoryStateClass"),
			TEXT("/Script/AvidScriptEditor.AvidScriptCSharpBindingEmitterTestObject"),
			TEXT("/Script/CoreUObject.Object"),
			TEXT("EditorLoad")
		},
		{
			TEXT("SceneComponentClass"),
			TEXT("/Script/Engine.SceneComponent"),
			TEXT("/Script/Engine.ActorComponent"),
			TEXT("EditorLoad")
		}
	};
	const TArray<FAvidScriptProjectObjectFactorySpec> ObjectFactories = {
		{
			TEXT("InventoryState"),
			TEXT("InventoryStateClass"),
			TEXT("/Script/CoreUObject.Object"),
			EAvidScriptProjectObjectFactoryKind::NewObject,
			EAvidScriptProjectObjectOwnership::Session,
			EAvidScriptProjectComponentRegistration::None
		},
		{
			TEXT("SceneComponent"),
			TEXT("SceneComponentClass"),
			TEXT("/Script/Engine.Actor"),
			EAvidScriptProjectObjectFactoryKind::ActorComponent,
			EAvidScriptProjectObjectOwnership::Session,
			EAvidScriptProjectComponentRegistration::RegisterInstance
		}
	};

	FAvidScriptCSharpBindingEmitResult AuthorizationPackage;
	if (!TestTrue(
		TEXT("Factory authorization package publishes"),
		FAvidScriptEditorCSharpBindingEmitter::PublishProfile(
			Profile,
			ClassReferences,
			ObjectFactories,
			OutputRoot,
			AuthorizationPackage)))
	{
		AddError(AuthorizationPackage.ErrorMessage);
		return false;
	}
	TArray<FAvidScriptFrontendBindingImport> ManifestImports;
	if (!TestTrue(
		TEXT("Factory authorization manifest imports read"),
		ReadAvidScriptBindingSliceTestManifestImports(
			AuthorizationPackage.ManifestPath,
			ManifestImports)))
	{
		return false;
	}
	TestEqual(
		TEXT("Factory authorization has type, factory, attachment, and owner capabilities"),
		ManifestImports.Num(),
		7);

	const FAvidScriptFrontendBindingPackage Provenance =
		MakeAvidScriptBindingSliceTestProvenance(
			AuthorizationPackage,
			ManifestImports);
	FAvidScriptCSharpBindingEmitResult SlicePackage;
	FAvidScriptEditorCSharpBindingSliceResult SliceResult;
	if (!TestTrue(
		TEXT("Factory runtime slice publishes"),
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

	FString SliceJson;
	FAvidScriptBindingPackageModel SliceModel;
	FString ParseCategory;
	FString ParseSource;
	if (!TestTrue(
		TEXT("Factory runtime slice reads"),
		FFileHelper::LoadFileToString(
			SliceJson,
			*SlicePackage.DescriptorPath))
		|| !TestTrue(
			TEXT("Factory runtime slice parses"),
			FAvidScriptBindingDescriptorParser::Parse(
				SliceJson,
				SliceModel,
				ParseCategory,
				ParseSource)))
	{
		return false;
	}
	TestEqual(
		TEXT("Factory runtime slice preserves descriptor v7"),
		SliceModel.SchemaVersion,
		7);
	TestEqual(
		TEXT("Factory runtime slice preserves both factories"),
		SliceModel.ObjectFactories.Num(),
		2);
	TestEqual(
		TEXT("Factory runtime slice preserves both class references"),
		SliceModel.ClassReferences.Num(),
		2);

	TSharedPtr<const FAvidScriptBindingPackage> LoadedSlice;
	const FAvidScriptBindingTypeModel* InactiveObjectType = nullptr;
	FAvidScriptBindingPackageLoadResult LoadResult;
	if (!TestTrue(
		TEXT("Factory runtime slice loads immutable plans"),
		FAvidScriptBindingPackage::LoadDescriptor(
			SliceJson,
			LoadedSlice,
			LoadResult))
		|| !TestNotNull(
			TEXT("Factory runtime package exists"),
			LoadedSlice.Get()))
	{
		AddError(LoadResult.ErrorCategory + TEXT(": ")
			+ LoadResult.ErrorDetails);
		return false;
	}
	TestEqual(
		TEXT("Factory runtime package exposes two immutable factory plans"),
		LoadedSlice->GetObjectFactoryCount(),
		2);
	TestEqual(
		TEXT("Factory runtime package publishes type, factory, and attachment imports"),
		LoadedSlice->GetVmPackage().Imports.Num(),
		6);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
