param(
    [string]$DotNetPath = "",
    [string]$OutputRoot = "",
    [string]$Configuration = "Release",
    [string]$SourcePath = "",
    [string]$ProjectPath = "",
    [string]$ModuleId = "",
    [string]$ArtifactStem = "",
    [string]$ReportPath = "",
    [string]$ManifestPath = "",
    [string]$GuestCompilerPath = "",
    [string]$BindingPackagePath = ""
)

$ErrorActionPreference = "Stop"
$BuildDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$PluginRoot = Split-Path -Parent $BuildDir
$ProjectPluginsDir = Split-Path -Parent $PluginRoot
$ProjectRoot = Split-Path -Parent $ProjectPluginsDir
$DefaultProjectPath = Join-Path $PluginRoot "Samples\CSharp\ActorLifecycle\AvidScript.ActorLifecycle.csproj"
$DefaultSourcePath = Join-Path $PluginRoot "Samples\CSharp\ActorLifecycle\ActorLifecycleScript.cs"
$DefaultModuleId = "csharp_actor_lifecycle"
$DefaultArtifactStem = "actor_lifecycle"
$DefaultGuestCompilerPath = Join-Path $BuildDir "InvokeCSharpGuestCompiler.ps1"
$Utf8 = [System.Text.UTF8Encoding]::new($false)
. (Join-Path $BuildDir "AvidScriptCSharpBindingPackage.ps1")

function Resolve-ExistingFile {
    param([string]$Path)
    if (-not [string]::IsNullOrWhiteSpace($Path) -and
        (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return (Resolve-Path -LiteralPath $Path).Path
    }
    return $null
}

function Resolve-DotNetTool {
    $Candidates = @()
    if (-not [string]::IsNullOrWhiteSpace($DotNetPath)) {
        $Candidates += [pscustomobject]@{ Source = "parameter"; Path = $DotNetPath }
    }
    if (-not [string]::IsNullOrWhiteSpace($env:AVIDSCRIPT_DOTNET)) {
        $Candidates += [pscustomobject]@{ Source = "env:AVIDSCRIPT_DOTNET"; Path = $env:AVIDSCRIPT_DOTNET }
    }
    if (-not [string]::IsNullOrWhiteSpace($env:USERPROFILE)) {
        $Candidates += [pscustomobject]@{
            Source = "user_profile_dotnet"
            Path = Join-Path $env:USERPROFILE ".dotnet\dotnet.exe"
        }
    }
    $PathCommand = Get-Command "dotnet" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -ne $PathCommand) {
        $Candidates += [pscustomobject]@{ Source = "PATH"; Path = $PathCommand.Source }
    }

    $Checked = @()
    foreach ($Candidate in $Candidates) {
        $Checked += "$($Candidate.Source):$($Candidate.Path)"
        $Resolved = Resolve-ExistingFile $Candidate.Path
        if ($null -ne $Resolved) {
            return [pscustomobject]@{
                Found = $true
                Source = $Candidate.Source
                Path = $Resolved
                Checked = $Checked
            }
        }
    }

    return [pscustomobject]@{ Found = $false; Source = ""; Path = ""; Checked = $Checked }
}

