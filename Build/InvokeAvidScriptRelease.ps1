[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$SourcePath,
    [Parameter(Mandatory = $true)][string]$CSharpProjectPath,
    [Parameter(Mandatory = $true)][string]$ModuleId,
    [Parameter(Mandatory = $true)][string]$ArtifactStem,
    [Parameter(Mandatory = $true)][string]$OutputRoot,
    [Parameter(Mandatory = $true)][string]$DotNetPath,
    [string]$BindingPackagePath = '',
    [string]$RuntimeBindingPackagePath = '',
    [string]$GeneratedTypeManifestPath = '',
    [ValidateSet('Development', 'Shipping')][string]$Configuration = 'Development',
    [string]$EngineRoot = 'C:\UnrealEngine'
)

$ErrorActionPreference = 'Stop'
$AvidScriptReleaseScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$AvidScriptReleasePluginRoot = Split-Path -Parent $AvidScriptReleaseScriptRoot
$AvidScriptReleaseProjectRoot = Split-Path -Parent (
    Split-Path -Parent $AvidScriptReleasePluginRoot)

function Throw-AvidScriptReleaseError {
    param(
        [Parameter(Mandatory = $true)][string]$Category,
        [Parameter(Mandatory = $true)][string]$Message
    )

    $Exception = [System.InvalidOperationException]::new($Message)
    $Exception.Data['category'] = $Category
    throw $Exception
}

function Test-AvidScriptReleasePathUnderRoot {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Root
    )

    $FullPath = [System.IO.Path]::GetFullPath($Path)
    $FullRoot = [System.IO.Path]::GetFullPath($Root).TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar)
    return $FullPath.Equals(
        $FullRoot,
        [System.StringComparison]::OrdinalIgnoreCase) -or
        $FullPath.StartsWith(
            $FullRoot + [System.IO.Path]::DirectorySeparatorChar,
            [System.StringComparison]::OrdinalIgnoreCase)
}

function Resolve-AvidScriptReleaseProjectPath {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$ProjectRoot,
        [Parameter(Mandatory = $true)][string]$Label,
        [ValidateSet('Leaf', 'Container', 'Any', 'AllowMissing')]
        [string]$PathType = 'Any'
    )

    if ([string]::IsNullOrWhiteSpace($Path)) {
        Throw-AvidScriptReleaseError `
            -Category 'input_invalid' `
            -Message "$Label must not be empty."
    }
    $FullProjectRoot = [System.IO.Path]::GetFullPath($ProjectRoot)
    $FullPath = if ([System.IO.Path]::IsPathRooted($Path)) {
        [System.IO.Path]::GetFullPath($Path)
    }
    else {
        [System.IO.Path]::GetFullPath((Join-Path $FullProjectRoot $Path))
    }
    if (-not (Test-AvidScriptReleasePathUnderRoot `
            -Path $FullPath `
            -Root $FullProjectRoot)) {
        Throw-AvidScriptReleaseError `
            -Category 'path_outside_project' `
            -Message "$Label must remain inside ProjectRoot: $FullPath"
    }

    $Exists = Test-Path -LiteralPath $FullPath
    if ($PathType -ceq 'Leaf' -and
        -not (Test-Path -LiteralPath $FullPath -PathType Leaf)) {
        Throw-AvidScriptReleaseError `
            -Category 'input_missing' `
            -Message "$Label file does not exist: $FullPath"
    }
    if ($PathType -ceq 'Container' -and
        -not (Test-Path -LiteralPath $FullPath -PathType Container)) {
        Throw-AvidScriptReleaseError `
            -Category 'input_missing' `
            -Message "$Label directory does not exist: $FullPath"
    }
    if ($PathType -ceq 'Any' -and -not $Exists) {
        Throw-AvidScriptReleaseError `
            -Category 'input_missing' `
            -Message "$Label does not exist: $FullPath"
    }
    if ($PathType -ceq 'AllowMissing' -and
        (Test-Path -LiteralPath $FullPath -PathType Leaf)) {
        Throw-AvidScriptReleaseError `
            -Category 'input_invalid' `
            -Message "$Label resolves to a file: $FullPath"
    }
    return $FullPath
}

function Resolve-AvidScriptReleaseExternalFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Label
    )

    if ([string]::IsNullOrWhiteSpace($Path) -or
        -not [System.IO.Path]::IsPathRooted($Path)) {
        Throw-AvidScriptReleaseError `
            -Category 'external_path_invalid' `
            -Message "$Label must be an absolute path."
    }
    $FullPath = [System.IO.Path]::GetFullPath($Path)
    if (-not (Test-Path -LiteralPath $FullPath -PathType Leaf)) {
        Throw-AvidScriptReleaseError `
            -Category 'input_missing' `
            -Message "$Label file does not exist: $FullPath"
    }
    return $FullPath
}

function Invoke-AvidScriptReleaseProcess {
    param(
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$WorkingDirectory
    )

    $StartInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $StartInfo.FileName = $Executable
    $StartInfo.WorkingDirectory = $WorkingDirectory
    $StartInfo.UseShellExecute = $false
    $StartInfo.CreateNoWindow = $true
    $StartInfo.RedirectStandardOutput = $true
    $StartInfo.RedirectStandardError = $true
    foreach ($Argument in $Arguments) {
        [void]$StartInfo.ArgumentList.Add($Argument)
    }

    $Process = [System.Diagnostics.Process]::new()
    $Process.StartInfo = $StartInfo
    try {
        try {
            if (-not $Process.Start()) {
                Throw-AvidScriptReleaseError `
                    -Category 'process_launch_failed' `
                    -Message "Process could not be launched: $Executable"
            }
        }
        catch {
            if ($_.Exception.Data.Contains('category')) {
                throw
            }
            Throw-AvidScriptReleaseError `
                -Category 'process_launch_failed' `
                -Message "Process could not be launched: $Executable"
        }
        $StdoutTask = $Process.StandardOutput.ReadToEndAsync()
        $StderrTask = $Process.StandardError.ReadToEndAsync()
        $Process.WaitForExit()
        return [pscustomobject][ordered]@{
            ExitCode = $Process.ExitCode
            Stdout = $StdoutTask.GetAwaiter().GetResult()
            Stderr = $StderrTask.GetAwaiter().GetResult()
        }
    }
    finally {
        $Process.Dispose()
    }
}

function New-AvidScriptReleaseCommandletArguments {
    param(
        [Parameter(Mandatory = $true)][string]$SourcePath,
        [Parameter(Mandatory = $true)][string]$CSharpProjectPath,
        [Parameter(Mandatory = $true)][string]$ModuleId,
        [Parameter(Mandatory = $true)][string]$ArtifactStem,
        [Parameter(Mandatory = $true)][string]$OutputRoot,
        [Parameter(Mandatory = $true)][string]$DotNetPath,
        [Parameter(Mandatory = $true)][string]$AbsLog,
        [string]$BindingPackagePath = '',
        [string]$RuntimeBindingPackagePath = '',
        [string]$GeneratedTypeManifestPath = ''
    )

    $Arguments = [System.Collections.Generic.List[string]]::new()
    foreach ($Argument in @(
            '-run=AvidScriptRelease',
            "-SourcePath=$SourcePath",
            "-CSharpProjectPath=$CSharpProjectPath",
            "-ModuleId=$ModuleId",
            "-ArtifactStem=$ArtifactStem",
            "-OutputRoot=$OutputRoot",
            "-DotNetPath=$DotNetPath")) {
        $Arguments.Add($Argument)
    }
    if (-not [string]::IsNullOrWhiteSpace($BindingPackagePath)) {
        $Arguments.Add("-BindingPackagePath=$BindingPackagePath")
    }
    if (-not [string]::IsNullOrWhiteSpace($RuntimeBindingPackagePath)) {
        $Arguments.Add("-RuntimeBindingPackagePath=$RuntimeBindingPackagePath")
    }
    if (-not [string]::IsNullOrWhiteSpace($GeneratedTypeManifestPath)) {
        $Arguments.Add("-GeneratedTypeManifestPath=$GeneratedTypeManifestPath")
    }
    foreach ($Argument in @(
            "-abslog=$AbsLog",
            '-unattended',
            '-nop4',
            '-nosplash',
            '-nullrhi',
            '-stdout',
            '-FullStdOutLogOutput')) {
        $Arguments.Add($Argument)
    }
    return $Arguments.ToArray()
}

