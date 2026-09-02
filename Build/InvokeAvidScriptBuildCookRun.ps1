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
    [ValidateSet('Development', 'Shipping')]
    [string]$Configuration = 'Development',
    [Parameter(Mandatory = $true)][string]$ArchiveRoot,
    [ValidateRange(10, 600)]
    [int]$PackagedOracleTimeoutSeconds = 120,
    [string]$EngineRoot = 'C:\UnrealEngine'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$AvidScriptBuildCookRunScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$AvidScriptBuildCookRunPluginRoot = Split-Path -Parent $AvidScriptBuildCookRunScriptRoot
$AvidScriptBuildCookRunProjectRoot = Split-Path -Parent (
    Split-Path -Parent $AvidScriptBuildCookRunPluginRoot)
$script:AvidScriptBuildCookRunStep = 'validation'
$script:AvidScriptBuildCookRunUatLog = ''

function Throw-AvidScriptBuildCookRunError {
    param(
        [Parameter(Mandatory = $true)][string]$Category,
        [Parameter(Mandatory = $true)][string]$Message
    )

    $Exception = [System.InvalidOperationException]::new($Message)
    $Exception.Data['category'] = $Category
    throw $Exception
}

function Test-AvidScriptBuildCookRunPathUnderRoot {
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

function Resolve-AvidScriptBuildCookRunArchiveRoot {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$ProjectRoot
    )

    if ([string]::IsNullOrWhiteSpace($Path)) {
        Throw-AvidScriptBuildCookRunError `
            -Category 'archive_root_invalid' `
            -Message 'ArchiveRoot must not be empty.'
    }
    $FullProjectRoot = [System.IO.Path]::GetFullPath($ProjectRoot).TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar)
    $FullArchiveRoot = if ([System.IO.Path]::IsPathRooted($Path)) {
        [System.IO.Path]::GetFullPath($Path)
    }
    else {
        [System.IO.Path]::GetFullPath((Join-Path $FullProjectRoot $Path))
    }
    if (-not (Test-AvidScriptBuildCookRunPathUnderRoot `
            -Path $FullArchiveRoot `
            -Root $FullProjectRoot) -or
        $FullArchiveRoot.Equals(
            $FullProjectRoot,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        Throw-AvidScriptBuildCookRunError `
            -Category 'archive_root_outside_project' `
            -Message "ArchiveRoot must be a directory below ProjectRoot: $FullArchiveRoot"
    }
    if (Test-Path -LiteralPath $FullArchiveRoot -PathType Leaf) {
        Throw-AvidScriptBuildCookRunError `
            -Category 'archive_root_invalid' `
            -Message "ArchiveRoot resolves to a file: $FullArchiveRoot"
    }
    if (Test-Path -LiteralPath $FullArchiveRoot -PathType Container) {
        $Entries = @([System.IO.Directory]::EnumerateFileSystemEntries($FullArchiveRoot))
        if ($Entries.Count -ne 0) {
            Throw-AvidScriptBuildCookRunError `
                -Category 'archive_root_not_empty' `
                -Message "ArchiveRoot already contains files: $FullArchiveRoot"
        }
    }
    return $FullArchiveRoot
}

function Assert-AvidScriptBuildCookRunNoDuplicateJsonProperties {
    param(
        [Parameter(Mandatory = $true)][System.Text.Json.JsonElement]$Element,
        [Parameter(Mandatory = $true)][string]$Label
    )

    if ($Element.ValueKind -eq [System.Text.Json.JsonValueKind]::Object) {
        $Names = [System.Collections.Generic.HashSet[string]]::new(
            [System.StringComparer]::Ordinal)
        foreach ($Property in $Element.EnumerateObject()) {
            if (-not $Names.Add($Property.Name)) {
                Throw-AvidScriptBuildCookRunError `
                    -Category 'json_duplicate_property' `
                    -Message "$Label contains duplicate property '$($Property.Name)'."
            }
            Assert-AvidScriptBuildCookRunNoDuplicateJsonProperties `
                -Element $Property.Value `
                -Label "$Label.$($Property.Name)"
        }
    }
    elseif ($Element.ValueKind -eq [System.Text.Json.JsonValueKind]::Array) {
        $Index = 0
        foreach ($Item in $Element.EnumerateArray()) {
            Assert-AvidScriptBuildCookRunNoDuplicateJsonProperties `
                -Element $Item `
                -Label "$Label[$Index]"
            ++$Index
        }
    }
}

