#pragma once

#include "AvidScriptEditorCSharpBuildService.h"

class FAvidScriptEditorCSharpBuildInvoker
{
public:
	static bool BuildOnce(
		const FAvidScriptEditorCSharpBuildConfig& Config,
		FAvidScriptEditorCSharpBuildResult& OutResult);
};
