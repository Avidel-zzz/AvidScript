#include "HostEffects/AvidScriptHostEffectTransaction.h"

#include "Components/SceneComponent.h"
#include "GameFramework/Actor.h"
#include "UObject/UnrealType.h"

FAvidScriptHostEffectTransaction::FPropertySnapshot::~FPropertySnapshot()
{
	Reset();
}

FAvidScriptHostEffectTransaction::FPropertySnapshot::FPropertySnapshot(
	FPropertySnapshot&& Other) noexcept
	: Property(Other.Property)
	, Data(Other.Data)
	, StrongObjectReferences(MoveTemp(Other.StrongObjectReferences))
{
	Other.Property = nullptr;
	Other.Data = nullptr;
}

FAvidScriptHostEffectTransaction::FPropertySnapshot&
FAvidScriptHostEffectTransaction::FPropertySnapshot::operator=(
	FPropertySnapshot&& Other) noexcept
{
	if (this != &Other)
	{
		Reset();
		Property = Other.Property;
		Data = Other.Data;
		StrongObjectReferences = MoveTemp(Other.StrongObjectReferences);
		Other.Property = nullptr;
		Other.Data = nullptr;
	}
	return *this;
}

bool FAvidScriptHostEffectTransaction::FPropertySnapshot::Capture(
	FProperty& InProperty,
	UObject& Source)
{
	Reset();
	const int32 Size = InProperty.GetSize();
	const uint32 Alignment = static_cast<uint32>(
		FMath::Max(1, InProperty.GetMinAlignment()));
	if (Size <= 0 || !FMath::IsPowerOfTwo(Alignment))
	{
		return false;
	}

	Data = FMemory::Malloc(Size, Alignment);
	if (Data == nullptr)
	{
		return false;
	}
	Property = &InProperty;
	Property->InitializeValue(Data);
	Property->CopyCompleteValue(
		Data,
		Property->ContainerPtrToValuePtr<void>(&Source));
	if (const FObjectPropertyBase* ObjectProperty =
		CastField<FObjectPropertyBase>(Property))
	{
		const int32 ElementSize = Property->GetElementSize();
		const int32 ElementCount = ElementSize > 0 ? Size / ElementSize : 0;
		StrongObjectReferences.Reserve(ElementCount);
		for (int32 Index = 0; Index < ElementCount; ++Index)
		{
			const void* ElementData =
				static_cast<const uint8*>(Data)
				+ static_cast<SIZE_T>(Index) * ElementSize;
			if (UObject* ReferencedObject =
				ObjectProperty->GetObjectPropertyValue(ElementData))
			{
				StrongObjectReferences.Emplace(ReferencedObject);
			}
		}
	}
	return true;
}

bool FAvidScriptHostEffectTransaction::FPropertySnapshot::Restore(
	UObject& Target) const
{
	if (!IsValid())
	{
		return false;
	}
	Property->CopyCompleteValue(
		Property->ContainerPtrToValuePtr<void>(&Target),
		Data);
	return true;
}

void FAvidScriptHostEffectTransaction::FPropertySnapshot::Reset()
{
	if (Property != nullptr && Data != nullptr)
	{
		Property->DestroyValue(Data);
	}
	if (Data != nullptr)
	{
		FMemory::Free(Data);
	}
	Property = nullptr;
	Data = nullptr;
	StrongObjectReferences.Reset();
}