function ConvertFrom-AvidScriptBuildCookRunJsonText {
    param(
        [Parameter(Mandatory = $true)][string]$Text,
        [Parameter(Mandatory = $true)][string]$Label,
        [switch]$RequireSingleLine
    )

    $JsonText = if ($RequireSingleLine) {
        $Text.TrimEnd([char[]]@("`r", "`n"))
    }
    else {
        $Text
    }
    if ([string]::IsNullOrWhiteSpace($JsonText)) {
        Throw-AvidScriptBuildCookRunError `
            -Category 'json_output_invalid' `
            -Message "$Label is empty."
    }
    if ($RequireSingleLine -and
        $JsonText.IndexOfAny([char[]]@("`r", "`n")) -ge 0) {
        Throw-AvidScriptBuildCookRunError `
            -Category 'json_output_invalid' `
            -Message "$Label must contain exactly one JSON line."
    }

    try {
        $Document = [System.Text.Json.JsonDocument]::Parse($JsonText)
        try {
            if ($Document.RootElement.ValueKind -ne
                [System.Text.Json.JsonValueKind]::Object) {
                Throw-AvidScriptBuildCookRunError `
                    -Category 'json_output_invalid' `
                    -Message "$Label must be a JSON object."
            }
            Assert-AvidScriptBuildCookRunNoDuplicateJsonProperties `
                -Element $Document.RootElement `
                -Label $Label
        }
        finally {
            $Document.Dispose()
        }
        return ($JsonText | ConvertFrom-Json -Depth 100 -NoEnumerate)
    }
    catch {
        if ($_.Exception.Data.Contains('category')) {
            throw
        }
        Throw-AvidScriptBuildCookRunError `
            -Category 'json_output_invalid' `
            -Message "$Label is not valid JSON: $($_.Exception.Message)"
    }
}

function Get-AvidScriptBuildCookRunRequiredProperty {
    param(
        [Parameter(Mandatory = $true)][object]$Value,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Label
    )

    $Property = $Value.PSObject.Properties[$Name]
    if ($null -eq $Property) {
        Throw-AvidScriptBuildCookRunError `
            -Category 'json_schema_invalid' `
            -Message "$Label is missing '$Name'."
    }
    return $Property.Value
}

function Get-AvidScriptBuildCookRunProjectContext {
    param([Parameter(Mandatory = $true)][string]$ProjectRoot)

    $FullProjectRoot = [System.IO.Path]::GetFullPath($ProjectRoot).TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar)
    if (-not (Test-Path -LiteralPath $FullProjectRoot -PathType Container)) {
        Throw-AvidScriptBuildCookRunError `
            -Category 'project_root_invalid' `
            -Message "ProjectRoot is missing: $FullProjectRoot"
    }
    $PluginRelativePath = [System.IO.Path]::GetRelativePath(
        $FullProjectRoot,
        $AvidScriptBuildCookRunPluginRoot).Replace('\', '/')
    if (-not $PluginRelativePath.Equals(
            'Plugins/AvidScript',
            [System.StringComparison]::OrdinalIgnoreCase)) {
        Throw-AvidScriptBuildCookRunError `
            -Category 'plugin_root_invalid' `
            -Message 'Runner must reside at ProjectRoot/Plugins/AvidScript/Build.'
    }

    $TargetName = Split-Path -Leaf $FullProjectRoot
    $ProjectFile = Join-Path $FullProjectRoot "$TargetName.uproject"
    $GameTargetFile = Join-Path $FullProjectRoot "Source/$TargetName.Target.cs"
    if (-not (Test-Path -LiteralPath $ProjectFile -PathType Leaf)) {
        Throw-AvidScriptBuildCookRunError `
            -Category 'project_file_missing' `
            -Message "Project file is missing: $ProjectFile"
    }
    if (-not (Test-Path -LiteralPath $GameTargetFile -PathType Leaf)) {
        Throw-AvidScriptBuildCookRunError `
            -Category 'game_target_missing' `
            -Message "Game target source is missing: $GameTargetFile"
    }
    $GameTargetSource = [System.IO.File]::ReadAllText($GameTargetFile)
    if ($GameTargetSource -cnotmatch 'Type\s*=\s*TargetType\.Game\s*;') {
        Throw-AvidScriptBuildCookRunError `
            -Category 'game_target_invalid' `
            -Message "$TargetName.Target.cs is not a Game target."
    }

    return [pscustomobject][ordered]@{
        ProjectRoot = $FullProjectRoot
        ProjectFile = $ProjectFile
        PluginRoot = [System.IO.Path]::GetFullPath($AvidScriptBuildCookRunPluginRoot)
        TargetName = $TargetName
    }
}

