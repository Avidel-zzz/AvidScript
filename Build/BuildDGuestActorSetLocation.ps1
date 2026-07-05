param(
    [int]$Slot = 1,
    [int]$Generation = 1,
    [double]$X = 123.0,
    [double]$Y = 456.0,
    [double]$Z = 789.0,
    [string]$OutputPath = "",
    [string]$ManifestPath = "",
    [string]$Ldc2Path = "",
    [string]$ToolchainRoot = ""
)

$ErrorActionPreference = "Stop"
$Culture = [System.Globalization.CultureInfo]::InvariantCulture

function Get-OptionalTool {
    param([Parameter(Mandatory = $true)][string]$Name)
    return Get-Command $Name -ErrorAction SilentlyContinue | Select-Object -First 1
}

function New-ToolchainCandidate {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Path
    )

    return [PSCustomObject]@{
        Source = $Source
        Path = $Path
    }
}

function Resolve-ExistingFilePath {
    param([Parameter(Mandatory = $true)][string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return $null
    }

    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        return (Resolve-Path -LiteralPath $Path).Path
    }

    return $null
}

function Resolve-Ldc2Tool {
    $Candidates = @()

    if (-not [string]::IsNullOrWhiteSpace($Ldc2Path)) {
        $Candidates += New-ToolchainCandidate -Source "parameter" -Path $Ldc2Path
    }

    if (-not [string]::IsNullOrWhiteSpace($ToolchainRoot)) {
        $Candidates += New-ToolchainCandidate -Source "toolchain_root" -Path (Join-Path $ToolchainRoot "bin\ldc2.exe")
    }

    if (-not [string]::IsNullOrWhiteSpace($env:AVIDSCRIPT_LDC2)) {
        $Candidates += New-ToolchainCandidate -Source "env:AVIDSCRIPT_LDC2" -Path $env:AVIDSCRIPT_LDC2
    }

    if (-not [string]::IsNullOrWhiteSpace($env:AVIDSCRIPT_D_TOOLCHAIN_ROOT)) {
        $Candidates += New-ToolchainCandidate -Source "env:AVIDSCRIPT_D_TOOLCHAIN_ROOT" -Path (Join-Path $env:AVIDSCRIPT_D_TOOLCHAIN_ROOT "bin\ldc2.exe")
    }

    $Checked = @()
    foreach ($Candidate in $Candidates) {
        $Checked += "$($Candidate.Source):$($Candidate.Path)"
        $ResolvedPath = Resolve-ExistingFilePath -Path $Candidate.Path
        if ($null -ne $ResolvedPath) {
            return [PSCustomObject]@{
                Found = $true
                Source = $Candidate.Source
                Path = $ResolvedPath
                Checked = $Checked
            }
        }
    }

    $PathCommand = Get-OptionalTool -Name "ldc2"
    $Checked += "PATH:ldc2"
    if ($null -ne $PathCommand) {
        return [PSCustomObject]@{
            Found = $true
            Source = "PATH"
            Path = $PathCommand.Source
            Checked = $Checked
        }
    }

    return [PSCustomObject]@{
        Found = $false
        Source = ""
        Path = ""
        Checked = $Checked
    }
}

function Convert-ToProjectRelativePath {
    param([Parameter(Mandatory = $true)][string]$Path)

    $FullPath = [System.IO.Path]::GetFullPath($Path)
    $RootPath = [System.IO.Path]::GetFullPath($ProjectRoot)
    $PathSeparators = @([System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar)
    $RootPath = $RootPath.TrimEnd($PathSeparators)

    if ($FullPath.StartsWith($RootPath, [System.StringComparison]::OrdinalIgnoreCase)) {
        $RelativePath = $FullPath.Substring($RootPath.Length)
        $RelativePath = $RelativePath.TrimStart($PathSeparators)
        return $RelativePath.Replace("\", "/")
    }

    return $FullPath
}

function Get-Ldc2Version {
    param([Parameter(Mandatory = $true)][string]$Path)

    try {
        $VersionOutput = & $Path --version 2>&1
        $FirstLine = $VersionOutput | Select-Object -First 1
        if ($FirstLine -match "\(([^)]+)\)") {
            return $Matches[1]
        }

        return [string]$FirstLine
    }
    catch {
        return "unknown"
    }
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

if ([string]::IsNullOrWhiteSpace($ManifestPath)) {
    $ManifestPath = [System.IO.Path]::ChangeExtension($OutputPath, ".avidscript.json")
}

New-Item -ItemType Directory -Force -Path $GeneratedRoot | Out-Null

$ManifestDirectory = Split-Path -Parent $ManifestPath
if (-not [string]::IsNullOrWhiteSpace($ManifestDirectory)) {
    New-Item -ItemType Directory -Force -Path $ManifestDirectory | Out-Null
}

if (Test-Path -LiteralPath $OutputPath) {
    Remove-Item -LiteralPath $OutputPath -Force
}

if (Test-Path -LiteralPath $ManifestPath) {
    Remove-Item -LiteralPath $ManifestPath -Force
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

$Ldc2 = Resolve-Ldc2Tool
$WasmLd = Get-OptionalTool -Name "wasm-ld"

if (-not $Ldc2.Found) {
    Write-Output "[AvidScript][DGuest][Toolchain] ldc2=MISSING checked=$($Ldc2.Checked -join ';')"
    Write-Output "[AvidScript][DGuest][Build] result=missing_toolchain missing=ldc2 output=$OutputPath"
    exit 0
}

Write-Output "[AvidScript][DGuest][Toolchain] ldc2=FOUND source=$($Ldc2.Source) path=$($Ldc2.Path)"

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

Write-Output "[AvidScript][DGuest][Build] compiler=$($Ldc2.Path)"
if ($null -ne $WasmLd) {
    Write-Output "[AvidScript][DGuest][Build] linker=$($WasmLd.Source)"
}
else {
    Write-Output "[AvidScript][DGuest][Build] linker=ldc2-internal-lld"
}
Write-Output "[AvidScript][DGuest][Build] output=$OutputPath"

$CompilerOutput = & $Ldc2.Path @Arguments 2>&1
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
$ArtifactHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $OutputPath).Hash.ToLowerInvariant()
$CompilerVersion = Get-Ldc2Version -Path $Ldc2.Path

$Manifest = [ordered]@{
    schema_version = 1
    module_id = "d_guest_actor_set_location"
    abi_version = 1
    language = "d"
    source = [ordered]@{
        file = Convert-ToProjectRelativePath -Path $GeneratedSourcePath
    }
    wasm = [ordered]@{
        file = Convert-ToProjectRelativePath -Path $OutputPath
        sha256 = $ArtifactHash
    }
    required_exports = @(
        "avid_on_begin_play",
        "avid_on_tick"
    )
    required_imports = @(
        [ordered]@{
            module = "env"
            name = "actor_set_location"
        }
    )
    toolchain = [ordered]@{
        compiler = "ldc2"
        version = $CompilerVersion
        target = "wasm32-unknown-unknown-wasm"
        linker = if ($null -ne $WasmLd) { $WasmLd.Source } else { "ldc2-internal-lld" }
    }
}

$ManifestJson = $Manifest | ConvertTo-Json -Depth 8
[System.IO.File]::WriteAllText($ManifestPath, $ManifestJson + [System.Environment]::NewLine, [System.Text.UTF8Encoding]::new($false))

Write-Output "[AvidScript][DGuest][Build] manifest=$ManifestPath sha256=$ArtifactHash"
Write-Output "[AvidScript][DGuest][Build] result=built output=$OutputPath bytes=$($Artifact.Length)"
exit 0
