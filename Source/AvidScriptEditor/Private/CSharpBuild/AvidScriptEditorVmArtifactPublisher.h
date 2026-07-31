#pragma once

#include "AvidScriptEditorCSharpBuildService.h"

class FAvidScriptEditorVmArtifactPublisher
{
public:
	static FString MakeArtifactPath(
		const FAvidScriptEditorCSharpBuildConfig& Config);

	static bool Publish(
		const FAvidScriptEditorCSharpBuildConfig& Config,
		FAvidScriptEditorCSharpBuildResult& OutResult);
};