function Get-AvidScriptBuildCookRunEngineContext {
    param([Parameter(Mandatory = $true)][string]$EngineRoot)

    $ExpectedEngineRoot = [System.IO.Path]::GetFullPath('C:\UnrealEngine')
    $FullEngineRoot = [System.IO.Path]::GetFullPath($EngineRoot).TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar)
    if (-not $FullEngineRoot.Equals(
            $ExpectedEngineRoot,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        Throw-AvidScriptBuildCookRunError `
            -Category 'engine_root_invalid' `
            -Message 'EngineRoot must be the fixed C:\UnrealEngine UE5.8 source checkout.'
    }

    $BuildVersionPath = Join-Path $FullEngineRoot 'Engine/Build/Build.version'
    $EngineSourceRoot = Join-Path $FullEngineRoot 'Engine/Source'
    $RunUatPath = Join-Path $FullEngineRoot 'Engine/Build/BatchFiles/RunUAT.bat'
    if (-not (Test-Path -LiteralPath $BuildVersionPath -PathType Leaf) -or
        -not (Test-Path -LiteralPath $EngineSourceRoot -PathType Container) -or
        -not (Test-Path -LiteralPath $RunUatPath -PathType Leaf)) {
        Throw-AvidScriptBuildCookRunError `
            -Category 'engine_missing' `
            -Message 'The fixed UE source checkout is incomplete.'
    }
    try {
        $BuildVersion = ConvertFrom-AvidScriptBuildCookRunJsonText `
            -Text ([System.IO.File]::ReadAllText($BuildVersionPath)) `
            -Label 'UE Build.version'
    }
    catch {
        Throw-AvidScriptBuildCookRunError `
            -Category 'engine_version_invalid' `
            -Message "UE Build.version is invalid: $($_.Exception.Message)"
    }
    $MajorVersion = [int](Get-AvidScriptBuildCookRunRequiredProperty `
            -Value $BuildVersion `
            -Name 'MajorVersion' `
            -Label 'UE Build.version')
    $MinorVersion = [int](Get-AvidScriptBuildCookRunRequiredProperty `
            -Value $BuildVersion `
            -Name 'MinorVersion' `
            -Label 'UE Build.version')
    if ($MajorVersion -ne 5 -or $MinorVersion -ne 8) {
        Throw-AvidScriptBuildCookRunError `
            -Category 'engine_version_invalid' `
            -Message 'BuildCookRun requires the fixed UE5.8 source checkout.'
    }

    return [pscustomobject][ordered]@{
        EngineRoot = $FullEngineRoot
        RunUatPath = $RunUatPath
        MajorVersion = $MajorVersion
        MinorVersion = $MinorVersion
    }
}

