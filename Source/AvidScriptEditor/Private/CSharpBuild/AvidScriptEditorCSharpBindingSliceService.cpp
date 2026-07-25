#include "CSharpBuild/AvidScriptEditorCSharpBindingSliceService.h"

#include "AvidScriptBindingDescriptor.h"
#include "AvidScriptObjectFactoryBinding.h"
#include "AvidScriptObjectLifecycleBinding.h"
#include "AvidScriptObjectTypeBinding.h"
#include "AvidScriptSceneAttachmentBinding.h"
#include "AvidScriptEditorBindingDescriptorGenerator.h"
#include "AvidScriptEditorCSharpBindingEmitter.h"
#include "BindingGeneration/AvidScriptEditorCSharpBindingRenderer.h"
#include "AvidScriptFrontendReport.h"
#include "AvidScriptHash.h"

#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
bool HasAvidScriptCSharpBindingSliceSceneComponentFactory(
	const FAvidScriptBindingPackageModel& Package)
{
	return Package.ObjectFactories.ContainsByPredicate(
		[&Package](const FAvidScriptBindingObjectFactoryModel& Factory)
		{
			const FAvidScriptBindingClassReferenceModel* Reference =
				Package.ClassReferences.FindByPredicate(
					[&Factory](
						const FAvidScriptBindingClassReferenceModel& Candidate)
					{
						return Candidate.StableId
							== Factory.ClassReferenceId;
					});
			const FAvidScriptBindingTypeModel* ConcreteType =
				Reference == nullptr
					? nullptr
					: Package.Types.FindByPredicate(
						[Reference](
							const FAvidScriptBindingTypeModel& Type)
						{
							return Type.ClassPath == Reference->ClassPath;
						});
			return ConcreteType != nullptr
				&& FAvidScriptBindingDescriptorTypeGraph::IsDerivedFromClassPath(
					Package,
					ConcreteType->StableId,
					TEXT("/Script/Engine.SceneComponent"));
		});
}

void SetAvidScriptCSharpBindingSliceFailure(
	FAvidScriptEditorCSharpBindingSliceResult& OutResult,
	const FString& ErrorCategory,
	const FString& ErrorSource,
	const FString& NextAction)
{
	OutResult.bSucceeded = false;
	OutResult.ErrorCategory = ErrorCategory;
	OutResult.ErrorSource = ErrorSource;
	OutResult.NextAction = NextAction;
	OutResult.ErrorMessage = FString::Printf(
		TEXT("%s: %s"),
		*ErrorCategory,
		*ErrorSource);
}

bool IsAvidScriptCSharpBindingSliceStableId(const FString& Value)
{
	if (Value.Len() != 64)
	{
		return false;
	}
	for (const TCHAR Character : Value)
	{
		if (!FChar::IsDigit(Character) && (Character < TEXT('a') || Character > TEXT('f')))
		{
			return false;
		}
	}
	return true;
}

FString NormalizeAvidScriptCSharpBindingSlicePath(FString Path)
{
	if (!FPaths::IsRelative(Path))
	{
		Path = FPaths::ConvertRelativePathToFull(Path);
	}
	else
	{
		Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), Path));
	}
	FPaths::NormalizeFilename(Path);
	return Path;
}