bool FAvidScriptHostEffectTransaction::PrepareEffect(
	FAvidScriptObjectRegistry& Registry,
	const FAvidScriptObjectHandle& Handle,
	UObject& Target,
	const EAvidScriptBindingReloadEffect Effect,
	FAvidScriptBindingHostEffectPrepareResult& OutResult)
{
	OutResult = FAvidScriptBindingHostEffectPrepareResult();
	if (State != EAvidScriptHostEffectTransactionState::Open)
	{
		SetPrepareFailure(
			OutResult,
			TEXT("host_effect_transaction_closed"),
			FormatHandle(Handle),
			TEXT("The candidate host effect transaction has already completed."));
		return false;
	}
	if (Effect == EAvidScriptBindingReloadEffect::None)
	{
		OutResult.bSucceeded = true;
		return true;
	}
	if (Effect == EAvidScriptBindingReloadEffect::Unsupported)
	{
		SetPrepareFailure(
			OutResult,
			TEXT("binding_reload_effect_unsupported"),
			Target.GetPathName(),
			TEXT("The reflected write has no reversible candidate reload adapter."));
		return false;
	}

	FAvidScriptObjectHandleResult ResolveResult;
	UObject* ResolvedObject = Registry.ResolveObject(Handle, ResolveResult);
	if (ResolvedObject == nullptr)
	{
		SetPrepareFailure(
			OutResult,
			TEXT("host_effect_handle_invalid"),
			FormatHandle(Handle),
			ResolveResult.ErrorCategory.IsEmpty()
				? FString(TEXT("The candidate write handle is not live."))
				: ResolveResult.ErrorCategory);
		return false;
	}
	if (ResolvedObject != &Target)
	{
		SetPrepareFailure(
			OutResult,
			TEXT("host_effect_target_mismatch"),
			FormatHandle(Handle),
			TEXT("The invocation target does not match the registry object for this handle."));
		return false;
	}

	const FEntryKey Key{ Handle.ToUInt64(), Effect };
	if (CapturedKeys.Contains(Key))
	{
		OutResult.bSucceeded = true;
		return true;
	}

	FEntry Entry;
	Entry.Handle = Handle;
	Entry.Object = &Target;
	Entry.Effect = Effect;
	if (Effect == EAvidScriptBindingReloadEffect::ActorTransform)
	{
		const AActor* Actor = Cast<AActor>(&Target);
		if (Actor == nullptr)
		{
			SetPrepareFailure(
				OutResult,
				TEXT("host_effect_target_type_mismatch"),
				Target.GetPathName(),
				TEXT("Actor transform effects require an AActor target."));
			return false;
		}
		Entry.OriginalTransform = Actor->GetActorTransform();
	}
	else if (Effect == EAvidScriptBindingReloadEffect::SceneComponentTransform)
	{
		const USceneComponent* Component = Cast<USceneComponent>(&Target);
		if (Component == nullptr)
		{
			SetPrepareFailure(
				OutResult,
				TEXT("host_effect_target_type_mismatch"),
				Target.GetPathName(),
				TEXT("Scene component transform effects require a USceneComponent target."));
			return false;
		}
		Entry.OriginalTransform = Component->GetComponentTransform();
	}
	else
	{
		SetPrepareFailure(
			OutResult,
			TEXT("binding_reload_effect_unsupported"),
			Target.GetPathName(),
			TEXT("The candidate effect domain has no transaction adapter."));
		return false;
	}

	CapturedKeys.Add(Key);
	Entries.Add(MoveTemp(Entry));
	OutResult.bSucceeded = true;
	return true;
}

bool FAvidScriptHostEffectTransaction::PrepareReflectedProperty(
	FAvidScriptObjectRegistry& Registry,
	const FAvidScriptObjectHandle& Handle,
	UObject& Target,
	FProperty& Property,
	FAvidScriptBindingHostEffectPrepareResult& OutResult)
{
	OutResult = FAvidScriptBindingHostEffectPrepareResult();
	if (State != EAvidScriptHostEffectTransactionState::Open)
	{
		SetPrepareFailure(
			OutResult,
			TEXT("host_effect_transaction_closed"),
			FormatHandle(Handle),
			TEXT("The candidate host effect transaction has already completed."));
		return false;
	}

	FAvidScriptObjectHandleResult ResolveResult;
	UObject* ResolvedObject = Registry.ResolveObject(Handle, ResolveResult);
	if (ResolvedObject == nullptr)
	{
		SetPrepareFailure(
			OutResult,
			TEXT("host_effect_handle_invalid"),
			FormatHandle(Handle),
			ResolveResult.ErrorCategory.IsEmpty()
				? FString(TEXT("The candidate write handle is not live."))
				: ResolveResult.ErrorCategory);
		return false;
	}
	if (ResolvedObject != &Target)
	{
		SetPrepareFailure(
			OutResult,
			TEXT("host_effect_target_mismatch"),
			FormatHandle(Handle),
			TEXT("The invocation target does not match the registry object for this handle."));
		return false;
	}

	const UClass* OwnerClass = Cast<UClass>(Property.GetOwnerStruct());
	if (OwnerClass == nullptr || !Target.IsA(OwnerClass))
	{
		SetPrepareFailure(
			OutResult,
			TEXT("host_effect_target_type_mismatch"),
			Property.GetPathName(),
			TEXT("The reflected property does not belong to the invocation target."));
		return false;
	}

	const FEntryKey Key{
		Handle.ToUInt64(),
		EAvidScriptBindingReloadEffect::ReflectedProperty,
		&Property
	};
	if (CapturedKeys.Contains(Key))
	{
		OutResult.bSucceeded = true;
		return true;
	}

	FEntry Entry;
	Entry.Handle = Handle;
	Entry.Object = &Target;
	Entry.Effect = EAvidScriptBindingReloadEffect::ReflectedProperty;
	if (!Entry.OriginalProperty.Capture(Property, Target))
	{
		SetPrepareFailure(
			OutResult,
			TEXT("host_effect_property_capture_failed"),
			Property.GetPathName(),
			TEXT("The reflected property snapshot could not satisfy its size and alignment contract."));
		return false;
	}

	CapturedKeys.Add(Key);
	Entries.Add(MoveTemp(Entry));
	OutResult.bSucceeded = true;
	return true;
}

bool FAvidScriptHostEffectTransaction::Commit(FAvidScriptHostEffectTransactionResult& OutResult)
{
	OutResult = FAvidScriptHostEffectTransactionResult();
	if (State != EAvidScriptHostEffectTransactionState::Open)
	{
		SetClosedFailure(OutResult);
		return false;
	}

	OutResult.bSucceeded = true;
	OutResult.CapturedObjectCount = Entries.Num();
	State = EAvidScriptHostEffectTransactionState::Committed;
	CapturedKeys.Reset();
	Entries.Reset();
	return true;
}