function Invoke-AvidScriptBuildCookRunProcess {
    param(
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [string]$OutputLogPath = ''
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
    $Stdout = ''
    $Stderr = ''
    try {
        try {
            if (-not $Process.Start()) {
                Throw-AvidScriptBuildCookRunError `
                    -Category 'process_launch_failed' `
                    -Message "Process could not be launched: $Executable"
            }
        }
        catch {
            if ($_.Exception.Data.Contains('category')) {
                throw
            }
            Throw-AvidScriptBuildCookRunError `
                -Category 'process_launch_failed' `
                -Message "Process could not be launched: $Executable"
        }
        $StdoutTask = $Process.StandardOutput.ReadToEndAsync()
        $StderrTask = $Process.StandardError.ReadToEndAsync()
        $Process.WaitForExit()
        $Stdout = $StdoutTask.GetAwaiter().GetResult()
        $Stderr = $StderrTask.GetAwaiter().GetResult()
        return [pscustomobject][ordered]@{
            ExitCode = $Process.ExitCode
            Stdout = $Stdout
            Stderr = $Stderr
        }
    }
    finally {
        if (-not [string]::IsNullOrWhiteSpace($OutputLogPath)) {
            $LogText = [System.Text.StringBuilder]::new()
            [void]$LogText.AppendLine("executable=$Executable")
            foreach ($Argument in $Arguments) {
                [void]$LogText.AppendLine("argument=$Argument")
            }
            [void]$LogText.AppendLine('--- stdout ---')
            [void]$LogText.Append($Stdout)
            [void]$LogText.AppendLine()
            [void]$LogText.AppendLine('--- stderr ---')
            [void]$LogText.Append($Stderr)
            [System.IO.File]::WriteAllText(
                $OutputLogPath,
                $LogText.ToString(),
                [System.Text.UTF8Encoding]::new($false))
        }
        $Process.Dispose()
    }
}

function New-AvidScriptBuildCookRunReleaseArguments {
    param(
        [Parameter(Mandatory = $true)][string]$ReleaseScriptPath,
        [Parameter(Mandatory = $true)][string]$SourcePath,
        [Parameter(Mandatory = $true)][string]$CSharpProjectPath,
        [Parameter(Mandatory = $true)][string]$ModuleId,
        [Parameter(Mandatory = $true)][string]$ArtifactStem,
        [Parameter(Mandatory = $true)][string]$OutputRoot,
        [Parameter(Mandatory = $true)][string]$DotNetPath,
        [string]$BindingPackagePath = '',
        [string]$RuntimeBindingPackagePath = '',
        [string]$GeneratedTypeManifestPath = '',
        [Parameter(Mandatory = $true)][string]$Configuration,
        [Parameter(Mandatory = $true)][string]$EngineRoot
    )

    $Arguments = [System.Collections.Generic.List[string]]::new()
    foreach ($Argument in @(
            '-NoProfile',
            '-NonInteractive',
            '-ExecutionPolicy',
            'Bypass',
            '-File',
            $ReleaseScriptPath,
            '-SourcePath',
            $SourcePath,
            '-CSharpProjectPath',
            $CSharpProjectPath,
            '-ModuleId',
            $ModuleId,
            '-ArtifactStem',
            $ArtifactStem,
            '-OutputRoot',
            $OutputRoot,
            '-DotNetPath',
            $DotNetPath)) {
        $Arguments.Add($Argument)
    }
    if (-not [string]::IsNullOrWhiteSpace($BindingPackagePath)) {
        $Arguments.Add('-BindingPackagePath')
        $Arguments.Add($BindingPackagePath)
    }
    if (-not [string]::IsNullOrWhiteSpace($RuntimeBindingPackagePath)) {
        $Arguments.Add('-RuntimeBindingPackagePath')
        $Arguments.Add($RuntimeBindingPackagePath)
    }
    if (-not [string]::IsNullOrWhiteSpace($GeneratedTypeManifestPath)) {
        $Arguments.Add('-GeneratedTypeManifestPath')
        $Arguments.Add($GeneratedTypeManifestPath)
    }
    foreach ($Argument in @(
            '-Configuration',
            $Configuration,
            '-EngineRoot',
            $EngineRoot)) {
        $Arguments.Add($Argument)
    }
    return $Arguments.ToArray()
}

function Invoke-AvidScriptBuildCookRunReleaseStep {
    param(
        [Parameter(Mandatory = $true)][object]$ProjectContext,
        [Parameter(Mandatory = $true)][object]$EngineContext,
        [Parameter(Mandatory = $true)][string]$SourcePath,
        [Parameter(Mandatory = $true)][string]$CSharpProjectPath,
        [Parameter(Mandatory = $true)][string]$ModuleId,
        [Parameter(Mandatory = $true)][string]$ArtifactStem,
        [Parameter(Mandatory = $true)][string]$OutputRoot,
        [Parameter(Mandatory = $true)][string]$DotNetPath,
        [string]$BindingPackagePath = '',
        [string]$RuntimeBindingPackagePath = '',
        [string]$GeneratedTypeManifestPath = '',
        [Parameter(Mandatory = $true)][string]$Configuration
    )

    $ReleaseScriptPath = Join-Path `
        $AvidScriptBuildCookRunScriptRoot `
        'InvokeAvidScriptRelease.ps1'
    $PowerShellPath = Join-Path $PSHOME 'pwsh.exe'
    foreach ($RequiredFile in @($ReleaseScriptPath, $PowerShellPath)) {
        if (-not (Test-Path -LiteralPath $RequiredFile -PathType Leaf)) {
            Throw-AvidScriptBuildCookRunError `
                -Category 'release_host_missing' `
                -Message "Release dependency is missing: $RequiredFile"
        }
    }
    $Arguments = New-AvidScriptBuildCookRunReleaseArguments `
        -ReleaseScriptPath $ReleaseScriptPath `
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
        -EngineRoot $EngineContext.EngineRoot
    $ProcessResult = Invoke-AvidScriptBuildCookRunProcess `
        -Executable $PowerShellPath `
        -Arguments $Arguments `
        -WorkingDirectory $ProjectContext.ProjectRoot

    if ($ProcessResult.ExitCode -eq 0) {
        if (-not [string]::IsNullOrWhiteSpace($ProcessResult.Stderr)) {
            Throw-AvidScriptBuildCookRunError `
                -Category 'release_output_invalid' `
                -Message 'Release succeeded but wrote unexpected stderr output.'
        }
        try {
            $Payload = ConvertFrom-AvidScriptBuildCookRunJsonText `
                -Text $ProcessResult.Stdout `
                -Label 'release stdout' `
                -RequireSingleLine
        }
        catch {
            Throw-AvidScriptBuildCookRunError `
                -Category 'release_output_invalid' `
                -Message $_.Exception.Message
        }
        if ([long](Get-AvidScriptBuildCookRunRequiredProperty $Payload 'schema_version' 'release result') -ne 1 -or
            [string](Get-AvidScriptBuildCookRunRequiredProperty $Payload 'result' 'release result') -cne
                'avidscript_module_release_succeeded' -or
            [string](Get-AvidScriptBuildCookRunRequiredProperty $Payload 'module_id' 'release result') -cne $ModuleId -or
            [string](Get-AvidScriptBuildCookRunRequiredProperty $Payload 'configuration' 'release result') -cne
                $Configuration.ToLowerInvariant()) {
            Throw-AvidScriptBuildCookRunError `
                -Category 'release_output_invalid' `
                -Message 'Release JSON does not match the requested module and configuration.'
        }
        return $Payload
    }

    if (-not [string]::IsNullOrWhiteSpace($ProcessResult.Stdout)) {
        Throw-AvidScriptBuildCookRunError `
            -Category 'release_output_invalid' `
            -Message 'Failed release wrote unexpected stdout output.'
    }
    try {
        $Failure = ConvertFrom-AvidScriptBuildCookRunJsonText `
            -Text $ProcessResult.Stderr `
            -Label 'release stderr' `
            -RequireSingleLine
    }
    catch {
        Throw-AvidScriptBuildCookRunError `
            -Category 'release_output_invalid' `
            -Message $_.Exception.Message
    }
    $ReleaseCategory = [string](Get-AvidScriptBuildCookRunRequiredProperty `
            $Failure 'category' 'release failure')
    $ReleaseMessage = [string](Get-AvidScriptBuildCookRunRequiredProperty `
            $Failure 'message' 'release failure')
    Throw-AvidScriptBuildCookRunError `
        -Category 'release_failed' `
        -Message "Release failed ($ReleaseCategory): $ReleaseMessage"
}

function New-AvidScriptBuildCookRunUatArguments {
    param(
        [Parameter(Mandatory = $true)][string]$ProjectFile,
        [Parameter(Mandatory = $true)][string]$TargetName,
        [ValidateSet('Development', 'Shipping')]
        [Parameter(Mandatory = $true)][string]$Configuration,
        [Parameter(Mandatory = $true)][string]$ArchiveRoot
    )

    return @(
        'BuildCookRun',
        '-nop4',
        '-unattended',
        '-utf8output',
        "-project=$ProjectFile",
        "-target=$TargetName",
        '-targetplatform=Win64',
        "-clientconfig=$Configuration",
        '-build',
        '-cook',
        '-stage',
        '-pak',
        '-archive',
        "-archivedirectory=$ArchiveRoot",
        '-AdditionalCookerOptions=-SkipZenStore'
    )
}

function Invoke-AvidScriptBuildCookRunUatStep {
    param(
        [Parameter(Mandatory = $true)][object]$ProjectContext,
        [Parameter(Mandatory = $true)][object]$EngineContext,
        [ValidateSet('Development', 'Shipping')]
        [Parameter(Mandatory = $true)][string]$Configuration,
        [Parameter(Mandatory = $true)][string]$ArchiveRoot
    )

    $LogRoot = Join-Path `
        $ProjectContext.ProjectRoot `
        'Saved/AvidScript/BuildCookRunLogs'
    [void][System.IO.Directory]::CreateDirectory($LogRoot)
    $UatLog = Join-Path `
        $LogRoot `
        ("BuildCookRun-$Configuration-$PID-$([Guid]::NewGuid().ToString('N')).log")
    [System.IO.File]::WriteAllText(
        $UatLog,
        '',
        [System.Text.UTF8Encoding]::new($false))
    $script:AvidScriptBuildCookRunUatLog = $UatLog

    $CmdPath = Join-Path `
        ([System.Environment]::GetFolderPath([System.Environment+SpecialFolder]::System)) `
        'cmd.exe'
    if (-not (Test-Path -LiteralPath $CmdPath -PathType Leaf)) {
        Throw-AvidScriptBuildCookRunError `
            -Category 'process_host_missing' `
            -Message "Windows command host is missing: $CmdPath"
    }
    $UatArguments = New-AvidScriptBuildCookRunUatArguments `
        -ProjectFile $ProjectContext.ProjectFile `
        -TargetName $ProjectContext.TargetName `
        -Configuration $Configuration `
        -ArchiveRoot $ArchiveRoot
    $ProcessArguments = @(
        '/d',
        '/s',
        '/c',
        $EngineContext.RunUatPath) + $UatArguments
    $StartedUtc = [System.DateTime]::UtcNow
    $ProcessResult = Invoke-AvidScriptBuildCookRunProcess `
        -Executable $CmdPath `
        -Arguments $ProcessArguments `
        -WorkingDirectory $ProjectContext.ProjectRoot `
        -OutputLogPath $UatLog
    if ($ProcessResult.ExitCode -ne 0) {
        Throw-AvidScriptBuildCookRunError `
            -Category 'uat_failed' `
            -Message "BuildCookRun failed with exit code $($ProcessResult.ExitCode). See $UatLog"
    }
    return [pscustomobject][ordered]@{
        StartedUtc = $StartedUtc
        ExitCode = $ProcessResult.ExitCode
        LogPath = $UatLog
        Arguments = $UatArguments
    }
}

function Get-AvidScriptBuildCookRunGameReceipt {
    param(
        [Parameter(Mandatory = $true)][string]$ProjectRoot,
        [Parameter(Mandatory = $true)][string]$TargetName,
        [ValidateSet('Development', 'Shipping')]
        [Parameter(Mandatory = $true)][string]$Configuration,
        [Parameter(Mandatory = $true)][datetime]$NotBeforeUtc
    )

    $ReceiptRoot = Join-Path $ProjectRoot 'Binaries/Win64'
    if (-not (Test-Path -LiteralPath $ReceiptRoot -PathType Container)) {
        Throw-AvidScriptBuildCookRunError `
            -Category 'receipt_missing' `
            -Message "Win64 receipt directory is missing: $ReceiptRoot"
    }
    $Matches = [System.Collections.Generic.List[object]]::new()
    foreach ($ReceiptFile in @(Get-ChildItem `
            -LiteralPath $ReceiptRoot `
            -Filter '*.target' `
            -File)) {
        try {
            $Receipt = ConvertFrom-AvidScriptBuildCookRunJsonText `
                -Text ([System.IO.File]::ReadAllText($ReceiptFile.FullName)) `
                -Label "receipt '$($ReceiptFile.Name)'"
            $ReceiptTargetName = [string](Get-AvidScriptBuildCookRunRequiredProperty `
                    $Receipt 'TargetName' "receipt '$($ReceiptFile.Name)'")
            $ReceiptPlatform = [string](Get-AvidScriptBuildCookRunRequiredProperty `
                    $Receipt 'Platform' "receipt '$($ReceiptFile.Name)'")
            $ReceiptConfiguration = [string](Get-AvidScriptBuildCookRunRequiredProperty `
                    $Receipt 'Configuration' "receipt '$($ReceiptFile.Name)'")
            $ReceiptTargetType = [string](Get-AvidScriptBuildCookRunRequiredProperty `
                    $Receipt 'TargetType' "receipt '$($ReceiptFile.Name)'")
        }
        catch {
            Throw-AvidScriptBuildCookRunError `
                -Category 'receipt_parse_failed' `
                -Message $_.Exception.Message
        }
        if ($ReceiptTargetName -ceq $TargetName -and
            $ReceiptPlatform -ceq 'Win64' -and
            $ReceiptConfiguration -ceq $Configuration -and
            $ReceiptTargetType -ceq 'Game') {
            $Matches.Add([pscustomobject][ordered]@{
                    Path = $ReceiptFile.FullName
                    LastWriteTimeUtc = $ReceiptFile.LastWriteTimeUtc
                    Receipt = $Receipt
                })
        }
    }
    if ($Matches.Count -eq 0) {
        Throw-AvidScriptBuildCookRunError `
            -Category 'receipt_missing' `
            -Message "No exact $TargetName Win64 $Configuration Game receipt was produced."
    }
    if ($Matches.Count -ne 1) {
        Throw-AvidScriptBuildCookRunError `
            -Category 'receipt_ambiguous' `
            -Message "Multiple exact $TargetName Win64 $Configuration Game receipts were produced."
    }
    $Selected = $Matches[0]
    if ($Selected.LastWriteTimeUtc -lt $NotBeforeUtc.ToUniversalTime()) {
        Throw-AvidScriptBuildCookRunError `
            -Category 'receipt_stale' `
            -Message "Selected receipt predates this UAT run: $($Selected.Path)"
    }
    return $Selected
}

