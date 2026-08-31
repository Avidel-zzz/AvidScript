#pragma once

#include "CoreMinimal.h"

class FProperty;
class UClass;
class UFunction;

enum class EAvidScriptGeneratedMemberKind : uint8
{
	Invalid,
	Property,
	Function,
};

struct AVIDSCRIPTRUNTIME_API FAvidScriptGeneratedMemberPlan
{
	uint32 MemberOrdinal = 0;
	EAvidScriptGeneratedMemberKind Kind = EAvidScriptGeneratedMemberKind::Invalid;
	FString StableMemberId;
	FProperty* Property = nullptr;
	UFunction* Function = nullptr;
	FString GetterImportName;
	FString SetterImportName;
	FString ExportName;
	bool bLifecycle = false;
};

struct AVIDSCRIPTRUNTIME_API FAvidScriptGeneratedTypePlan
{
	uint32 TypeOrdinal = 0;
	FString StableTypeId;
	UClass* Class = nullptr;
	TArray<FAvidScriptGeneratedMemberPlan> Members;

	const FAvidScriptGeneratedMemberPlan* FindMember(uint32 MemberOrdinal) const;
};

class AVIDSCRIPTRUNTIME_API FAvidScriptGeneratedTypeRegistrySnapshot final
{
public:
	const FAvidScriptGeneratedTypePlan* FindTypeByOrdinal(uint32 TypeOrdinal) const;
	const FAvidScriptGeneratedTypePlan* FindTypeByStableId(const FString& StableTypeId) const;
	const FAvidScriptGeneratedTypePlan* FindTypeByClass(const UClass* Class) const;
	TConstArrayView<FAvidScriptGeneratedTypePlan> GetTypes() const;
	int32 Num() const;

private:
	friend class FAvidScriptGeneratedTypeRegistry;

	TArray<FAvidScriptGeneratedTypePlan> Types;
	TMap<FString, uint32> TypeOrdinalsByStableId;
	TMap<const UClass*, uint32> TypeOrdinalsByClass;
};

class AVIDSCRIPTRUNTIME_API FAvidScriptGeneratedTypeRegistry final
{
public:
	static constexpr int32 ManifestSchemaVersion = 6;
	static constexpr int32 MaxTypeCount = 4096;
	static constexpr int32 MaxMemberCountPerType = 65536;

	static bool BuildFromJson(
		const FString& Json,
		TSharedPtr<const FAvidScriptGeneratedTypeRegistrySnapshot>& OutSnapshot,
		FString& OutError);
};