bool BuildAvidScriptCSharpBindingSliceTypeClosure(
	const FAvidScriptBindingPackageModel& AuthorizationModel,
	FAvidScriptBindingPackageModel& SliceModel,
	FString& OutErrorSource)
{
	TMap<FString, const FAvidScriptBindingTypeModel*> AuthorizationTypesById;
	for (const FAvidScriptBindingTypeModel& Type : AuthorizationModel.Types)
	{
		AuthorizationTypesById.Add(Type.StableId, &Type);
	}

	SliceModel.SelfTypeId = AuthorizationModel.SelfTypeId;
	TSet<FString> RequiredTypeIds;
	// Prepared C# constants use authorization ordinals. Keep the complete object
	// table so a runtime slice cannot reinterpret an already-lowered ordinal.
	for (const FAvidScriptBindingTypeModel& Type : AuthorizationModel.Types)
	{
		if (Type.ObjectTypeOrdinal != INDEX_NONE)
		{
			RequiredTypeIds.Add(Type.StableId);
		}
	}
	if (!SliceModel.SelfTypeId.IsEmpty())
	{
		RequiredTypeIds.Add(SliceModel.SelfTypeId);
	}
	for (FAvidScriptBindingClassReferenceModel& Reference : SliceModel.ClassReferences)
	{
		const FAvidScriptBindingClassReferenceModel* AuthorizationReference =
			AuthorizationModel.ClassReferences.FindByPredicate(
				[&Reference](const FAvidScriptBindingClassReferenceModel& Candidate)
				{
					return Candidate.StableId == Reference.StableId;
				});
		if (AuthorizationReference == nullptr)
		{
			OutErrorSource = Reference.StableId;
			return false;
		}
		Reference.ResultTypeId = AuthorizationReference->ResultTypeId;
		RequiredTypeIds.Add(Reference.ResultTypeId);
	}
	for (const FAvidScriptBindingObjectFactoryModel& Factory :
		SliceModel.ObjectFactories)
	{
		const FAvidScriptBindingObjectFactoryModel* AuthorizationFactory =
			AuthorizationModel.ObjectFactories.FindByPredicate(
				[&Factory](
					const FAvidScriptBindingObjectFactoryModel& Candidate)
				{
					return Candidate.StableId == Factory.StableId;
				});
		const FAvidScriptBindingClassReferenceModel* AuthorizationReference =
			AuthorizationModel.ClassReferences.FindByPredicate(
				[&Factory](
					const FAvidScriptBindingClassReferenceModel& Candidate)
				{
					return Candidate.StableId
						== Factory.ClassReferenceId;
				});
		const FAvidScriptBindingTypeModel* ConcreteType =
			AuthorizationReference == nullptr
				? nullptr
				: AuthorizationModel.Types.FindByPredicate(
					[AuthorizationReference](
						const FAvidScriptBindingTypeModel& Type)
					{
						return Type.ClassPath
							== AuthorizationReference->ClassPath;
					});
		if (AuthorizationFactory == nullptr
			|| AuthorizationReference == nullptr
			|| ConcreteType == nullptr
			|| Factory.Ordinal != AuthorizationFactory->Ordinal
			|| Factory.ScriptName != AuthorizationFactory->ScriptName
			|| Factory.ClassReferenceId
				!= AuthorizationFactory->ClassReferenceId
			|| Factory.OuterTypeId != AuthorizationFactory->OuterTypeId
			|| Factory.Kind != AuthorizationFactory->Kind
			|| Factory.Ownership != AuthorizationFactory->Ownership
			|| Factory.Registration != AuthorizationFactory->Registration)
		{
			OutErrorSource = Factory.StableId;
			return false;
		}
		RequiredTypeIds.Add(Factory.OuterTypeId);
		RequiredTypeIds.Add(ConcreteType->StableId);
	}

	for (const FAvidScriptBindingFunctionModel& Binding : SliceModel.Bindings)
	{
		RequiredTypeIds.Add(FAvidScriptBindingDescriptorIdentity::MakeTypeStableId(
			TEXT("object:") + Binding.OwnerClass,
			{}));
		if (Binding.ReturnValue.CanonicalType != TEXT("void"))
		{
			RequiredTypeIds.Add(Binding.ReturnValue.TypeId);
		}
		for (const FAvidScriptBindingValueModel& Parameter : Binding.Parameters)
		{
			RequiredTypeIds.Add(Parameter.TypeId);
		}
	}

	TArray<FString> PendingTypeIds = RequiredTypeIds.Array();
	while (!PendingTypeIds.IsEmpty())
	{
		const FString TypeId = PendingTypeIds.Pop(EAllowShrinking::No);
		const FAvidScriptBindingTypeModel* Type = AuthorizationTypesById.FindRef(TypeId);
		if (Type == nullptr)
		{
			OutErrorSource = TypeId;
			return false;
		}
		if (!Type->BaseTypeId.IsEmpty() && !RequiredTypeIds.Contains(Type->BaseTypeId))
		{
			RequiredTypeIds.Add(Type->BaseTypeId);
			PendingTypeIds.Add(Type->BaseTypeId);
		}
	}

	SliceModel.Types.Reset();
	for (const FAvidScriptBindingTypeModel& Type : AuthorizationModel.Types)
	{
		if (RequiredTypeIds.Contains(Type.StableId))
		{
			SliceModel.Types.Add(Type);
		}
	}
	SliceModel.Types.Sort([](
		const FAvidScriptBindingTypeModel& Left,
		const FAvidScriptBindingTypeModel& Right)
	{
		return Left.CanonicalType.Compare(Right.CanonicalType, ESearchCase::CaseSensitive) < 0;
	});
	SliceModel.SelectionHash = FAvidScriptBindingDescriptorIdentity::MakeSelectionHash(SliceModel);
	SliceModel.PackageHash = FAvidScriptBindingDescriptorIdentity::MakePackageHash(SliceModel);
	return true;
}

