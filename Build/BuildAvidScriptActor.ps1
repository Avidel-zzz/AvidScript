param(
    [string]$SourcePath = "",
    [string]$OutputRoot = "",
    [string]$OutputPath = "",
    [string]$ManifestPath = "",
    [string]$Ldc2Path = "",
    [string]$ToolchainRoot = "",
    [switch]$SkipCompile
)

$ErrorActionPreference = "Stop"
$Culture = [System.Globalization.CultureInfo]::InvariantCulture

function Write-FrontendFailure {
    param(
        [Parameter(Mandatory = $true)][string]$Result,
        [Parameter(Mandatory = $true)][string]$Details,
        [int]$ExitCode = 1
    )

    Write-Output "[AvidScript][Frontend][Build] result=$Result $Details"
    exit $ExitCode
}

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

function Read-RequiredIntLiteral {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Value
    )

    [int]$Parsed = 0
    if (-not [int]::TryParse($Value, [System.Globalization.NumberStyles]::Integer, $Culture, [ref]$Parsed)) {
        Write-FrontendFailure -Result "invalid_literal" -Details "argument=$Name value=$Value expected=int"
    }

    return $Parsed
}

function Read-RequiredFloatLiteral {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Value
    )

    [double]$Parsed = 0.0
    if (-not [double]::TryParse($Value, [System.Globalization.NumberStyles]::Float, $Culture, [ref]$Parsed)) {
        Write-FrontendFailure -Result "invalid_literal" -Details "argument=$Name value=$Value expected=float"
    }

    return $Parsed
}

$BuildDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$PluginRoot = Split-Path -Parent $BuildDir
$ProjectPluginsDir = Split-Path -Parent $PluginRoot
$ProjectRoot = Split-Path -Parent $ProjectPluginsDir

if ([string]::IsNullOrWhiteSpace($SourcePath) -or -not (Test-Path -LiteralPath $SourcePath -PathType Leaf)) {
    Write-FrontendFailure -Result "source_missing" -Details "source=$SourcePath"
}

$SourceFullPath = (Resolve-Path -LiteralPath $SourcePath).Path
$SourceText = [System.IO.File]::ReadAllText($SourceFullPath)
$Pattern = '(?s)^\s*module\s+(?<module>[A-Za-z_][A-Za-z0-9_]*)\s+use\s+(?<binding>[A-Za-z_][A-Za-z0-9_]*)\s+on\s+begin_play\s*\{\s*(?<call>[A-Za-z_][A-Za-z0-9_]*)\s*\((?<args>[^)]*)\)\s*\}\s+on\s+tick\s*\(\s*delta_seconds\s*\)\s*\{\s*\}\s*$'
$Match = [regex]::Match($SourceText, $Pattern)

if (-not $Match.Success) {
    Write-FrontendFailure -Result "parse_failed" -Details "source=$SourceFullPath expected=minimal_actor_set_location_grammar"
}

$ModuleId = $Match.Groups["module"].Value
$BindingName = $Match.Groups["binding"].Value
$CallName = $Match.Groups["call"].Value

if ($BindingName -ne "actor_set_location") {
    Write-FrontendFailure -Result "unknown_binding" -Details "binding=$BindingName"
}

if ($CallName -ne "actor_set_location") {
    Write-FrontendFailure -Result "unknown_binding" -Details "binding=$CallName"
}

$ArgumentValues = @($Match.Groups["args"].Value.Split(",") | ForEach-Object { $_.Trim() })
if ($ArgumentValues.Count -ne 5) {
    Write-FrontendFailure -Result "invalid_argument_count" -Details "binding=actor_set_location expected=5 actual=$($ArgumentValues.Count)"
}

$SlotValue = Read-RequiredIntLiteral -Name "slot" -Value $ArgumentValues[0]
$GenerationValue = Read-RequiredIntLiteral -Name "generation" -Value $ArgumentValues[1]
$XValue = Read-RequiredFloatLiteral -Name "x" -Value $ArgumentValues[2]
$YValue = Read-RequiredFloatLiteral -Name "y" -Value $ArgumentValues[3]
$ZValue = Read-RequiredFloatLiteral -Name "z" -Value $ArgumentValues[4]

if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path (Join-Path $ProjectRoot "Saved\AvidScriptGenerated") $ModuleId
}

New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null

if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path $OutputRoot "$ModuleId.wasm"
}

if ([string]::IsNullOrWhiteSpace($ManifestPath)) {
    $ManifestPath = Join-Path $OutputRoot "$ModuleId.avidscript.json"
}