bool FAvidScriptHostEffectTransaction::Rollback(
	FAvidScriptObjectRegistry& Registry,
	FAvidScriptHostEffectTransactionResult& OutResult)
{
	OutResult = FAvidScriptHostEffectTransactionResult();
	if (State != EAvidScriptHostEffectTransactionState::Open)
	{
		SetClosedFailure(OutResult);
		return false;
	}

	OutResult.CapturedObjectCount = Entries.Num();
	CopyFirstPrepareFailure(OutResult);
	for (int32 Index = Entries.Num() - 1; Index >= 0; --Index)
	{
		const FEntry& Entry = Entries[Index];
		FAvidScriptObjectHandleResult ResolveResult;
		UObject* ResolvedObject = Registry.ResolveObject(Entry.Handle, ResolveResult);
		UObject* CapturedObject = Entry.Object.Get();
		if (ResolvedObject == nullptr || CapturedObject == nullptr || ResolvedObject != CapturedObject)
		{
			RecordRestoreFailure(
				OutResult,
				Entry,
				ResolveResult.ErrorCategory.IsEmpty()
					? FString(TEXT("The captured object is no longer live under its original handle."))
					: ResolveResult.ErrorCategory);
			continue;
		}

		bool bRestored = false;
		if (Entry.Effect == EAvidScriptBindingReloadEffect::ActorTransform)
		{
			if (AActor* Actor = Cast<AActor>(CapturedObject))
			{
				Actor->SetActorTransform(
					Entry.OriginalTransform,
					false,
					nullptr,
					ETeleportType::TeleportPhysics);
				bRestored = Actor->GetActorTransform().Equals(Entry.OriginalTransform, 0.01);
			}
		}
		else if (Entry.Effect == EAvidScriptBindingReloadEffect::SceneComponentTransform)
		{
			if (USceneComponent* Component = Cast<USceneComponent>(CapturedObject))
			{
				Component->SetWorldTransform(
					Entry.OriginalTransform,
					false,
					nullptr,
					ETeleportType::TeleportPhysics);
				bRestored = Component->GetComponentTransform().Equals(Entry.OriginalTransform, 0.01);
			}
		}
		else if (Entry.Effect == EAvidScriptBindingReloadEffect::ReflectedProperty)
		{
			bRestored = Entry.OriginalProperty.Restore(*CapturedObject);
		}

		if (bRestored)
		{
			++OutResult.RestoredObjectCount;
		}
		else
		{
			RecordRestoreFailure(
				OutResult,
				Entry,
				TEXT("The host object did not accept its captured state."));
		}
	}

	State = EAvidScriptHostEffectTransactionState::RolledBack;
	CapturedKeys.Reset();
	Entries.Reset();
	OutResult.bSucceeded = OutResult.FailedObjectCount == 0;
	return OutResult.bSucceeded;
}

FString FAvidScriptHostEffectTransaction::FormatHandle(const FAvidScriptObjectHandle& Handle)
{
	return FString::Printf(TEXT("%u:%u"), Handle.Slot, Handle.Generation);
}

void FAvidScriptHostEffectTransaction::SetPrepareFailure(
	FAvidScriptBindingHostEffectPrepareResult& OutResult,
	const FString& Category,
	const FString& Source,
	const FString& Details)
{
	OutResult = FAvidScriptBindingHostEffectPrepareResult();
	OutResult.ErrorCategory = Category;
	OutResult.ErrorSource = Source;
	OutResult.ErrorDetails = Details;
	if (FirstPrepareErrorCategory.IsEmpty())
	{
		FirstPrepareErrorCategory = Category;
		FirstPrepareErrorSource = Source;
		FirstPrepareErrorDetails = Details;
	}
}

void FAvidScriptHostEffectTransaction::CopyFirstPrepareFailure(
	FAvidScriptHostEffectTransactionResult& OutResult) const
{
	if (!FirstPrepareErrorCategory.IsEmpty())
	{
		OutResult.ErrorCategory = FirstPrepareErrorCategory;
		OutResult.ErrorSource = FirstPrepareErrorSource;
		OutResult.ErrorDetails = FirstPrepareErrorDetails;
	}
}

void FAvidScriptHostEffectTransaction::SetClosedFailure(
	FAvidScriptHostEffectTransactionResult& OutResult)
{
	OutResult = FAvidScriptHostEffectTransactionResult();
	OutResult.ErrorCategory = TEXT("host_effect_transaction_closed");
	OutResult.ErrorSource = TEXT("transaction_state");
	OutResult.ErrorDetails = TEXT("A host effect transaction can commit or roll back exactly once.");
}

void FAvidScriptHostEffectTransaction::RecordRestoreFailure(
	FAvidScriptHostEffectTransactionResult& OutResult,
	const FEntry& Entry,
	const FString& Details)
{
	++OutResult.FailedObjectCount;
	if (OutResult.ErrorCategory.IsEmpty())
	{
		OutResult.ErrorCategory = TEXT("host_effect_restore_failed");
		OutResult.ErrorSource = FormatHandle(Entry.Handle);
		OutResult.ErrorDetails = Details;
	}
}
