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
    [string]$BindingPackagePath = "",
    [string]$RuntimeBindingPackagePath = "",
    [switch]$OmitRuntimeBindingPackage,
    [string]$PreparedBuildReportPath = "",
    [string]$SemanticCacheRoot = "",
    [string]$CompilationCacheRoot = "",
    [ValidateSet("enabled", "disabled")]
    [string]$DataLaneFusion = "enabled",
    [switch]$AllowGeneratedTypeImports,
    [string]$GeneratedTypeManifestPath = "",
    [switch]$DisableSemanticCache,
    [switch]$DisableCompilationCache
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
$script:GeneratedTypeImportNames = [System.Collections.Generic.HashSet[string]]::new(
    [System.StringComparer]::Ordinal)
$script:GeneratedTypeExportNames = [System.Collections.Generic.HashSet[string]]::new(
    [System.StringComparer]::Ordinal)
. (Join-Path $BuildDir "AvidScriptCSharpSemanticCache.ps1")
. (Join-Path $BuildDir "AvidScriptCSharpCompilationCache.ps1")

if ([string]::IsNullOrWhiteSpace($CompilationCacheRoot)) {
    $CompilationCacheRoot = Join-Path $ProjectRoot "Saved\AvidScript\CSharpCompilationCache\v1"
}

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
    foreach ($Artifact in @(
        $ManifestPath,
        $GuestIrArtifactPath,
        $DebugMapArtifactPath,
        $StateSchemaArtifactPath,
        $WasmArtifactPath)) {
        if (Test-Path -LiteralPath $Artifact -PathType Leaf) {
            Remove-Item -LiteralPath $Artifact -Force
        }
    }
}

function Invoke-AvidScriptPowerShell {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)
    $PowerShellHost = Join-Path $PSHOME "pwsh.exe"
    if (-not (Test-Path -LiteralPath $PowerShellHost -PathType Leaf)) {
        throw "PowerShell 7 host is missing: $PowerShellHost"
    }
    $PreviousErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        $RawOutput = @(& $PowerShellHost @Arguments 2>&1)
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

function New-BindingPackageReportValue {
    param(
        [AllowNull()][object]$PackageInfo,
        [AllowEmptyCollection()][object[]]$UsedImports,
        [AllowEmptyCollection()][int[]]$UsedObjectTypeOrdinals,
        [bool]$Required,
        [switch]$ExplicitEmpty
    )

    if ($null -eq $PackageInfo) {
        if ($ExplicitEmpty) {
            return [ordered]@{
                required = $false
                package_name = ""
                package_hash = ""
                manifest_file = ""
                manifest_sha256 = ""
                descriptor_file = ""
                descriptor_sha256 = ""
                reference_source_file = ""
                reference_source_sha256 = ""
                profile_import_count = 0
                used_import_count = 0
                used_imports = @()
                used_object_type_count = 0
                used_object_type_ordinals = @()
            }
        }
        return $null
    }
    return [ordered]@{
        required = $Required
        package_name = [string]$PackageInfo.PackageName
        package_hash = [string]$PackageInfo.PackageHash
        manifest_file = Convert-ToProjectRelativePath $PackageInfo.ManifestPath
        manifest_sha256 = [string]$PackageInfo.ManifestSha256
        descriptor_file = Convert-ToProjectRelativePath $PackageInfo.DescriptorPath
        descriptor_sha256 = [string]$PackageInfo.DescriptorSha256
        reference_source_file = Convert-ToProjectRelativePath $PackageInfo.ReferenceSourcePath
        reference_source_sha256 = [string]$PackageInfo.ReferenceSourceSha256
        profile_import_count = @($PackageInfo.RequiredImports).Count
        used_import_count = @($UsedImports).Count
        used_imports = @($UsedImports)
        used_object_type_count = @($UsedObjectTypeOrdinals).Count
        used_object_type_ordinals = @($UsedObjectTypeOrdinals)
    }
}

function Test-CompilerInjectedBindingImport {
    param(
        [Parameter(Mandatory = $true)][object]$Import,
        [Parameter(Mandatory = $true)][bool]$AllowDataLaneImports,
        [Parameter(Mandatory = $true)][bool]$AllowGeneratedTypeImports
    )

    if ([string]$Import.module -cne "avidscript" -or
        [string]$Import.optimization_class -cne "none" -or
        [int]$Import.binding_ordinal -ne -1) {
        return $false
    }

    $ParameterTypes = @($Import.parameter_type_ids | ForEach-Object { [string]$_ })
    if ([string]$Import.id -ceq "avidscript.delegate_output_write.v1" -and
        [string]$Import.name -ceq "avid_delegate_output_write" -and
        [string]$Import.dispatch_class -ceq "semantic") {
        return $ParameterTypes.Count -eq 3 -and
            $ParameterTypes[0] -ceq "type:int32" -and
            $ParameterTypes[1] -ceq "type:int32" -and
            $ParameterTypes[2] -ceq "type:address" -and
            [string]$Import.return_type_id -ceq "type:int32"
    }
    if ($AllowGeneratedTypeImports -and
        [string]$Import.dispatch_class -ceq "semantic") {
        $GeneratedMatch = [System.Text.RegularExpressions.Regex]::Match(
            [string]$Import.id,
            '^import:method:synthetic:ue_property:([0-9]+):([0-9]+):(get|set)$',
            [System.Text.RegularExpressions.RegexOptions]::CultureInvariant)
        if ($GeneratedMatch.Success) {
            $ExpectedName = "avid_ue_property_$($GeneratedMatch.Groups[1].Value)_$($GeneratedMatch.Groups[2].Value)_$($GeneratedMatch.Groups[3].Value)"
            if ([string]$Import.name -cne $ExpectedName -or
                -not $script:GeneratedTypeImportNames.Contains($ExpectedName) -or
                $ParameterTypes.Count -lt 1 -or
                -not $ParameterTypes[0].StartsWith("type:global::", [System.StringComparison]::Ordinal)) {
                return $false
            }
            if ($GeneratedMatch.Groups[3].Value -ceq "get") {
                return $ParameterTypes.Count -eq 1 -and
                    [string]$Import.return_type_id -cne "type:void"
            }
            return $ParameterTypes.Count -eq 2 -and
                [string]$Import.return_type_id -ceq "type:void"
        }
    }
    if (-not $AllowDataLaneImports -or
        [string]$Import.dispatch_class -cne "data_lane") {
        return $false
    }
    if ([string]$Import.id -ceq "import:__avidscript_internal.avid_data_lane_epoch" -and
        [string]$Import.name -ceq "avid_data_lane_epoch") {
        return $ParameterTypes.Count -eq 0 -and
            [string]$Import.return_type_id -ceq "type:uint64"
    }
    if ([string]$Import.id -ceq "import:__avidscript_internal.avid_data_lane_submit" -and
        [string]$Import.name -ceq "avid_data_lane_submit") {
        return $ParameterTypes.Count -eq 2 -and
            $ParameterTypes[0] -ceq "type:address" -and
            $ParameterTypes[1] -ceq "type:int32" -and
            [string]$Import.return_type_id -ceq "type:int32"
    }
    return $false
}