function Invoke-AvidScriptBuildCookRunReceiptStep {
    param(
        [Parameter(Mandatory = $true)][object]$ProjectContext,
        [Parameter(Mandatory = $true)][string]$ReceiptPath,
        [ValidateSet('Development', 'Shipping')]
        [Parameter(Mandatory = $true)][string]$Configuration
    )

    $ValidatorPath = Join-Path `
        $AvidScriptBuildCookRunScriptRoot `
        'TestAvidScriptPackageReceipt.ps1'
    $PowerShellPath = Join-Path $PSHOME 'pwsh.exe'
    foreach ($RequiredFile in @($ValidatorPath, $PowerShellPath)) {
        if (-not (Test-Path -LiteralPath $RequiredFile -PathType Leaf)) {
            Throw-AvidScriptBuildCookRunError `
                -Category 'receipt_validator_missing' `
                -Message "Receipt validation dependency is missing: $RequiredFile"
        }
    }
    $Arguments = @(
        '-NoProfile',
        '-NonInteractive',
        '-ExecutionPolicy',
        'Bypass',
        '-File',
        $ValidatorPath,
        '-ReceiptPath',
        $ReceiptPath,
        '-ProjectRoot',
        $ProjectContext.ProjectRoot,
        '-PluginRoot',
        $ProjectContext.PluginRoot,
        '-Configuration',
        $Configuration)
    $ProcessResult = Invoke-AvidScriptBuildCookRunProcess `
        -Executable $PowerShellPath `
        -Arguments $Arguments `
        -WorkingDirectory $ProjectContext.ProjectRoot
    if (-not [string]::IsNullOrWhiteSpace($ProcessResult.Stderr)) {
        Throw-AvidScriptBuildCookRunError `
            -Category 'receipt_output_invalid' `
            -Message 'Receipt validator wrote unexpected stderr output.'
    }
    try {
        $Payload = ConvertFrom-AvidScriptBuildCookRunJsonText `
            -Text $ProcessResult.Stdout `
            -Label 'receipt validator stdout' `
            -RequireSingleLine
    }
    catch {
        Throw-AvidScriptBuildCookRunError `
            -Category 'receipt_output_invalid' `
            -Message $_.Exception.Message
    }
    if ($ProcessResult.ExitCode -ne 0) {
        $ErrorCode = [string](Get-AvidScriptBuildCookRunRequiredProperty `
                $Payload 'error_code' 'receipt validation failure')
        $Message = [string](Get-AvidScriptBuildCookRunRequiredProperty `
                $Payload 'message' 'receipt validation failure')
        Throw-AvidScriptBuildCookRunError `
            -Category 'receipt_validation_failed' `
            -Message "Receipt validation failed ($ErrorCode): $Message"
    }
    if ([long](Get-AvidScriptBuildCookRunRequiredProperty $Payload 'schema_version' 'receipt result') -ne 1 -or
        [string](Get-AvidScriptBuildCookRunRequiredProperty $Payload 'result' 'receipt result') -cne
            'avidscript_package_receipt_valid' -or
        [string](Get-AvidScriptBuildCookRunRequiredProperty $Payload 'configuration' 'receipt result') -cne
            $Configuration -or
        [string](Get-AvidScriptBuildCookRunRequiredProperty $Payload 'target_name' 'receipt result') -cne
            $ProjectContext.TargetName) {
        Throw-AvidScriptBuildCookRunError `
            -Category 'receipt_output_invalid' `
            -Message 'Receipt validator JSON does not match the selected target.'
    }
    return $Payload
}