function Convert-ToProjectRelativePath {
    param([string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path)) {
        return ""
    }

    $FullPath = [System.IO.Path]::GetFullPath($Path)
    $RootPath = [System.IO.Path]::GetFullPath($ProjectRoot).TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar)
    if ($FullPath.StartsWith($RootPath, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $FullPath.Substring($RootPath.Length).TrimStart(
            [System.IO.Path]::DirectorySeparatorChar,
            [System.IO.Path]::AltDirectorySeparatorChar).Replace("\", "/")
    }
    return $FullPath
}

function Get-Sha256Hex {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return ""
    }

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

function Write-JsonAtomic {
    param([string]$Path, [Parameter(Mandatory = $true)]$Value, [int]$Depth = 12)
    if (Test-Path -LiteralPath $Path -PathType Container) {
        throw "JSON output path is a directory: $Path"
    }
    $Directory = Split-Path -Parent $Path
    New-Item -ItemType Directory -Force -Path $Directory | Out-Null
    $TemporaryPath = "$Path.tmp.$PID"
    $Json = $Value | ConvertTo-Json -Depth $Depth
    try {
        [System.IO.File]::WriteAllText(
            $TemporaryPath,
            $Json + [System.Environment]::NewLine,
            $Utf8)
        Move-Item -LiteralPath $TemporaryPath -Destination $Path -Force
    }
    finally {
        if (Test-Path -LiteralPath $TemporaryPath -PathType Leaf) {
            Remove-Item -LiteralPath $TemporaryPath -Force
        }
    }
}

function Remove-LoadableArtifacts {
    foreach ($Artifact in @($ManifestPath, $WasmArtifactPath)) {
        if (Test-Path -LiteralPath $Artifact -PathType Leaf) {
            Remove-Item -LiteralPath $Artifact -Force
        }
    }
}

function Invoke-AvidScriptPowerShell {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)
    $PreviousErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        $RawOutput = @(& powershell.exe @Arguments 2>&1)
        $ProcessExitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $PreviousErrorActionPreference
    }

    return [pscustomobject]@{
        ExitCode = $ProcessExitCode
        Output = @($RawOutput | ForEach-Object { $_.ToString() })
    }
}

function Convert-CompilerDiagnostics {
    param([Parameter(Mandatory = $true)]$Model, [string]$SourceId)
    $Converted = @()
    foreach ($Diagnostic in @($Model.diagnostics)) {
        $Converted += [ordered]@{
            code = [string]$Diagnostic.code
            severity = [string]$Diagnostic.severity
            message = [string]$Diagnostic.message
            file = $SourceId
            start = [int]$Diagnostic.span.start
            length = [int]$Diagnostic.span.length
            line = [int]$Diagnostic.span.line
            column = [int]$Diagnostic.span.column
            end_line = [int]$Diagnostic.span.end_line
            end_column = [int]$Diagnostic.span.end_column
        }
    }
    return $Converted
}

function Get-SelectedScriptTypeName {
    if ($null -eq $SemanticModel) {
        return ""
    }
    $ExportCallable = @($SemanticModel.callables | Where-Object { $null -ne $_.export } | Select-Object -First 1)
    if ($ExportCallable.Count -eq 0) {
        return ""
    }
    $ContainingTypeId = [string]$ExportCallable[0].containing_type_id
    $TypeSymbol = @($SemanticModel.symbols | Where-Object {
        $_.kind -eq "type" -and $_.type_id -eq $ContainingTypeId
    } | Select-Object -First 1)
    if ($TypeSymbol.Count -eq 0) {
        return ""
    }
    return [string]$TypeSymbol[0].name
}

