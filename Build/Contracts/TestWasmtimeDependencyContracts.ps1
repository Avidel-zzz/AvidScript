[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$PluginRoot = [System.IO.Path]::GetFullPath((Split-Path -Parent (Split-Path -Parent $PSScriptRoot)))
$InstallerPath = Join-Path $PluginRoot 'Build/InstallWasmtimeDependency.ps1'
$ThirdPartyRoot = Join-Path $PluginRoot 'Source/ThirdParty/Wasmtime'
$LockPath = Join-Path $ThirdPartyRoot 'WasmtimeDependency.lock.json'
$SchemaPath = Join-Path $ThirdPartyRoot 'WasmtimeDependency.schema.json'

function Assert-True {
    param(
        [Parameter(Mandatory = $true)][bool]$Condition,
        [Parameter(Mandatory = $true)][string]$Message
    )

    if (-not $Condition) {
        throw "ASP54WT1000 $Message"
    }
}

function Assert-ThrowsCode {
    param(
        [Parameter(Mandatory = $true)][scriptblock]$Action,
        [Parameter(Mandatory = $true)][string]$ExpectedCode
    )

    try {
        & $Action
    }
    catch {
        Assert-True $_.Exception.Message.Contains($ExpectedCode) `
            "expected $ExpectedCode but received: $($_.Exception.Message)"
        return
    }
    throw "ASP54WT1001 expected failure containing $ExpectedCode"
}

function Copy-LockObject {
    param([Parameter(Mandatory = $true)]$Lock)

    return (($Lock | ConvertTo-Json -Depth 32) | ConvertFrom-Json)
}

function Add-ZipTextEntry {
    param(
        [Parameter(Mandatory = $true)]$Archive,
        [Parameter(Mandatory = $true)][string]$EntryName,
        [Parameter(Mandatory = $true)][string]$Text
    )

    $Entry = $Archive.CreateEntry($EntryName)
    $Stream = $Entry.Open()
    $Writer = [System.IO.StreamWriter]::new(
        $Stream,
        [System.Text.UTF8Encoding]::new($false))
    try {
        $Writer.Write($Text)
    }
    finally {
        $Writer.Dispose()
    }
}

function New-SyntheticArchive {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [string]$Missing = '',
        [switch]$Traversal,
        [switch]$AlternateDataStream
    )

    $Parent = Split-Path -Parent $Path
    New-Item -ItemType Directory -Force -Path $Parent | Out-Null
    $FileStream = [System.IO.File]::Open(
        $Path,
        [System.IO.FileMode]::Create,
        [System.IO.FileAccess]::ReadWrite,
        [System.IO.FileShare]::None)
    $Archive = [System.IO.Compression.ZipArchive]::new(
        $FileStream,
        [System.IO.Compression.ZipArchiveMode]::Create,
        $false)
    try {
        $Root = 'wasmtime-v45.0.0-x86_64-windows-c-api'
        if ($Missing -cne 'header') {
            Add-ZipTextEntry $Archive "$Root/min/include/wasmtime.h" 'synthetic wasmtime header'
        }
        Add-ZipTextEntry $Archive "$Root/min/include/wasm.h" 'synthetic wasm header'
        if ($Missing -cne 'dll') {
            Add-ZipTextEntry $Archive "$Root/min/lib/wasmtime.dll" 'synthetic dll'
        }
        if ($Missing -cne 'import') {
            Add-ZipTextEntry $Archive "$Root/min/lib/wasmtime.dll.lib" 'synthetic import library'
        }
        Add-ZipTextEntry $Archive "$Root/min/lib/wasmtime.lib" 'synthetic static library'
        Add-ZipTextEntry $Archive "$Root/LICENSE" 'synthetic official archive license'
        if ($Traversal) {
            Add-ZipTextEntry $Archive "$Root/../escape.txt" 'must not escape'
        }
        if ($AlternateDataStream) {
            Add-ZipTextEntry $Archive "$Root/LICENSE:untracked" 'must not create an ADS'
        }
    }
    finally {
        $Archive.Dispose()
        $FileStream.Dispose()
    }
}

function Set-ArchiveIdentity {
    param(
        [Parameter(Mandatory = $true)]$Lock,
        [Parameter(Mandatory = $true)][string]$ArchivePath
    )

    $Lock.archive.size_bytes = [int64](Get-Item -LiteralPath $ArchivePath).Length
    $Lock.archive.sha256 = (Get-FileHash -LiteralPath $ArchivePath -Algorithm SHA256).Hash.ToLowerInvariant()
}

function New-FixtureRepository {
    param(
        [Parameter(Mandatory = $true)][string]$FixtureRoot,
        [Parameter(Mandatory = $true)][string]$Name
    )

    $Root = Join-Path $FixtureRoot $Name
    New-Item -ItemType Directory -Force -Path (Join-Path $Root 'Source/ThirdParty/Wasmtime') | Out-Null
    return $Root
}

function New-TestJunction {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Target
    )

    New-Item -ItemType Junction -Path $Path -Target $Target | Out-Null
    Assert-True (
        ((Get-Item -LiteralPath $Path -Force).Attributes -band
            [System.IO.FileAttributes]::ReparsePoint) -ne 0) `
        "junction was not created: $Path"
}

function Add-TestAlternateDataStream {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$StreamName
    )

    [System.IO.File]::WriteAllText(
        $Path + ':' + $StreamName,
        'untracked alternate stream',
        [System.Text.UTF8Encoding]::new($false))
    $NamedStreams = @(
        Get-Item -LiteralPath $Path -Stream * |
            Where-Object { $_.Stream -cne ':$DATA' })
    Assert-True ($NamedStreams.Stream -contains $StreamName) `
        "alternate stream was not created: $StreamName"
}

function Install-Fixture {
    param(
        [Parameter(Mandatory = $true)][string]$RepositoryRoot,
        [Parameter(Mandatory = $true)][string]$CacheRoot,
        [Parameter(Mandatory = $true)]$Lock,
        [Parameter(Mandatory = $true)][string]$LockSha256
    )

    return Install-WasmtimeDependency `
        -RepositoryRoot $RepositoryRoot `
        -CacheRoot $CacheRoot `
        -Lock $Lock `
        -LockSha256 $LockSha256
}

function Get-CSharpCodeMask {
    param([Parameter(Mandatory = $true)][string]$Source)

    $NonCodePattern = '(?ms)//[^\r\n]*|/\*.*?\*/|(?<raw>"{3,}).*?\k<raw>|@"(?:""|[^"])*"|' +
        '"(?:\\.|[^"\\])*"|''(?:\\.|[^''\\])*'''
    return [regex]::Replace(
        $Source,
        $NonCodePattern,
        {
            param($Match)
            return [regex]::Replace($Match.Value, '[^\r\n]', ' ')
        })
}

function Get-LiteralDependencyNames {
    param(
        [Parameter(Mandatory = $true)][string]$Method,
        [Parameter(Mandatory = $true)][string]$ArgumentText
    )

    if ($Method -ceq 'Add') {
        $Literal = [regex]::Match(
            $ArgumentText,
            '^\s*"(?<name>[A-Za-z_][A-Za-z0-9_.]*)"\s*$')
        if (-not $Literal.Success) {
            return $null
        }
        return @($Literal.Groups['name'].Value)
    }

    $Range = [regex]::Match(
        $ArgumentText,
        '(?s)^\s*new\s*(?:string\s*)?\[\s*\]\s*\{\s*(?<items>.*?)\s*\}\s*$')
    if (-not $Range.Success) {
        return $null
    }
    $Items = $Range.Groups['items'].Value
    if ([string]::IsNullOrWhiteSpace($Items)) {
        return @()
    }
    $Parts = @($Items.Split(','))
    if ([string]::IsNullOrWhiteSpace($Parts[-1])) {
        $Parts = @($Parts | Select-Object -First ($Parts.Count - 1))
    }
    $Names = [System.Collections.Generic.List[string]]::new()
    foreach ($Part in $Parts) {
        $Literal = [regex]::Match(
            $Part,
            '^\s*"(?<name>[A-Za-z_][A-Za-z0-9_.]*)"\s*$')
        if (-not $Literal.Success) {
            return $null
        }
        $Names.Add($Literal.Groups['name'].Value)
    }
    return @($Names)
}

function Get-BuildDependencyAnalysis {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][ValidateSet('Public', 'Private')][string]$Visibility
    )

    $CodeMask = Get-CSharpCodeMask -Source $Source
    $InvocationPattern = '(?s)\b' + [regex]::Escape($Visibility) +
        'DependencyModuleNames\s*\.\s*(?<method>Add|AddRange)\s*\('
    $Names = [System.Collections.Generic.List[string]]::new()
    $UnresolvedCount = 0
    foreach ($Invocation in [regex]::Matches($CodeMask, $InvocationPattern)) {
        $OpenParenthesis = $Invocation.Index + $Invocation.Length - 1
        $Depth = 0
        $CloseParenthesis = -1
        for ($Index = $OpenParenthesis; $Index -lt $CodeMask.Length; ++$Index) {
            if ($CodeMask[$Index] -ceq '(') {
                ++$Depth
            }
            elseif ($CodeMask[$Index] -ceq ')') {
                --$Depth
                if ($Depth -eq 0) {
                    $CloseParenthesis = $Index
                    break
                }
            }
        }
        if ($CloseParenthesis -lt 0 -or
            -not $CodeMask.Substring($CloseParenthesis + 1) -match '^\s*;') {
            ++$UnresolvedCount
            continue
        }
        $ArgumentText = $Source.Substring(
            $OpenParenthesis + 1,
            $CloseParenthesis - $OpenParenthesis - 1)
        $LiteralNames = Get-LiteralDependencyNames `
            -Method $Invocation.Groups['method'].Value `
            -ArgumentText $ArgumentText
        if ($null -eq $LiteralNames) {
            ++$UnresolvedCount
            continue
        }
        foreach ($Name in @($LiteralNames)) {
            $Names.Add($Name)
        }
    }
    return [pscustomobject]@{
        Names = @($Names)
        UnresolvedCount = $UnresolvedCount
    }
}

Assert-True (Test-Path -LiteralPath $InstallerPath -PathType Leaf) 'installer is missing'
$ParserErrors = $null
$Tokens = $null
[void][System.Management.Automation.Language.Parser]::ParseFile(
    $InstallerPath,
    [ref]$Tokens,
    [ref]$ParserErrors)
Assert-True (@($ParserErrors).Count -eq 0) 'installer has PowerShell parser errors'

Assert-True (Test-Path -LiteralPath $LockPath -PathType Leaf) 'tracked lock is missing'
Assert-True (Test-Path -LiteralPath $SchemaPath -PathType Leaf) 'tracked schema is missing'
$LockRaw = Get-Content -LiteralPath $LockPath -Raw
Assert-True ($LockRaw | Test-Json -SchemaFile $SchemaPath) 'tracked lock does not satisfy its schema'
$TrackedLock = $LockRaw | ConvertFrom-Json
Assert-True ($TrackedLock.version -ceq 'v45.0.0') 'tracked version drifted'
Assert-True ($TrackedLock.release_url -ceq 'https://github.com/bytecodealliance/wasmtime/releases/tag/v45.0.0') 'release URL drifted'
Assert-True ($TrackedLock.archive.name -ceq 'wasmtime-v45.0.0-x86_64-windows-c-api.zip') 'archive name drifted'
Assert-True ($TrackedLock.archive.url -ceq 'https://github.com/bytecodealliance/wasmtime/releases/download/v45.0.0/wasmtime-v45.0.0-x86_64-windows-c-api.zip') 'archive URL drifted'
Assert-True ([int64]$TrackedLock.archive.size_bytes -eq 28820070) 'archive size drifted'
Assert-True ($TrackedLock.archive.sha256 -ceq 'd5ee516fc141576ccd6c43146aafee1074c3c26764cba73b3a97f599a3791f9c') 'archive SHA-256 drifted'
Assert-True ($TrackedLock.archive.root -ceq 'wasmtime-v45.0.0-x86_64-windows-c-api') 'archive root drifted'
Assert-True ($TrackedLock.install.relative_path -ceq 'Source/ThirdParty/Wasmtime/installed/Win64/v45.0.0') 'install path drifted'
$TrackedLicensePath = Join-Path $ThirdPartyRoot 'LICENSE.txt'
Assert-True (Test-Path -LiteralPath $TrackedLicensePath -PathType Leaf) 'tracked archive license is missing'
Assert-True (
    (Get-FileHash -LiteralPath $TrackedLicensePath -Algorithm SHA256).Hash.ToLowerInvariant() -ceq
        '268872b9816f90fd8e85db5a28d33f8150ebb8dd016653fb39ef1f94f2686bc5') `
    'tracked archive license drifted'

$ValidateOutput = & pwsh -NoProfile -File $InstallerPath -Mode ValidateLock -RepositoryRoot $PluginRoot
Assert-True ($LASTEXITCODE -eq 0) 'tracked lock CLI validation failed'
$ValidateResult = $ValidateOutput | ConvertFrom-Json
Assert-True ($ValidateResult.succeeded -eq $true) 'tracked lock CLI did not report success'

. $InstallerPath
$InstallerSource = Get-Content -LiteralPath $InstallerPath -Raw
Assert-True (
    $InstallerSource -match (
        '(?s)\$PublishPaths\s*=\s*Get-WasmtimeInstallPaths' +
        '.*?\[System\.IO\.Directory\]::Move\(\$CandidatePath,\s*\$PublishPaths\.InstallPath\)')) `
    'installer is missing the immediate pre-publication ancestor recheck'
Assert-True (
    $InstallerSource -match (
        '(?s)\[void\]\(Test-WasmtimeDependency.*?' +
        '\$RemovalPaths\s*=\s*Get-WasmtimeInstallPaths' +
        '.*?Remove-Item\s+-LiteralPath\s+\$InstallFullPath\s+-Recurse')) `
    'Remove is missing the immediate ancestor recheck'

$GitIgnore = Get-Content -LiteralPath (Join-Path $PluginRoot '.gitignore') -Raw
$KeepSourceIndex = $GitIgnore.IndexOf('!Source/', [System.StringComparison]::Ordinal)
$InstalledIgnoreIndex = $GitIgnore.IndexOf(
    'Source/ThirdParty/Wasmtime/installed/',
    [System.StringComparison]::Ordinal)
Assert-True ($InstalledIgnoreIndex -gt $KeepSourceIndex) 'installed ignore rule must follow !Source/'

$WasmtimeBuildPath = Join-Path $ThirdPartyRoot 'Wasmtime.Build.cs'
$WasmtimeBuild = Get-Content -LiteralPath $WasmtimeBuildPath -Raw
foreach ($RequiredToken in @(
    'ModuleType.External',
    'AVIDSCRIPT_WITH_WASMTIME=0',
    'AVIDSCRIPT_WITH_WASMTIME=1',
    'PublicIncludePaths.Add',
    'wasmtime.dll.lib',
    'PublicDelayLoadDLLs.Add("wasmtime.dll")',
    'RuntimeDependencies.Add')) {
    Assert-True $WasmtimeBuild.Contains($RequiredToken) "Wasmtime.Build.cs is missing $RequiredToken"
}
Assert-True (-not [regex]::IsMatch($WasmtimeBuild, '"wasmtime\.lib"')) 'Wasmtime.Build.cs must not link the static library'

$VmBuild = Get-Content -LiteralPath (Join-Path $PluginRoot 'Source/AvidScriptVM/AvidScriptVM.Build.cs') -Raw
$VmPrivateDependencyAnalysis = Get-BuildDependencyAnalysis -Source $VmBuild -Visibility Private
$VmPublicDependencyAnalysis = Get-BuildDependencyAnalysis -Source $VmBuild -Visibility Public
Assert-True ($VmPrivateDependencyAnalysis.UnresolvedCount -eq 0) `
    'AvidScriptVM has a non-literal private dependency declaration'
Assert-True ($VmPublicDependencyAnalysis.UnresolvedCount -eq 0) `
    'AvidScriptVM has a non-literal public dependency declaration'
Assert-True ($VmPrivateDependencyAnalysis.Names -contains 'Wasmtime') `
    'AvidScriptVM is missing its structured private Wasmtime dependency'
Assert-True ($VmPublicDependencyAnalysis.Names -notcontains 'Wasmtime') `
    'AvidScriptVM exposes Wasmtime as a public dependency'
$PublicDependencyMutation = $VmBuild.Replace(
    'PrivateDependencyModuleNames.AddRange',
    'PublicDependencyModuleNames.AddRange')
$MutatedPrivateAnalysis = Get-BuildDependencyAnalysis `
    -Source $PublicDependencyMutation `
    -Visibility Private
$MutatedPublicAnalysis = Get-BuildDependencyAnalysis `
    -Source $PublicDependencyMutation `
    -Visibility Public
Assert-True ($MutatedPrivateAnalysis.Names -notcontains 'Wasmtime') `
    'structured dependency parser did not notice removal from PrivateDependencyModuleNames'
Assert-True ($MutatedPublicAnalysis.Names -contains 'Wasmtime') `
    'structured dependency parser did not notice addition to PublicDependencyModuleNames'
$DecoyMutation = $VmBuild.Replace('"Wasmtime"', '"NotWasmtime"')
$DecoyMutation = $DecoyMutation.Replace(
    'PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;',
    'PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;' + "`n`t`t" +
        'string DependencyDecoy = "PrivateDependencyModuleNames.Add(\"Wasmtime\");";')
$DecoyPrivateAnalysis = Get-BuildDependencyAnalysis -Source $DecoyMutation -Visibility Private
Assert-True ($DecoyPrivateAnalysis.Names -notcontains 'Wasmtime') `
    'decoy string fabricated a private Wasmtime dependency'
$PublicVariableMutation = $VmBuild.Replace('"Wasmtime"', '"NotWasmtime"')
$PublicVariableMutation = $PublicVariableMutation.Replace(
    'PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;',
    'PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;' + "`n`t`t" +
        'string PublicBackendDependency = "Wasmtime";' + "`n`t`t" +
        'PublicDependencyModuleNames.Add(PublicBackendDependency);')
$PublicVariableAnalysis = Get-BuildDependencyAnalysis `
    -Source $PublicVariableMutation `
    -Visibility Public
Assert-True ($PublicVariableAnalysis.UnresolvedCount -eq 1) `
    'public variable dependency did not fail closed'
$ArchitectureSource = Get-Content -LiteralPath (
    Join-Path $PluginRoot 'Build/CheckAvidScriptArchitecture.ps1') -Raw
Assert-True $ArchitectureSource.Contains('Get-CSharpCodeMask') `
    'architecture checker does not mask C# comments and strings before locating calls'
Assert-True $ArchitectureSource.Contains('Get-BuildDependencyAnalysis') `
    'architecture checker does not structurally inspect module dependencies'
Assert-True $ArchitectureSource.Contains('$VmPrivateDependencyAnalysis') `
    'architecture checker does not inspect PrivateDependencyModuleNames'
Assert-True $ArchitectureSource.Contains('$VmPublicDependencyAnalysis') `
    'architecture checker does not inspect PublicDependencyModuleNames'
Assert-True $ArchitectureSource.Contains(
    '$VmPrivateDependencyAnalysis.Names -notcontains $RequiredVmBackendDependency') `
    'architecture checker does not reject a missing private Wasmtime dependency'
Assert-True $ArchitectureSource.Contains(
    '$VmPublicDependencyAnalysis.Names -contains ''Wasmtime''') `
    'architecture checker does not reject a public Wasmtime dependency'
Assert-True $ArchitectureSource.Contains('$VmPublicDependencyAnalysis.UnresolvedCount -gt 0') `
    'architecture checker does not fail closed on unresolved public dependencies'
$VmPublicText = (
    Get-ChildItem -LiteralPath (Join-Path $PluginRoot 'Source/AvidScriptVM/Public') -File -Recurse |
        ForEach-Object { Get-Content -LiteralPath $_.FullName -Raw }) -join "`n"
Assert-True (-not [regex]::IsMatch($VmPublicText, 'wasmtime\.h|AVIDSCRIPT_WITH_WASMTIME')) `
    'AvidScriptVM Public leaked Wasmtime details'

$FixtureRoot = Join-Path ([System.IO.Path]::GetTempPath()) (
    'AvidScriptP54WasmtimeContracts-' + [Guid]::NewGuid().ToString('N'))
try {
    New-Item -ItemType Directory -Force -Path $FixtureRoot | Out-Null
    $CacheRoot = Join-Path $FixtureRoot 'cache'
    $FixtureArchive = Get-WasmtimeArchiveCachePath -CacheRoot $CacheRoot -Lock $TrackedLock
    New-SyntheticArchive -Path $FixtureArchive
    $FixtureLock = Copy-LockObject $TrackedLock
    Set-ArchiveIdentity -Lock $FixtureLock -ArchivePath $FixtureArchive
    $FixtureLockPath = Join-Path $FixtureRoot 'fixture-lock.json'
    $FixtureLock | ConvertTo-Json -Depth 32 | Set-Content -LiteralPath $FixtureLockPath -Encoding utf8NoBOM
    $FixtureLockSha = Get-CanonicalFileSha256 -Path $FixtureLockPath

    $HashMismatchLock = Copy-LockObject $FixtureLock
    $HashMismatchLock.archive.sha256 = '0' * 64
    Assert-ThrowsCode {
        Assert-WasmtimeArchive -Path $FixtureArchive -Lock $HashMismatchLock
    } 'ASP54W1302'

    $SizeMismatchLock = Copy-LockObject $FixtureLock
    $SizeMismatchLock.archive.size_bytes = [int64]$SizeMismatchLock.archive.size_bytes + 1
    Assert-ThrowsCode {
        Assert-WasmtimeArchive -Path $FixtureArchive -Lock $SizeMismatchLock
    } 'ASP54W1301'

    foreach ($MissingCase in @('dll', 'import', 'header')) {
        $MissingCache = Join-Path $FixtureRoot "cache-missing-$MissingCase"
        $MissingArchive = Get-WasmtimeArchiveCachePath -CacheRoot $MissingCache -Lock $FixtureLock
        New-SyntheticArchive -Path $MissingArchive -Missing $MissingCase
        $MissingLock = Copy-LockObject $FixtureLock
        Set-ArchiveIdentity -Lock $MissingLock -ArchivePath $MissingArchive
        $MissingRepository = New-FixtureRepository $FixtureRoot "missing-$MissingCase"
        Assert-ThrowsCode {
            Install-Fixture `
                -RepositoryRoot $MissingRepository `
                -CacheRoot $MissingCache `
                -Lock $MissingLock `
                -LockSha256 $FixtureLockSha
        } 'ASP54W1404'
    }

    $TraversalCache = Join-Path $FixtureRoot 'cache-traversal'
    $TraversalArchive = Get-WasmtimeArchiveCachePath -CacheRoot $TraversalCache -Lock $FixtureLock
    New-SyntheticArchive -Path $TraversalArchive -Traversal
    $TraversalLock = Copy-LockObject $FixtureLock
    Set-ArchiveIdentity -Lock $TraversalLock -ArchivePath $TraversalArchive
    $TraversalRepository = New-FixtureRepository $FixtureRoot 'traversal'
    Assert-ThrowsCode {
        Install-Fixture `
            -RepositoryRoot $TraversalRepository `
            -CacheRoot $TraversalCache `
            -Lock $TraversalLock `
            -LockSha256 $FixtureLockSha
    } 'ASP54W1401'

    $AdsCache = Join-Path $FixtureRoot 'cache-ads'
    $AdsArchive = Get-WasmtimeArchiveCachePath -CacheRoot $AdsCache -Lock $FixtureLock
    New-SyntheticArchive -Path $AdsArchive -AlternateDataStream
    $AdsLock = Copy-LockObject $FixtureLock
    Set-ArchiveIdentity -Lock $AdsLock -ArchivePath $AdsArchive
    $AdsRepository = New-FixtureRepository $FixtureRoot 'ads'
    Assert-ThrowsCode {
        Install-Fixture `
            -RepositoryRoot $AdsRepository `
            -CacheRoot $AdsCache `
            -Lock $AdsLock `
            -LockSha256 $FixtureLockSha
    } 'ASP54W1401'

    $HappyRepository = New-FixtureRepository $FixtureRoot 'happy'
    $InstallResult = Install-Fixture `
        -RepositoryRoot $HappyRepository `
        -CacheRoot $CacheRoot `
        -Lock $FixtureLock `
        -LockSha256 $FixtureLockSha
    Assert-True ($InstallResult.status -ceq 'installed') 'happy install did not report installed'
    $InstallPath = Join-Path $HappyRepository $FixtureLock.install.relative_path
    Assert-True (Test-Path -LiteralPath (Join-Path $InstallPath 'include/wasmtime.h') -PathType Leaf) 'installed header is missing'
    Assert-True (Test-Path -LiteralPath (Join-Path $InstallPath 'lib/wasmtime.dll') -PathType Leaf) 'installed DLL is missing'
    Assert-True (Test-Path -LiteralPath (Join-Path $InstallPath 'lib/wasmtime.dll.lib') -PathType Leaf) 'installed import library is missing'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $InstallPath 'lib/wasmtime.lib'))) 'static library was installed'
    Assert-True (Test-Path -LiteralPath (Join-Path $InstallPath 'LICENSE') -PathType Leaf) 'installed license is missing'

    $VerifyResult = Test-WasmtimeDependency `
        -RepositoryRoot $HappyRepository `
        -Lock $FixtureLock `
        -LockSha256 $FixtureLockSha
    Assert-True ($VerifyResult.status -ceq 'verified') 'happy verify did not report verified'
    $MarkerPath = Join-Path $InstallPath '.avidscript-wasmtime-managed.json'
    $Marker = Get-Content -LiteralPath $MarkerPath -Raw | ConvertFrom-Json
    Assert-True ($Marker.schema_version -eq 1) 'marker schema version drifted'
    Assert-True ($Marker.installed_content_sha256 -cmatch '^[0-9a-f]{64}$') 'marker content digest is invalid'
    Assert-True ([int64]$Marker.installed_file_count -eq 5) 'marker file count is unexpected'
    $IdempotentResult = Install-Fixture `
        -RepositoryRoot $HappyRepository `
        -CacheRoot $CacheRoot `
        -Lock $FixtureLock `
        -LockSha256 $FixtureLockSha
    Assert-True ($IdempotentResult.status -ceq 'already_verified') 'Install is not idempotent'

    $TamperRepository = New-FixtureRepository $FixtureRoot 'tamper'
    Install-Fixture $TamperRepository $CacheRoot $FixtureLock $FixtureLockSha | Out-Null
    Add-Content -LiteralPath (
        Join-Path $TamperRepository "$($FixtureLock.install.relative_path)/lib/wasmtime.dll") -Value 'tamper'
    Assert-ThrowsCode {
        Test-WasmtimeDependency $TamperRepository $FixtureLock $FixtureLockSha
    } 'ASP54W1605'

    $ExtraRepository = New-FixtureRepository $FixtureRoot 'extra'
    Install-Fixture $ExtraRepository $CacheRoot $FixtureLock $FixtureLockSha | Out-Null
    'extra' | Set-Content -LiteralPath (
        Join-Path $ExtraRepository "$($FixtureLock.install.relative_path)/extra.txt") -Encoding utf8NoBOM
    Assert-ThrowsCode {
        Test-WasmtimeDependency $ExtraRepository $FixtureLock $FixtureLockSha
    } 'ASP54W1605'

    $MarkerRepository = New-FixtureRepository $FixtureRoot 'marker'
    Install-Fixture $MarkerRepository $CacheRoot $FixtureLock $FixtureLockSha | Out-Null
    $DriftedMarkerPath = Join-Path $MarkerRepository (
        "$($FixtureLock.install.relative_path)/.avidscript-wasmtime-managed.json")
    $DriftedMarker = Get-Content -LiteralPath $DriftedMarkerPath -Raw | ConvertFrom-Json
    $DriftedMarker.lock_canonical_sha256 = 'f' * 64
    $DriftedMarker | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $DriftedMarkerPath -Encoding utf8NoBOM
    Assert-ThrowsCode {
        Test-WasmtimeDependency $MarkerRepository $FixtureLock $FixtureLockSha
    } 'ASP54W1604'

    $ContentAdsRepository = New-FixtureRepository $FixtureRoot 'content-ads'
    Install-Fixture $ContentAdsRepository $CacheRoot $FixtureLock $FixtureLockSha | Out-Null
    $ContentAdsInstallPath = Join-Path $ContentAdsRepository $FixtureLock.install.relative_path
    $ContentAdsDllPath = Join-Path $ContentAdsInstallPath 'lib/wasmtime.dll'
    Add-TestAlternateDataStream -Path $ContentAdsDllPath -StreamName 'avidscript-tamper'
    Assert-ThrowsCode {
        Test-WasmtimeDependency $ContentAdsRepository $FixtureLock $FixtureLockSha
    } 'ASP54W1606'
    Assert-ThrowsCode {
        Remove-WasmtimeDependency $ContentAdsRepository $FixtureLock $FixtureLockSha
    } 'ASP54W1606'
    Assert-True (Test-Path -LiteralPath $ContentAdsDllPath -PathType Leaf) `
        'Remove deleted managed content with an alternate stream'

    $MarkerAdsRepository = New-FixtureRepository $FixtureRoot 'marker-ads'
    Install-Fixture $MarkerAdsRepository $CacheRoot $FixtureLock $FixtureLockSha | Out-Null
    $MarkerAdsPath = Join-Path $MarkerAdsRepository (
        "$($FixtureLock.install.relative_path)/.avidscript-wasmtime-managed.json")
    Add-TestAlternateDataStream -Path $MarkerAdsPath -StreamName 'avidscript-marker-tamper'
    Assert-ThrowsCode {
        Test-WasmtimeDependency $MarkerAdsRepository $FixtureLock $FixtureLockSha
    } 'ASP54W1606'

    $UnmanagedRepository = New-FixtureRepository $FixtureRoot 'unmanaged'
    New-Item -ItemType Directory -Force -Path (
        Join-Path $UnmanagedRepository $FixtureLock.install.relative_path) | Out-Null
    Assert-ThrowsCode {
        Remove-WasmtimeDependency $UnmanagedRepository $FixtureLock $FixtureLockSha
    } 'ASP54W1602'

    $DriftRemoveRepository = New-FixtureRepository $FixtureRoot 'drift-remove'
    Install-Fixture $DriftRemoveRepository $CacheRoot $FixtureLock $FixtureLockSha | Out-Null
    Add-Content -LiteralPath (
        Join-Path $DriftRemoveRepository "$($FixtureLock.install.relative_path)/LICENSE") -Value 'drift'
    Assert-ThrowsCode {
        Remove-WasmtimeDependency $DriftRemoveRepository $FixtureLock $FixtureLockSha
    } 'ASP54W1605'

    $AncestorRepository = New-FixtureRepository $FixtureRoot 'ancestor-reparse'
    Install-Fixture $AncestorRepository $CacheRoot $FixtureLock $FixtureLockSha | Out-Null
    $AncestorThirdPartyPath = Join-Path $AncestorRepository 'Source/ThirdParty'
    $ExternalThirdPartyPath = Join-Path $FixtureRoot 'external-third-party'
    Move-Item -LiteralPath $AncestorThirdPartyPath -Destination $ExternalThirdPartyPath
    New-TestJunction -Path $AncestorThirdPartyPath -Target $ExternalThirdPartyPath
    $ExternalSentinelPath = Join-Path $ExternalThirdPartyPath 'outside-repository-sentinel.txt'
    'must survive rejected remove' |
        Set-Content -LiteralPath $ExternalSentinelPath -Encoding utf8NoBOM
    Assert-ThrowsCode {
        Get-WasmtimeInstallPaths -RepositoryRoot $AncestorRepository -Lock $FixtureLock
    } 'ASP54W1104'
    Assert-ThrowsCode {
        Remove-WasmtimeDependency $AncestorRepository $FixtureLock $FixtureLockSha
    } 'ASP54W1104'
    Assert-True (Test-Path -LiteralPath $ExternalSentinelPath -PathType Leaf) `
        'ancestor-junction rejection modified the external target'
    Assert-True (Test-Path -LiteralPath (
        Join-Path $ExternalThirdPartyPath 'Wasmtime/installed/Win64/v45.0.0') -PathType Container) `
        'ancestor-junction rejection removed the external managed tree'

    $RemoveRepository = New-FixtureRepository $FixtureRoot 'remove'
    Install-Fixture $RemoveRepository $CacheRoot $FixtureLock $FixtureLockSha | Out-Null
    $InstalledRoot = Join-Path $RemoveRepository 'Source/ThirdParty/Wasmtime/installed'
    'boundary sentinel' | Set-Content -LiteralPath (Join-Path $InstalledRoot 'sentinel.txt') -Encoding utf8NoBOM
    $RemoveResult = Remove-WasmtimeDependency $RemoveRepository $FixtureLock $FixtureLockSha
    Assert-True ($RemoveResult.status -ceq 'removed') 'managed remove did not report removed'
    Assert-True (-not (Test-Path -LiteralPath (
        Join-Path $RemoveRepository $FixtureLock.install.relative_path))) 'managed version directory remains'
    Assert-True (Test-Path -LiteralPath (Join-Path $InstalledRoot 'sentinel.txt') -PathType Leaf) `
        'Remove crossed the installed boundary'
}
finally {
    if (Test-Path -LiteralPath $FixtureRoot) {
        Remove-Item -LiteralPath $FixtureRoot -Recurse -Force
    }
}

Write-Output (
    'Wasmtime dependency contracts passed: ' +
    'tracked_lock=1 schema=1 license=1 cli_validate=1 parser=1 archive_identity=2 traversal=1 ads=1 ' +
    'missing_layout=3 install=1 verify=1 idempotent=1 content_tamper=1 extra_file=1 ' +
    'marker_drift=1 installed_ads=2 unmanaged_remove=1 drift_remove=1 ancestor_reparse=2 managed_remove=1 ' +
    'gitignore=1 ubt=1 private_dependency_structure=2 dependency_decoys=2 public_boundary=1')
