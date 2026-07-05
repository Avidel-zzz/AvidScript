param(
    [string]$SourcePath = "",
    [string]$BindingsPath = "",
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

function Test-AvidScriptIdentifier {
    param([Parameter(Mandatory = $true)][string]$Value)
    return $Value -match '^[A-Za-z_][A-Za-z0-9_]*$'
}

function Test-AvidScriptType {
    param([Parameter(Mandatory = $true)][string]$Type)
    return @("int", "float", "void").Contains($Type)
}

function Convert-AvidScriptTypeToDType {
    param([Parameter(Mandatory = $true)][string]$Type)

    switch ($Type) {
        "int" { return "int" }
        "float" { return "float" }
        "void" { return "void" }
        default { Write-FrontendFailure -Result "binding_manifest_invalid" -Details "type=$Type reason=unsupported_type" }
    }
}

function Read-BindingDeclaration {
    param([Parameter(Mandatory = $true)][string]$Path)

    $ResolvedPath = Resolve-ExistingFilePath -Path $Path
    if ($null -eq $ResolvedPath) {
        Write-FrontendFailure -Result "binding_manifest_invalid" -Details "path=$Path reason=missing"
    }

    try {
        $DeclarationText = [System.IO.File]::ReadAllText($ResolvedPath)
        $Declaration = $DeclarationText | ConvertFrom-Json
    }
    catch {
        Write-FrontendFailure -Result "binding_manifest_invalid" -Details "path=$ResolvedPath reason=json_parse_failed"
    }

    if ($null -eq $Declaration.schema_version -or [int]$Declaration.schema_version -ne 1) {
        Write-FrontendFailure -Result "binding_manifest_invalid" -Details "path=$ResolvedPath reason=schema_version"
    }

    $Bindings = @()
    if ($null -ne $Declaration.bindings) {
        $Bindings = @($Declaration.bindings)
    }

    if ($Bindings.Count -eq 0) {
        Write-FrontendFailure -Result "binding_manifest_invalid" -Details "path=$ResolvedPath reason=no_bindings"
    }

    $ByName = [System.Collections.Generic.Dictionary[string,object]]::new()

    foreach ($Binding in $Bindings) {
        $Name = [string]$Binding.name
        $ImportModule = [string]$Binding.import_module
        $ImportName = [string]$Binding.import_name
        $ReturnType = [string]$Binding.return_type

        if ([string]::IsNullOrWhiteSpace($Name) -or -not (Test-AvidScriptIdentifier -Value $Name)) {
            Write-FrontendFailure -Result "binding_manifest_invalid" -Details "path=$ResolvedPath reason=invalid_binding_name value=$Name"
        }

        if ($ByName.ContainsKey($Name)) {
            Write-FrontendFailure -Result "binding_manifest_invalid" -Details "path=$ResolvedPath reason=duplicate_binding value=$Name"
        }

        if ([string]::IsNullOrWhiteSpace($ImportModule) -or -not (Test-AvidScriptIdentifier -Value $ImportModule)) {
            Write-FrontendFailure -Result "binding_manifest_invalid" -Details "binding=$Name reason=invalid_import_module value=$ImportModule"
        }

        if ([string]::IsNullOrWhiteSpace($ImportName) -or -not (Test-AvidScriptIdentifier -Value $ImportName)) {
            Write-FrontendFailure -Result "binding_manifest_invalid" -Details "binding=$Name reason=invalid_import_name value=$ImportName"
        }

        if ([string]::IsNullOrWhiteSpace($ReturnType) -or -not (Test-AvidScriptType -Type $ReturnType)) {
            Write-FrontendFailure -Result "binding_manifest_invalid" -Details "binding=$Name reason=invalid_return_type value=$ReturnType"
        }

        $Parameters = @()
        if ($null -ne $Binding.parameters) {
            $Parameters = @($Binding.parameters)
        }

        $ParameterNames = [System.Collections.Generic.HashSet[string]]::new()
        foreach ($Parameter in $Parameters) {
            $ParameterName = [string]$Parameter.name
            $ParameterType = [string]$Parameter.type

            if ([string]::IsNullOrWhiteSpace($ParameterName) -or -not (Test-AvidScriptIdentifier -Value $ParameterName)) {
                Write-FrontendFailure -Result "binding_manifest_invalid" -Details "binding=$Name reason=invalid_parameter_name value=$ParameterName"
            }

            if (-not $ParameterNames.Add($ParameterName)) {
                Write-FrontendFailure -Result "binding_manifest_invalid" -Details "binding=$Name reason=duplicate_parameter value=$ParameterName"
            }

            if ([string]::IsNullOrWhiteSpace($ParameterType) -or $ParameterType -eq "void" -or -not (Test-AvidScriptType -Type $ParameterType)) {
                Write-FrontendFailure -Result "binding_manifest_invalid" -Details "binding=$Name parameter=$ParameterName reason=invalid_parameter_type value=$ParameterType"
            }
        }

        $ByName.Add($Name, $Binding)
    }

    return [PSCustomObject]@{
        Path = $ResolvedPath
        Bindings = $Bindings
        ByName = $ByName
    }
}

function New-DImportSignature {
    param([Parameter(Mandatory = $true)]$Binding)

    $ReturnType = Convert-AvidScriptTypeToDType -Type ([string]$Binding.return_type)
    $Parameters = @()
    if ($null -ne $Binding.parameters) {
        $Parameters = @($Binding.parameters)
    }

    $ParameterTexts = @()
    foreach ($Parameter in $Parameters) {
        $ParameterType = Convert-AvidScriptTypeToDType -Type ([string]$Parameter.type)
        $ParameterTexts += "$ParameterType $($Parameter.name)"
    }

    return "$ReturnType $($Binding.import_name)($($ParameterTexts -join ', '));"
}

function Convert-ArgumentLiteralToDValue {
    param(
        [Parameter(Mandatory = $true)]$Parameter,
        [Parameter(Mandatory = $true)][string]$Value
    )

    $ParameterName = [string]$Parameter.name
    $ParameterType = [string]$Parameter.type

    switch ($ParameterType) {
        "int" {
            $Parsed = Read-RequiredIntLiteral -Name $ParameterName -Value $Value
            return $Parsed.ToString($Culture)
        }
        "float" {
            $Parsed = Read-RequiredFloatLiteral -Name $ParameterName -Value $Value
            return Format-FloatLiteral -Value $Parsed
        }
        default {
            Write-FrontendFailure -Result "binding_manifest_invalid" -Details "parameter=$ParameterName type=$ParameterType reason=unsupported_parameter_type"
        }
    }
}

$BuildDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$PluginRoot = Split-Path -Parent $BuildDir
$ProjectPluginsDir = Split-Path -Parent $PluginRoot
$ProjectRoot = Split-Path -Parent $ProjectPluginsDir

if ([string]::IsNullOrWhiteSpace($BindingsPath)) {
    $BindingsPath = Join-Path $PluginRoot "Bindings\ActorHostBindings.avidscript.json"
}

if ([string]::IsNullOrWhiteSpace($SourcePath) -or -not (Test-Path -LiteralPath $SourcePath -PathType Leaf)) {
    Write-FrontendFailure -Result "source_missing" -Details "source=$SourcePath"
}

$BindingDeclaration = Read-BindingDeclaration -Path $BindingsPath
$BindingsFullPath = $BindingDeclaration.Path

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

$SelectedBinding = $null
if (-not $BindingDeclaration.ByName.TryGetValue($BindingName, [ref]$SelectedBinding)) {
    Write-FrontendFailure -Result "unknown_binding" -Details "binding=$BindingName"
}

if ($CallName -ne $BindingName) {
    Write-FrontendFailure -Result "unknown_binding" -Details "binding=$CallName"
}

$ArgsText = $Match.Groups["args"].Value.Trim()
$ArgumentValues = @()
if (-not [string]::IsNullOrWhiteSpace($ArgsText)) {
    $ArgumentValues = @($ArgsText.Split(",") | ForEach-Object { $_.Trim() })
}

$Parameters = @()
if ($null -ne $SelectedBinding.parameters) {
    $Parameters = @($SelectedBinding.parameters)
}

if ($ArgumentValues.Count -ne $Parameters.Count) {
    Write-FrontendFailure -Result "invalid_argument_count" -Details "binding=$BindingName expected=$($Parameters.Count) actual=$($ArgumentValues.Count)"
}

$DArgumentValues = @()
for ($Index = 0; $Index -lt $Parameters.Count; ++$Index) {
    $DArgumentValues += Convert-ArgumentLiteralToDValue -Parameter $Parameters[$Index] -Value $ArgumentValues[$Index]
}

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
$DImportSignature = New-DImportSignature -Binding $SelectedBinding
$CallArgumentText = $DArgumentValues -join ", "
$ReturnType = [string]$SelectedBinding.return_type
if ($ReturnType -eq "void") {
    $BeginPlayStatement = "$($SelectedBinding.import_name)($CallArgumentText);"
}
else {
    $BeginPlayStatement = "cast(void)$($SelectedBinding.import_name)($CallArgumentText);"
}

$GeneratedSource = @"
module $DModuleName;

extern(C) @nogc nothrow
{
    $DImportSignature

    export void avid_on_begin_play()
    {
        $BeginPlayStatement
    }

    export void avid_on_tick(float delta_seconds)
    {
        cast(void)delta_seconds;
    }
}
"@

Set-Content -LiteralPath $GeneratedSourcePath -Value $GeneratedSource -Encoding ASCII
Write-Output "[AvidScript][Frontend][Build] source=$SourceFullPath"
Write-Output "[AvidScript][Frontend][Build] bindings=$BindingsFullPath"
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
        bindings = Convert-ToProjectRelativePath -Path $BindingsFullPath
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
            module = [string]$SelectedBinding.import_module
            name = [string]$SelectedBinding.import_name
        }
    )
    frontend = [ordered]@{
        compiler = "BuildAvidScriptActor.ps1"
        version = "p7.1"
        backend = "d"
        binding_schema_version = 1
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
