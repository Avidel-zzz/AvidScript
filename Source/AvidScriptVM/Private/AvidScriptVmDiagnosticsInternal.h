#pragma once

struct FAvidScriptVmMemorySnapshot;

namespace AvidScriptVmDiagnosticsInternal
{
void CaptureArtifactMemory(FAvidScriptVmMemorySnapshot& OutSnapshot);
}