function Write-BuildReport {
    param(
        [Parameter(Mandatory = $true)][string]$Result,
        [Parameter(Mandatory = $true)][bool]$DirectAbiSupported,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][object[]]$ReportDiagnostics,
        [string]$Compiler = "avidscript-csharp-guest-wasm"
    )

    $Report = [ordered]@{
        schema_version = 1
        language = "csharp"
        module_id = $ModuleId
        result = $Result
        succeeded = $Result -eq "direct_abi_built"
        direct_abi_supported = $DirectAbiSupported
        source = [ordered]@{
            project = Convert-ToProjectRelativePath $ProjectPath
            file = Convert-ToProjectRelativePath $SourcePath
            sha256 = if ($null -eq $FrontendModel) { "" } else { [string]$FrontendModel.source.sha256 }
            script_type = $SelectedScriptTypeName
        }
        output_root = Convert-ToProjectRelativePath $OutputRoot
        binding_package = [ordered]@{
            required = -not $IsDefaultSource
            package_name = if ($null -eq $BindingPackageInfo) { "" } else { [string]$BindingPackageInfo.PackageName }
            package_hash = if ($null -eq $BindingPackageInfo) { "" } else { [string]$BindingPackageInfo.PackageHash }
            manifest_file = if ($null -eq $BindingPackageInfo) { "" } else { Convert-ToProjectRelativePath $BindingPackageInfo.ManifestPath }
            manifest_sha256 = if ($null -eq $BindingPackageInfo) { "" } else { [string]$BindingPackageInfo.ManifestSha256 }
            descriptor_file = if ($null -eq $BindingPackageInfo) { "" } else { Convert-ToProjectRelativePath $BindingPackageInfo.DescriptorPath }
            descriptor_sha256 = if ($null -eq $BindingPackageInfo) { "" } else { [string]$BindingPackageInfo.DescriptorSha256 }
            reference_source_file = if ($null -eq $BindingPackageInfo) { "" } else { Convert-ToProjectRelativePath $BindingPackageInfo.ReferenceSourcePath }
            reference_source_sha256 = if ($null -eq $BindingPackageInfo) { "" } else { [string]$BindingPackageInfo.ReferenceSourceSha256 }
        }
        required_exports = @($RequiredExports)
        required_imports = @($RequiredImports)
        observed_exports = @($ObservedExports)
        artifacts = [ordered]@{
            wasm_file = if (Test-Path -LiteralPath $WasmArtifactPath -PathType Leaf) { Convert-ToProjectRelativePath $WasmArtifactPath } else { "" }
            manifest_file = if (Test-Path -LiteralPath $ManifestPath -PathType Leaf) { Convert-ToProjectRelativePath $ManifestPath } else { "" }
            report_file = Convert-ToProjectRelativePath $ReportPath
            frontend_file = if (Test-Path -LiteralPath $FrontendArtifactPath -PathType Leaf) { Convert-ToProjectRelativePath $FrontendArtifactPath } else { "" }
            semantic_file = if (Test-Path -LiteralPath $SemanticArtifactPath -PathType Leaf) { Convert-ToProjectRelativePath $SemanticArtifactPath } else { "" }
            guest_ir_file = if (Test-Path -LiteralPath $GuestIrArtifactPath -PathType Leaf) { Convert-ToProjectRelativePath $GuestIrArtifactPath } else { "" }
        }
        frontend = [ordered]@{
            schema_version = if ($null -eq $FrontendModel) { 0 } else { [int]$FrontendModel.schema_version }
            version = if ($null -eq $FrontendModel) { "" } else { [string]$FrontendModel.frontend_version }
            artifact_sha256 = Get-Sha256Hex $FrontendArtifactPath
        }
        semantic = [ordered]@{
            schema_version = if ($null -eq $SemanticModel) { 0 } else { [int]$SemanticModel.schema_version }
            version = if ($null -eq $SemanticModel) { "" } else { [string]$SemanticModel.semantic_version }
            succeeded = if ($null -eq $SemanticModel) { $false } else { [bool]$SemanticModel.succeeded }
            source_sha256 = if ($null -eq $SemanticModel) { "" } else { [string]$SemanticModel.source.sha256 }
            frontend_sha256 = if ($null -eq $SemanticModel) { "" } else { [string]$SemanticModel.source.frontend_sha256 }
            artifact_sha256 = Get-Sha256Hex $SemanticArtifactPath
            diagnostic_count = if ($null -eq $SemanticModel) { 0 } else { @($SemanticModel.diagnostics).Count }
        }
        guest_ir = [ordered]@{
            schema_version = if ($null -eq $GuestIrModel) { 0 } else { [int]$GuestIrModel.schema_version }
            version = if ($null -eq $GuestIrModel) { "" } else { [string]$GuestIrModel.ir_version }
            succeeded = if ($null -eq $GuestIrModel) { $false } else { [bool]$GuestIrModel.succeeded }
            semantic_sha256 = if ($null -eq $GuestIrModel) { "" } else { [string]$GuestIrModel.provenance.semantic_sha256 }
            sha256 = Get-Sha256Hex $GuestIrArtifactPath
        }
        wasm = [ordered]@{
            sha256 = Get-Sha256Hex $WasmArtifactPath
        }
        toolchain = [ordered]@{
            compiler = $Compiler
            dotnet = if ($DotNet.Found) { $DotNet.Path } else { "" }
            dotnet_source = if ($DotNet.Found) { $DotNet.Source } else { "" }
            target_framework = "net8.0"
            target = "wasm32"
        }
        diagnostics = @($ReportDiagnostics)
    }
    Write-JsonAtomic -Path $ReportPath -Value $Report
}

