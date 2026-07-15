param(
    [string]$DotNetPath = "C:\Users\user0\.dotnet\dotnet.exe"
)

$ErrorActionPreference = "Stop"
$TestDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ToolsRoot = Split-Path -Parent $TestDir
$PluginRoot = Split-Path -Parent $ToolsRoot
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PluginRoot)
$BuildScript = Join-Path $PluginRoot "Build\BuildCSharpActorLifecycle.ps1"
$RunRoot = Join-Path $PluginRoot "Saved\AvidScriptFrontendDotNet\BuildIntegration"
$Utf8 = [System.Text.UTF8Encoding]::new($false)

function Assert-Condition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) {
        throw $Message
    }
}

function Resolve-ArtifactPath {
    param([string]$Path)
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $ProjectRoot $Path))
}

function Get-Sha256Hex {
    param([string]$Path)
    $Stream = [System.IO.File]::OpenRead($Path)
    try {
        $Sha256 = [System.Security.Cryptography.SHA256]::Create()
        try {
            $HashBytes = $Sha256.ComputeHash($Stream)
        }
        finally {
            $Sha256.Dispose()
        }
    }
    finally {
        $Stream.Dispose()
    }
    return [System.BitConverter]::ToString($HashBytes).Replace("-", "").ToLowerInvariant()
}

New-Item -ItemType Directory -Force -Path $RunRoot | Out-Null

$BrokenRoot = Join-Path $RunRoot "Broken"
New-Item -ItemType Directory -Force -Path $BrokenRoot | Out-Null
$BrokenSource = Join-Path $BrokenRoot "BrokenScript.cs"
$BrokenReport = Join-Path $BrokenRoot "broken.csharp.report.json"
$BrokenManifest = Join-Path $BrokenRoot "broken.avidscript.json"
$BrokenText = @'
public static class BrokenScript
{
    public static void BeginPlay()
    {
        Actor.SetLocation(1.0f, 2.0f, 3.0f);
    }

