#include "CSharpBuild/AvidScriptEditorCSharpBindingSliceService.h"

#include "AvidScriptBindingDescriptor.h"
#include "AvidScriptObjectFactoryBinding.h"
#include "AvidScriptObjectLifecycleBinding.h"
#include "AvidScriptObjectTypeBinding.h"
#include "AvidScriptSceneAttachmentBinding.h"
#include "AvidScriptValueCapability.h"
#include "AvidScriptEditorBindingDescriptorGenerator.h"
#include "AvidScriptEditorCSharpBindingEmitter.h"
#include "BindingGeneration/AvidScriptEditorBindingDescriptorModel.h"
#include "BindingGeneration/AvidScriptEditorCSharpBindingRenderer.h"
#include "AvidScriptFrontendReport.h"
#include "AvidScriptHash.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

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
	const TConstArrayView<int32> ActiveObjectTypeOrdinals,
	FAvidScriptBindingPackageModel& SliceModel,
	FString& OutErrorSource)
{
	TMap<FString, const FAvidScriptBindingTypeModel*> AuthorizationTypesById;
	for (const FAvidScriptBindingTypeModel& Type : AuthorizationModel.Types)
	{
		AuthorizationTypesById.Add(Type.StableId, &Type);
	}

	SliceModel.SelfTypeId = AuthorizationModel.SelfTypeId;
	SliceModel.bHasActiveObjectTypeOrdinals = true;
	SliceModel.ActiveObjectTypeOrdinals.Reset(ActiveObjectTypeOrdinals.Num());
	SliceModel.ActiveObjectTypeOrdinals.Append(
		ActiveObjectTypeOrdinals.GetData(),
		ActiveObjectTypeOrdinals.Num());
	TSet<FString> RequiredTypeIds;
	// Prepared C# constants use authorization ordinals. Preserve the complete
	// object table while the runtime loader activates only the selected closure.
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
	for (const FAvidScriptBindingDelegateEventModel& Event :
		SliceModel.DelegateEvents)
	{
		RequiredTypeIds.Add(
			FAvidScriptBindingDescriptorIdentity::MakeTypeStableId(
				TEXT("object:") + Event.OwnerClass,
				{}));
		for (const FAvidScriptBindingValueModel& Parameter : Event.Parameters)
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
		if (!Type->ElementTypeId.IsEmpty()
			&& !RequiredTypeIds.Contains(Type->ElementTypeId))
		{
			RequiredTypeIds.Add(Type->ElementTypeId);
			PendingTypeIds.Add(Type->ElementTypeId);
		}
		for (const FAvidScriptBindingStructFieldModel& Field : Type->StructFields)
		{
			if (!RequiredTypeIds.Contains(Field.TypeId))
			{
				RequiredTypeIds.Add(Field.TypeId);
				PendingTypeIds.Add(Field.TypeId);
			}
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
	SliceModel.GeneratedSourcePackageHash =
		SliceModel.Bindings.ContainsByPredicate(
			[](const FAvidScriptBindingFunctionModel& Binding)
			{
				return Binding.DispatchMode == TEXT("generated_native_s1");
			})
		? AuthorizationModel.PackageHash
		: FString();
	SliceModel.SelectionHash = FAvidScriptBindingDescriptorIdentity::MakeSelectionHash(SliceModel);
	SliceModel.PackageHash = FAvidScriptBindingDescriptorIdentity::MakePackageHash(SliceModel);
	return true;
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
		|| Provenance.UsedObjectTypeCount
			!= Provenance.UsedObjectTypeOrdinals.Num()
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
	TMap<FString, int32> PropertySelectionIndices;
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
		const FAvidScriptValueCapabilityImportSpec* ValueCapabilitySpec =
			FAvidScriptValueCapability::GetArrayImportSpecs().FindByPredicate(
				[&Import](const FAvidScriptValueCapabilityImportSpec& Spec)
				{
					return Import.StableId == Spec.StableId;
				});
		const bool bHasValueCapabilityStableId = ValueCapabilitySpec != nullptr;
		const bool bIsValueCapability =
			bHasValueCapabilityStableId
			&& Import.Ordinal == INDEX_NONE
			&& Import.Module == ValueCapabilitySpec->ModuleName
			&& Import.Name == ValueCapabilitySpec->ImportName
			&& Import.Signature == ValueCapabilitySpec->Signature;
		if (!IsAvidScriptCSharpBindingSliceStableId(Import.StableId)
			&& !bHasPackedOwnerStableId
			&& !bHasValueCapabilityStableId)
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
			if (Binding.BindingKind == TEXT("property_get")
				|| Binding.BindingKind == TEXT("property_set"))
			{
				const FString PropertyKey = Binding.OwnerClass
					+ TEXT("\n") + Binding.UeMember;
				if (int32* ExistingIndex = PropertySelectionIndices.Find(PropertyKey))
				{
					PropertySelections[*ExistingIndex].bWritable |=
						Binding.BindingKind == TEXT("property_set");
				}
				else
				{
					PropertySelectionIndices.Add(
						PropertyKey,
						PropertySelections.Add({
							Binding.OwnerClass,
							FName(*Binding.UeMember),
							Binding.BindingKind == TEXT("property_set")
						}));
				}
			}
			else if (Binding.BindingKind == TEXT("function"))
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
		if (bIsValueCapability
			&& AuthorizationModel.Types.ContainsByPredicate(
				[](const FAvidScriptBindingTypeModel& Type)
				{
					return Type.Kind == TEXT("array");
				}))
		{
			continue;
		}
		if (bHasValueCapabilityStableId)
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
	FAvidScriptBindingSelectionProfile SliceProfile;
	SliceProfile.PackageName = AuthorizationModel.PackageName;
	TMap<FString, FAvidScriptReflectedClassSelection> SpecializedClassRules;
	for (const FAvidScriptBindingFunctionModel& Binding : AuthorizationModel.Bindings)
	{
		if (!RequestedReflectedStableIds.Contains(Binding.StableId))
		{
			continue;
		}

		if (Binding.BindingKind == TEXT("function")
			&& (Binding.DispatchMode == TEXT("qualified_native_direct")
				|| Binding.DispatchMode == TEXT("generated_native_s1")))
		{
			FAvidScriptReflectedClassSelection& Rule =
				SpecializedClassRules.FindOrAdd(Binding.OwnerClass);
			Rule.OwnerClassPath = Binding.OwnerClass;
			if (Binding.DispatchMode == TEXT("qualified_native_direct"))
			{
				Rule.NativeDirectFunctions.AddUnique(FName(*Binding.UeMember));
			}
			else
			{
				Rule.GeneratedNativeFunctions.AddUnique(FName(*Binding.UeMember));
			}
		}
		else if ((Binding.BindingKind == TEXT("property_get")
				|| Binding.BindingKind == TEXT("property_set"))
			&& Binding.DispatchMode == TEXT("generated_native_s1"))
		{
			FAvidScriptReflectedClassSelection& Rule =
				SpecializedClassRules.FindOrAdd(Binding.OwnerClass);
			Rule.OwnerClassPath = Binding.OwnerClass;
			Rule.GeneratedNativeProperties.AddUnique(FName(*Binding.UeMember));
		}
	}
	for (const FAvidScriptBindingDelegateEventModel& Event :
		AuthorizationModel.DelegateEvents)
	{
		FAvidScriptReflectedClassSelection& Rule =
			SpecializedClassRules.FindOrAdd(Event.OwnerClass);
		Rule.OwnerClassPath = Event.OwnerClass;
		if (Event.DelegateKind == TEXT("multicast"))
		{
			Rule.IncludeEvents.AddUnique(FName(*Event.UeMember));
		}
		else
		{
			Rule.IncludeHandlers.AddUnique(FName(*Event.UeMember));
		}
	}
	for (FAvidScriptReflectedPropertySelection& Selection : PropertySelections)
	{
		if (FAvidScriptReflectedClassSelection* Rule =
			SpecializedClassRules.Find(Selection.OwnerClassPath))
		{
			Rule->IncludeProperties.AddUnique(Selection.PropertyName);
			if (Selection.bWritable)
			{
				Rule->WritableProperties.AddUnique(Selection.PropertyName);
			}
		}
		else
		{
			SliceProfile.ExplicitProperties.Add(MoveTemp(Selection));
		}
	}
	for (FAvidScriptReflectedFunctionSelection& Selection : FunctionSelections)
	{
		if (FAvidScriptReflectedClassSelection* Rule =
			SpecializedClassRules.Find(Selection.OwnerClassPath))
		{
			Rule->IncludeFunctions.AddUnique(Selection.FunctionName);
		}
		else
		{
			SliceProfile.ExplicitFunctions.Add(MoveTemp(Selection));
		}
	}
	SpecializedClassRules.GenerateValueArray(SliceProfile.Classes);

	FAvidScriptBindingSelectionResolveResult SelectionResult;
	if (!FAvidScriptEditorBindingDescriptorGenerator::GenerateFromProfile(
			SliceProfile,
			ClassReferences,
			ObjectFactories,
			SliceDescriptorJson,
			SelectionResult,
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
	SliceModel.Bindings.RemoveAll(
		[&RequestedReflectedStableIds](
			const FAvidScriptBindingFunctionModel& Binding)
		{
			return !RequestedReflectedStableIds.Contains(Binding.StableId);
		});
	for (int32 Index = 0; Index < SliceModel.Bindings.Num(); ++Index)
	{
		SliceModel.Bindings[Index].Ordinal = Index;
	}
	SliceModel.SchemaVersion = AuthorizationModel.SchemaVersion;
	SliceModel.GeneratorVersion = AuthorizationModel.GeneratorVersion;
	FString ClosureErrorSource;
	if (!BuildAvidScriptCSharpBindingSliceTypeClosure(
			AuthorizationModel,
			Provenance.UsedObjectTypeOrdinals,
			SliceModel,
			ClosureErrorSource)
		|| !FAvidScriptEditorBindingDescriptorModelSerializer::SerializeCanonical(
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
	if (SliceModel.DelegateEvents.Num()
		!= AuthorizationModel.DelegateEvents.Num())
	{
		SetAvidScriptCSharpBindingSliceFailure(
			OutResult,
			TEXT("slice_delegate_event_mismatch"),
			AuthorizationModel.PackageName,
			TEXT("preserve the complete authorization delegate event table in the runtime slice"));
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