function Invoke-AvidScriptBuildCookRunOracleStep {
    param(
        [Parameter(Mandatory = $true)][object]$ProjectContext,
        [Parameter(Mandatory = $true)][string]$ArchiveRoot,
        [Parameter(Mandatory = $true)][string]$ModuleId,
        [ValidateSet('Development', 'Shipping')]
        [Parameter(Mandatory = $true)][string]$Configuration,
        [ValidateRange(10, 600)]
        [Parameter(Mandatory = $true)][int]$TimeoutSeconds
    )

    $OraclePath = Join-Path `
        $AvidScriptBuildCookRunScriptRoot `
        'InvokeAvidScriptPackagedOracle.ps1'
    $PowerShellPath = Join-Path $PSHOME 'pwsh.exe'
    foreach ($RequiredFile in @($OraclePath, $PowerShellPath)) {
        if (-not (Test-Path -LiteralPath $RequiredFile -PathType Leaf)) {
            Throw-AvidScriptBuildCookRunError `
                -Category 'packaged_oracle_host_missing' `
                -Message "Packaged oracle dependency is missing: $RequiredFile"
        }
    }
    $Arguments = @(
        '-NoProfile',
        '-NonInteractive',
        '-ExecutionPolicy',
        'Bypass',
        '-File',
        $OraclePath,
        '-ArchiveRoot',
        $ArchiveRoot,
        '-TargetName',
        $ProjectContext.TargetName,
        '-ModuleId',
        $ModuleId,
        '-Configuration',
        $Configuration,
        '-TimeoutSeconds',
        $TimeoutSeconds.ToString([System.Globalization.CultureInfo]::InvariantCulture))
    $ProcessResult = Invoke-AvidScriptBuildCookRunProcess `
        -Executable $PowerShellPath `
        -Arguments $Arguments `
        -WorkingDirectory $ProjectContext.ProjectRoot
    if (-not [string]::IsNullOrWhiteSpace($ProcessResult.Stderr)) {
        Throw-AvidScriptBuildCookRunError `
            -Category 'packaged_oracle_output_invalid' `
            -Message 'Packaged oracle runner wrote unexpected stderr output.'
    }
    try {
        $Payload = ConvertFrom-AvidScriptBuildCookRunJsonText `
            -Text $ProcessResult.Stdout `
            -Label 'packaged oracle stdout' `
            -RequireSingleLine
    }
    catch {
        Throw-AvidScriptBuildCookRunError `
            -Category 'packaged_oracle_output_invalid' `
            -Message $_.Exception.Message
    }
    if ($ProcessResult.ExitCode -ne 0) {
        $Category = [string](Get-AvidScriptBuildCookRunRequiredProperty `
                $Payload 'category' 'packaged oracle failure')
        $Message = [string](Get-AvidScriptBuildCookRunRequiredProperty `
                $Payload 'message' 'packaged oracle failure')
        Throw-AvidScriptBuildCookRunError `
            -Category 'packaged_oracle_failed' `
            -Message "Packaged oracle failed ($Category): $Message"
    }
    if ([long](Get-AvidScriptBuildCookRunRequiredProperty $Payload 'schema_version' 'packaged oracle result') -ne 1 -or
        [string](Get-AvidScriptBuildCookRunRequiredProperty $Payload 'result' 'packaged oracle result') -cne
            'avidscript_packaged_oracle_process_passed' -or
        [string](Get-AvidScriptBuildCookRunRequiredProperty $Payload 'configuration' 'packaged oracle result') -cne
            $Configuration -or
        [string](Get-AvidScriptBuildCookRunRequiredProperty $Payload 'module_id' 'packaged oracle result') -cne
            $ModuleId) {
        Throw-AvidScriptBuildCookRunError `
            -Category 'packaged_oracle_output_invalid' `
            -Message 'Packaged oracle JSON does not match the requested release.'
    }
    return $Payload
}