if ([string]::IsNullOrWhiteSpace($SourcePath)) { $SourcePath = $DefaultSourcePath }
if ([string]::IsNullOrWhiteSpace($ProjectPath)) { $ProjectPath = $DefaultProjectPath }
if ([string]::IsNullOrWhiteSpace($ModuleId)) { $ModuleId = $DefaultModuleId }
if ([string]::IsNullOrWhiteSpace($ArtifactStem)) { $ArtifactStem = $DefaultArtifactStem }
if ([string]::IsNullOrWhiteSpace($GuestCompilerPath)) { $GuestCompilerPath = $DefaultGuestCompilerPath }
if ($ArtifactStem.IndexOfAny([System.IO.Path]::GetInvalidFileNameChars()) -ge 0) {
    throw "ArtifactStem contains invalid file name characters: $ArtifactStem"
}
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $ProjectRoot "Saved\AvidScriptCSharpGuest\ActorLifecycle"
}

$SourcePath = [System.IO.Path]::GetFullPath($SourcePath)
$ProjectPath = [System.IO.Path]::GetFullPath($ProjectPath)
$OutputRoot = [System.IO.Path]::GetFullPath($OutputRoot)
$IsDefaultSource = $SourcePath.Equals(
    [System.IO.Path]::GetFullPath($DefaultSourcePath),
    [System.StringComparison]::OrdinalIgnoreCase)
if ([string]::IsNullOrWhiteSpace($ReportPath)) { $ReportPath = Join-Path $OutputRoot "$ArtifactStem.csharp.report.json" }
if ([string]::IsNullOrWhiteSpace($ManifestPath)) { $ManifestPath = Join-Path $OutputRoot "$ArtifactStem.avidscript.json" }
$ReportPath = [System.IO.Path]::GetFullPath($ReportPath)
$ManifestPath = [System.IO.Path]::GetFullPath($ManifestPath)
$FrontendArtifactPath = Join-Path $OutputRoot "$ArtifactStem.csharp.frontend.json"
$SemanticArtifactPath = Join-Path $OutputRoot "$ArtifactStem.csharp.semantic.json"
$GuestIrArtifactPath = Join-Path $OutputRoot "$ArtifactStem.guestir.json"
$WasmArtifactPath = Join-Path $OutputRoot "$ArtifactStem.wasm"
$WasmInspectionArtifactPath = Join-Path $OutputRoot "$ArtifactStem.wasm.inspect.json"
$LegacyAdapterWasmPath = Join-Path $OutputRoot "$ArtifactStem.csharp_adapter.wasm"
$LegacyDotNetWasmPath = Join-Path $OutputRoot "$ArtifactStem.dotnet.wasm"
$FrontendModel = $null
$SemanticModel = $null
$GuestIrModel = $null
$WasmInspectionModel = $null
$SelectedScriptTypeName = ""
$RequiredExports = @()
$RequiredImports = @()
$ObservedExports = @()
$BindingPackageInfo = $null
$Diagnostics = @()
$DotNet = [pscustomobject]@{ Found = $false; Source = ""; Path = ""; Checked = @() }

New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
foreach ($Artifact in @(
    $ReportPath,
    $ManifestPath,
    $FrontendArtifactPath,
    $SemanticArtifactPath,
    $GuestIrArtifactPath,
    $WasmArtifactPath,
    $WasmInspectionArtifactPath,
    $LegacyAdapterWasmPath,
    $LegacyDotNetWasmPath)) {
    if (Test-Path -LiteralPath $Artifact -PathType Leaf) {
        Remove-Item -LiteralPath $Artifact -Force
    }
}