$GeneratedSourcePath = Join-Path $OutputRoot "$ModuleId.generated.d"
$DModuleName = "${ModuleId}_generated"
$XLiteral = Format-FloatLiteral -Value $XValue
$YLiteral = Format-FloatLiteral -Value $YValue
$ZLiteral = Format-FloatLiteral -Value $ZValue

$GeneratedSource = @"
module $DModuleName;

extern(C) @nogc nothrow
{
    int actor_set_location(int slot, int generation, float x, float y, float z);

    export void avid_on_begin_play()
    {
        cast(void)actor_set_location($SlotValue, $GenerationValue, $XLiteral, $YLiteral, $ZLiteral);
    }

    export void avid_on_tick(float delta_seconds)
    {
        cast(void)delta_seconds;
    }
}
"@

Set-Content -LiteralPath $GeneratedSourcePath -Value $GeneratedSource -Encoding ASCII
Write-Output "[AvidScript][Frontend][Build] source=$SourceFullPath"
Write-Output "[AvidScript][Frontend][Build] generated_source=$GeneratedSourcePath"

if ($SkipCompile) {
    Write-Output "[AvidScript][Frontend][Build] result=generated source=$GeneratedSourcePath"
    exit 0
}

if (Test-Path -LiteralPath $OutputPath) {
    Remove-Item -LiteralPath $OutputPath -Force
}

if (Test-Path -LiteralPath $ManifestPath) {
    Remove-Item -LiteralPath $ManifestPath -Force
}

$Ldc2 = Resolve-Ldc2Tool
$WasmLd = Get-OptionalTool -Name "wasm-ld"

if (-not $Ldc2.Found) {
    Write-Output "[AvidScript][Frontend][Toolchain] ldc2=MISSING checked=$($Ldc2.Checked -join ';')"
    Write-Output "[AvidScript][Frontend][Build] result=missing_toolchain missing=ldc2 output=$OutputPath"
    exit 0
}

Write-Output "[AvidScript][Frontend][Toolchain] ldc2=FOUND source=$($Ldc2.Source) path=$($Ldc2.Path)"

$CompilerArguments = @(
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

Write-Output "[AvidScript][Frontend][Build] compiler=$($Ldc2.Path)"
if ($null -ne $WasmLd) {
    Write-Output "[AvidScript][Frontend][Build] linker=$($WasmLd.Source)"
}
else {
    Write-Output "[AvidScript][Frontend][Build] linker=ldc2-internal-lld"
}
Write-Output "[AvidScript][Frontend][Build] output=$OutputPath"

$CompilerOutput = & $Ldc2.Path @CompilerArguments 2>&1
$ExitCode = $LASTEXITCODE

foreach ($Line in $CompilerOutput) {
    Write-Output "[AvidScript][Frontend][Compiler] $Line"
}

if ($ExitCode -ne 0) {
    Write-Output "[AvidScript][Frontend][Build] result=compiler_failed exit_code=$ExitCode"
    exit $ExitCode
}

if (-not (Test-Path -LiteralPath $OutputPath)) {
    Write-Output "[AvidScript][Frontend][Build] result=artifact_missing output=$OutputPath"
    exit 1
}

$Artifact = Get-Item -LiteralPath $OutputPath
$ArtifactHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $OutputPath).Hash.ToLowerInvariant()
$CompilerVersion = Get-Ldc2Version -Path $Ldc2.Path

$Manifest = [ordered]@{
    schema_version = 1
    module_id = $ModuleId
    abi_version = 1
    language = "avidscript"
    source = [ordered]@{
        file = Convert-ToProjectRelativePath -Path $SourceFullPath
        generated_d = Convert-ToProjectRelativePath -Path $GeneratedSourcePath
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
    frontend = [ordered]@{
        compiler = "BuildAvidScriptActor.ps1"
        version = "p6.1"
        backend = "d"
    }
    toolchain = [ordered]@{
        compiler = "ldc2"
        version = $CompilerVersion
        target = "wasm32-unknown-unknown-wasm"
        linker = if ($null -ne $WasmLd) { $WasmLd.Source } else { "ldc2-internal-lld" }
    }
}

$ManifestJson = $Manifest | ConvertTo-Json -Depth 8
[System.IO.File]::WriteAllText($ManifestPath, $ManifestJson + [System.Environment]::NewLine, [System.Text.UTF8Encoding]::new($false))

Write-Output "[AvidScript][Frontend][Build] manifest=$ManifestPath sha256=$ArtifactHash"
Write-Output "[AvidScript][Frontend][Build] result=built output=$OutputPath bytes=$($Artifact.Length)"
exit 0