bool RewriteAvidScriptCSharpBindingSliceDescriptor(
	const FAvidScriptBindingPackageModel& SliceModel,
	FString& InOutJson)
{
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(InOutJson);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return false;
	}

	Root->SetStringField(TEXT("self_type_id"), SliceModel.SelfTypeId);
	Root->SetStringField(TEXT("selection_hash"), SliceModel.SelectionHash);
	Root->SetStringField(TEXT("package_hash"), SliceModel.PackageHash);
	TArray<TSharedPtr<FJsonValue>> TypeValues;
	for (const FAvidScriptBindingTypeModel& Type : SliceModel.Types)
	{
		TSharedPtr<FJsonObject> TypeObject = MakeShared<FJsonObject>();
		TypeObject->SetStringField(TEXT("stable_id"), Type.StableId);
		TypeObject->SetStringField(TEXT("canonical_type"), Type.CanonicalType);
		TypeObject->SetStringField(TEXT("kind"), Type.Kind);
		TypeObject->SetStringField(TEXT("cpp_type"), Type.CppType);
		TypeObject->SetNumberField(TEXT("size"), Type.Size);
		TypeObject->SetNumberField(TEXT("alignment"), Type.Alignment);
		TArray<TSharedPtr<FJsonValue>> AbiTypes;
		for (const FString& AbiType : Type.AbiTypes)
		{
			AbiTypes.Add(MakeShared<FJsonValueString>(AbiType));
		}
		TypeObject->SetArrayField(TEXT("abi_types"), MoveTemp(AbiTypes));
		TypeObject->SetNumberField(TEXT("object_type_ordinal"), Type.ObjectTypeOrdinal);
		TypeObject->SetStringField(TEXT("class_path"), Type.ClassPath);
		TypeObject->SetStringField(TEXT("base_type_id"), Type.BaseTypeId);
		if (Type.Kind == TEXT("enum"))
		{
			TArray<TSharedPtr<FJsonValue>> EnumValues;
			for (const FAvidScriptBindingEnumValue& EnumValue : Type.EnumValues)
			{
				TSharedPtr<FJsonObject> EnumObject = MakeShared<FJsonObject>();
				EnumObject->SetStringField(TEXT("name"), EnumValue.Name);
				EnumObject->SetNumberField(TEXT("value"), EnumValue.Value);
				EnumValues.Add(MakeShared<FJsonValueObject>(MoveTemp(EnumObject)));
			}
			TypeObject->SetArrayField(TEXT("enum_values"), MoveTemp(EnumValues));
		}
		TypeValues.Add(MakeShared<FJsonValueObject>(MoveTemp(TypeObject)));
	}
	Root->SetArrayField(TEXT("types"), MoveTemp(TypeValues));

	const TArray<TSharedPtr<FJsonValue>>& ReferenceValues =
		Root->GetArrayField(TEXT("class_references"));
	if (ReferenceValues.Num() != SliceModel.ClassReferences.Num())
	{
		return false;
	}
	for (int32 Index = 0; Index < ReferenceValues.Num(); ++Index)
	{
		ReferenceValues[Index]->AsObject()->SetStringField(
			TEXT("result_type_id"),
			SliceModel.ClassReferences[Index].ResultTypeId);
	}

	InOutJson.Empty();
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&InOutJson);
	return FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
}
} // namespace