$SourceId = Convert-ToProjectRelativePath $SourcePath
if (-not $IsDefaultSource) {
    if ([string]::IsNullOrWhiteSpace($BindingPackagePath)) {
        $Diagnostics += [ordered]@{
            code = "ASBI4201"
            severity = "error"
            message = "Custom C# profiles require an explicit generated binding package manifest."
            file = $SourceId
        }
        Write-BuildReport -Result "phase42_binding_required" -DirectAbiSupported $false -ReportDiagnostics $Diagnostics
        Write-Output "[AvidScript][CSharp][Build] result=phase42_binding_required report=$ReportPath"
        exit 1
    }

    try {
        $BindingPackageInfo = Resolve-AvidScriptCSharpBindingPackage -ManifestPath $BindingPackagePath
    }
    catch {
        $Diagnostics += [ordered]@{
            code = "ASBI4202"
            severity = "error"
            message = $_.Exception.Message
            file = Convert-ToProjectRelativePath $BindingPackagePath
        }
        Write-BuildReport -Result "binding_package_invalid" -DirectAbiSupported $false -ReportDiagnostics $Diagnostics
        Write-Output "[AvidScript][CSharp][Build] result=binding_package_invalid report=$ReportPath"
        exit 1
    }
}
$DotNet = Resolve-DotNetTool
if (-not $DotNet.Found) {
    $Diagnostics += [ordered]@{
        code = "dotnet_missing"
        severity = "error"
        message = "dotnet was not found"
        checked = @($DotNet.Checked)
    }
    Write-BuildReport -Result "missing_toolchain" -DirectAbiSupported $false -ReportDiagnostics $Diagnostics
    Write-Output "[AvidScript][CSharp][Build] result=missing_toolchain report=$ReportPath"
    exit 1
}

$FrontendArguments = @(
    "-NoProfile", "-ExecutionPolicy", "Bypass",
    "-File", (Join-Path $BuildDir "InvokeCSharpFrontend.ps1"),
    "-DotNetPath", $DotNet.Path,
    "-SourcePath", $SourcePath,
    "-SourceId", $SourceId,
    "-OutputPath", $FrontendArtifactPath,
    "-Configuration", $Configuration)
$FrontendInvocation = Invoke-AvidScriptPowerShell -Arguments $FrontendArguments
$FrontendOutput = @($FrontendInvocation.Output)
$FrontendExitCode = [int]$FrontendInvocation.ExitCode
if (Test-Path -LiteralPath $FrontendArtifactPath -PathType Leaf) {
    try {
        $FrontendModel = Get-Content -Raw -LiteralPath $FrontendArtifactPath | ConvertFrom-Json
        $Diagnostics += @(Convert-CompilerDiagnostics $FrontendModel $SourceId)
    }
    catch {
        $Diagnostics += [ordered]@{ code = "frontend_artifact_invalid"; severity = "error"; message = $_.Exception.Message; file = $SourceId }
    }
}
if ($FrontendExitCode -ne 0 -or $null -eq $FrontendModel -or -not $FrontendModel.succeeded) {
    if ($null -eq $FrontendModel) {
        $Diagnostics += [ordered]@{ code = "frontend_failed"; severity = "error"; message = "C# frontend did not publish a valid artifact."; output = @($FrontendOutput) }
    }
    Write-BuildReport -Result "frontend_failed" -DirectAbiSupported $false -ReportDiagnostics $Diagnostics -Compiler "avidscript-csharp-roslyn"
    Write-Output "[AvidScript][CSharp][Frontend] result=frontend_failed exit_code=$FrontendExitCode report=$ReportPath"
    exit 1
}

$SemanticArguments = @(
    "-NoProfile", "-ExecutionPolicy", "Bypass",
    "-File", (Join-Path $BuildDir "InvokeCSharpSemantic.ps1"),
    "-DotNetPath", $DotNet.Path,
    "-SourcePath", $SourcePath,
    "-SourceId", $SourceId,
    "-FrontendPath", $FrontendArtifactPath,
    "-OutputPath", $SemanticArtifactPath,
    "-Configuration", $Configuration)
