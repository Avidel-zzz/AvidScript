[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$OutputRoot,
    [string]$Version = '',
    [ValidateSet('preview')][string]$Channel = 'preview',
    [string]$Commit = 'HEAD',
    [string]$PluginRoot = ''
)

$ErrorActionPreference = 'Stop'
$ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
if ([string]::IsNullOrWhiteSpace($PluginRoot)) {
    $PluginRoot = Split-Path -Parent $ScriptRoot
}
$PluginRoot = [System.IO.Path]::GetFullPath($PluginRoot)
$OutputRoot = [System.IO.Path]::GetFullPath($OutputRoot)
. (Join-Path $ScriptRoot 'ReleaseEngineering\AvidScriptPluginReleasePackage.ps1')

function Invoke-AvidScriptPluginReleaseGit {
    param(
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [switch]$AllowFailure
    )

    $StartInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $StartInfo.FileName = 'git'
    $StartInfo.WorkingDirectory = $PluginRoot
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
        if (-not $Process.Start()) {
            Throw-AvidScriptPluginReleaseError 'ASRE2001' 'process_failed' 'git could not be started.'
        }
        $StdoutTask = $Process.StandardOutput.ReadToEndAsync()
        $StderrTask = $Process.StandardError.ReadToEndAsync()
        $Process.WaitForExit()
        $Result = [pscustomobject][ordered]@{
            ExitCode = $Process.ExitCode
            Stdout = $StdoutTask.GetAwaiter().GetResult().Trim()
            Stderr = $StderrTask.GetAwaiter().GetResult().Trim()
        }
        if (-not $AllowFailure -and $Result.ExitCode -ne 0) {
            Throw-AvidScriptPluginReleaseError `
                'ASRE2002' `
                'git_failed' `
                "git failed with exit $($Result.ExitCode): $($Result.Stderr)"
        }
        return $Result
    }
    finally {
        $Process.Dispose()
    }
}

function Assert-AvidScriptPluginReleasePrivacy {
    param([Parameter(Mandatory = $true)][string]$PayloadPluginRoot)

    $DeniedExtensions = @('.key', '.pem', '.pfx', '.p12', '.snk')
    $TextExtensions = @(
        '.bat', '.c', '.cmake', '.cpp', '.cs', '.csproj', '.h', '.hpp', '.ini',
        '.json', '.md', '.props', '.ps1', '.psm1', '.py', '.sh', '.targets',
        '.toml', '.txt', '.uplugin', '.xml', '.yaml', '.yml')
    foreach ($File in @(Get-ChildItem -LiteralPath $PayloadPluginRoot -File -Force -Recurse)) {
        $LowerName = $File.Name.ToLowerInvariant()
        if ($DeniedExtensions -contains $File.Extension.ToLowerInvariant() -or
            $LowerName -eq '.env' -or $LowerName.StartsWith('.env.')) {
            Throw-AvidScriptPluginReleaseError 'ASRE2004' 'privacy_failed' "release contains denied credential material: $($File.FullName)"
        }
        if ($TextExtensions -notcontains $File.Extension.ToLowerInvariant()) {
            continue
        }
        $Text = [System.IO.File]::ReadAllText($File.FullName)
        foreach ($Pattern in @(
                '(?i)[A-Z]:[\\/]+Users[\\/]+',
                '-----BEGIN [A-Z ]*PRIVATE KEY-----',
                'gh[pousr]_[A-Za-z0-9_]{20,}')) {
            if ($Text -match $Pattern) {
                Throw-AvidScriptPluginReleaseError 'ASRE2005' 'privacy_failed' "release privacy scan failed: $($File.FullName)"
            }
        }
    }
}

function Test-AvidScriptPluginReleaseFileContainsAscii {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string[]]$Needles
    )

    $MaximumNeedleLength = [int]($Needles | Measure-Object Length -Maximum).Maximum
    $Carry = [byte[]]::new(0)
    $Buffer = [byte[]]::new(65536)
    $Stream = [System.IO.File]::OpenRead($Path)
    try {
        while (($Read = $Stream.Read($Buffer, 0, $Buffer.Length)) -gt 0) {
            $Combined = [byte[]]::new($Carry.Length + $Read)
            if ($Carry.Length -gt 0) {
                [System.Buffer]::BlockCopy($Carry, 0, $Combined, 0, $Carry.Length)
            }
            [System.Buffer]::BlockCopy($Buffer, 0, $Combined, $Carry.Length, $Read)
            $Chunk = [System.Text.Encoding]::ASCII.GetString($Combined)
            foreach ($Needle in $Needles) {
                if ($Chunk.IndexOf($Needle, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
                    return $true
                }
            }
            $CarryLength = [Math]::Min($MaximumNeedleLength - 1, $Combined.Length)
            $Carry = [byte[]]::new($CarryLength)
            if ($CarryLength -gt 0) {
                [System.Buffer]::BlockCopy(
                    $Combined,
                    $Combined.Length - $CarryLength,
                    $Carry,
                    0,
                    $CarryLength)
            }
        }
        return $false
    }
    finally {
        $Stream.Dispose()
    }
}

function Assert-AvidScriptPluginReleaseBinaryPrivacy {
    param([Parameter(Mandatory = $true)][string]$PayloadPluginRoot)

    $BinaryExtensions = @('.a', '.dll', '.dylib', '.exe', '.lib', '.node', '.pdb', '.so', '.wasm')
    $Needles = @(
        'C:\Users\',
        'C:/Users/',
        '/Users/',
        '/home/')
    foreach ($File in @(Get-ChildItem -LiteralPath $PayloadPluginRoot -File -Force -Recurse)) {
        if ($BinaryExtensions -cnotcontains $File.Extension.ToLowerInvariant()) {
            continue
        }
        if (Test-AvidScriptPluginReleaseFileContainsAscii $File.FullName $Needles) {
            Throw-AvidScriptPluginReleaseError `
                'ASRE2006' `
                'privacy_failed' `
                "release binary privacy scan found a local user path: $($File.FullName)"
        }
    }
}