function Test-BindingPackageImports {
    param(
        [AllowNull()][object]$PackageInfo,
        [AllowEmptyCollection()][object[]]$GuestImports,
        [bool]$AllowDataLaneImports = $false,
        [bool]$AllowGeneratedTypeImports = $false
    )

    $DeclaredByKey = [System.Collections.Generic.Dictionary[string, object]]::new(
        [System.StringComparer]::Ordinal)
    if ($null -ne $PackageInfo) {
        foreach ($Import in @($PackageInfo.RequiredImports)) {
            $DeclaredByKey.Add("$($Import.Module)`n$($Import.Name)", $Import)
        }
    }

    $ObservedKeys = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::Ordinal)
    $UnexpectedImports = @()
    foreach ($Import in @($GuestImports | Where-Object { [string]$_.module -eq "avidscript" })) {
        $Key = "$([string]$Import.module)`n$([string]$Import.name)"
        [void]$ObservedKeys.Add($Key)
        if (-not $DeclaredByKey.ContainsKey($Key) -and
            -not (Test-CompilerInjectedBindingImport `
                -Import $Import `
                -AllowDataLaneImports $AllowDataLaneImports `
                -AllowGeneratedTypeImports $AllowGeneratedTypeImports)) {
            $UnexpectedImports += "$([string]$Import.module).$([string]$Import.name)"
        }
    }

    $UsedImports = @($DeclaredByKey.Values | Where-Object {
        $ObservedKeys.Contains("$($_.Module)`n$($_.Name)")
    } | Sort-Object Ordinal | ForEach-Object {
        [ordered]@{
            stable_id = [string]$_.StableId
            ordinal = [int]$_.Ordinal
            module = [string]$_.Module
            name = [string]$_.Name
            signature = [string]$_.Signature
        }
    })
    return [pscustomobject]@{
        UnexpectedImports = @($UnexpectedImports)
        UsedImports = @($UsedImports)
    }
}

function Test-JsonObjectHasProperties {
    param(
        [object]$Value,
        [Parameter(Mandatory = $true)][string[]]$RequiredProperties
    )

    if ($null -eq $Value -or $Value -isnot [System.Management.Automation.PSCustomObject]) {
        return $false
    }

    $PropertyNames = @($Value.PSObject.Properties.Name)
    foreach ($RequiredProperty in $RequiredProperties) {
        if ($PropertyNames -notcontains $RequiredProperty) {
            return $false
        }
    }
    return $true
}

function Test-JsonNonEmptyString {
    param([object]$Value)

    return $Value -is [string] -and -not [string]::IsNullOrWhiteSpace($Value)
}

function Try-GetJsonInt32 {
    param(
        [object]$Value,
        [Parameter(Mandatory = $true)][ref]$ParsedValue
    )

    if ($Value -is [int]) {
        $ParsedValue.Value = $Value
        return $true
    }

    if ($Value -is [long] -and
        $Value -ge [int]::MinValue -and
        $Value -le [int]::MaxValue) {
        $ParsedValue.Value = [int]$Value
        return $true
    }

    return $false
}

function Test-JsonLowercaseSha256 {
    param([object]$Value)

    return $Value -is [string] -and $Value -cmatch '^[0-9a-f]{64}$'
}

function Get-UsedObjectTypeOrdinals {
    param([Parameter(Mandatory = $true)]$Model)

    $Ordinals = [System.Collections.Generic.SortedSet[int]]::new()
    $ObjectTypeImportIds = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::Ordinal)
    foreach ($Import in @($Model.imports)) {
        if ([string]$Import.module -ceq "avidscript" -and
            [string]$Import.name -ceq "avid_object_type_is_a") {
            [void]$ObjectTypeImportIds.Add([string]$Import.id)
        }
    }

    $ConstantsByResultId =
        [System.Collections.Generic.Dictionary[string, object]]::new(
            [System.StringComparer]::Ordinal)
    foreach ($Function in @($Model.functions)) {
        foreach ($Block in @($Function.blocks)) {
            foreach ($Instruction in @($Block.instructions)) {
                if ([string]$Instruction.op -cne "constant") {
                    continue
                }
                $ResultId = [string]$Instruction.result_id
                if (-not [string]::IsNullOrWhiteSpace($ResultId)) {
                    $ConstantsByResultId[$ResultId] = $Instruction
                }
                if ([string]$Instruction.constant.kind -cne "object_type_ref") {
                    continue
                }
                $Ordinal = 0
                $HasObjectTypeOrdinal =
                    $Instruction.constant.value -is [string] -and
                    [int]::TryParse(
                        [string]$Instruction.constant.value,
                        [System.Globalization.NumberStyles]::None,
                        [System.Globalization.CultureInfo]::InvariantCulture,
                        [ref]$Ordinal) -and
                    $Ordinal -ge 0
                if (-not $HasObjectTypeOrdinal) {
                    throw "Guest IR object_type_ref constants must contain a direct non-negative int32 ordinal."
                }
                [void]$Ordinals.Add($Ordinal)
            }
        }
    }

    foreach ($Function in @($Model.functions)) {
        foreach ($Block in @($Function.blocks)) {
            foreach ($Instruction in @($Block.instructions)) {
                if ([string]$Instruction.op -cne "call" -or
                    -not $ObjectTypeImportIds.Contains(
                        [string]$Instruction.target_id)) {
                    continue
                }

                $Operands = @($Instruction.operand_ids)
                $OrdinalInstruction = $null
                $Ordinal = 0
                $HasDirectOrdinal =
                    $Operands.Count -eq 3 -and
                    $ConstantsByResultId.TryGetValue(
                        [string]$Operands[2],
                        [ref]$OrdinalInstruction) -and
                    [string]$OrdinalInstruction.constant.kind -ceq "int32" -and
                    $OrdinalInstruction.constant.value -is [string] -and
                    [int]::TryParse(
                        [string]$OrdinalInstruction.constant.value,
                        [System.Globalization.NumberStyles]::None,
                        [System.Globalization.CultureInfo]::InvariantCulture,
                        [ref]$Ordinal) -and
                    $Ordinal -ge 0
                if (-not $HasDirectOrdinal) {
                    throw "Guest IR avid_object_type_is_a calls must use a direct non-negative int32 constant ordinal."
                }
                [void]$Ordinals.Add($Ordinal)
            }
        }
    }
    return @($Ordinals)
}