if (-not $IsDefaultSource) {
    $SemanticArguments += @(
        "-ExecutableReferenceSourcePath",
        $BindingPackageInfo.ReferenceSourcePath)
}
$SemanticInvocation = Invoke-AvidScriptPowerShell -Arguments $SemanticArguments
$SemanticOutput = @($SemanticInvocation.Output)
$SemanticExitCode = [int]$SemanticInvocation.ExitCode
if (Test-Path -LiteralPath $SemanticArtifactPath -PathType Leaf) {
    try {
        $SemanticModel = Get-Content -Raw -LiteralPath $SemanticArtifactPath | ConvertFrom-Json
        $Diagnostics += @(Convert-CompilerDiagnostics $SemanticModel $SourceId)
        $SelectedScriptTypeName = Get-SelectedScriptTypeName
    }
    catch {
        $Diagnostics += [ordered]@{ code = "semantic_artifact_invalid"; severity = "error"; message = $_.Exception.Message; file = $SourceId }
    }
}
if ($SemanticExitCode -ne 0 -or $null -eq $SemanticModel -or -not $SemanticModel.succeeded) {
    if ($null -eq $SemanticModel) {
        $Diagnostics += [ordered]@{ code = "semantic_failed"; severity = "error"; message = "C# semantic analyzer did not publish a valid artifact."; output = @($SemanticOutput) }
    }
    Write-BuildReport -Result "semantic_failed" -DirectAbiSupported $false -ReportDiagnostics $Diagnostics -Compiler "avidscript-csharp-roslyn-semantic"
    Write-Output "[AvidScript][CSharp][Semantic] result=semantic_failed exit_code=$SemanticExitCode report=$ReportPath"
    exit 1
}


$CompilerArguments = @(
    "-NoProfile", "-ExecutionPolicy", "Bypass",
    "-File", $GuestCompilerPath,
    "-DotNetPath", $DotNet.Path,
    "-SemanticPath", $SemanticArtifactPath,
    "-GuestIrPath", $GuestIrArtifactPath,
    "-WasmPath", $WasmArtifactPath,
    "-InspectionPath", $WasmInspectionArtifactPath,
    "-Configuration", $Configuration)
$CompilerInvocation = Invoke-AvidScriptPowerShell -Arguments $CompilerArguments
$CompilerOutput = @($CompilerInvocation.Output)
$CompilerExitCode = [int]$CompilerInvocation.ExitCode
if (Test-Path -LiteralPath $GuestIrArtifactPath -PathType Leaf) {
    try {
        $GuestIrModel = Get-Content -Raw -LiteralPath $GuestIrArtifactPath | ConvertFrom-Json
    }
    catch {
        $Diagnostics += [ordered]@{ code = "guest_ir_artifact_invalid"; severity = "error"; message = $_.Exception.Message; file = $SourceId }
    }
}
if (Test-Path -LiteralPath $WasmInspectionArtifactPath -PathType Leaf) {
    try {
        $WasmInspectionModel = Get-Content -Raw -LiteralPath $WasmInspectionArtifactPath | ConvertFrom-Json
    }
    catch {
        $Diagnostics += [ordered]@{ code = "wasm_inspection_invalid"; severity = "error"; message = $_.Exception.Message; file = $SourceId }
    }
}
$GuestIrSucceeded = $null -ne $GuestIrModel -and [bool]$GuestIrModel.succeeded
if ($CompilerExitCode -ne 0 -or -not $GuestIrSucceeded -or $null -eq $WasmInspectionModel -or
    -not (Test-Path -LiteralPath $WasmArtifactPath -PathType Leaf)) {
    if (Test-Path -LiteralPath $WasmArtifactPath -PathType Leaf) {
        Remove-Item -LiteralPath $WasmArtifactPath -Force
    }
    $FailureResult = if (-not $GuestIrSucceeded) { "guest_ir_failed" } else { "wasm_backend_failed" }
    $Diagnostics += [ordered]@{
        code = if (-not $GuestIrSucceeded) { "guest_ir_compile_failed" } else { "wasm_backend_compile_failed" }
        severity = "error"
        message = "Formal C# guest compiler failed with exit code $CompilerExitCode."
        output = @($CompilerOutput)
    }
    Write-BuildReport -Result $FailureResult -DirectAbiSupported $false -ReportDiagnostics $Diagnostics
    Write-Output "[AvidScript][CSharp][Build] result=$FailureResult exit_code=$CompilerExitCode report=$ReportPath"
    exit 1
}

