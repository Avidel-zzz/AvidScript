param(
    [int]$Slot = 1,
    [int]$Generation = 1,
    [double]$X = 123.0,
    [double]$Y = 456.0,
    [double]$Z = 789.0,
    [string]$OutputPath = ""
)

$ErrorActionPreference = "Stop"
$Culture = [System.Globalization.CultureInfo]::InvariantCulture

function Get-RequiredTool {
    param([Parameter(Mandatory = $true)][string]$Name)
    return Get-Command $Name -ErrorAction SilentlyContinue | Select-Object -First 1
}

function Format-FloatLiteral {
    param([Parameter(Mandatory = $true)][double]$Value)
    $Text = $Value.ToString("R", $Culture)
    if (-not $Text.Contains(".")) {
        $Text = "$Text.0"
    }
    return "${Text}f"
}

$BuildDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$PluginRoot = Split-Path -Parent $BuildDir
$ProjectPluginsDir = Split-Path -Parent $PluginRoot
$ProjectRoot = Split-Path -Parent $ProjectPluginsDir
$GuestRoot = Join-Path $ProjectRoot "Saved\AvidScriptDGuest"
$GeneratedRoot = Join-Path $GuestRoot "Generated"

if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path $GuestRoot "actor_set_location_guest.wasm"
}

New-Item -ItemType Directory -Force -Path $GeneratedRoot | Out-Null

if (Test-Path -LiteralPath $OutputPath) {
    Remove-Item -LiteralPath $OutputPath -Force
}

$GeneratedSourcePath = Join-Path $GeneratedRoot "actor_set_location_guest.generated.d"
$XLiteral = Format-FloatLiteral -Value $X
$YLiteral = Format-FloatLiteral -Value $Y
$ZLiteral = Format-FloatLiteral -Value $Z

$GeneratedSource = @"
module actor_set_location_guest_generated;

extern(C) @nogc nothrow
{
    int actor_set_location(int slot, int generation, float x, float y, float z);

    export void avid_on_begin_play()
    {
        cast(void)actor_set_location($Slot, $Generation, $XLiteral, $YLiteral, $ZLiteral);
    }

    export void avid_on_tick(float delta_seconds)
    {
        cast(void)delta_seconds;
    }
}
"@

Set-Content -LiteralPath $GeneratedSourcePath -Value $GeneratedSource -Encoding ASCII
Write-Output "[AvidScript][DGuest][Build] generated_source=$GeneratedSourcePath"

$Ldc2 = Get-RequiredTool -Name "ldc2"
$WasmLd = Get-RequiredTool -Name "wasm-ld"

$MissingTools = @()
if ($null -eq $Ldc2) {
    $MissingTools += "ldc2"
}
if ($null -eq $WasmLd) {
    $MissingTools += "wasm-ld"
}

if ($MissingTools.Count -gt 0) {
    Write-Output "[AvidScript][DGuest][Build] result=missing_toolchain missing=$($MissingTools -join ',') output=$OutputPath"
    exit 0
}

$Arguments = @(
    "-betterC",
    "-mtriple=wasm32-unknown-unknown-wasm",
    "-defaultlib=",
    "-of=$OutputPath",
    "-L--no-entry",
    "-L--allow-undefined",
    "-L--export=avid_on_begin_play",
    "-L--export=avid_on_tick",
    $GeneratedSourcePath
)

Write-Output "[AvidScript][DGuest][Build] compiler=$($Ldc2.Source)"
Write-Output "[AvidScript][DGuest][Build] linker=$($WasmLd.Source)"
Write-Output "[AvidScript][DGuest][Build] output=$OutputPath"

$CompilerOutput = & $Ldc2.Source @Arguments 2>&1
$ExitCode = $LASTEXITCODE

foreach ($Line in $CompilerOutput) {
    Write-Output "[AvidScript][DGuest][Compiler] $Line"
}

if ($ExitCode -ne 0) {
    Write-Output "[AvidScript][DGuest][Build] result=compiler_failed exit_code=$ExitCode"
    exit $ExitCode
}

if (-not (Test-Path -LiteralPath $OutputPath)) {
    Write-Output "[AvidScript][DGuest][Build] result=artifact_missing output=$OutputPath"
    exit 1
}

$Artifact = Get-Item -LiteralPath $OutputPath
Write-Output "[AvidScript][DGuest][Build] result=built output=$OutputPath bytes=$($Artifact.Length)"
exit 0