function Write-BuildReport {
    param(
        [Parameter(Mandatory = $true)][string]$Result,
        [Parameter(Mandatory = $true)][bool]$DirectAbiSupported,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][object[]]$ReportDiagnostics,
        [string]$Compiler = "avidscript-csharp-guest-wasm"
    )

    $StateSchemaVersion = 0
    $StateContractVersion = 0
    if ($null -ne $StateSchemaModel) {
        [void](Try-GetJsonInt32 -Value $StateSchemaModel.schema_version -ParsedValue ([ref]$StateSchemaVersion))
        [void](Try-GetJsonInt32 -Value $StateSchemaModel.contract_version -ParsedValue ([ref]$StateContractVersion))
    }

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
        compilation = [ordered]@{
            data_lane_fusion = $DataLaneFusion
        }
        build_reuse = $BuildReuse
        semantic_cache = $SemanticCache
        compilation_cache = $CompilationCache
        tool_invocations = $ToolInvocations
        binding_authorization = New-BindingPackageReportValue `
            -PackageInfo $BindingAuthorizationInfo `
            -UsedImports $UsedAuthorizationBindingImports `
            -UsedObjectTypeOrdinals $UsedObjectTypeOrdinals `
            -Required (-not $IsDefaultSource) `
            -ExplicitEmpty
        binding_package = New-BindingPackageReportValue `
            -PackageInfo $BindingPackageInfo `
            -UsedImports $UsedRuntimeBindingImports `
            -UsedObjectTypeOrdinals $UsedObjectTypeOrdinals `
            -Required ($null -ne $BindingPackageInfo)
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
            debug_map_file = if (Test-Path -LiteralPath $DebugMapArtifactPath -PathType Leaf) { Convert-ToProjectRelativePath $DebugMapArtifactPath } else { "" }
            state_schema_file = if (Test-Path -LiteralPath $StateSchemaArtifactPath -PathType Leaf) { Convert-ToProjectRelativePath $StateSchemaArtifactPath } else { "" }
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
            reachability_mode = if ($null -eq $SemanticModel -or $null -eq $SemanticModel.reachability) { "" } else { [string]$SemanticModel.reachability.mode }
            root_callable_count = if ($null -eq $SemanticModel -or $null -eq $SemanticModel.reachability) { 0 } else { @($SemanticModel.reachability.root_callable_ids).Count }
            reachable_callable_count = if ($null -eq $SemanticModel -or $null -eq $SemanticModel.reachability) { 0 } else { @($SemanticModel.reachability.reachable_callable_ids).Count }
            reachable_import_count = if ($null -eq $SemanticModel -or $null -eq $SemanticModel.reachability) { 0 } else { @($SemanticModel.reachability.reachable_imports).Count }
        }
        guest_ir = [ordered]@{
            schema_version = if ($null -eq $GuestIrModel) { 0 } else { [int]$GuestIrModel.schema_version }
            version = if ($null -eq $GuestIrModel) { "" } else { [string]$GuestIrModel.ir_version }
            succeeded = if ($null -eq $GuestIrModel) { $false } else { [bool]$GuestIrModel.succeeded }
            semantic_sha256 = if ($null -eq $GuestIrModel) { "" } else { [string]$GuestIrModel.provenance.semantic_sha256 }
            sha256 = Get-Sha256Hex $GuestIrArtifactPath
        }
        debug_map = [ordered]@{
            schema_version = if ($null -eq $DebugMapModel) { 0 } else { [int]$DebugMapModel.schema_version }
            version = if ($null -eq $DebugMapModel) { "" } else { [string]$DebugMapModel.debug_version }
            module_id = if ($null -eq $DebugMapModel) { "" } else { [string]$DebugMapModel.module_id }
            imported_function_count = if ($null -eq $DebugMapModel) { 0 } else { [int]$DebugMapModel.imported_function_count }
            defined_function_count = if ($null -eq $DebugMapModel) { 0 } else { [int]$DebugMapModel.defined_function_count }
            function_count = if ($null -eq $DebugMapModel -or $DebugMapModel.functions -isnot [System.Array]) { 0 } else { $DebugMapModel.functions.Count }
            sha256 = Get-Sha256Hex $DebugMapArtifactPath
        }
        state_migration = [ordered]@{
            schema_version = $StateSchemaVersion
            strategy = if ($null -ne $StateSchemaModel -and $StateSchemaModel.strategy -is [string]) { $StateSchemaModel.strategy } else { "" }
            policy = if ($null -ne $StateSchemaModel -and $StateSchemaModel.policy -is [string]) { $StateSchemaModel.policy } else { "" }
            contract_version = $StateContractVersion
            owner_type_id = if ($null -ne $StateSchemaModel -and $StateSchemaModel.owner_type_id -is [string]) { $StateSchemaModel.owner_type_id } else { "" }
            slot_count = if ($null -ne $StateSchemaModel -and $StateSchemaModel.slots -is [System.Array]) { $StateSchemaModel.slots.Count } else { 0 }
            sha256 = Get-Sha256Hex $StateSchemaArtifactPath
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
if (-not [string]::IsNullOrWhiteSpace($PreparedBuildReportPath)) {
    $PreparedBuildReportPath = [System.IO.Path]::GetFullPath($PreparedBuildReportPath)
}
if ([string]::IsNullOrWhiteSpace($SemanticCacheRoot)) {
    $SemanticCacheRoot = Join-Path $ProjectRoot "Saved\AvidScript\CSharpSemanticCache\v1"
}
$SemanticCacheRoot = [System.IO.Path]::GetFullPath($SemanticCacheRoot)
if ($AllowGeneratedTypeImports) {
    if ([string]::IsNullOrWhiteSpace($GeneratedTypeManifestPath) -or
        -not (Test-Path -LiteralPath $GeneratedTypeManifestPath -PathType Leaf)) {
        throw "Generated type import authorization requires a generated type manifest."
    }
    $GeneratedTypeManifestPath = (Resolve-Path -LiteralPath $GeneratedTypeManifestPath).Path
    $GeneratedTypeManifest = Get-Content -Raw -LiteralPath $GeneratedTypeManifestPath | ConvertFrom-Json
    if ([int]$GeneratedTypeManifest.schema_version -ne 6 -or
        @($GeneratedTypeManifest.types).Count -eq 0) {
        throw "Generated type import authorization manifest must use schema 6 and contain types."
    }
    foreach ($GeneratedType in @($GeneratedTypeManifest.types)) {
        foreach ($GeneratedFunction in @($GeneratedType.functions)) {
            $ExportName = [string]$GeneratedFunction.export_name
            if ($ExportName -cnotmatch '^avid_ue_[0-9a-f]{32}$' -or
                -not $script:GeneratedTypeExportNames.Add($ExportName)) {
                throw "Generated type manifest contains an invalid or duplicate canonical export."
            }
        }
        foreach ($GeneratedProperty in @($GeneratedType.properties)) {
            foreach ($ImportName in @(
                [string]$GeneratedProperty.getter_import_name,
                [string]$GeneratedProperty.setter_import_name)) {
                if (-not [string]::IsNullOrWhiteSpace($ImportName) -and
                    ($ImportName -cnotmatch '^avid_ue_property_[0-9]+_[0-9]+_(get|set)$' -or
                     -not $script:GeneratedTypeImportNames.Add($ImportName))) {
                    throw "Generated type manifest contains an invalid or duplicate property import."
                }
            }
        }
    }
}
$FrontendArtifactPath = Join-Path $OutputRoot "$ArtifactStem.csharp.frontend.json"
$SemanticArtifactPath = Join-Path $OutputRoot "$ArtifactStem.csharp.semantic.json"
$GuestIrArtifactPath = Join-Path $OutputRoot "$ArtifactStem.guestir.json"
$DebugMapArtifactPath = Join-Path $OutputRoot "$ArtifactStem.csharp.debug.json"
$StateSchemaArtifactPath = Join-Path $OutputRoot "$ArtifactStem.state.json"
$WasmArtifactPath = Join-Path $OutputRoot "$ArtifactStem.wasm"
$WasmInspectionArtifactPath = Join-Path $OutputRoot "$ArtifactStem.wasm.inspect.json"
$LegacyAdapterWasmPath = Join-Path $OutputRoot "$ArtifactStem.csharp_adapter.wasm"
$LegacyDotNetWasmPath = Join-Path $OutputRoot "$ArtifactStem.dotnet.wasm"
$FrontendModel = $null
$SemanticModel = $null
$GuestIrModel = $null
$DebugMapModel = $null
$StateSchemaModel = $null
$StateSchemaArtifactExists = $false
$WasmInspectionModel = $null
$SelectedScriptTypeName = ""
$RequiredExports = @()
$RequiredImports = @()
$ObservedExports = @()
$UsedAuthorizationBindingImports = @()
$UsedRuntimeBindingImports = @()
$UsedObjectTypeOrdinals = @()
$BindingAuthorizationInfo = $null
$BindingPackageInfo = $null
$Diagnostics = @()
$DotNet = [pscustomobject]@{ Found = $false; Source = ""; Path = ""; Checked = @() }
$BuildReuse = [ordered]@{
    prepared_report_file = ""
    prepared_report_sha256 = ""
    frontend_reused = $false
    semantic_reused = $false
    guest_ir_reused = $false
    debug_map_reused = $false
    state_schema_reused = $false
    wasm_reused = $false
}
$SemanticCache = [ordered]@{
    schema_version = 1
    enabled = -not [bool]$DisableSemanticCache
    key = ""
    toolchain_fingerprint = ""
    lookup = "disabled"
    entry_report_file = ""
    entry_report_sha256 = ""
    published = $false
    diagnostic_code = ""
    diagnostic_message = ""
}
$CompilationCache = [ordered]@{
    schema_version = 1
    enabled = -not [bool]$DisableCompilationCache
    key = ""
    toolchain_fingerprint = ""
    lookup = "disabled"
    entry_report_file = ""
    entry_report_sha256 = ""
    published = $false
    diagnostic_code = ""
    diagnostic_message = ""
}
$ToolInvocations = [ordered]@{
    frontend = 0
    semantic = 0
    guest_ir = 0
    wasm_backend = 0
}

New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
foreach ($Artifact in @(
    $ReportPath,
    $ManifestPath,
    $FrontendArtifactPath,
    $SemanticArtifactPath,
    $GuestIrArtifactPath,
    $DebugMapArtifactPath,
    $StateSchemaArtifactPath,
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
        $BindingAuthorizationInfo = Resolve-AvidScriptCSharpBindingPackage -ManifestPath $BindingPackagePath
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

    if ($OmitRuntimeBindingPackage -and -not [string]::IsNullOrWhiteSpace($RuntimeBindingPackagePath)) {
        $Diagnostics += [ordered]@{
            code = "ASBI4301"
            severity = "error"
            message = "RuntimeBindingPackagePath and OmitRuntimeBindingPackage are mutually exclusive."
            file = Convert-ToProjectRelativePath $RuntimeBindingPackagePath
        }
        Write-BuildReport -Result "runtime_binding_package_invalid" -DirectAbiSupported $false -ReportDiagnostics $Diagnostics
        Write-Output "[AvidScript][CSharp][Build] result=runtime_binding_package_invalid report=$ReportPath"
        exit 1
    }
    if (-not $OmitRuntimeBindingPackage) {
        if ([string]::IsNullOrWhiteSpace($RuntimeBindingPackagePath)) {
            $RuntimeBindingPackagePath = $BindingPackagePath
        }
        try {
            $BindingPackageInfo = Resolve-AvidScriptCSharpBindingPackage -ManifestPath $RuntimeBindingPackagePath
        }
        catch {
            $Diagnostics += [ordered]@{
                code = "ASBI4302"
                severity = "error"
                message = $_.Exception.Message
                file = Convert-ToProjectRelativePath $RuntimeBindingPackagePath
            }
            Write-BuildReport -Result "runtime_binding_package_invalid" -DirectAbiSupported $false -ReportDiagnostics $Diagnostics
            Write-Output "[AvidScript][CSharp][Build] result=runtime_binding_package_invalid report=$ReportPath"
            exit 1
        }
        if ($BindingPackageInfo.PackageName -cne $BindingAuthorizationInfo.PackageName) {
            $Diagnostics += [ordered]@{
                code = "ASBI4302"
                severity = "error"
                message = "Runtime and authorization binding packages must have the same package_name capability."
                file = Convert-ToProjectRelativePath $RuntimeBindingPackagePath
            }
            Write-BuildReport -Result "runtime_binding_package_invalid" -DirectAbiSupported $false -ReportDiagnostics $Diagnostics
            Write-Output "[AvidScript][CSharp][Build] result=runtime_binding_package_invalid report=$ReportPath"
            exit 1
        }
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

$SemanticCacheContext = $null
$SemanticCacheHit = $false
if ([string]::IsNullOrWhiteSpace($PreparedBuildReportPath) -and -not $DisableSemanticCache) {
    try {
        $SemanticCacheContext = Get-AvidScriptCSharpSemanticCacheContext `
            -PluginRoot $PluginRoot `
            -ProjectRoot $ProjectRoot `
            -CacheRoot $SemanticCacheRoot `
            -Configuration $Configuration `
            -SourcePath $SourcePath `
            -ProjectPath $ProjectPath `
            -AuthorizationPackage $BindingAuthorizationInfo
        $SemanticCache.key = [string]$SemanticCacheContext.CacheKey
        $SemanticCache.toolchain_fingerprint = [string]$SemanticCacheContext.ToolchainFingerprint
        $CacheImport = Import-AvidScriptCSharpSemanticCacheEntry `
            -Context $SemanticCacheContext `
            -ProjectRoot $ProjectRoot `
            -ExpectedSourcePath $SourcePath `
            -ExpectedAuthorizationPackage $BindingAuthorizationInfo `
            -FrontendDestinationPath $FrontendArtifactPath `
            -SemanticDestinationPath $SemanticArtifactPath
        $SemanticCache.lookup = [string]$CacheImport.Status
        $SemanticCache.diagnostic_code = [string]$CacheImport.DiagnosticCode
        $SemanticCache.diagnostic_message = [string]$CacheImport.DiagnosticMessage
        if ($CacheImport.Status -ceq "rejected" -and
            -not [string]::IsNullOrWhiteSpace([string]$CacheImport.DiagnosticCode)) {
            $Diagnostics += [ordered]@{
                code = [string]$CacheImport.DiagnosticCode
                severity = "warning"
                message = [string]$CacheImport.DiagnosticMessage
                file = Convert-ToProjectRelativePath $SemanticCacheRoot
            }
        }
        if ($CacheImport.Status -ceq "hit") {
            $SemanticCacheHit = $true
            $SemanticCache.entry_report_file = Convert-ToProjectRelativePath $CacheImport.EntryReportPath
            $SemanticCache.entry_report_sha256 = [string]$CacheImport.EntryReportSha256
            $FrontendModel = $CacheImport.FrontendModel
            $SemanticModel = $CacheImport.SemanticModel
            $Diagnostics += @(Convert-CompilerDiagnostics $FrontendModel $SourceId)
            $Diagnostics += @(Convert-CompilerDiagnostics $SemanticModel $SourceId)
            $SelectedScriptTypeName = Get-SelectedScriptTypeName
            $BuildReuse.frontend_reused = $true
            $BuildReuse.semantic_reused = $true
        }
    }
    catch {
        $SemanticCacheContext = $null
        $SemanticCache.lookup = "rejected"
        $CacheErrorCode = [string]$_.Exception.Data["AvidScriptCode"]
        if ([string]::IsNullOrWhiteSpace($CacheErrorCode)) {
            $CacheErrorCode = "ASBI4501"
        }
        $SemanticCache.diagnostic_code = $CacheErrorCode
        $SemanticCache.diagnostic_message = $_.Exception.Message
        $Diagnostics += [ordered]@{
            code = $CacheErrorCode
            severity = "warning"
            message = $_.Exception.Message
            file = Convert-ToProjectRelativePath $SemanticCacheRoot
        }
    }
}