$SemanticSha256 = Get-Sha256Hex $SemanticArtifactPath
$GuestIrSha256 = Get-Sha256Hex $GuestIrArtifactPath
$WasmSha256 = Get-Sha256Hex $WasmArtifactPath
$RequiredExports = @($GuestIrModel.exports | ForEach-Object { [string]$_.name })
$ObservedExports = @($WasmInspectionModel.exports | Where-Object { [int]$_.kind -eq 0 } | ForEach-Object { [string]$_.name })
$RequiredImports = @($GuestIrModel.imports | ForEach-Object {
    [ordered]@{ module = [string]$_.module; name = [string]$_.name }
})
if (-not $IsDefaultSource) {
    $DeclaredBindingImportKeys = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::Ordinal)
    foreach ($Import in @($BindingPackageInfo.RequiredImports)) {
        [void]$DeclaredBindingImportKeys.Add("$($Import.Module)`n$($Import.Name)")
    }

    $ObservedBindingImportKeys = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::Ordinal)
    $UnexpectedBindingImports = @()
    foreach ($Import in @($RequiredImports | Where-Object { [string]$_.module -eq "avidscript" })) {
        $Key = "$([string]$Import.module)`n$([string]$Import.name)"
        [void]$ObservedBindingImportKeys.Add($Key)
        if (-not $DeclaredBindingImportKeys.Contains($Key)) {
            $UnexpectedBindingImports += "$([string]$Import.module).$([string]$Import.name)"
        }
    }
    $MissingBindingImports = @($BindingPackageInfo.RequiredImports | Where-Object {
        -not $ObservedBindingImportKeys.Contains("$($_.Module)`n$($_.Name)")
    } | ForEach-Object { "$($_.Module).$($_.Name)" })
    if ($UnexpectedBindingImports.Count -gt 0 -or $MissingBindingImports.Count -gt 0) {
        Remove-LoadableArtifacts
        $Diagnostics += [ordered]@{
            code = "ASBI4203"
            severity = "error"
            message = "Guest IR dynamic imports do not match the selected binding package."
            unexpected_imports = @($UnexpectedBindingImports)
            missing_imports = @($MissingBindingImports)
        }
        Write-BuildReport -Result "binding_import_mismatch" -DirectAbiSupported $false -ReportDiagnostics $Diagnostics
        Write-Output "[AvidScript][CSharp][Build] result=binding_import_mismatch report=$ReportPath"
        exit 1
    }
}
$DirectAbiExports = @(
    "avid_on_begin_play",
    "avid_on_tick",
    "avid_on_end_play",
    "avid_on_timer",
    "avid_on_event",
    "avid_on_gameplay_event")
$MissingDeclaredExports = @($DirectAbiExports | Where-Object { $RequiredExports -notcontains $_ })
$MissingObservedExports = @($DirectAbiExports | Where-Object { $ObservedExports -notcontains $_ })
$GuestContractValid = [int]$GuestIrModel.schema_version -eq 1 -and
    [string]$GuestIrModel.ir_version -eq "1.0" -and
    [bool]$GuestIrModel.succeeded -and
    [string]$GuestIrModel.provenance.semantic_sha256 -eq $SemanticSha256 -and
    [string]$GuestIrModel.provenance.source_sha256 -eq [string]$FrontendModel.source.sha256
$WasmInspectionValid = [int]$WasmInspectionModel.schema_version -eq 1 -and
    [string]$WasmInspectionModel.sha256 -eq $WasmSha256
