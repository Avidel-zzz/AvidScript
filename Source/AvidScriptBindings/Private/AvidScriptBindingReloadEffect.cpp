#include "AvidScriptBindingReloadEffect.h"

const TCHAR* LexToString(const EAvidScriptBindingReloadEffect Effect)
{
	switch (Effect)
	{
	case EAvidScriptBindingReloadEffect::None:
		return TEXT("none");
	case EAvidScriptBindingReloadEffect::ActorTransform:
		return TEXT("actor_transform");
	case EAvidScriptBindingReloadEffect::SceneComponentTransform:
		return TEXT("scene_component_transform");
	case EAvidScriptBindingReloadEffect::ReflectedProperty:
		return TEXT("reflected_property");
	case EAvidScriptBindingReloadEffect::ContinuationProducer:
		return TEXT("continuation_producer");
	case EAvidScriptBindingReloadEffect::Unsupported:
		return TEXT("unsupported");
	default:
		return TEXT("unsupported");
	}
}

bool TryParseAvidScriptBindingReloadEffect(
	const FString& Value,
	EAvidScriptBindingReloadEffect& OutEffect)
{
	if (Value == TEXT("none"))
	{
		OutEffect = EAvidScriptBindingReloadEffect::None;
		return true;
	}
	if (Value == TEXT("actor_transform"))
	{
		OutEffect = EAvidScriptBindingReloadEffect::ActorTransform;
		return true;
	}
	if (Value == TEXT("scene_component_transform"))
	{
		OutEffect = EAvidScriptBindingReloadEffect::SceneComponentTransform;
		return true;
	}
	if (Value == TEXT("reflected_property"))
	{
		OutEffect = EAvidScriptBindingReloadEffect::ReflectedProperty;
		return true;
	}
	if (Value == TEXT("continuation_producer"))
	{
		OutEffect = EAvidScriptBindingReloadEffect::ContinuationProducer;
		return true;
	}
	if (Value == TEXT("unsupported"))
	{
		OutEffect = EAvidScriptBindingReloadEffect::Unsupported;
		return true;
	}
	return false;
}
