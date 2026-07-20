#include "BindingGeneration/AvidScriptEditorBindingReloadEffectPolicy.h"

#include "UObject/Class.h"

EAvidScriptBindingReloadEffect FAvidScriptEditorBindingReloadEffectPolicy::Classify(
	const UFunction& Function)
{
	if (Function.HasAnyFunctionFlags(FUNC_Const | FUNC_BlueprintPure))
	{
		return EAvidScriptBindingReloadEffect::None;
	}

	const FString FunctionPath = Function.GetPathName();
	static const TSet<FString> ActorTransformFunctions = {
		TEXT("/Script/Engine.Actor:K2_SetActorLocation"),
		TEXT("/Script/Engine.Actor:K2_AddActorWorldOffset"),
		TEXT("/Script/Engine.Actor:K2_SetActorRotation"),
		TEXT("/Script/Engine.Actor:SetActorScale3D")
	};
	if (ActorTransformFunctions.Contains(FunctionPath))
	{
		return EAvidScriptBindingReloadEffect::ActorTransform;
	}

	static const TSet<FString> SceneComponentTransformFunctions = {
		TEXT("/Script/Engine.SceneComponent:K2_SetWorldLocation")
	};
	if (SceneComponentTransformFunctions.Contains(FunctionPath))
	{
		return EAvidScriptBindingReloadEffect::SceneComponentTransform;
	}

	return EAvidScriptBindingReloadEffect::Unsupported;
}
