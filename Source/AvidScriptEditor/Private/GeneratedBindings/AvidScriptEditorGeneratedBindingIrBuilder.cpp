#include "GeneratedBindings/AvidScriptEditorGeneratedBindingIrBuilder.h"

#include "AvidScriptBindingDescriptor.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

namespace
{
void SetIrFailure(
	FAvidScriptEditorGeneratedBindingResult& OutResult,
	const FString& Category,
	const FString& Source)
{
	OutResult = FAvidScriptEditorGeneratedBindingResult();
	OutResult.ErrorCategory = Category;
	OutResult.ErrorSource = Source;
	OutResult.ErrorMessage = FString::Printf(
		TEXT("Generated binding IR rejected | category=%s | source=%s"),
		*Category,
		Source.IsEmpty() ? TEXT("<none>") : *Source);
}

bool IsSafeIdentifier(const FString& Value)
{
	if (Value.IsEmpty()
		|| (!FChar::IsAlpha(Value[0]) && Value[0] != TEXT('_')))
	{
		return false;
	}
	for (const TCHAR Character : Value)
	{
		if (!FChar::IsAlnum(Character) && Character != TEXT('_'))
		{
			return false;
		}
	}
	return true;
}

bool IsSafeOwnerHeader(const FString& Value)
{
	if (Value.IsEmpty()
		|| Value.StartsWith(TEXT("/"))
		|| Value.StartsWith(TEXT("\\"))
		|| Value.Contains(TEXT(".."))
		|| Value.Contains(TEXT("\\"))
		|| Value.Contains(TEXT(":"))
		|| !Value.EndsWith(TEXT(".h"), ESearchCase::IgnoreCase))
	{
		return false;
	}
	for (const TCHAR Character : Value)
	{
		if (!FChar::IsAlnum(Character)
			&& Character != TEXT('_')
			&& Character != TEXT('-')
			&& Character != TEXT('/')
			&& Character != TEXT('.'))
		{
			return false;
		}
	}
	return true;
}

bool TryParseShape(
	const FString& Value,
	EAvidScriptGeneratedBindingShape& OutShape)
{
	if (Value == TEXT("i32_pair_to_i32"))
	{
		OutShape = EAvidScriptGeneratedBindingShape::I32PairToI32;
		return true;
	}
	if (Value == TEXT("property_i32_get_set"))
	{
		OutShape = EAvidScriptGeneratedBindingShape::PropertyI32GetSet;
		return true;
	}
	if (Value == TEXT("vector_value"))
	{
		OutShape = EAvidScriptGeneratedBindingShape::VectorValue;
		return true;
	}
	if (Value == TEXT("stable_object_roundtrip"))
	{
		OutShape = EAvidScriptGeneratedBindingShape::StableObjectRoundtrip;
		return true;
	}
	return false;
}

bool TryParseReceiverMode(
	const FString& Value,
	EAvidScriptGeneratedReceiverMode& OutMode)
{
	if (Value == TEXT("self_bound"))
	{
		OutMode = EAvidScriptGeneratedReceiverMode::SelfBound;
		return true;
	}
	if (Value == TEXT("stable_borrow"))
	{
		OutMode = EAvidScriptGeneratedReceiverMode::StableBorrow;
		return true;
	}
	return false;
}
} // namespace