if (-not $GuestContractValid -or -not $WasmInspectionValid -or
    $MissingDeclaredExports.Count -gt 0 -or $MissingObservedExports.Count -gt 0) {
    Remove-LoadableArtifacts
    $Diagnostics += [ordered]@{
        code = "direct_abi_contract_invalid"
        severity = "error"
        message = "Guest IR provenance or final WASM direct ABI exports are invalid."
        missing_declared_exports = @($MissingDeclaredExports)
        missing_observed_exports = @($MissingObservedExports)
    }
    Write-BuildReport -Result "direct_abi_unsupported" -DirectAbiSupported $false -ReportDiagnostics $Diagnostics
    Write-Output "[AvidScript][CSharp][Build] result=direct_abi_unsupported report=$ReportPath"
    exit 1
}

$Manifest = [ordered]@{
    schema_version = 1
    module_id = $ModuleId
    abi_version = 1
    language = "csharp"
    source = [ordered]@{
        file = Convert-ToProjectRelativePath $SourcePath
        sha256 = [string]$FrontendModel.source.sha256
        script_type = $SelectedScriptTypeName
        frontend_file = Convert-ToProjectRelativePath $FrontendArtifactPath
        frontend_schema_version = [int]$FrontendModel.schema_version
        frontend_version = [string]$FrontendModel.frontend_version
        frontend_sha256 = Get-Sha256Hex $FrontendArtifactPath
        semantic_file = Convert-ToProjectRelativePath $SemanticArtifactPath
        semantic_schema_version = [int]$SemanticModel.schema_version
        semantic_version = [string]$SemanticModel.semantic_version
        semantic_sha256 = $SemanticSha256
    }
    binding_package = if ($null -eq $BindingPackageInfo) { $null } else {
        [ordered]@{
            package_name = [string]$BindingPackageInfo.PackageName
            package_hash = [string]$BindingPackageInfo.PackageHash
            manifest_file = Convert-ToProjectRelativePath $BindingPackageInfo.ManifestPath
            manifest_sha256 = [string]$BindingPackageInfo.ManifestSha256
            descriptor_file = Convert-ToProjectRelativePath $BindingPackageInfo.DescriptorPath
            descriptor_sha256 = [string]$BindingPackageInfo.DescriptorSha256
            reference_source_file = Convert-ToProjectRelativePath $BindingPackageInfo.ReferenceSourcePath
            reference_source_sha256 = [string]$BindingPackageInfo.ReferenceSourceSha256
        }
    }
    guest_ir = [ordered]@{
        file = Convert-ToProjectRelativePath $GuestIrArtifactPath
        schema_version = [int]$GuestIrModel.schema_version
        version = [string]$GuestIrModel.ir_version
        sha256 = $GuestIrSha256
    }
    wasm = [ordered]@{
        file = Convert-ToProjectRelativePath $WasmArtifactPath
        sha256 = $WasmSha256
    }
    required_exports = @($RequiredExports)
    required_imports = @($RequiredImports)
    toolchain = [ordered]@{
        compiler = "avidscript-csharp-guest-wasm"
        frontend = "AvidScript.CSharpFrontend"
        semantic = "AvidScript.CSharpSemantic"
        guest = "AvidScript.CSharpGuest"
        backend = "AvidScript.WasmBackend"
    }
}
try {
    Write-JsonAtomic -Path $ManifestPath -Value $Manifest
    Write-BuildReport -Result "direct_abi_built" -DirectAbiSupported $true -ReportDiagnostics $Diagnostics
}
catch {
    Remove-LoadableArtifacts
    if (Test-Path -LiteralPath $ReportPath -PathType Leaf) {
        Remove-Item -LiteralPath $ReportPath -Force
    }
    Write-Output "[AvidScript][CSharp][Build] result=artifact_publication_failed error=$($_.Exception.Message)"
    exit 1
}
Write-Output "[AvidScript][CSharp][Build] result=direct_abi_built compiler=avidscript-csharp-guest-wasm manifest=$ManifestPath guest_ir=$GuestIrArtifactPath wasm=$WasmArtifactPath sha256=$WasmSha256"
exit 0