    public static void Tick(float deltaSeconds)
    {
        Actor.SetLocation(deltaSeconds, 0.0f, 0.0f);
    }
'@
[System.IO.File]::WriteAllText($BrokenSource, $BrokenText, $Utf8)

& $BuildScript `
    -DotNetPath $DotNetPath `
    -OutputRoot $BrokenRoot `
    -SourcePath $BrokenSource `
    -ModuleId "p39_broken" `
    -ArtifactStem "broken" `
    -ReportPath $BrokenReport `
    -ManifestPath $BrokenManifest | Out-Null
$BrokenExit = $LASTEXITCODE
Assert-Condition ($BrokenExit -eq 1) "syntax errors must return exit code 1; actual=$BrokenExit"
Assert-Condition (Test-Path -LiteralPath $BrokenReport -PathType Leaf) "syntax failure report is missing"
$BrokenJson = Get-Content -Raw -LiteralPath $BrokenReport | ConvertFrom-Json
Assert-Condition ($BrokenJson.result -eq "frontend_failed") "syntax failure report result is not frontend_failed"
Assert-Condition (@($BrokenJson.diagnostics | Where-Object severity -eq "error").Count -gt 0) "syntax failure report has no error diagnostic"
Assert-Condition (-not (Test-Path -LiteralPath $BrokenManifest -PathType Leaf)) "syntax failure must not produce a manifest"
Assert-Condition (-not (Test-Path -LiteralPath (Join-Path $BrokenRoot "broken.csharp_adapter.wasm") -PathType Leaf)) "syntax failure must remove stale adapter WASM"
$BrokenFrontendPath = Resolve-ArtifactPath $BrokenJson.artifacts.frontend_file
Assert-Condition (Test-Path -LiteralPath $BrokenFrontendPath -PathType Leaf) "syntax failure frontend artifact is missing"

$SemanticBrokenRoot = Join-Path $RunRoot "SemanticBroken"
New-Item -ItemType Directory -Force -Path $SemanticBrokenRoot | Out-Null
$SemanticBrokenSource = Join-Path $SemanticBrokenRoot "SemanticBrokenScript.cs"
$SemanticBrokenReport = Join-Path $SemanticBrokenRoot "semantic_broken.csharp.report.json"
$SemanticBrokenManifest = Join-Path $SemanticBrokenRoot "semantic_broken.avidscript.json"
$SemanticBrokenWasm = Join-Path $SemanticBrokenRoot "semantic_broken.csharp_adapter.wasm"
$SemanticBrokenDotNetWasm = Join-Path $SemanticBrokenRoot "semantic_broken.dotnet.wasm"
$SemanticBrokenGuestIr = Join-Path $SemanticBrokenRoot "semantic_broken.guestir.json"
$SemanticBrokenFinalWasm = Join-Path $SemanticBrokenRoot "semantic_broken.wasm"
$SemanticBrokenText = @"
public static class SemanticBrokenScript
{
    public static void BeginPlay()
    {
        int value = "bad";
    }

    public static void Tick(float deltaSeconds)
    {
    }
}
"@
[System.IO.File]::WriteAllText($SemanticBrokenSource, $SemanticBrokenText, $Utf8)
[System.IO.File]::WriteAllText($SemanticBrokenManifest, "stale", $Utf8)
[System.IO.File]::WriteAllBytes($SemanticBrokenWasm, [byte[]]@(0, 97, 115, 109))
[System.IO.File]::WriteAllBytes($SemanticBrokenDotNetWasm, [byte[]]@(0, 97, 115, 109))
[System.IO.File]::WriteAllText($SemanticBrokenGuestIr, "stale", $Utf8)
[System.IO.File]::WriteAllBytes($SemanticBrokenFinalWasm, [byte[]]@(0, 97, 115, 109))

& $BuildScript `
    -DotNetPath $DotNetPath `
    -OutputRoot $SemanticBrokenRoot `
    -SourcePath $SemanticBrokenSource `
    -ModuleId "p40_semantic_broken" `
    -ArtifactStem "semantic_broken" `
    -ReportPath $SemanticBrokenReport `
    -ManifestPath $SemanticBrokenManifest | Out-Null
$SemanticBrokenExit = $LASTEXITCODE
Assert-Condition ($SemanticBrokenExit -eq 1) "semantic errors must return exit code 1; actual=$SemanticBrokenExit"
Assert-Condition (Test-Path -LiteralPath $SemanticBrokenReport -PathType Leaf) "semantic failure report is missing"
$SemanticBrokenJson = Get-Content -Raw -LiteralPath $SemanticBrokenReport | ConvertFrom-Json
Assert-Condition ($SemanticBrokenJson.result -eq "semantic_failed") "semantic failure report result is not semantic_failed"
Assert-Condition (-not [string]::IsNullOrWhiteSpace([string]$SemanticBrokenJson.artifacts.semantic_file)) "semantic failure report does not reference a semantic artifact"
$SemanticBrokenArtifact = Resolve-ArtifactPath $SemanticBrokenJson.artifacts.semantic_file
Assert-Condition (Test-Path -LiteralPath $SemanticBrokenArtifact -PathType Leaf) "semantic failure artifact is missing"
$SemanticBrokenArtifactJson = Get-Content -Raw -LiteralPath $SemanticBrokenArtifact | ConvertFrom-Json
Assert-Condition (-not $SemanticBrokenArtifactJson.succeeded) "semantic failure artifact reports success"
Assert-Condition (@($SemanticBrokenJson.diagnostics | Where-Object code -eq "CS0029").Count -eq 1) "semantic failure report did not retain CS0029"
Assert-Condition (-not (Test-Path -LiteralPath $SemanticBrokenManifest -PathType Leaf)) "semantic failure must remove stale manifest"
Assert-Condition (-not (Test-Path -LiteralPath $SemanticBrokenWasm -PathType Leaf)) "semantic failure must remove stale adapter WASM"
Assert-Condition (-not (Test-Path -LiteralPath $SemanticBrokenDotNetWasm -PathType Leaf)) "semantic failure must remove stale dotnet WASM"
Assert-Condition (-not (Test-Path -LiteralPath $SemanticBrokenGuestIr -PathType Leaf)) "semantic failure must remove stale Guest IR"
Assert-Condition (-not (Test-Path -LiteralPath $SemanticBrokenFinalWasm -PathType Leaf)) "semantic failure must remove stale final WASM"

$NormalRoot = Join-Path $RunRoot "Normal"
$NormalReport = Join-Path $NormalRoot "normal.csharp.report.json"
$NormalManifest = Join-Path $NormalRoot "normal.avidscript.json"
$NormalSource = Join-Path $PluginRoot "Samples\CSharp\ActorLifecycle\ActorLifecycleScript.cs"
New-Item -ItemType Directory -Force -Path $NormalRoot | Out-Null

& $BuildScript `
    -DotNetPath $DotNetPath `
    -OutputRoot $NormalRoot `
    -SourcePath $NormalSource `
    -ModuleId "p39_normal" `
    -ArtifactStem "normal" `
    -ReportPath $NormalReport `
    -ManifestPath $NormalManifest | Out-Null
$NormalExit = $LASTEXITCODE
Assert-Condition ($NormalExit -eq 0) "valid source build failed; exit=$NormalExit"
$NormalJson = Get-Content -Raw -LiteralPath $NormalReport | ConvertFrom-Json
Assert-Condition ($NormalJson.result -eq "direct_abi_built") "valid source did not build direct ABI"
$NormalFrontendPath = Resolve-ArtifactPath $NormalJson.artifacts.frontend_file
Assert-Condition (Test-Path -LiteralPath $NormalFrontendPath -PathType Leaf) "valid source frontend artifact is missing"
$FrontendJson = Get-Content -Raw -LiteralPath $NormalFrontendPath | ConvertFrom-Json
Assert-Condition ($FrontendJson.source.sha256 -eq $NormalJson.source.sha256) "report/frontend source hashes differ"
$NormalSemanticPath = Resolve-ArtifactPath $NormalJson.artifacts.semantic_file
Assert-Condition (Test-Path -LiteralPath $NormalSemanticPath -PathType Leaf) "valid source semantic artifact is missing"
$SemanticJson = Get-Content -Raw -LiteralPath $NormalSemanticPath | ConvertFrom-Json
Assert-Condition ($SemanticJson.schema_version -eq 4) "semantic artifact schema version is not 4"
Assert-Condition ($SemanticJson.semantic_version -eq "1.4") "semantic artifact version is not 1.4"
Assert-Condition ($SemanticJson.succeeded) "valid source semantic artifact reports failure"
Assert-Condition ($SemanticJson.source.sha256 -eq $FrontendJson.source.sha256) "semantic/frontend source hashes differ"
Assert-Condition ($SemanticJson.source.frontend_sha256 -eq $FrontendJson.source.sha256) "semantic artifact did not preserve the frontend source hash"
Assert-Condition (@($SemanticJson.callables).Count -eq 53) "ActorLifecycle semantic callable count is not 53"
Assert-Condition (@($SemanticJson.callables | Where-Object { $null -ne $_.import }).Count -eq 14) "ActorLifecycle semantic import count is not 14"
Assert-Condition (@($SemanticJson.callables | Where-Object { $null -ne $_.export }).Count -eq 6) "ActorLifecycle semantic export count is not 6"
Assert-Condition ($NormalJson.semantic.source_sha256 -eq $FrontendJson.source.sha256) "report semantic source hash differs"
Assert-Condition ($NormalJson.semantic.frontend_sha256 -eq $FrontendJson.source.sha256) "report semantic frontend hash differs"
Assert-Condition ($NormalJson.source.script_type -eq "ActorLifecycleScript") "report does not identify the AST-selected script type"
Assert-Condition (-not [string]::IsNullOrWhiteSpace([string]$NormalJson.artifacts.guest_ir_file)) "report does not reference Guest IR"
$NormalGuestIrPath = Resolve-ArtifactPath $NormalJson.artifacts.guest_ir_file
$NormalWasmPath = Resolve-ArtifactPath $NormalJson.artifacts.wasm_file
Assert-Condition (Test-Path -LiteralPath $NormalGuestIrPath -PathType Leaf) "valid source Guest IR artifact is missing"
Assert-Condition (Test-Path -LiteralPath $NormalWasmPath -PathType Leaf) "valid source WASM artifact is missing"
$GuestIrJson = Get-Content -Raw -LiteralPath $NormalGuestIrPath | ConvertFrom-Json
$SemanticSha256 = Get-Sha256Hex $NormalSemanticPath
$GuestIrSha256 = Get-Sha256Hex $NormalGuestIrPath
$WasmSha256 = Get-Sha256Hex $NormalWasmPath
Assert-Condition ($GuestIrJson.schema_version -eq 1 -and $GuestIrJson.ir_version -eq "1.0" -and $GuestIrJson.succeeded) "Guest IR contract is invalid"
Assert-Condition ($GuestIrJson.provenance.semantic_sha256 -eq $SemanticSha256) "Guest IR semantic provenance hash differs"
Assert-Condition ($NormalJson.guest_ir.schema_version -eq 1 -and $NormalJson.guest_ir.version -eq "1.0") "report Guest IR contract is invalid"
Assert-Condition ($NormalJson.guest_ir.semantic_sha256 -eq $SemanticSha256) "report Guest IR semantic hash differs"
Assert-Condition ($NormalJson.guest_ir.sha256 -eq $GuestIrSha256) "report Guest IR artifact hash differs"
Assert-Condition ($NormalJson.wasm.sha256 -eq $WasmSha256) "report WASM artifact hash differs"
Assert-Condition (@($NormalJson.observed_exports).Count -eq 6 -and $NormalJson.observed_exports -contains "avid_on_gameplay_event") "report does not expose all six direct ABI exports"
$ManifestJson = Get-Content -Raw -LiteralPath $NormalManifest | ConvertFrom-Json
Assert-Condition ($ManifestJson.source.sha256 -eq $FrontendJson.source.sha256) "manifest/frontend source hashes differ"
Assert-Condition ($ManifestJson.source.script_type -eq "ActorLifecycleScript") "manifest does not identify the AST-selected script type"
Assert-Condition (-not [string]::IsNullOrWhiteSpace($ManifestJson.source.frontend_file)) "manifest does not reference the frontend artifact"
Assert-Condition (-not [string]::IsNullOrWhiteSpace($ManifestJson.source.semantic_file)) "manifest does not reference the semantic artifact"
Assert-Condition ($ManifestJson.source.semantic_sha256 -eq $SemanticSha256) "manifest semantic hash differs"
Assert-Condition ($ManifestJson.guest_ir.file -eq $NormalJson.artifacts.guest_ir_file) "manifest Guest IR path differs"
Assert-Condition ($ManifestJson.guest_ir.schema_version -eq 1 -and $ManifestJson.guest_ir.version -eq "1.0") "manifest Guest IR contract is invalid"
Assert-Condition ($ManifestJson.guest_ir.sha256 -eq $GuestIrSha256) "manifest Guest IR hash differs"
Assert-Condition ($ManifestJson.wasm.sha256 -eq $WasmSha256) "manifest WASM hash differs"
Assert-Condition ($ManifestJson.toolchain.compiler -eq "avidscript-csharp-guest-wasm") "manifest does not identify the formal compiler chain"

Write-Output "AvidScript.CSharpFrontend.BuildIntegration: 3/3 passed"