bool FAvidScriptEditorGeneratedBindingIrBuilder::Build(
	const FString& DescriptorJson,
	FAvidScriptGeneratedBindingPackageIr& OutPackage,
	FAvidScriptEditorGeneratedBindingResult& OutResult)
{
	OutPackage = FAvidScriptGeneratedBindingPackageIr();
	OutResult = FAvidScriptEditorGeneratedBindingResult();

	FAvidScriptBindingPackageModel Descriptor;
	FString ParseCategory;
	FString ParseSource;
	if (!FAvidScriptBindingDescriptorParser::Parse(
			DescriptorJson,
			Descriptor,
			ParseCategory,
			ParseSource))
	{
		SetIrFailure(
			OutResult,
			TEXT("generated_descriptor_invalid:") + ParseCategory,
			ParseSource);
		return false;
	}

	OutPackage.PackageName = Descriptor.PackageName;
	OutPackage.PackageHash = Descriptor.PackageHash;
	for (const FAvidScriptBindingFunctionModel& Binding : Descriptor.Bindings)
	{
		if (Binding.DispatchMode != TEXT("generated_native_s1"))
		{
			continue;
		}

		UClass* OwnerClass = LoadObject<UClass>(nullptr, *Binding.OwnerClass);
		UFunction* Function = OwnerClass == nullptr
			? nullptr
			: OwnerClass->FindFunctionByName(*Binding.UeMember);
		if (OwnerClass == nullptr || Function == nullptr
			|| Function->GetOwnerClass() != OwnerClass)
		{
			SetIrFailure(
				OutResult,
				TEXT("generated_owner_or_function_missing"),
				Binding.OwnerClass + TEXT(".") + Binding.UeMember);
			return false;
		}
		if (OwnerClass->HasAnyClassFlags(
				CLASS_CompiledFromBlueprint | CLASS_Interface))
		{
			SetIrFailure(
				OutResult,
				TEXT("generated_owner_unsupported"),
				Binding.OwnerClass);
			return false;
		}

		FAvidScriptGeneratedBindingIr Ir;
		Ir.StableId = Binding.StableId;
		Ir.OwnerModule = OwnerClass->GetOutermost()->GetName();
		Ir.OwnerModule.RemoveFromStart(TEXT("/Script/"));
		Ir.OwnerHeader = OwnerClass->GetMetaData(TEXT("ModuleRelativePath"));
		Ir.OwnerCppType =
			FString(OwnerClass->GetPrefixCPP()) + OwnerClass->GetName();
		Ir.FunctionName = Function->GetName();
		Ir.ImportModule = Binding.HostImport.Module;
		Ir.ImportName = Binding.GeneratedImportName;
		Ir.AbiSignature = Binding.HostImport.Signature;
		Ir.DescriptorIdentity = Binding.CanonicalIdentity;
		if (!IsSafeIdentifier(Ir.OwnerModule)
			|| !IsSafeIdentifier(Ir.OwnerCppType)
			|| !IsSafeIdentifier(Ir.FunctionName)
			|| !IsSafeIdentifier(Ir.ImportName)
			|| !IsSafeOwnerHeader(Ir.OwnerHeader)
			|| !TryParseShape(Binding.GeneratedShape, Ir.Shape)
			|| !TryParseReceiverMode(
				Binding.GeneratedReceiverMode,
				Ir.ReceiverMode))
		{
			SetIrFailure(
				OutResult,
				TEXT("generated_owner_identity_invalid"),
				Binding.CanonicalIdentity);
			return false;
		}
		OutPackage.Bindings.Add(MoveTemp(Ir));
	}

	OutPackage.Bindings.Sort([](
		const FAvidScriptGeneratedBindingIr& Left,
		const FAvidScriptGeneratedBindingIr& Right)
	{
		return Left.StableId.Compare(
			Right.StableId,
			ESearchCase::CaseSensitive) < 0;
	});
	for (int32 Index = 1; Index < OutPackage.Bindings.Num(); ++Index)
	{
		if (OutPackage.Bindings[Index - 1].StableId
			== OutPackage.Bindings[Index].StableId)
		{
			SetIrFailure(
				OutResult,
				TEXT("generated_binding_duplicate"),
				OutPackage.Bindings[Index].StableId);
			OutPackage = FAvidScriptGeneratedBindingPackageIr();
			return false;
		}
	}

	OutResult.bSucceeded = true;
	OutResult.BindingCount = OutPackage.Bindings.Num();
	OutResult.PackageHash = OutPackage.PackageHash;
	return true;
}
