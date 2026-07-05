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

function Write-FrontendDiagnostic {
    param(
        [Parameter(Mandatory = $true)][string]$Code,
        [string]$Severity = "error",
        [int]$Line = 0,
        [int]$Column = 0,
        [Parameter(Mandatory = $true)][string]$Message
    )

    $EscapedMessage = $Message.Replace('"', '\"')
    Write-Output "[AvidScript][Frontend][Diagnostic] code=$Code severity=$Severity line=$Line column=$Column message=`"$EscapedMessage`""
}

function Write-FrontendFailure {
    param(
        [Parameter(Mandatory = $true)][string]$Result,
        [Parameter(Mandatory = $true)][string]$Details,
        [int]$ExitCode = 1,
        [string]$Code = "",
        [int]$Line = 0,
        [int]$Column = 0,
        [string]$Message = ""
    )

    $BuildDetails = $Details
    if (-not [string]::IsNullOrWhiteSpace($Code)) {
        if ([string]::IsNullOrWhiteSpace($Message)) {
            $Message = $Result
        }

        Write-FrontendDiagnostic -Code $Code -Line $Line -Column $Column -Message $Message
        $BuildDetails = "code=$Code $Details"
    }

    Write-Output "[AvidScript][Frontend][Build] result=$Result $BuildDetails"
    exit $ExitCode
}

function New-SourceLocation {
    param(
        [int]$Line,
        [int]$Column
    )

    return [PSCustomObject]@{
        Line = $Line
        Column = $Column
    }
}

function Convert-SourceIndexToLocation {
    param(
        [Parameter(Mandatory = $true)][string]$Text,
        [int]$Index
    )

    if ($Index -lt 0) {
        return New-SourceLocation -Line 0 -Column 0
    }

    $Line = 1
    $Column = 1
    $Limit = [Math]::Min($Index, $Text.Length)
    for ($Cursor = 0; $Cursor -lt $Limit; ++$Cursor) {
        $Character = $Text[$Cursor]
        if ($Character -eq "`n") {
            ++$Line
            $Column = 1
        }
        elseif ($Character -ne "`r") {
            ++$Column
        }
    }

    return New-SourceLocation -Line $Line -Column $Column
}

function Split-ArgumentList {
    param(
        [Parameter(Mandatory = $true)][string]$ArgsText,
        [int]$ArgsStartIndex,
        [Parameter(Mandatory = $true)][string]$SourceText
    )

    $Tokens = @()
    if ([string]::IsNullOrWhiteSpace($ArgsText)) {
        return $Tokens
    }

    $SegmentStart = 0
    for ($Index = 0; $Index -le $ArgsText.Length; ++$Index) {
        if ($Index -eq $ArgsText.Length -or $ArgsText[$Index] -eq ',') {
            $Raw = $ArgsText.Substring($SegmentStart, $Index - $SegmentStart)
            $Trimmed = $Raw.Trim()
            $LeadingWhitespace = $Raw.Length - $Raw.TrimStart().Length
            $SourceIndex = $ArgsStartIndex + $SegmentStart + $LeadingWhitespace
            $Location = Convert-SourceIndexToLocation -Text $SourceText -Index $SourceIndex
            $Tokens += [PSCustomObject]@{
                Value = $Trimmed
                SourceIndex = $SourceIndex
                Line = $Location.Line
                Column = $Location.Column
            }
            $SegmentStart = $Index + 1
        }
    }

    return $Tokens
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

function Get-Sha256FileHashLowerInvariant {
    param([Parameter(Mandatory = $true)][string]$Path)

    $Stream = [System.IO.File]::OpenRead($Path)
    try {
        $Sha256 = [System.Security.Cryptography.SHA256]::Create()
        try {
            $HashBytes = $Sha256.ComputeHash($Stream)
            return ([System.BitConverter]::ToString($HashBytes)).Replace("-", "").ToLowerInvariant()
        }
        finally {
            $Sha256.Dispose()
        }
    }
    finally {
        $Stream.Dispose()
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
        [Parameter(Mandatory = $true)][string]$Value,
        [int]$Line = 0,
        [int]$Column = 0
    )

    [int]$Parsed = 0
    if (-not [int]::TryParse($Value, [System.Globalization.NumberStyles]::Integer, $Culture, [ref]$Parsed)) {
        Write-FrontendFailure -Result "invalid_literal" -Details "argument=$Name value=$Value expected=int" -Code "ASL1204" -Line $Line -Column $Column -Message "invalid literal for argument '$Name': expected int but got '$Value'"
    }

    return $Parsed
}

function Read-RequiredFloatLiteral {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Value,
        [int]$Line = 0,
        [int]$Column = 0
    )

    [double]$Parsed = 0.0
    if (-not [double]::TryParse($Value, [System.Globalization.NumberStyles]::Float, $Culture, [ref]$Parsed)) {
        Write-FrontendFailure -Result "invalid_literal" -Details "argument=$Name value=$Value expected=float" -Code "ASL1204" -Line $Line -Column $Column -Message "invalid literal for argument '$Name': expected float but got '$Value'"
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
        default { Write-FrontendFailure -Result "binding_manifest_invalid" -Details "type=$Type reason=unsupported_type" -Code "ASL1201" -Message "unsupported binding type '$Type'" }
    }
}