if (-not [string]::IsNullOrWhiteSpace($PreparedBuildReportPath)) {
    try {
        Assert-AvidScriptPreparedSemantic `
            -Condition (-not $PreparedBuildReportPath.Equals(
                $ReportPath,
                [System.StringComparison]::OrdinalIgnoreCase)) `
            -Code "ASBI4404" `
            -Message "Prepared and final build report paths must be different."
        $PreparedSemantic = Import-AvidScriptCSharpPreparedSemantic `
            -PreparedReportPath $PreparedBuildReportPath `
            -ProjectRoot $ProjectRoot `
            -ExpectedSourcePath $SourcePath `
            -ExpectedAuthorizationPackage $BindingAuthorizationInfo `
            -FrontendDestinationPath $FrontendArtifactPath `
            -SemanticDestinationPath $SemanticArtifactPath
        $FrontendModel = $PreparedSemantic.FrontendModel
        $SemanticModel = $PreparedSemantic.SemanticModel
        $Diagnostics += @(Convert-CompilerDiagnostics $FrontendModel $SourceId)
        $Diagnostics += @(Convert-CompilerDiagnostics $SemanticModel $SourceId)
        $SelectedScriptTypeName = Get-SelectedScriptTypeName
        $BuildReuse.prepared_report_file = Convert-ToProjectRelativePath $PreparedSemantic.PreparedReportPath
        $BuildReuse.prepared_report_sha256 = [string]$PreparedSemantic.PreparedReportSha256
        $BuildReuse.frontend_reused = $true
        $BuildReuse.semantic_reused = $true
    }
    catch {
        $PreparedErrorCode = [string]$_.Exception.Data["AvidScriptCode"]
        if ([string]::IsNullOrWhiteSpace($PreparedErrorCode)) {
            $PreparedErrorCode = "ASBI4403"
        }
        $Diagnostics += [ordered]@{
            code = $PreparedErrorCode
            severity = "error"
            message = $_.Exception.Message
            file = Convert-ToProjectRelativePath $PreparedBuildReportPath
        }
        Remove-LoadableArtifacts
        Write-BuildReport `
            -Result "prepared_semantic_invalid" `
            -DirectAbiSupported $false `
            -ReportDiagnostics $Diagnostics `
            -Compiler "avidscript-csharp-prepared-semantic"
        Write-Output "[AvidScript][CSharp][PreparedSemantic] result=prepared_semantic_invalid report=$ReportPath"
        exit 1
    }
}
elseif (-not $SemanticCacheHit) {
    $FrontendArguments = @(
        "-NoProfile", "-ExecutionPolicy", "Bypass",
        "-File", (Join-Path $BuildDir "InvokeCSharpFrontend.ps1"),
        "-DotNetPath", $DotNet.Path,
        "-SourcePath", $SourcePath,
        "-SourceId", $SourceId,
        "-OutputPath", $FrontendArtifactPath,
        "-Configuration", $Configuration)
    ++$ToolInvocations.frontend
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
            $BindingAuthorizationInfo.ReferenceSourcePath)
    }
    ++$ToolInvocations.semantic
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
}


$FrontendArtifactSha256 = Get-Sha256Hex $FrontendArtifactPath
$SemanticSha256 = Get-Sha256Hex $SemanticArtifactPath
$CompilationCacheContext = $null
$CompilationCacheHit = $false
if (-not $DisableCompilationCache) {
    try {
        $CompilationCacheContext = Get-AvidScriptCSharpCompilationCacheContext `
            -PluginRoot $PluginRoot `
            -ProjectRoot $ProjectRoot `
            -CacheRoot $CompilationCacheRoot `
            -SemanticArtifactPath $SemanticArtifactPath `
            -FrontendArtifactSha256 $FrontendArtifactSha256 `
            -GuestCompilerPath $GuestCompilerPath `
            -DotNetPath $DotNet.Path `
            -ModuleId $ModuleId `
            -Configuration $Configuration `
            -DataLaneFusion $DataLaneFusion `
            -AuthorizationPackage $BindingAuthorizationInfo `
            -RuntimePackage $BindingPackageInfo
        $CompilationCache.key = [string]$CompilationCacheContext.CacheKey
        $CompilationCache.toolchain_fingerprint =
            [string]$CompilationCacheContext.ToolchainFingerprint
        $CompilationImport = Import-AvidScriptCSharpCompilationCacheEntry `
            -Context $CompilationCacheContext `
            -Destinations @{
                guest_ir = $GuestIrArtifactPath
                debug_map = $DebugMapArtifactPath
                state_schema = $StateSchemaArtifactPath
                wasm = $WasmArtifactPath
                wasm_inspection = $WasmInspectionArtifactPath
            }
        $CompilationCache.lookup = [string]$CompilationImport.Status
        $CompilationCache.entry_report_file = if (
            [string]::IsNullOrWhiteSpace([string]$CompilationImport.EntryReportPath)) {
            ""
        }
        else {
            Convert-ToProjectRelativePath $CompilationImport.EntryReportPath
        }
        $CompilationCache.entry_report_sha256 =
            [string]$CompilationImport.EntryReportSha256
        $CompilationCache.diagnostic_code = [string]$CompilationImport.DiagnosticCode
        $CompilationCache.diagnostic_message = [string]$CompilationImport.DiagnosticMessage
        if ($CompilationImport.Status -ceq "hit") {
            $CompilationCacheHit = $true
            $BuildReuse.guest_ir_reused = $true
            $BuildReuse.debug_map_reused = $true
            $BuildReuse.state_schema_reused = $true
            $BuildReuse.wasm_reused = $true
        }
        elseif ($CompilationImport.Status -ceq "rejected") {
            $Diagnostics += [ordered]@{
                code = [string]$CompilationImport.DiagnosticCode
                severity = "warning"
                message = [string]$CompilationImport.DiagnosticMessage
                file = Convert-ToProjectRelativePath $CompilationCacheRoot
            }
        }
    }
    catch {
        $CompilationCacheContext = $null
        $CompilationCache.lookup = "rejected"
        $CompilationErrorCode = [string]$_.Exception.Data["AvidScriptCode"]
        if ([string]::IsNullOrWhiteSpace($CompilationErrorCode)) {
            $CompilationErrorCode = "ASBI4603"
        }
        $CompilationCache.diagnostic_code = $CompilationErrorCode
        $CompilationCache.diagnostic_message = $_.Exception.Message
        $Diagnostics += [ordered]@{
            code = $CompilationErrorCode
            severity = "warning"
            message = $_.Exception.Message
            file = Convert-ToProjectRelativePath $CompilationCacheRoot
        }
    }
}

$CompilerOutput = @()
$CompilerExitCode = 0
if (-not $CompilationCacheHit) {
    $CompilerArguments = @(
        "-NoProfile", "-ExecutionPolicy", "Bypass",
        "-File", $GuestCompilerPath,
        "-DotNetPath", $DotNet.Path,
        "-SemanticPath", $SemanticArtifactPath,
        "-FrontendArtifactSha256", $FrontendArtifactSha256,
        "-GuestIrPath", $GuestIrArtifactPath,
        "-DebugMapPath", $DebugMapArtifactPath,
        "-StateSchemaPath", $StateSchemaArtifactPath,
        "-WasmPath", $WasmArtifactPath,
        "-InspectionPath", $WasmInspectionArtifactPath,
        "-Configuration", $Configuration,
        "-DataLaneFusion", $DataLaneFusion)
        ++$ToolInvocations.guest_ir
        ++$ToolInvocations.wasm_backend
    $CompilerInvocation = Invoke-AvidScriptPowerShell -Arguments $CompilerArguments
    $CompilerOutput = @($CompilerInvocation.Output)
    $CompilerExitCode = [int]$CompilerInvocation.ExitCode
}
if (Test-Path -LiteralPath $GuestIrArtifactPath -PathType Leaf) {
    try {
        $GuestIrModel = Get-Content -Raw -LiteralPath $GuestIrArtifactPath | ConvertFrom-Json
    }
    catch {
        $Diagnostics += [ordered]@{ code = "guest_ir_artifact_invalid"; severity = "error"; message = $_.Exception.Message; file = $SourceId }
    }
}
if (Test-Path -LiteralPath $DebugMapArtifactPath -PathType Leaf) {
    try {
        $DebugMapModel = Get-Content -Raw -LiteralPath $DebugMapArtifactPath | ConvertFrom-Json
    }
    catch {
        $Diagnostics += [ordered]@{ code = "debug_map_artifact_invalid"; severity = "error"; message = $_.Exception.Message; file = $SourceId }
    }
}
$StateSchemaArtifactExists = Test-Path -LiteralPath $StateSchemaArtifactPath -PathType Leaf
if ($StateSchemaArtifactExists) {
    try {
        $StateSchemaModel = Get-Content -Raw -LiteralPath $StateSchemaArtifactPath | ConvertFrom-Json
    }
    catch {
        $Diagnostics += [ordered]@{ code = "state_schema_artifact_invalid"; severity = "error"; message = $_.Exception.Message; file = $SourceId }
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
$DebugMapPublished = $null -ne $DebugMapModel -and (Test-Path -LiteralPath $DebugMapArtifactPath -PathType Leaf)
if ($CompilerExitCode -ne 0 -or -not $GuestIrSucceeded -or -not $DebugMapPublished -or -not $StateSchemaArtifactExists -or $null -eq $WasmInspectionModel -or
    -not (Test-Path -LiteralPath $WasmArtifactPath -PathType Leaf)) {
    Remove-LoadableArtifacts
    $FailureResult = if (-not $GuestIrSucceeded) { "guest_ir_failed" } elseif (-not $DebugMapPublished) { "debug_map_failed" } else { "wasm_backend_failed" }
    $Diagnostics += [ordered]@{
        code = if (-not $GuestIrSucceeded) { "guest_ir_compile_failed" } elseif (-not $DebugMapPublished) { "debug_map_compile_failed" } else { "wasm_backend_compile_failed" }
        severity = "error"
        message = "Formal C# guest compiler failed with exit code $CompilerExitCode."
        output = @($CompilerOutput)
    }
    Write-BuildReport -Result $FailureResult -DirectAbiSupported $false -ReportDiagnostics $Diagnostics
    Write-Output "[AvidScript][CSharp][Build] result=$FailureResult exit_code=$CompilerExitCode report=$ReportPath"
    exit 1
}

$GuestIrSha256 = Get-Sha256Hex $GuestIrArtifactPath
$DebugMapSha256 = Get-Sha256Hex $DebugMapArtifactPath
$StateSchemaSha256 = Get-Sha256Hex $StateSchemaArtifactPath
$WasmSha256 = Get-Sha256Hex $WasmArtifactPath
$RequiredExports = @($GuestIrModel.exports | ForEach-Object { [string]$_.name })
try {
    $UsedObjectTypeOrdinals = @(
        Get-UsedObjectTypeOrdinals -Model $GuestIrModel)
}
catch {
    Remove-LoadableArtifacts
    $Diagnostics += [ordered]@{
        code = "ASBI4305"
        severity = "error"
        message = $_.Exception.Message
        file = $SourceId
    }
    Write-BuildReport `
        -Result "guest_object_type_provenance_invalid" `
        -DirectAbiSupported $false `
        -ReportDiagnostics $Diagnostics
    Write-Output "[AvidScript][CSharp][Build] result=guest_object_type_provenance_invalid report=$ReportPath"
    exit 1
}
$ObservedExports = @($WasmInspectionModel.exports | Where-Object { [int]$_.kind -eq 0 } | ForEach-Object { [string]$_.name })
$RequiredImports = @($GuestIrModel.imports | ForEach-Object {
    [ordered]@{ module = [string]$_.module; name = [string]$_.name }
})
if (-not $IsDefaultSource) {
    $AuthorizationValidation = Test-BindingPackageImports `
        -PackageInfo $BindingAuthorizationInfo `
        -GuestImports @($GuestIrModel.imports) `
        -AllowDataLaneImports ($DataLaneFusion -ceq "enabled") `
        -AllowGeneratedTypeImports ([bool]$AllowGeneratedTypeImports)
    $UsedAuthorizationBindingImports = @($AuthorizationValidation.UsedImports)
    if (@($AuthorizationValidation.UnexpectedImports).Count -gt 0) {
        Remove-LoadableArtifacts
        $Diagnostics += [ordered]@{
            code = "ASBI4203"
            severity = "error"
            message = "Guest IR dynamic imports exceed the selected binding package authorization."
            unexpected_imports = @($AuthorizationValidation.UnexpectedImports)
        }
        Write-BuildReport -Result "binding_import_mismatch" -DirectAbiSupported $false -ReportDiagnostics $Diagnostics
        Write-Output "[AvidScript][CSharp][Build] result=binding_import_mismatch report=$ReportPath"
        exit 1
    }

    $RuntimeValidation = Test-BindingPackageImports `
        -PackageInfo $BindingPackageInfo `
        -GuestImports @($GuestIrModel.imports) `
        -AllowDataLaneImports ($DataLaneFusion -ceq "enabled") `
        -AllowGeneratedTypeImports ([bool]$AllowGeneratedTypeImports)
    $UsedRuntimeBindingImports = @($RuntimeValidation.UsedImports)
    $RuntimeIdentityMismatch = @()
    $RuntimeUsedByKey = [System.Collections.Generic.Dictionary[string, object]]::new(
        [System.StringComparer]::Ordinal)
    foreach ($Import in $UsedRuntimeBindingImports) {
        $RuntimeUsedByKey.Add("$($Import.module)`n$($Import.name)", $Import)
    }
    foreach ($Import in $UsedAuthorizationBindingImports) {
        $Key = "$($Import.module)`n$($Import.name)"
        if ($RuntimeUsedByKey.ContainsKey($Key)) {
            $RuntimeImport = $RuntimeUsedByKey[$Key]
            if ([string]$RuntimeImport.stable_id -cne [string]$Import.stable_id -or
                [string]$RuntimeImport.signature -cne [string]$Import.signature) {
                $RuntimeIdentityMismatch += "$($Import.module).$($Import.name)"
            }
        }
    }
    if (@($RuntimeValidation.UnexpectedImports).Count -gt 0 -or $RuntimeIdentityMismatch.Count -gt 0) {
        Remove-LoadableArtifacts
        $Diagnostics += [ordered]@{
            code = "ASBI4303"
            severity = "error"
            message = "Guest IR dynamic imports exceed or disagree with the selected runtime binding package."
            unexpected_imports = @($RuntimeValidation.UnexpectedImports)
            identity_mismatches = @($RuntimeIdentityMismatch)
            runtime_package_omitted = [bool]$OmitRuntimeBindingPackage
        }
        Write-BuildReport -Result "binding_runtime_import_mismatch" -DirectAbiSupported $false -ReportDiagnostics $Diagnostics
        Write-Output "[AvidScript][CSharp][Build] result=binding_runtime_import_mismatch report=$ReportPath"
        exit 1
    }

    if ([bool]$BindingPackageInfo.HasActiveObjectTypeOrdinals) {
        $RuntimeActiveObjectTypeOrdinals =
            @($BindingPackageInfo.ActiveObjectTypeOrdinals)
        $ObjectTypeOrdinalsMatch =
            $RuntimeActiveObjectTypeOrdinals.Count -eq $UsedObjectTypeOrdinals.Count
        if ($ObjectTypeOrdinalsMatch) {
            for ($Index = 0; $Index -lt $UsedObjectTypeOrdinals.Count; ++$Index) {
                if ([int]$RuntimeActiveObjectTypeOrdinals[$Index] -ne
                    [int]$UsedObjectTypeOrdinals[$Index]) {
                    $ObjectTypeOrdinalsMatch = $false
                    break
                }
            }
        }
        if (-not $ObjectTypeOrdinalsMatch) {
            Remove-LoadableArtifacts
            $Diagnostics += [ordered]@{
                code = "ASBI4304"
                severity = "error"
                message = "Final Guest IR object-type provenance does not match the selected runtime binding package activation set."
                runtime_active_object_type_ordinals =
                    @($RuntimeActiveObjectTypeOrdinals)
                guest_used_object_type_ordinals = @($UsedObjectTypeOrdinals)
            }
            Write-BuildReport `
                -Result "binding_runtime_object_type_mismatch" `
                -DirectAbiSupported $false `
                -ReportDiagnostics $Diagnostics
            Write-Output "[AvidScript][CSharp][Build] result=binding_runtime_object_type_mismatch report=$ReportPath"
            exit 1
        }
    }
}
$DirectAbiExports = @(
    "avid_on_begin_play",
    "avid_on_tick",
    "avid_on_end_play",
    "avid_on_timer",
    "avid_on_event",
    "avid_on_gameplay_event",
    "avid_on_continuation",
    "avid_on_continuation_v2")
$DirectAbiExports += @($SemanticModel.delegate_event_callbacks |
    ForEach-Object { [string]$_.export_name })
$DirectAbiExports += @($script:GeneratedTypeExportNames)
$DirectAbiExports = @($DirectAbiExports | Sort-Object -Unique)
$UnexpectedDeclaredExports = @($RequiredExports | Where-Object { $DirectAbiExports -notcontains $_ })
$MissingObservedExports = @($RequiredExports | Where-Object { $ObservedExports -notcontains $_ })
$UnexpectedObservedExports = @($ObservedExports | Where-Object { $RequiredExports -notcontains $_ })
$GuestContractValid = [int]$GuestIrModel.schema_version -eq 2 -and
    [string]$GuestIrModel.ir_version -eq "1.1" -and
    [bool]$GuestIrModel.succeeded -and
    [string]$GuestIrModel.provenance.semantic_sha256 -eq $SemanticSha256 -and
    [string]$GuestIrModel.provenance.source_sha256 -eq [string]$FrontendModel.source.sha256
$DebugImportedFunctionCount = -1
$DebugDefinedFunctionCount = -1
$DebugIndexSpaceValid = (Try-GetJsonInt32 -Value $DebugMapModel.imported_function_count -ParsedValue ([ref]$DebugImportedFunctionCount)) -and
    (Try-GetJsonInt32 -Value $DebugMapModel.defined_function_count -ParsedValue ([ref]$DebugDefinedFunctionCount)) -and
    $DebugImportedFunctionCount -ge 0 -and
    $DebugDefinedFunctionCount -gt 0 -and
    $DebugDefinedFunctionCount -le 65536 -and
    $DebugImportedFunctionCount -le ([int]::MaxValue - $DebugDefinedFunctionCount) -and
    $DebugImportedFunctionCount -eq @($GuestIrModel.imports).Count -and
    $DebugDefinedFunctionCount -eq @($GuestIrModel.functions).Count
$DebugMapContractValid = (Test-JsonObjectHasProperties -Value $DebugMapModel -RequiredProperties @(
        "schema_version",
        "debug_version",
        "module_id",
        "imported_function_count",
        "defined_function_count",
        "source",
        "provenance",
        "functions")) -and
    [int]$DebugMapModel.schema_version -eq 1 -and
    [string]$DebugMapModel.debug_version -eq "1.0" -and
    [string]$DebugMapModel.module_id -ceq [string]$GuestIrModel.module_id -and
    $DebugIndexSpaceValid -and
    (Test-JsonObjectHasProperties -Value $DebugMapModel.source -RequiredProperties @("id", "sha256")) -and
    [string]$DebugMapModel.source.id -ceq $SourceId -and
    (Test-JsonLowercaseSha256 -Value $DebugMapModel.source.sha256) -and
    [string]$DebugMapModel.source.sha256 -ceq [string]$FrontendModel.source.sha256 -and
    (Test-JsonObjectHasProperties -Value $DebugMapModel.provenance -RequiredProperties @(
        "frontend_artifact_sha256",
        "semantic_sha256",
        "guest_ir_sha256")) -and
    (Test-JsonLowercaseSha256 -Value $DebugMapModel.provenance.frontend_artifact_sha256) -and
    (Test-JsonLowercaseSha256 -Value $DebugMapModel.provenance.semantic_sha256) -and
    (Test-JsonLowercaseSha256 -Value $DebugMapModel.provenance.guest_ir_sha256) -and
    [string]$DebugMapModel.provenance.frontend_artifact_sha256 -ceq $FrontendArtifactSha256 -and
    [string]$DebugMapModel.provenance.semantic_sha256 -ceq $SemanticSha256 -and
    [string]$DebugMapModel.provenance.guest_ir_sha256 -ceq $GuestIrSha256 -and
    $DebugMapModel.functions -is [System.Array] -and
    $DebugMapModel.functions.Count -gt 0
if ($DebugMapContractValid) {
    $DebugFunctionIndices = [System.Collections.Generic.HashSet[int]]::new()
    $DebugGuestFunctionIds = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
    $PreviousDebugFunctionIndex = -1
    foreach ($Function in $DebugMapModel.functions) {
        $FunctionIndex = -1
        $SpanStart = -1
        $SpanLength = -1
        $SpanLine = -1
        $SpanColumn = -1
        $SpanEndLine = -1
        $SpanEndColumn = -1
        if (-not (Test-JsonObjectHasProperties -Value $Function -RequiredProperties @(
                "wasm_function_index",
                "guest_function_id",
                "method_symbol_id",
                "display_name",
                "span")) -or
            -not (Try-GetJsonInt32 -Value $Function.wasm_function_index -ParsedValue ([ref]$FunctionIndex)) -or
            -not (Test-JsonNonEmptyString -Value $Function.guest_function_id) -or
            -not (Test-JsonNonEmptyString -Value $Function.method_symbol_id) -or
            -not (Test-JsonNonEmptyString -Value $Function.display_name) -or
            -not (Test-JsonObjectHasProperties -Value $Function.span -RequiredProperties @(
                    "start",
                    "length",
                    "line",
                    "column",
                    "end_line",
                    "end_column")) -or
            -not (Try-GetJsonInt32 -Value $Function.span.start -ParsedValue ([ref]$SpanStart)) -or
            -not (Try-GetJsonInt32 -Value $Function.span.length -ParsedValue ([ref]$SpanLength)) -or
            -not (Try-GetJsonInt32 -Value $Function.span.line -ParsedValue ([ref]$SpanLine)) -or
            -not (Try-GetJsonInt32 -Value $Function.span.column -ParsedValue ([ref]$SpanColumn)) -or
            -not (Try-GetJsonInt32 -Value $Function.span.end_line -ParsedValue ([ref]$SpanEndLine)) -or
            -not (Try-GetJsonInt32 -Value $Function.span.end_column -ParsedValue ([ref]$SpanEndColumn)) -or
            $FunctionIndex -lt $DebugImportedFunctionCount -or
            $FunctionIndex -ge ($DebugImportedFunctionCount + $DebugDefinedFunctionCount) -or
            $FunctionIndex -le $PreviousDebugFunctionIndex -or
            -not $DebugFunctionIndices.Add($FunctionIndex) -or
            -not $DebugGuestFunctionIds.Add([string]$Function.guest_function_id) -or
            $SpanStart -lt 0 -or
            $SpanLength -le 0 -or
            $SpanLine -lt 0 -or
            $SpanColumn -lt 0 -or
            $SpanEndLine -lt $SpanLine -or
            $SpanEndColumn -lt 0 -or
            ($SpanEndLine -eq $SpanLine -and $SpanEndColumn -lt $SpanColumn)) {
            $DebugMapContractValid = $false
            break
        }
        $PreviousDebugFunctionIndex = $FunctionIndex
    }
}
$StateSchemaVersion = 0
$StateContractVersion = 0
$StateSchemaContractValid = (Test-JsonObjectHasProperties -Value $StateSchemaModel -RequiredProperties @(
        "schema_version",
        "strategy",
        "policy",
        "contract_version",
        "owner_type_id",
        "slots")) -and
    (Try-GetJsonInt32 -Value $StateSchemaModel.schema_version -ParsedValue ([ref]$StateSchemaVersion)) -and
    $StateSchemaVersion -eq 2 -and
    (Test-JsonNonEmptyString -Value $StateSchemaModel.strategy) -and
    $StateSchemaModel.strategy -eq "host_snapshot" -and
    (Test-JsonNonEmptyString -Value $StateSchemaModel.policy) -and
    ($StateSchemaModel.policy -in @("compatible", "explicit") -or
     ($StateSchemaModel.policy -eq "implicit" -and @($StateSchemaModel.slots).Count -eq 0)) -and
    (Try-GetJsonInt32 -Value $StateSchemaModel.contract_version -ParsedValue ([ref]$StateContractVersion)) -and
    $StateContractVersion -ge 1 -and
    $StateContractVersion -le 65535 -and
    (Test-JsonNonEmptyString -Value $StateSchemaModel.owner_type_id) -and
    $StateSchemaModel.slots -is [System.Array]
if ($StateSchemaContractValid) {
    $StateSlots = $StateSchemaModel.slots
    $StateIdentities = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
    $PreviousStableId = $null
    foreach ($Slot in $StateSlots) {
        $Offset = 0
        $Size = 0
        $Alignment = 0
        if (-not (Test-JsonObjectHasProperties -Value $Slot -RequiredProperties @(
                "stable_id",
                "aliases",
                "type_fingerprint",
                "offset",
                "size",
                "alignment")) -or
            -not (Test-JsonNonEmptyString -Value $Slot.stable_id) -or
            $Slot.aliases -isnot [System.Array] -or
            -not (Test-JsonNonEmptyString -Value $Slot.type_fingerprint) -or
            -not (Try-GetJsonInt32 -Value $Slot.offset -ParsedValue ([ref]$Offset)) -or
            -not (Try-GetJsonInt32 -Value $Slot.size -ParsedValue ([ref]$Size)) -or
            -not (Try-GetJsonInt32 -Value $Slot.alignment -ParsedValue ([ref]$Alignment))) {
            $StateSchemaContractValid = $false
            break
        }

        $CurrentStableId = $Slot.stable_id
        if (-not $CurrentStableId.StartsWith("state:$($StateSchemaModel.owner_type_id):", [System.StringComparison]::Ordinal) -or
            ($null -ne $PreviousStableId -and [string]::CompareOrdinal($PreviousStableId, $CurrentStableId) -ge 0) -or
            $Offset -lt 0 -or
            $Size -le 0 -or
            $Alignment -le 0 -or
            -not $StateIdentities.Add($CurrentStableId)) {
            $StateSchemaContractValid = $false
            break
        }
        $PreviousStableId = $CurrentStableId
    }
    if ($StateSchemaContractValid) {
        foreach ($Slot in $StateSlots) {
            $PreviousAlias = $null
            foreach ($AliasValue in $Slot.aliases) {
                if (-not (Test-JsonNonEmptyString -Value $AliasValue) -or
                    -not $AliasValue.StartsWith("state:$($StateSchemaModel.owner_type_id):", [System.StringComparison]::Ordinal) -or
                    ($null -ne $PreviousAlias -and [string]::CompareOrdinal($PreviousAlias, $AliasValue) -ge 0) -or
                    -not $StateIdentities.Add($AliasValue)) {
                    $StateSchemaContractValid = $false
                    break
                }
                $PreviousAlias = $AliasValue
            }
            if (-not $StateSchemaContractValid) {
                break
            }
        }
    }
}
$WasmInspectionValid = [int]$WasmInspectionModel.schema_version -eq 1 -and
    [string]$WasmInspectionModel.sha256 -eq $WasmSha256
if (-not $GuestContractValid -or -not $DebugMapContractValid -or -not $StateSchemaContractValid -or -not $WasmInspectionValid -or
    $RequiredExports.Count -eq 0 -or $UnexpectedDeclaredExports.Count -gt 0 -or
    $MissingObservedExports.Count -gt 0 -or
    $UnexpectedObservedExports.Count -gt 0) {
    Remove-LoadableArtifacts
    $Diagnostics += [ordered]@{
        code = "direct_abi_contract_invalid"
        severity = "error"
        message = "Guest IR, C# debug map, state migration, or final WASM direct ABI contract is invalid."
        guest_contract_valid = $GuestContractValid
        debug_map_contract_valid = $DebugMapContractValid
        state_schema_contract_valid = $StateSchemaContractValid
        wasm_inspection_valid = $WasmInspectionValid
        required_export_count = $RequiredExports.Count
        unexpected_declared_exports = @($UnexpectedDeclaredExports)
        missing_observed_exports = @($MissingObservedExports)
        unexpected_observed_exports = @($UnexpectedObservedExports)
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
    compilation = [ordered]@{
        data_lane_fusion = $DataLaneFusion
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
            profile_import_count = @($BindingPackageInfo.RequiredImports).Count
            used_import_count = @($UsedRuntimeBindingImports).Count
            used_imports = @($UsedRuntimeBindingImports)
            used_object_type_count = @($UsedObjectTypeOrdinals).Count
            used_object_type_ordinals = @($UsedObjectTypeOrdinals)
        }
    }
    guest_ir = [ordered]@{
        file = Convert-ToProjectRelativePath $GuestIrArtifactPath
        schema_version = [int]$GuestIrModel.schema_version
        version = [string]$GuestIrModel.ir_version
        module_id = [string]$GuestIrModel.module_id
        sha256 = $GuestIrSha256
    }
    debug_map = [ordered]@{
        file = Convert-ToProjectRelativePath $DebugMapArtifactPath
        schema_version = [int]$DebugMapModel.schema_version
        version = [string]$DebugMapModel.debug_version
        module_id = [string]$DebugMapModel.module_id
        imported_function_count = $DebugImportedFunctionCount
        defined_function_count = $DebugDefinedFunctionCount
        sha256 = $DebugMapSha256
    }
    state_migration = $StateSchemaModel
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
if ($null -eq $BindingPackageInfo) {
    [void]$Manifest.Remove("binding_package")
}
try {
    Write-JsonAtomic -Path $ManifestPath -Value $Manifest
    $ShouldPublishCompilationCache = $null -ne $CompilationCacheContext -and
        -not $CompilationCacheHit -and
        -not $DisableCompilationCache
    if ($ShouldPublishCompilationCache) {
        try {
            $CompilationPublication = Publish-AvidScriptCSharpCompilationCacheEntry `
                -Context $CompilationCacheContext `
                -Artifacts @{
                    guest_ir = $GuestIrArtifactPath
                    debug_map = $DebugMapArtifactPath
                    state_schema = $StateSchemaArtifactPath
                    wasm = $WasmArtifactPath
                    wasm_inspection = $WasmInspectionArtifactPath
                }
            $CompilationCache.entry_report_file =
                Convert-ToProjectRelativePath $CompilationPublication.EntryReportPath
            $CompilationCache.entry_report_sha256 =
                [string]$CompilationPublication.EntryReportSha256
            $CompilationCache.published = [bool]$CompilationPublication.Published
        }
        catch {
            $CompilationPublicationCode =
                [string]$_.Exception.Data["AvidScriptCode"]
            if ([string]::IsNullOrWhiteSpace($CompilationPublicationCode)) {
                $CompilationPublicationCode = "ASBI4604"
            }
            $CompilationCache.diagnostic_code = $CompilationPublicationCode
            $CompilationCache.diagnostic_message = $_.Exception.Message
            $Diagnostics += [ordered]@{
                code = $CompilationPublicationCode
                severity = "warning"
                message = $_.Exception.Message
                file = Convert-ToProjectRelativePath $CompilationCacheRoot
            }
        }
    }
    Write-BuildReport -Result "direct_abi_built" -DirectAbiSupported $true -ReportDiagnostics $Diagnostics
    $ShouldPublishSemanticCache = $null -ne $SemanticCacheContext -and
        -not $SemanticCacheHit -and
        [string]::IsNullOrWhiteSpace($PreparedBuildReportPath) -and
        -not $DisableSemanticCache
    if ($ShouldPublishSemanticCache) {
        try {
            $CachePublication = Publish-AvidScriptCSharpSemanticCacheEntry `
                -Context $SemanticCacheContext `
                -ProjectRoot $ProjectRoot `
                -ExpectedSourcePath $SourcePath `
                -ExpectedAuthorizationPackage $BindingAuthorizationInfo `
                -SourceReportPath $ReportPath
            $SemanticCache.entry_report_file = Convert-ToProjectRelativePath $CachePublication.EntryReportPath
            $SemanticCache.entry_report_sha256 = [string]$CachePublication.EntryReportSha256
            $SemanticCache.published = [bool]$CachePublication.Published
            if (-not [string]::IsNullOrWhiteSpace([string]$CachePublication.DiagnosticCode)) {
                $SemanticCache.diagnostic_code = [string]$CachePublication.DiagnosticCode
                $SemanticCache.diagnostic_message = [string]$CachePublication.DiagnosticMessage
                $Diagnostics += [ordered]@{
                    code = [string]$CachePublication.DiagnosticCode
                    severity = "warning"
                    message = [string]$CachePublication.DiagnosticMessage
                    file = Convert-ToProjectRelativePath $SemanticCacheRoot
                }
            }
        }
        catch {
            $CachePublicationCode = [string]$_.Exception.Data["AvidScriptCode"]
            if ([string]::IsNullOrWhiteSpace($CachePublicationCode)) {
                $CachePublicationCode = "ASBI4504"
            }
            $SemanticCache.diagnostic_code = $CachePublicationCode
            $SemanticCache.diagnostic_message = $_.Exception.Message
            $Diagnostics += [ordered]@{
                code = $CachePublicationCode
                severity = "warning"
                message = $_.Exception.Message
                file = Convert-ToProjectRelativePath $SemanticCacheRoot
            }
        }
        Write-BuildReport -Result "direct_abi_built" -DirectAbiSupported $true -ReportDiagnostics $Diagnostics
    }
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