function Assert-AvidScriptSourceReleaseProfile {
    param([Parameter(Mandatory = $true)]$Profile)

    Assert-AvidScriptPluginReleaseObjectShape `
        -Value $Profile `
        -Required @(
            'schema_version', 'profile', 'publisher_version', 'include', 'exclude',
            'generated', 'targets', 'contracts', 'dependencies') `
        -Label 'source release file profile'
    if ([int]$Profile.schema_version -ne 1 -or
        [string]$Profile.profile -cne 'source-developer' -or
        [string]$Profile.publisher_version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$') {
        Throw-AvidScriptPluginReleaseError 'ASRE2009' 'profile_invalid' 'selected commit release file profile is unsupported.'
    }

    $IncludeNames = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::Ordinal)
    foreach ($Included in @($Profile.include)) {
        $Normalized = Normalize-AvidScriptPluginReleaseRelativePath `
            ([string]$Included) `
            'source release include'
        if ($Normalized.Contains('*') -or -not $IncludeNames.Add($Normalized)) {
            Throw-AvidScriptPluginReleaseError 'ASRE2010' 'profile_invalid' 'source release include paths must be unique literal repository paths.'
        }
    }
    foreach ($RequiredPath in @('AvidScript.uplugin', 'LICENSE', 'global.json', 'Build', 'Source')) {
        if (-not $IncludeNames.Contains($RequiredPath)) {
            Throw-AvidScriptPluginReleaseError 'ASRE2011' 'profile_invalid' "source release include is missing: $RequiredPath"
        }
    }

    $ExcludeNames = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::Ordinal)
    foreach ($Excluded in @($Profile.exclude)) {
        $Normalized = if ([string]$Excluded -ceq 'Tools/*.Tests') {
            'Tools/*.Tests'
        }
        else {
            Normalize-AvidScriptPluginReleaseRelativePath `
                ([string]$Excluded) `
                'source release exclude'
        }
        if (-not $ExcludeNames.Add($Normalized)) {
            Throw-AvidScriptPluginReleaseError 'ASRE2012' 'profile_invalid' 'source release exclude paths must be unique.'
        }
    }
    foreach ($RequiredExclusion in @(
            'Build/Contracts', 'Build/InvokeAgentHarness.ps1',
            'Build/InvokePhaseWorkflow.ps1', 'Source/ThirdParty/WAMR/upstream/tests',
            'Source/ThirdParty/WAMR/upstream/test-tools', 'Tools/*.Tests')) {
        if (-not $ExcludeNames.Contains($RequiredExclusion)) {
            Throw-AvidScriptPluginReleaseError 'ASRE2013' 'profile_invalid' "source release exclusion is missing: $RequiredExclusion"
        }
    }

    $DependencyIds = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::Ordinal)
    foreach ($Dependency in @($Profile.dependencies)) {
        if (-not $DependencyIds.Add([string]$Dependency.id)) {
            Throw-AvidScriptPluginReleaseError 'ASRE2014' 'profile_invalid' 'source release dependency ids must be unique.'
        }
        $IdentityPath = Normalize-AvidScriptPluginReleaseRelativePath `
            ([string]$Dependency.identity_path) `
            'source release dependency identity'
        if (-not $IdentityPath.StartsWith(
                'Source/ThirdParty/',
                [System.StringComparison]::Ordinal)) {
            Throw-AvidScriptPluginReleaseError 'ASRE2015' 'profile_invalid' 'dependency identity must remain under Source/ThirdParty.'
        }
    }
}

if (-not (Test-Path -LiteralPath (Join-Path $PluginRoot 'AvidScript.uplugin') -PathType Leaf)) {
    Throw-AvidScriptPluginReleaseError 'ASRE2007' 'repository_invalid' "AvidScript.uplugin is missing: $PluginRoot"
}
$RepositoryRoot = (Invoke-AvidScriptPluginReleaseGit @('rev-parse', '--show-toplevel')).Stdout
if (-not [System.IO.Path]::GetFullPath($RepositoryRoot).Equals(
        $PluginRoot,
        [System.StringComparison]::OrdinalIgnoreCase)) {
    Throw-AvidScriptPluginReleaseError 'ASRE2008' 'repository_invalid' 'plugin root is not the Git repository root.'
}
$VerifiedCommit = (Invoke-AvidScriptPluginReleaseGit @('rev-parse', "$Commit`^{commit}")).Stdout
$VerifiedTree = (Invoke-AvidScriptPluginReleaseGit @('rev-parse', "$VerifiedCommit`^{tree}")).Stdout
$CommitTimeText = (Invoke-AvidScriptPluginReleaseGit @(
        'show', '-s', '--format=%cI', $VerifiedCommit)).Stdout
$CommittedAtUtc = ([DateTimeOffset]::Parse($CommitTimeText)).ToUniversalTime().ToString('o')
[System.IO.Directory]::CreateDirectory($OutputRoot) | Out-Null
if ($OutputRoot.Equals($PluginRoot, [System.StringComparison]::OrdinalIgnoreCase) -or
    (Test-AvidScriptPluginReleasePathUnderRoot $OutputRoot $PluginRoot)) {
    Throw-AvidScriptPluginReleaseError 'ASRE2009' 'path_invalid' 'release output root must remain outside the source repository.'
}
$PreparationRoot = Join-Path $OutputRoot (
    '.avidscript-release-input-' + [guid]::NewGuid().ToString('N'))
$ArchivePath = Join-Path $PreparationRoot 'tracked.zip'
$PayloadRoot = Join-Path $PreparationRoot 'payload'
$PayloadPluginRoot = Join-Path $PayloadRoot 'AvidScript'
try {
    [System.IO.Directory]::CreateDirectory($PreparationRoot) | Out-Null
    $ReleaseFilesRelativePath = 'Build/ReleaseEngineering/AvidScriptSourceRelease.files.json'
    $ReleaseFilesPath = Join-Path $PreparationRoot 'source-release-files.json'
    $ReleaseFilesText = (Invoke-AvidScriptPluginReleaseGit @(
            'show', "${VerifiedCommit}:$ReleaseFilesRelativePath")).Stdout
    [System.IO.File]::WriteAllText(
        $ReleaseFilesPath,
        $ReleaseFilesText + "`n",
        $script:AvidScriptPluginReleaseUtf8)
    $ReleaseFiles = Read-AvidScriptPluginReleaseJsonObject `
        $ReleaseFilesPath `
        'source release file profile'
    Assert-AvidScriptPluginReleaseJsonSchema `
        -JsonPath $ReleaseFilesPath `
        -SchemaPath (Join-Path $ScriptRoot 'ReleaseEngineering\AvidScriptSourceReleaseFiles.schema.json') `
        -Label 'source release file profile'
    Assert-AvidScriptSourceReleaseProfile $ReleaseFiles

    [System.IO.Directory]::CreateDirectory($PayloadPluginRoot) | Out-Null
    $ArchiveArguments = [System.Collections.Generic.List[string]]::new()
    foreach ($Argument in @('archive', '--format=zip', "--output=$ArchivePath", $VerifiedCommit, '--')) {
        $ArchiveArguments.Add($Argument)
    }
    foreach ($Included in @($ReleaseFiles.include)) {
        $ArchiveArguments.Add([string]$Included)
    }
    foreach ($Excluded in @($ReleaseFiles.exclude)) {
        $PathSpec = if ([string]$Excluded -eq 'Tools/*.Tests') {
            ':(exclude,glob)Tools/*.Tests/**'
        }
        else {
            ":(exclude)$Excluded"
        }
        $ArchiveArguments.Add($PathSpec)
    }
    Invoke-AvidScriptPluginReleaseGit @($ArchiveArguments) | Out-Null
    Expand-Archive -LiteralPath $ArchivePath -DestinationPath $PayloadPluginRoot
    $ReadmeSource = Join-Path $PayloadPluginRoot ([string]$ReleaseFiles.generated.readme_source)
    $ReadmeDestination = Join-Path $PayloadPluginRoot ([string]$ReleaseFiles.generated.readme_destination)
    if (-not (Test-Path -LiteralPath $ReadmeSource -PathType Leaf)) {
        Throw-AvidScriptPluginReleaseError 'ASRE2016' 'profile_invalid' 'public release README source is missing.'
    }
    [System.IO.File]::Copy($ReadmeSource, $ReadmeDestination, $false)

    $Descriptor = Read-AvidScriptPluginReleaseJsonObject `
        (Join-Path $PayloadPluginRoot 'AvidScript.uplugin') `
        'plugin descriptor'
    $DescriptorVersion = [string]$Descriptor.VersionName
    if ([string]::IsNullOrWhiteSpace($Version)) {
        $Version = $DescriptorVersion
    }
    if ($Version -cne $DescriptorVersion) {
        Throw-AvidScriptPluginReleaseError 'ASRE2017' 'version_invalid' "requested version differs from AvidScript.uplugin: $Version / $DescriptorVersion"
    }

    $Dependencies = [System.Collections.Generic.List[object]]::new()
    foreach ($Dependency in @($ReleaseFiles.dependencies | Sort-Object id -CaseSensitive)) {
        $IdentityRelative = Normalize-AvidScriptPluginReleaseRelativePath `
            ([string]$Dependency.identity_path) `
            'dependency identity path'
        $IdentityPath = Join-Path $PayloadPluginRoot $IdentityRelative
        if (-not (Test-Path -LiteralPath $IdentityPath -PathType Leaf)) {
            Throw-AvidScriptPluginReleaseError 'ASRE2018' 'dependency_missing' "dependency identity is missing from selected commit: $IdentityRelative"
        }
        $Dependencies.Add([pscustomobject][ordered]@{
                id = [string]$Dependency.id
                version = [string]$Dependency.version
                mode = 'external'
                identity_path = "AvidScript/$IdentityRelative"
                identity_sha256 = Get-AvidScriptPluginReleaseSha256 $IdentityPath
            })
    }

    Assert-AvidScriptPluginReleasePrivacy $PayloadPluginRoot
    Assert-AvidScriptPluginReleaseBinaryPrivacy $PayloadPluginRoot
    $Package = Publish-AvidScriptPluginReleasePackage `
        -PayloadSourceRoot $PayloadRoot `
        -OutputRoot $OutputRoot `
        -Version $Version `
        -Channel $Channel `
        -Commit $VerifiedCommit `
        -Tree $VerifiedTree `
        -CommittedAtUtc $CommittedAtUtc `
        -Dependencies @($Dependencies) `
        -PublisherVersion ([string]$ReleaseFiles.publisher_version) `
        -Targets @($ReleaseFiles.targets) `
        -Contracts $ReleaseFiles.contracts
    [pscustomobject][ordered]@{
        schema_version = 1
        result = 'passed'
        package_root = $Package.Root
        release_id = [string]$Package.Manifest.release_id
        version = [string]$Package.Manifest.version
        source_commit = $VerifiedCommit
        source_tree = $VerifiedTree
        manifest_sha256 = $Package.ManifestSha256
        inventory_sha256 = [string]$Package.Manifest.payload.inventory_sha256
        file_count = [int64]$Package.Manifest.payload.file_count
        total_bytes = [int64]$Package.Manifest.payload.total_bytes
    } | ConvertTo-Json -Depth 8
}
finally {
    if (Test-Path -LiteralPath $PreparationRoot) {
        if (-not (Test-AvidScriptPluginReleasePathUnderRoot $PreparationRoot $OutputRoot)) {
            throw 'ASRE2019 preparation root escaped the output root.'
        }
        Remove-Item -LiteralPath $PreparationRoot -Recurse -Force
    }
}
