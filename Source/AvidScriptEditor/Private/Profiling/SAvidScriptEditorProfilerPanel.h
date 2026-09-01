#pragma once

#include "CoreMinimal.h"

class FAvidScriptEditorDebugTargetController;
class SWidget;

TSharedRef<SWidget> MakeAvidScriptEditorProfilerPanel(
	TSharedRef<FAvidScriptEditorDebugTargetController> Controller);