function Read-BindingDeclaration {
    param([Parameter(Mandatory = $true)][string]$Path)

    $ResolvedPath = Resolve-ExistingFilePath -Path $Path
    if ($null -eq $ResolvedPath) {
        Write-FrontendFailure -Result "binding_manifest_invalid" -Details "path=$Path reason=missing" -Code "ASL1201" -Message "binding declaration missing: $Path"
    }

    try {
        $DeclarationText = [System.IO.File]::ReadAllText($ResolvedPath)
        $Declaration = $DeclarationText | ConvertFrom-Json
    }
    catch {
        Write-FrontendFailure -Result "binding_manifest_invalid" -Details "path=$ResolvedPath reason=json_parse_failed" -Code "ASL1201" -Message "binding declaration is not valid JSON: $ResolvedPath"
    }

    if ($null -eq $Declaration.schema_version -or [int]$Declaration.schema_version -ne 1) {
        Write-FrontendFailure -Result "binding_manifest_invalid" -Details "path=$ResolvedPath reason=schema_version" -Code "ASL1201" -Message "binding declaration schema_version must be 1"
    }

    $Bindings = @()
    if ($null -ne $Declaration.bindings) {
        $Bindings = @($Declaration.bindings)
    }

    if ($Bindings.Count -eq 0) {
        Write-FrontendFailure -Result "binding_manifest_invalid" -Details "path=$ResolvedPath reason=no_bindings" -Code "ASL1201" -Message "binding declaration contains no bindings"
    }

    $ByName = [System.Collections.Generic.Dictionary[string,object]]::new()

    foreach ($Binding in $Bindings) {
        $Name = [string]$Binding.name
        $ImportModule = [string]$Binding.import_module
        $ImportName = [string]$Binding.import_name
        $ReturnType = [string]$Binding.return_type

        if ([string]::IsNullOrWhiteSpace($Name) -or -not (Test-AvidScriptIdentifier -Value $Name)) {
            Write-FrontendFailure -Result "binding_manifest_invalid" -Details "path=$ResolvedPath reason=invalid_binding_name value=$Name" -Code "ASL1201" -Message "binding declaration has an invalid binding name '$Name'"
        }

        if ($ByName.ContainsKey($Name)) {
            Write-FrontendFailure -Result "binding_manifest_invalid" -Details "path=$ResolvedPath reason=duplicate_binding value=$Name" -Code "ASL1201" -Message "binding declaration has duplicate binding '$Name'"
        }

        if ([string]::IsNullOrWhiteSpace($ImportModule) -or -not (Test-AvidScriptIdentifier -Value $ImportModule)) {
            Write-FrontendFailure -Result "binding_manifest_invalid" -Details "binding=$Name reason=invalid_import_module value=$ImportModule" -Code "ASL1201" -Message "binding '$Name' has invalid import_module '$ImportModule'"
        }

        if ([string]::IsNullOrWhiteSpace($ImportName) -or -not (Test-AvidScriptIdentifier -Value $ImportName)) {
            Write-FrontendFailure -Result "binding_manifest_invalid" -Details "binding=$Name reason=invalid_import_name value=$ImportName" -Code "ASL1201" -Message "binding '$Name' has invalid import_name '$ImportName'"
        }

        if ([string]::IsNullOrWhiteSpace($ReturnType) -or -not (Test-AvidScriptType -Type $ReturnType)) {
            Write-FrontendFailure -Result "binding_manifest_invalid" -Details "binding=$Name reason=invalid_return_type value=$ReturnType" -Code "ASL1201" -Message "binding '$Name' has invalid return_type '$ReturnType'"
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
                Write-FrontendFailure -Result "binding_manifest_invalid" -Details "binding=$Name reason=invalid_parameter_name value=$ParameterName" -Code "ASL1201" -Message "binding '$Name' has invalid parameter name '$ParameterName'"
            }

            if (-not $ParameterNames.Add($ParameterName)) {
                Write-FrontendFailure -Result "binding_manifest_invalid" -Details "binding=$Name reason=duplicate_parameter value=$ParameterName" -Code "ASL1201" -Message "binding '$Name' has duplicate parameter '$ParameterName'"
            }

            if ([string]::IsNullOrWhiteSpace($ParameterType) -or $ParameterType -eq "void" -or -not (Test-AvidScriptType -Type $ParameterType)) {
                Write-FrontendFailure -Result "binding_manifest_invalid" -Details "binding=$Name parameter=$ParameterName reason=invalid_parameter_type value=$ParameterType" -Code "ASL1201" -Message "binding '$Name' parameter '$ParameterName' has invalid type '$ParameterType'"
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
        [Parameter(Mandatory = $true)]$Argument
    )

    $ParameterName = [string]$Parameter.name
    $ParameterType = [string]$Parameter.type
    $Value = [string]$Argument.Value

    switch ($ParameterType) {
        "int" {
            $Parsed = Read-RequiredIntLiteral -Name $ParameterName -Value $Value -Line $Argument.Line -Column $Argument.Column
            return $Parsed.ToString($Culture)
        }
        "float" {
            $Parsed = Read-RequiredFloatLiteral -Name $ParameterName -Value $Value -Line $Argument.Line -Column $Argument.Column
            return Format-FloatLiteral -Value $Parsed
        }
        default {
            Write-FrontendFailure -Result "binding_manifest_invalid" -Details "parameter=$ParameterName type=$ParameterType reason=unsupported_parameter_type" -Code "ASL1201" -Message "unsupported parameter type '$ParameterType'"
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
    Write-FrontendFailure -Result "source_missing" -Details "source=$SourcePath" -Code "ASL1001" -Message "source file missing: $SourcePath"
}

$BindingDeclaration = Read-BindingDeclaration -Path $BindingsPath
$BindingsFullPath = $BindingDeclaration.Path

$SourceFullPath = (Resolve-Path -LiteralPath $SourcePath).Path
$SourceText = [System.IO.File]::ReadAllText($SourceFullPath)
$Pattern = '(?s)^\s*module\s+(?<module>[A-Za-z_][A-Za-z0-9_]*)\s+use\s+(?<binding>[A-Za-z_][A-Za-z0-9_]*)\s+on\s+begin_play\s*\{\s*(?<call>[A-Za-z_][A-Za-z0-9_]*)\s*\((?<args>[^)]*)\)\s*\}\s+on\s+tick\s*\(\s*delta_seconds\s*\)\s*\{\s*\}\s*$'
$Match = [regex]::Match($SourceText, $Pattern)

if (-not $Match.Success) {
    Write-FrontendFailure -Result "parse_failed" -Details "source=$SourceFullPath expected=minimal_actor_set_location_grammar" -Code "ASL1100" -Line 1 -Column 1 -Message "source does not match the minimal AvidScript actor grammar"
}

$ModuleId = $Match.Groups["module"].Value
$BindingName = $Match.Groups["binding"].Value
$CallName = $Match.Groups["call"].Value
$BindingLocation = Convert-SourceIndexToLocation -Text $SourceText -Index $Match.Groups["binding"].Index
$CallLocation = Convert-SourceIndexToLocation -Text $SourceText -Index $Match.Groups["call"].Index

$SelectedBinding = $null
if (-not $BindingDeclaration.ByName.TryGetValue($BindingName, [ref]$SelectedBinding)) {
    Write-FrontendFailure -Result "unknown_binding" -Details "binding=$BindingName" -Code "ASL1202" -Line $BindingLocation.Line -Column $BindingLocation.Column -Message "unknown binding '$BindingName'"
}

if ($CallName -ne $BindingName) {
    Write-FrontendFailure -Result "unknown_binding" -Details "binding=$CallName" -Code "ASL1202" -Line $CallLocation.Line -Column $CallLocation.Column -Message "unknown binding '$CallName'"
}

$ArgsText = $Match.Groups["args"].Value
$ArgumentTokens = @(Split-ArgumentList -ArgsText $ArgsText -ArgsStartIndex $Match.Groups["args"].Index -SourceText $SourceText)

$Parameters = @()
if ($null -ne $SelectedBinding.parameters) {
    $Parameters = @($SelectedBinding.parameters)
}

if ($ArgumentTokens.Count -ne $Parameters.Count) {
    Write-FrontendFailure -Result "invalid_argument_count" -Details "binding=$BindingName expected=$($Parameters.Count) actual=$($ArgumentTokens.Count)" -Code "ASL1203" -Line $CallLocation.Line -Column $CallLocation.Column -Message "binding '$BindingName' expects $($Parameters.Count) arguments but got $($ArgumentTokens.Count)"
}

$DArgumentValues = @()
for ($Index = 0; $Index -lt $Parameters.Count; ++$Index) {
    $DArgumentValues += Convert-ArgumentLiteralToDValue -Parameter $Parameters[$Index] -Argument $ArgumentTokens[$Index]
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
    Write-FrontendDiagnostic -Code "ASL1301" -Line 0 -Column 0 -Message "ldc2 toolchain not found"
    Write-Output "[AvidScript][Frontend][Toolchain] ldc2=MISSING checked=$($Ldc2.Checked -join ';')"
    Write-Output "[AvidScript][Frontend][Build] result=missing_toolchain code=ASL1301 missing=ldc2 output=$OutputPath"
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
    Write-FrontendDiagnostic -Code "ASL1302" -Line 0 -Column 0 -Message "D backend compiler failed with exit code $ExitCode"
    Write-Output "[AvidScript][Frontend][Build] result=compiler_failed code=ASL1302 exit_code=$ExitCode"
    exit $ExitCode
}

if (-not (Test-Path -LiteralPath $OutputPath)) {
    Write-FrontendFailure -Result "artifact_missing" -Details "output=$OutputPath" -Code "ASL1303" -Message "compiler completed without producing wasm artifact: $OutputPath"
}

$Artifact = Get-Item -LiteralPath $OutputPath
$ArtifactHash = Get-Sha256FileHashLowerInvariant -Path $OutputPath
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
        version = "p7.2"
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