function Publish-AvidScriptReleaseRuntimePackage {
    param(
        [Parameter(Mandatory = $true)][string]$RuntimeManifestPath,
        [Parameter(Mandatory = $true)][string]$ProjectRoot,
        [Parameter(Mandatory = $true)][string]$ModuleId,
        [ValidateSet('Development', 'Shipping')]
        [Parameter(Mandatory = $true)][string]$Configuration
    )

    $PublisherPath = Join-Path `
        $AvidScriptReleaseScriptRoot `
        'AvidScriptModuleReleasePackage.ps1'
    if (-not (Test-Path -LiteralPath $PublisherPath -PathType Leaf)) {
        Throw-AvidScriptReleaseError `
            -Category 'publisher_missing' `
            -Message "Module release publisher is missing: $PublisherPath"
    }
    . $PublisherPath
    return Publish-AvidScriptModuleReleasePackage `
        -RuntimeManifestPath $RuntimeManifestPath `
        -ProjectRoot $ProjectRoot `
        -ModuleId $ModuleId `
        -Configuration $Configuration
}

function Get-AvidScriptReleaseCommandletCategory {
    param(
        [Parameter(Mandatory = $true)][string]$Output,
        [Parameter(Mandatory = $true)][string]$AbsLog
    )

    $Evidence = $Output
    if (Test-Path -LiteralPath $AbsLog -PathType Leaf) {
        $Evidence += "`n" + (Get-Content -Raw -LiteralPath $AbsLog)
    }
    $Matches = [System.Text.RegularExpressions.Regex]::Matches(
        $Evidence,
        'AVIDSCRIPT_RELEASE_RESULT\s+category=([a-z0-9_]+)')
    if ($Matches.Count -eq 0) {
        return ''
    }
    return $Matches[$Matches.Count - 1].Groups[1].Value
}