function Invoke-AvidScriptBuildCookRun {
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
        [ValidateSet('Development', 'Shipping')]
        [Parameter(Mandatory = $true)][string]$Configuration,
        [Parameter(Mandatory = $true)][string]$ArchiveRoot,
        [ValidateRange(10, 600)]
        [Parameter(Mandatory = $true)][int]$PackagedOracleTimeoutSeconds,
        [Parameter(Mandatory = $true)][string]$EngineRoot
    )

    $script:AvidScriptBuildCookRunStep = 'validation'
    $ProjectContext = Get-AvidScriptBuildCookRunProjectContext `
        -ProjectRoot $AvidScriptBuildCookRunProjectRoot
    $EngineContext = Get-AvidScriptBuildCookRunEngineContext `
        -EngineRoot $EngineRoot
    $ResolvedArchiveRoot = Resolve-AvidScriptBuildCookRunArchiveRoot `
        -Path $ArchiveRoot `
        -ProjectRoot $ProjectContext.ProjectRoot

    $script:AvidScriptBuildCookRunStep = 'release'
    $ReleaseResult = Invoke-AvidScriptBuildCookRunReleaseStep `
        -ProjectContext $ProjectContext `
        -EngineContext $EngineContext `
        -SourcePath $SourcePath `
        -CSharpProjectPath $CSharpProjectPath `
        -ModuleId $ModuleId `
        -ArtifactStem $ArtifactStem `
        -OutputRoot $OutputRoot `
        -DotNetPath $DotNetPath `
        -BindingPackagePath $BindingPackagePath `
        -RuntimeBindingPackagePath $RuntimeBindingPackagePath `
        -GeneratedTypeManifestPath $GeneratedTypeManifestPath `
        -Configuration $Configuration

    $script:AvidScriptBuildCookRunStep = 'uat'
    $UatResult = Invoke-AvidScriptBuildCookRunUatStep `
        -ProjectContext $ProjectContext `
        -EngineContext $EngineContext `
        -Configuration $Configuration `
        -ArchiveRoot $ResolvedArchiveRoot

    $script:AvidScriptBuildCookRunStep = 'receipt_selection'
    $SelectedReceipt = Get-AvidScriptBuildCookRunGameReceipt `
        -ProjectRoot $ProjectContext.ProjectRoot `
        -TargetName $ProjectContext.TargetName `
        -Configuration $Configuration `
        -NotBeforeUtc $UatResult.StartedUtc

    $script:AvidScriptBuildCookRunStep = 'receipt_validation'
    $ReceiptResult = Invoke-AvidScriptBuildCookRunReceiptStep `
        -ProjectContext $ProjectContext `
        -ReceiptPath $SelectedReceipt.Path `
        -Configuration $Configuration

    $script:AvidScriptBuildCookRunStep = 'packaged_oracle'
    $OracleResult = Invoke-AvidScriptBuildCookRunOracleStep `
        -ProjectContext $ProjectContext `
        -ArchiveRoot $ResolvedArchiveRoot `
        -ModuleId $ModuleId `
        -Configuration $Configuration `
        -TimeoutSeconds $PackagedOracleTimeoutSeconds

    return [pscustomobject][ordered]@{
        schema_version = 1
        result = 'avidscript_build_cook_run_succeeded'
        status = 'ok'
        target_name = $ProjectContext.TargetName
        platform = 'Win64'
        configuration = $Configuration
        archive_root = $ResolvedArchiveRoot
        uat_log = $UatResult.LogPath
        receipt_path = $SelectedReceipt.Path
        release = $ReleaseResult
        receipt_validation = $ReceiptResult
        packaged_oracle = $OracleResult
    }
}

if ($MyInvocation.InvocationName -eq '.') {
    return
}

try {
    $Summary = Invoke-AvidScriptBuildCookRun `
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
        -ArchiveRoot $ArchiveRoot `
        -PackagedOracleTimeoutSeconds $PackagedOracleTimeoutSeconds `
        -EngineRoot $EngineRoot
    [Console]::Out.WriteLine(($Summary | ConvertTo-Json -Depth 32 -Compress))
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
        result = 'avidscript_build_cook_run_failed'
        status = 'error'
        category = $Category
        step = $script:AvidScriptBuildCookRunStep
        configuration = $Configuration
        archive_root = $ArchiveRoot
        uat_log = $script:AvidScriptBuildCookRunUatLog
        message = $_.Exception.Message
    }
    [Console]::Out.WriteLine(($Failure | ConvertTo-Json -Depth 8 -Compress))
    exit 1
}
