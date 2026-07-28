#pragma once

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "CoreTypes.h"

enum class EAvidScriptGeneratedBindingShape : uint8
{
	I32PairToI32,
	PropertyI32GetSet,
	PropertyI32Get,
	PropertyI32Set,
	VectorValue,
	StableObjectRoundtrip
};

enum class EAvidScriptGeneratedReceiverMode : uint8
{
	SelfBound,
	StableBorrow
};

struct FAvidScriptGeneratedBindingIr
{
	FString StableId;
	FString OwnerModule;
	FString OwnerHeader;
	FString OwnerCppType;
	FString FunctionName;
	FString ImportModule;
	FString ImportName;
	FString AbiSignature;
	EAvidScriptGeneratedBindingShape Shape =
		EAvidScriptGeneratedBindingShape::I32PairToI32;
	EAvidScriptGeneratedReceiverMode ReceiverMode =
		EAvidScriptGeneratedReceiverMode::SelfBound;
	FString DescriptorIdentity;
};

struct FAvidScriptGeneratedBindingPackageIr
{
	int32 SchemaVersion = 1;
	FString PackageName;
	FString PackageHash;
	TArray<FAvidScriptGeneratedBindingIr> Bindings;
};
