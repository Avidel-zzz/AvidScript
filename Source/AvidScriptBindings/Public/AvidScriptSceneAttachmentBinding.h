#pragma once

#include "AvidScriptBindingInvocationKind.h"
#include "AvidScriptObjectRegistry.h"
#include "Containers/ArrayView.h"
#include "CoreMinimal.h"

enum class EAvidScriptSceneAttachmentRule : uint8
{
	KeepRelative = 0,
	KeepWorld = 1,
	SnapToTarget = 2
};

enum class EAvidScriptSceneDetachmentRule : uint8
{
	KeepRelative = 0,
	KeepWorld = 1
};

struct FAvidScriptSceneAttachmentRules
{
	static constexpr uint32 WeldSimulatedBodiesBit = 1u << 2;

	static constexpr uint32 EncodeAttach(
		const EAvidScriptSceneAttachmentRule Rule,
		const bool bWeldSimulatedBodies)
	{
		return static_cast<uint32>(Rule)
			| (bWeldSimulatedBodies ? WeldSimulatedBodiesBit : 0u);
	}

	static constexpr uint32 EncodeDetach(
		const EAvidScriptSceneDetachmentRule Rule)
	{
		return static_cast<uint32>(Rule);
	}
};

struct FAvidScriptSceneAttachmentBindingSpec
{
	EAvidScriptBindingInvocationKind Kind =
		EAvidScriptBindingInvocationKind::SceneComponentAttach;
	FString StableId;
	FString ModuleName;
	FString ImportName;
	FString Signature;
};

class AVIDSCRIPTBINDINGS_API FAvidScriptSceneAttachmentBinding
{
public:
	static TConstArrayView<FAvidScriptSceneAttachmentBindingSpec> GetSpecs();

	static bool Attach(
		const FAvidScriptObjectRegistry& Registry,
		const FAvidScriptObjectHandle& ChildHandle,
		const FAvidScriptObjectHandle& ParentHandle,
		uint32 Rules,
		FAvidScriptObjectHandleResult& OutResult);

	static bool Detach(
		const FAvidScriptObjectRegistry& Registry,
		const FAvidScriptObjectHandle& ChildHandle,
		uint32 Rules,
		FAvidScriptObjectHandleResult& OutResult);
};