bool FAvidScriptEditorCSharpBindingSliceService::Publish(
	const FString& AuthorizationDescriptorPath,
	const FAvidScriptFrontendBindingPackage& Provenance,
	const FString& OutputRoot,
	FAvidScriptCSharpBindingEmitResult& OutPackage,
	FAvidScriptEditorCSharpBindingSliceResult& OutResult)
{
	OutPackage = FAvidScriptCSharpBindingEmitResult();
	OutResult = FAvidScriptEditorCSharpBindingSliceResult();
	OutResult.RequestedBindingCount = Provenance.UsedImports.Num();

	if (!Provenance.bPresent
		|| Provenance.UsedImportCount != Provenance.UsedImports.Num()
		|| Provenance.UsedImports.IsEmpty())
	{
		SetAvidScriptCSharpBindingSliceFailure(
			OutResult,
			TEXT("slice_provenance_invalid"),
			TEXT("binding_package.used_imports"),
			TEXT("rerun the bootstrap build and preserve complete binding provenance"));
		return false;
	}

	const FString FullDescriptorPath = NormalizeAvidScriptCSharpBindingSlicePath(AuthorizationDescriptorPath);
	const FString ProvenanceDescriptorPath = NormalizeAvidScriptCSharpBindingSlicePath(Provenance.DescriptorFile);
	if (FullDescriptorPath != ProvenanceDescriptorPath)
	{
		SetAvidScriptCSharpBindingSliceFailure(
			OutResult,
			TEXT("slice_descriptor_mismatch"),
			Provenance.DescriptorFile,
			TEXT("use the authorization descriptor recorded by the bootstrap report"));
		return false;
	}

	FString AuthorizationJson;
	if (!FFileHelper::LoadFileToString(AuthorizationJson, *FullDescriptorPath))
	{
		SetAvidScriptCSharpBindingSliceFailure(
			OutResult,
			TEXT("slice_descriptor_read_failed"),
			FullDescriptorPath,
			TEXT("republish the complete authorization binding package"));
		return false;
	}
	if (Provenance.DescriptorSha256.IsEmpty()
		|| FAvidScriptHash::Sha256HexUtf8(AuthorizationJson) != Provenance.DescriptorSha256)
	{
		SetAvidScriptCSharpBindingSliceFailure(
			OutResult,
			TEXT("slice_descriptor_hash_mismatch"),
			FullDescriptorPath,
			TEXT("rerun bootstrap with an untampered authorization package"));
		return false;
	}

	FAvidScriptBindingPackageModel AuthorizationModel;
	FString ParseCategory;
	FString ParseSource;
	if (!FAvidScriptBindingDescriptorParser::Parse(
			AuthorizationJson,
			AuthorizationModel,
			ParseCategory,
			ParseSource))
	{
		SetAvidScriptCSharpBindingSliceFailure(
			OutResult,
			ParseCategory,
			ParseSource,
			TEXT("regenerate the authorization descriptor from UE reflection"));
		return false;
	}
	const TConstArrayView<FAvidScriptObjectLifecycleBindingSpec> LifecycleSpecs =
		FAvidScriptObjectLifecycleBindings::GetSpecs();
	const TConstArrayView<FAvidScriptObjectTypeBindingSpec> ObjectTypeSpecs =
		FAvidScriptObjectTypeBindings::GetSpecs();
	const TConstArrayView<FAvidScriptObjectFactoryBindingSpec> ObjectFactorySpecs =
		FAvidScriptObjectFactoryBinding::GetSpecs();
	const TConstArrayView<FAvidScriptSceneAttachmentBindingSpec> AttachmentSpecs =
		FAvidScriptSceneAttachmentBinding::GetSpecs();
	const int32 ReflectedBindingCount = AuthorizationModel.Bindings.Num();
	const int32 LifecycleImportCount =
		FAvidScriptEditorCSharpBindingRenderer::GetLifecycleImportCount(
			AuthorizationModel);
	const bool bPublishesLifecycleCapabilities = LifecycleImportCount > 0;
	const bool bPublishesObjectTypeCapability = AuthorizationModel.Types.ContainsByPredicate(
		[](const FAvidScriptBindingTypeModel& Type)
		{
			return Type.ObjectTypeOrdinal != INDEX_NONE;
		});
	const int32 ObjectTypeImportOffset =
		ReflectedBindingCount
		+ LifecycleImportCount;
	const bool bPublishesObjectFactoryCapabilities =
		AuthorizationModel.SchemaVersion >= 7
		&& !AuthorizationModel.ObjectFactories.IsEmpty();
	const int32 ObjectFactoryImportOffset =
		ObjectTypeImportOffset
		+ (bPublishesObjectTypeCapability ? ObjectTypeSpecs.Num() : 0);
	const bool bPublishesSceneAttachmentCapabilities =
		AuthorizationModel.SchemaVersion >= 7
		&& HasAvidScriptCSharpBindingSliceSceneComponentFactory(
			AuthorizationModel);
	const int32 SceneAttachmentImportOffset =
		ObjectFactoryImportOffset
		+ (bPublishesObjectFactoryCapabilities
			? ObjectFactorySpecs.Num()
			: 0);
	const int32 ExpectedProfileImportCount =
		FAvidScriptEditorCSharpBindingRenderer::GetManifestImportCount(AuthorizationModel);
	if (AuthorizationModel.PackageName != Provenance.PackageName
		|| AuthorizationModel.PackageHash != Provenance.PackageHash
		|| ExpectedProfileImportCount != Provenance.ProfileImportCount)
	{
		SetAvidScriptCSharpBindingSliceFailure(
			OutResult,
			TEXT("slice_package_identity_mismatch"),
			Provenance.PackageName,
			TEXT("rerun bootstrap with matching package and descriptor provenance"));
		return false;
	}

	TSet<FString> SeenRequestedStableIds;
	TSet<FString> RequestedReflectedStableIds;
	TArray<FAvidScriptReflectedFunctionSelection> FunctionSelections;
	TArray<FAvidScriptReflectedPropertySelection> PropertySelections;
	FunctionSelections.Reserve(Provenance.UsedImports.Num());
	PropertySelections.Reserve(Provenance.UsedImports.Num());
	for (const FAvidScriptFrontendBindingImport& Import : Provenance.UsedImports)
	{
		const bool bHasPackedOwnerStableId =
			Import.StableId == TEXT("avidscript.owner_get_handle.v1");
		const bool bIsPackedOwner =
			bHasPackedOwnerStableId
			&& Import.Ordinal == INDEX_NONE
			&& Import.Module == TEXT("avidscript")
			&& Import.Name == TEXT("avid_owner_get_handle")
			&& Import.Signature == TEXT("()I");
		if (!IsAvidScriptCSharpBindingSliceStableId(Import.StableId)
			&& !bHasPackedOwnerStableId)
		{
			SetAvidScriptCSharpBindingSliceFailure(
				OutResult,
				TEXT("slice_stable_id_invalid"),
				Import.StableId,
				TEXT("rerun bootstrap with canonical lowercase SHA-256 stable identities"));
			return false;
		}
		if (SeenRequestedStableIds.Contains(Import.StableId))
		{
			SetAvidScriptCSharpBindingSliceFailure(
				OutResult,
				TEXT("slice_duplicate_stable_id"),
				Import.StableId,
				TEXT("deduplicate bootstrap used_imports before slice generation"));
			return false;
		}
		SeenRequestedStableIds.Add(Import.StableId);

		if (AuthorizationModel.Bindings.IsValidIndex(Import.Ordinal))
		{
			const FAvidScriptBindingFunctionModel& Binding = AuthorizationModel.Bindings[Import.Ordinal];
			if (Binding.StableId != Import.StableId)
			{
				SetAvidScriptCSharpBindingSliceFailure(
					OutResult,
					TEXT("slice_binding_missing"),
					Import.StableId,
					TEXT("regenerate the complete package and rerun bootstrap"));
				return false;
			}
			if (Binding.HostImport.Module != Import.Module
				|| Binding.HostImport.Name != Import.Name
				|| Binding.HostImport.Signature != Import.Signature)
			{
				SetAvidScriptCSharpBindingSliceFailure(
					OutResult,
					TEXT("slice_import_identity_mismatch"),
					Import.StableId,
					TEXT("rerun bootstrap with untampered import provenance"));
				return false;
			}

			RequestedReflectedStableIds.Add(Import.StableId);
			if (Binding.BindingKind == TEXT("property_get"))
			{
				PropertySelections.Add({ Binding.OwnerClass, FName(*Binding.UeMember) });
			}
			else
			{
				FunctionSelections.Add({ Binding.OwnerClass, FName(*Binding.UeMember) });
			}
			continue;
		}

		const int32 LifecycleSpecIndex = Import.Ordinal - ReflectedBindingCount;
		if (bPublishesLifecycleCapabilities
			&& LifecycleSpecs.IsValidIndex(LifecycleSpecIndex))
		{
			const FAvidScriptObjectLifecycleBindingSpec& LifecycleSpec =
				LifecycleSpecs[LifecycleSpecIndex];
			if (LifecycleSpec.StableId != Import.StableId
				|| LifecycleSpec.ModuleName != Import.Module
				|| LifecycleSpec.ImportName != Import.Name
				|| LifecycleSpec.Signature != Import.Signature)
			{
				SetAvidScriptCSharpBindingSliceFailure(
					OutResult,
					TEXT("slice_import_identity_mismatch"),
					Import.StableId,
					TEXT("rerun bootstrap with untampered import provenance"));
				return false;
			}
			continue;
		}

		const int32 ObjectTypeSpecIndex = Import.Ordinal - ObjectTypeImportOffset;
		if (bPublishesObjectTypeCapability
			&& ObjectTypeSpecs.IsValidIndex(ObjectTypeSpecIndex))
		{
			const FAvidScriptObjectTypeBindingSpec& ObjectTypeSpec =
				ObjectTypeSpecs[ObjectTypeSpecIndex];
			if (ObjectTypeSpec.StableId != Import.StableId
				|| ObjectTypeSpec.ModuleName != Import.Module
				|| ObjectTypeSpec.ImportName != Import.Name
				|| ObjectTypeSpec.Signature != Import.Signature)
			{
				SetAvidScriptCSharpBindingSliceFailure(
					OutResult,
					TEXT("slice_import_identity_mismatch"),
					Import.StableId,
					TEXT("rerun bootstrap with untampered import provenance"));
				return false;
			}
			continue;
		}

		const int32 ObjectFactorySpecIndex =
			Import.Ordinal - ObjectFactoryImportOffset;
		if (bPublishesObjectFactoryCapabilities
			&& ObjectFactorySpecs.IsValidIndex(ObjectFactorySpecIndex))
		{
			const FAvidScriptObjectFactoryBindingSpec& ObjectFactorySpec =
				ObjectFactorySpecs[ObjectFactorySpecIndex];
			if (ObjectFactorySpec.StableId != Import.StableId
				|| ObjectFactorySpec.ModuleName != Import.Module
				|| ObjectFactorySpec.ImportName != Import.Name
				|| ObjectFactorySpec.Signature != Import.Signature)
			{
				SetAvidScriptCSharpBindingSliceFailure(
					OutResult,
					TEXT("slice_import_identity_mismatch"),
					Import.StableId,
					TEXT("rerun bootstrap with untampered import provenance"));
				return false;
			}
			continue;
		}

		const int32 AttachmentSpecIndex =
			Import.Ordinal - SceneAttachmentImportOffset;
		if (bPublishesSceneAttachmentCapabilities
			&& AttachmentSpecs.IsValidIndex(AttachmentSpecIndex))
		{
			const FAvidScriptSceneAttachmentBindingSpec& AttachmentSpec =
				AttachmentSpecs[AttachmentSpecIndex];
			if (AttachmentSpec.StableId != Import.StableId
				|| AttachmentSpec.ModuleName != Import.Module
				|| AttachmentSpec.ImportName != Import.Name
				|| AttachmentSpec.Signature != Import.Signature)
			{
				SetAvidScriptCSharpBindingSliceFailure(
					OutResult,
					TEXT("slice_import_identity_mismatch"),
					Import.StableId,
					TEXT("rerun bootstrap with untampered import provenance"));
				return false;
			}
			continue;
		}

		if (bIsPackedOwner && !AuthorizationModel.SelfTypeId.IsEmpty())
		{
			continue;
		}
		if (bHasPackedOwnerStableId)
		{
			SetAvidScriptCSharpBindingSliceFailure(
				OutResult,
				TEXT("slice_import_identity_mismatch"),
				Import.StableId,
				TEXT("rerun bootstrap with untampered import provenance"));
			return false;
		}

		SetAvidScriptCSharpBindingSliceFailure(
			OutResult,
			TEXT("slice_binding_missing"),
			Import.StableId,
			TEXT("regenerate the complete package and rerun bootstrap"));
		return false;
	}

	FString SliceDescriptorJson;
	FAvidScriptBindingDescriptorGenerateResult GenerateResult;
	TArray<FAvidScriptProjectBindingClassSpec> ClassReferences;
	ClassReferences.Reserve(AuthorizationModel.ClassReferences.Num());
	for (const FAvidScriptBindingClassReferenceModel& Reference : AuthorizationModel.ClassReferences)
	{
		ClassReferences.Add({
			Reference.ScriptName,
			Reference.ClassPath,
			Reference.BaseClassPath,
			Reference.LoadPolicy
		});
	}
	TArray<FAvidScriptProjectObjectFactorySpec> ObjectFactories;
	ObjectFactories.Reserve(AuthorizationModel.ObjectFactories.Num());
	for (const FAvidScriptBindingObjectFactoryModel& Factory :
		AuthorizationModel.ObjectFactories)
	{
		const FAvidScriptBindingClassReferenceModel* ClassReference =
			AuthorizationModel.ClassReferences.FindByPredicate(
				[&Factory](
					const FAvidScriptBindingClassReferenceModel& Candidate)
				{
					return Candidate.StableId
						== Factory.ClassReferenceId;
				});
		const FAvidScriptBindingTypeModel* OuterType =
			AuthorizationModel.Types.FindByPredicate(
				[&Factory](const FAvidScriptBindingTypeModel& Type)
				{
					return Type.StableId == Factory.OuterTypeId;
				});
		if (ClassReference == nullptr || OuterType == nullptr)
		{
			SetAvidScriptCSharpBindingSliceFailure(
				OutResult,
				TEXT("slice_factory_provenance_invalid"),
				Factory.StableId,
				TEXT("preserve factory class-reference and Outer type provenance"));
			return false;
		}

		FAvidScriptProjectObjectFactorySpec FactorySpec;
		FactorySpec.ScriptName = Factory.ScriptName;
		FactorySpec.ClassReference = ClassReference->ScriptName;
		FactorySpec.OuterBaseClassPath = OuterType->ClassPath;
		FactorySpec.Kind =
			Factory.Kind == EAvidScriptObjectFactoryKind::ActorComponent
				? EAvidScriptProjectObjectFactoryKind::ActorComponent
				: EAvidScriptProjectObjectFactoryKind::NewObject;
		FactorySpec.Ownership =
			EAvidScriptProjectObjectOwnership::Session;
		FactorySpec.Registration =
			Factory.Registration
				== EAvidScriptComponentRegistrationPolicy::RegisterInstance
				? EAvidScriptProjectComponentRegistration::RegisterInstance
				: EAvidScriptProjectComponentRegistration::None;
		ObjectFactories.Add(MoveTemp(FactorySpec));
	}
	if (!FAvidScriptEditorBindingDescriptorGenerator::GenerateWithObjectFactories(
			AuthorizationModel.PackageName,
			FunctionSelections,
			PropertySelections,
			ClassReferences,
			ObjectFactories,
			SliceDescriptorJson,
			GenerateResult))
	{
		SetAvidScriptCSharpBindingSliceFailure(
			OutResult,
			GenerateResult.ErrorCategory,
			GenerateResult.ErrorSource,
			GenerateResult.NextAction);
		return false;
	}

	FAvidScriptBindingPackageModel SliceModel;
	if (!FAvidScriptBindingDescriptorParser::Parse(
			SliceDescriptorJson,
			SliceModel,
			ParseCategory,
			ParseSource))
	{
		SetAvidScriptCSharpBindingSliceFailure(
			OutResult,
			ParseCategory,
			ParseSource,
			TEXT("repair the generated slice descriptor contract"));
		return false;
	}
	FString ClosureErrorSource;
	if (!BuildAvidScriptCSharpBindingSliceTypeClosure(
			AuthorizationModel,
			SliceModel,
			ClosureErrorSource)
		|| !RewriteAvidScriptCSharpBindingSliceDescriptor(
			SliceModel,
			SliceDescriptorJson))
	{
		SetAvidScriptCSharpBindingSliceFailure(
			OutResult,
			TEXT("slice_type_closure_invalid"),
			ClosureErrorSource.IsEmpty()
				? AuthorizationModel.PackageName
				: ClosureErrorSource,
			TEXT("preserve Self, class-reference results, binding value types, receivers, and every object ancestor"));
		return false;
	}

	FAvidScriptBindingPackageModel VerifiedSliceModel;
	if (!FAvidScriptBindingDescriptorParser::Parse(
			SliceDescriptorJson,
			VerifiedSliceModel,
			ParseCategory,
			ParseSource)
		|| VerifiedSliceModel.SchemaVersion
			!= AuthorizationModel.SchemaVersion
		|| VerifiedSliceModel.SelectionHash
			!= FAvidScriptBindingDescriptorIdentity::MakeSelectionHash(VerifiedSliceModel)
		|| VerifiedSliceModel.PackageHash
			!= FAvidScriptBindingDescriptorIdentity::MakePackageHash(VerifiedSliceModel))
	{
		SetAvidScriptCSharpBindingSliceFailure(
			OutResult,
			ParseCategory.IsEmpty()
				? FString(TEXT("slice_identity_invalid"))
				: ParseCategory,
			ParseSource.IsEmpty()
				? AuthorizationModel.PackageName
				: ParseSource,
			TEXT("recompute the complete descriptor slice identity after closing its type graph"));
		return false;
	}
	SliceModel = MoveTemp(VerifiedSliceModel);
	if (SliceModel.Bindings.Num() != RequestedReflectedStableIds.Num())
	{
		SetAvidScriptCSharpBindingSliceFailure(
			OutResult,
			TEXT("slice_selection_mismatch"),
			AuthorizationModel.PackageName,
			TEXT("regenerate the slice from stable reflected selections"));
		return false;
	}
	if (SliceModel.ClassReferences.Num() != AuthorizationModel.ClassReferences.Num())
	{
		SetAvidScriptCSharpBindingSliceFailure(
			OutResult,
			TEXT("slice_class_reference_mismatch"),
			AuthorizationModel.PackageName,
			TEXT("preserve the complete authorization class table in the runtime slice"));
		return false;
	}
	if (SliceModel.ObjectFactories.Num()
		!= AuthorizationModel.ObjectFactories.Num())
	{
		SetAvidScriptCSharpBindingSliceFailure(
			OutResult,
			TEXT("slice_object_factory_mismatch"),
			AuthorizationModel.PackageName,
			TEXT("preserve the complete authorization object factory table in the runtime slice"));
		return false;
	}
	for (const FAvidScriptBindingFunctionModel& Binding : SliceModel.Bindings)
	{
		if (!RequestedReflectedStableIds.Contains(Binding.StableId))
		{
			SetAvidScriptCSharpBindingSliceFailure(
				OutResult,
				TEXT("slice_selection_mismatch"),
				Binding.StableId,
				TEXT("regenerate the slice from stable reflected selections"));
			return false;
		}
	}

	if (!FAvidScriptEditorCSharpBindingEmitter::PublishDerivedSliceDescriptor(
			SliceDescriptorJson,
			OutputRoot,
			OutPackage))
	{
		SetAvidScriptCSharpBindingSliceFailure(
			OutResult,
			OutPackage.ErrorCategory,
			OutPackage.ErrorSource,
			OutPackage.NextAction);
		return false;
	}

	OutResult.bSucceeded = true;
	return true;
}
