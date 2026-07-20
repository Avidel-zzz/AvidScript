#pragma once

#include "AvidScriptBindingReloadEffect.h"

class UFunction;

class FAvidScriptEditorBindingReloadEffectPolicy
{
public:
	static EAvidScriptBindingReloadEffect Classify(const UFunction& Function);
};