function Invoke-AvidScriptRelease {
    param(
        [Parameter(Mandatory = $true)][string]$SourcePath,
        [Parameter(Mandatory = $true)][string]$CSharpProjectPath,
        [Parameter(Mandatory = $true)][string]$ModuleId,
        [Parameter(Mandatory = $true)][string]$ArtifactStem,
        [Parameter(Mandatory = $true)][string]$OutputRoot,
        [Parameter(Mandatory = $true)][string]$DotNetPath,
        [string]$BindingPackagePath,
        [string]$RuntimeBindingPackagePath,
        [string]$GeneratedTypeManifestPath,
        [ValidateSet('Development', 'Shipping')]
        [Parameter(Mandatory = $true)][string]$Configuration,
        [Parameter(Mandatory = $true)][string]$EngineRoot
    )

    if ($ModuleId -cnotmatch '^[a-z][a-z0-9_.-]{0,63}$') {
        Throw-AvidScriptReleaseError `
            -Category 'module_id_invalid' `
            -Message 'ModuleId must match ^[a-z][a-z0-9_.-]{0,63}$.'
    }
    if ($ArtifactStem -cnotmatch '^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$') {
        Throw-AvidScriptReleaseError `
            -Category 'artifact_stem_invalid' `
            -Message 'ArtifactStem does not match the release command schema.'
    }

    $ProjectRoot = Resolve-AvidScriptReleaseProjectPath `
        -Path $AvidScriptReleaseProjectRoot `
        -ProjectRoot $AvidScriptReleaseProjectRoot `
        -Label 'ProjectRoot' `
        -PathType Container
    $NormalizedSourcePath = Resolve-AvidScriptReleaseProjectPath `
        -Path $SourcePath `
        -ProjectRoot $ProjectRoot `
        -Label 'SourcePath' `
        -PathType Leaf
    $NormalizedProjectPath = Resolve-AvidScriptReleaseProjectPath `
        -Path $CSharpProjectPath `
        -ProjectRoot $ProjectRoot `
        -Label 'CSharpProjectPath' `
        -PathType Leaf
    $NormalizedOutputRoot = Resolve-AvidScriptReleaseProjectPath `
        -Path $OutputRoot `
        -ProjectRoot $ProjectRoot `
        -Label 'OutputRoot' `
        -PathType AllowMissing
    if ($NormalizedOutputRoot.Equals(
            $ProjectRoot,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        Throw-AvidScriptReleaseError `
            -Category 'output_root_invalid' `
            -Message 'OutputRoot must be a directory below ProjectRoot.'
    }
    $NormalizedDotNetPath = Resolve-AvidScriptReleaseExternalFile `
        -Path $DotNetPath `
        -Label 'DotNetPath'
    $NormalizedBindingPackagePath = ''
    if (-not [string]::IsNullOrWhiteSpace($BindingPackagePath)) {
        $NormalizedBindingPackagePath = Resolve-AvidScriptReleaseProjectPath `
            -Path $BindingPackagePath `
            -ProjectRoot $ProjectRoot `
            -Label 'BindingPackagePath' `
            -PathType Leaf
    }
    $NormalizedRuntimeBindingPackagePath = ''
    if (-not [string]::IsNullOrWhiteSpace($RuntimeBindingPackagePath)) {
        $NormalizedRuntimeBindingPackagePath = Resolve-AvidScriptReleaseProjectPath `
            -Path $RuntimeBindingPackagePath `
            -ProjectRoot $ProjectRoot `
            -Label 'RuntimeBindingPackagePath' `
            -PathType Leaf
    }
    $NormalizedGeneratedTypeManifestPath = ''
    if (-not [string]::IsNullOrWhiteSpace($GeneratedTypeManifestPath)) {
        $NormalizedGeneratedTypeManifestPath = Resolve-AvidScriptReleaseProjectPath `
            -Path $GeneratedTypeManifestPath `
            -ProjectRoot $ProjectRoot `
            -Label 'GeneratedTypeManifestPath' `
            -PathType Leaf
    }

    $ExpectedEngineRoot = [System.IO.Path]::GetFullPath('C:\UnrealEngine')
    $NormalizedEngineRoot = [System.IO.Path]::GetFullPath($EngineRoot)
    if (-not $NormalizedEngineRoot.Equals(
            $ExpectedEngineRoot,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        Throw-AvidScriptReleaseError `
            -Category 'engine_root_invalid' `
            -Message 'EngineRoot must be the fixed C:\UnrealEngine UE5.8 source checkout.'
    }
    $BuildVersionPath = Join-Path $NormalizedEngineRoot 'Engine/Build/Build.version'
    if (-not (Test-Path -LiteralPath $BuildVersionPath -PathType Leaf)) {
        Throw-AvidScriptReleaseError `
            -Category 'engine_missing' `
            -Message "UE build version is missing: $BuildVersionPath"
    }
    $BuildVersion = Get-Content -Raw -LiteralPath $BuildVersionPath |
        ConvertFrom-Json -Depth 8
    if ([int]$BuildVersion.MajorVersion -ne 5 -or
        [int]$BuildVersion.MinorVersion -ne 8) {
        Throw-AvidScriptReleaseError `
            -Category 'engine_version_invalid' `
            -Message 'InvokeAvidScriptRelease requires the UE5.8 source Editor-Cmd.'
    }
    $EditorCmdPath = Resolve-AvidScriptReleaseExternalFile `
        -Path (Join-Path $NormalizedEngineRoot 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe') `
        -Label 'UE5.8 Editor-Cmd'

    $ProjectName = Split-Path -Leaf $ProjectRoot
    $ProjectFile = Resolve-AvidScriptReleaseProjectPath `
        -Path (Join-Path $ProjectRoot "$ProjectName.uproject") `
        -ProjectRoot $ProjectRoot `
        -Label 'Project file' `
        -PathType Leaf

    $ToolchainScript = Resolve-AvidScriptReleaseProjectPath `
        -Path (Join-Path $AvidScriptReleaseScriptRoot 'BuildAvidScriptWasmtimePerformanceToolchain.ps1') `
        -ProjectRoot $ProjectRoot `
        -Label 'Performance toolchain verifier' `
        -PathType Leaf
    $PowerShellPath = Resolve-AvidScriptReleaseExternalFile `
        -Path (Join-Path $PSHOME 'pwsh.exe') `
        -Label 'PowerShell 7 host'
    $ToolchainResult = Invoke-AvidScriptReleaseProcess `
        -Executable $PowerShellPath `
        -Arguments @(
            '-NoProfile',
            '-File',
            $ToolchainScript,
            '-Mode',
            'Verify',
            '-RepositoryRoot',
            $AvidScriptReleasePluginRoot) `
        -WorkingDirectory $AvidScriptReleasePluginRoot
    if ($ToolchainResult.ExitCode -ne 0) {
        Throw-AvidScriptReleaseError `
            -Category 'toolchain_verify_failed' `
            -Message $ToolchainResult.Stderr.Trim()
    }
    try {
        $ToolchainEvidence = $ToolchainResult.Stdout |
            ConvertFrom-Json -Depth 16 -NoEnumerate
    }
    catch {
        Throw-AvidScriptReleaseError `
            -Category 'toolchain_verify_invalid' `
            -Message 'Performance toolchain Verify did not return valid JSON.'
    }
    if ([string]$ToolchainEvidence.result -cne
        'wasmtime_performance_toolchain_verified') {
        Throw-AvidScriptReleaseError `
            -Category 'toolchain_verify_invalid' `
            -Message 'Performance toolchain Verify returned an unexpected result.'
    }

    $LogRoot = Resolve-AvidScriptReleaseProjectPath `
        -Path (Join-Path $ProjectRoot 'Saved/AvidScript/ReleaseLogs') `
        -ProjectRoot $ProjectRoot `
        -Label 'Release log root' `
        -PathType AllowMissing
    [void][System.IO.Directory]::CreateDirectory($LogRoot)
    $AbsLog = Join-Path `
        $LogRoot `
        ("AvidScriptRelease-$PID-$([Guid]::NewGuid().ToString('N')).log")
    $CommandletArguments = New-AvidScriptReleaseCommandletArguments `
        -SourcePath $NormalizedSourcePath `
        -CSharpProjectPath $NormalizedProjectPath `
        -ModuleId $ModuleId `
        -ArtifactStem $ArtifactStem `
        -OutputRoot $NormalizedOutputRoot `
        -DotNetPath $NormalizedDotNetPath `
        -BindingPackagePath $NormalizedBindingPackagePath `
        -RuntimeBindingPackagePath $NormalizedRuntimeBindingPackagePath `
        -GeneratedTypeManifestPath $NormalizedGeneratedTypeManifestPath `
        -AbsLog $AbsLog
    $EditorResult = Invoke-AvidScriptReleaseProcess `
        -Executable $EditorCmdPath `
        -Arguments (@($ProjectFile) + @($CommandletArguments)) `
        -WorkingDirectory $ProjectRoot
    $CommandletOutput = $EditorResult.Stdout + "`n" + $EditorResult.Stderr
    $CommandletCategory = Get-AvidScriptReleaseCommandletCategory `
        -Output $CommandletOutput `
        -AbsLog $AbsLog
    if ($EditorResult.ExitCode -ne 0) {
        Throw-AvidScriptReleaseError `
            -Category $(if ([string]::IsNullOrWhiteSpace($CommandletCategory)) {
                    'commandlet_failed'
                }
                else {
                    $CommandletCategory
                }) `
            -Message "AvidScriptRelease commandlet failed with exit code $($EditorResult.ExitCode). See $AbsLog"
    }
    if ($CommandletCategory -cne 'success') {
        Throw-AvidScriptReleaseError `
            -Category 'commandlet_result_missing' `
            -Message "AvidScriptRelease commandlet did not record success. See $AbsLog"
    }

    $RuntimeManifestPath = Resolve-AvidScriptReleaseProjectPath `
        -Path (Join-Path $NormalizedOutputRoot "$ArtifactStem.avidscript.json") `
        -ProjectRoot $ProjectRoot `
        -Label 'Runtime manifest' `
        -PathType Leaf
    $PrecompiledArtifactPath = Resolve-AvidScriptReleaseProjectPath `
        -Path (Join-Path $NormalizedOutputRoot "$ArtifactStem.wasmtime.cwasm") `
        -ProjectRoot $ProjectRoot `
        -Label 'Precompiled artifact' `
        -PathType Leaf
    try {
        $Package = Publish-AvidScriptReleaseRuntimePackage `
            -RuntimeManifestPath $RuntimeManifestPath `
            -ProjectRoot $ProjectRoot `
            -ModuleId $ModuleId `
            -Configuration $Configuration
    }
    catch {
        if ($_.Exception.Data.Contains('category')) {
            throw
        }
        Throw-AvidScriptReleaseError `
            -Category 'publisher_failed' `
            -Message $_.Exception.Message
    }

    return [pscustomobject][ordered]@{
        schema_version = 1
        result = 'avidscript_module_release_succeeded'
        module_id = [string]$Package.ModuleId
        package_id = [string]$Package.PackageId
        configuration = [string]$Package.Configuration
        source_path = $NormalizedSourcePath
        csharp_project_path = $NormalizedProjectPath
        build_output_root = $NormalizedOutputRoot
        runtime_manifest_path = $RuntimeManifestPath
        precompiled_artifact_path = $PrecompiledArtifactPath
        generated_type_manifest_path = $NormalizedGeneratedTypeManifestPath
        package_root = [string]$Package.PackageRoot
        descriptor_path = [string]$Package.DescriptorPath
        catalog_path = [string]$Package.CatalogPath
        editor_log = $AbsLog
        toolchain = $ToolchainEvidence.evidence
    }
}

if ($MyInvocation.InvocationName -eq '.') {
    return
}

try {
    $Summary = Invoke-AvidScriptRelease `
        -SourcePath $SourcePath `
        -CSharpProjectPath $CSharpProjectPath `
        -ModuleId $ModuleId `
        -ArtifactStem $ArtifactStem `
        -OutputRoot $OutputRoot `
        -DotNetPath $DotNetPath `
        -BindingPackagePath $BindingPackagePath `
        -RuntimeBindingPackagePath $RuntimeBindingPackagePath `
        -GeneratedTypeManifestPath $GeneratedTypeManifestPath `
        -Configuration $Configuration `
        -EngineRoot $EngineRoot
    Write-Output ($Summary | ConvertTo-Json -Depth 16 -Compress)
    exit 0
}
catch {
    $Category = if ($_.Exception.Data.Contains('category')) {
        [string]$_.Exception.Data['category']
    }
    else {
        'unexpected_failure'
    }
    $Failure = [pscustomobject][ordered]@{
        schema_version = 1
        result = 'avidscript_module_release_failed'
        category = $Category
        message = $_.Exception.Message
    }
    [Console]::Error.WriteLine(
        ($Failure | ConvertTo-Json -Depth 8 -Compress))
    exit 1
}
