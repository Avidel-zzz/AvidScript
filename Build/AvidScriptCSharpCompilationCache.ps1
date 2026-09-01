Set-StrictMode -Version Latest

function Fail-AvidScriptCSharpCompilationCache {
    param(
        [Parameter(Mandatory = $true)][string]$Code,
        [Parameter(Mandatory = $true)][string]$Message
    )

    $Exception = [System.InvalidOperationException]::new("${Code}: $Message")
    $Exception.Data["AvidScriptCode"] = $Code
    throw $Exception
}

function Assert-AvidScriptCSharpCompilationCache {
    param(
        [Parameter(Mandatory = $true)][bool]$Condition,
        [Parameter(Mandatory = $true)][string]$Code,
        [Parameter(Mandatory = $true)][string]$Message
    )

    if (-not $Condition) {
        Fail-AvidScriptCSharpCompilationCache -Code $Code -Message $Message
    }
}

function Test-AvidScriptCompilationCacheSha256 {
    param([AllowEmptyString()][string]$Value)

    return $Value -cmatch '^[0-9a-f]{64}$'
}

function Get-AvidScriptCompilationCachePackageIdentity {
    param([AllowNull()][object]$Package)

    if ($null -eq $Package) {
        return $null
    }
    return [ordered]@{
        package_name = [string]$Package.PackageName
        package_hash = [string]$Package.PackageHash
        manifest_sha256 = [string]$Package.ManifestSha256
        descriptor_sha256 = [string]$Package.DescriptorSha256
        reference_source_sha256 = [string]$Package.ReferenceSourceSha256
    }
}

function Get-AvidScriptCompilationCacheToolchainFingerprint {
    param([Parameter(Mandatory = $true)][string]$PluginRoot)

    $PluginRootFullPath = Get-AvidScriptBindingFullPath $PluginRoot
    $RequiredFiles = @(
        (Join-Path $PluginRootFullPath "global.json"),
        (Join-Path $PluginRootFullPath "Build\BuildCSharpActorLifecycle.ps1"),
        (Join-Path $PluginRootFullPath "Build\InvokeCSharpGuestCompiler.ps1"),
        (Join-Path $PluginRootFullPath "Build\AvidScriptCSharpCompilerWorker.ps1"),
        (Join-Path $PluginRootFullPath "Build\AvidScriptCSharpCompilationCache.ps1"))
    $Files = foreach ($RequiredFile in $RequiredFiles) {
        Assert-AvidScriptCSharpCompilationCache `
            -Condition (Test-Path -LiteralPath $RequiredFile -PathType Leaf) `
            -Code "ASBI4601" `
            -Message "Required compilation-cache toolchain file is missing: $RequiredFile"
        Get-Item -LiteralPath $RequiredFile
    }
    foreach ($SourceRootName in @(
        "AvidScript.GuestIr",
        "AvidScript.CSharpGuest",
        "AvidScript.WasmBackend",
        "AvidScript.CSharpCompilerWorker")) {
        $SourceRoot = Join-Path $PluginRootFullPath ("Tools\" + $SourceRootName)
        Assert-AvidScriptCSharpCompilationCache `
            -Condition (Test-Path -LiteralPath $SourceRoot -PathType Container) `
            -Code "ASBI4601" `
            -Message "Compilation-cache toolchain source root is missing: $SourceRoot"
        $Files += @(Get-ChildItem -LiteralPath $SourceRoot -Recurse -File | Where-Object {
            ($_.Extension -eq ".cs" -or $_.Extension -eq ".csproj") -and
            $_.FullName -notmatch '[\\/](?:bin|obj)[\\/]'
        })
    }

    $PluginPrefix = $PluginRootFullPath.TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar) +
        [System.IO.Path]::DirectorySeparatorChar
    $Records = @($Files | ForEach-Object {
        $FullPath = Get-AvidScriptBindingFullPath $_.FullName
        Assert-AvidScriptCSharpCompilationCache `
            -Condition (Test-AvidScriptBindingPathContained `
                -RootPath $PluginRootFullPath `
                -CandidatePath $FullPath) `
            -Code "ASBI4601" `
            -Message "Compilation-cache toolchain file escapes the plugin root: $FullPath"
        [ordered]@{
            path = $FullPath.Substring($PluginPrefix.Length).Replace("\", "/")
            length = [long]$_.Length
            sha256 = Get-AvidScriptBindingSha256Hex $FullPath
        }
    } | Sort-Object -Property path)
    return [pscustomobject]@{
        Sha256 = Get-AvidScriptUtf8JsonSha256 ([ordered]@{
            schema_version = 1
            files = @($Records)
        })
        Files = @($Records)
    }
}

function Get-AvidScriptCSharpCompilationCacheContext {
    param(
        [Parameter(Mandatory = $true)][string]$PluginRoot,
        [Parameter(Mandatory = $true)][string]$ProjectRoot,
        [string]$CacheRoot = "",
        [Parameter(Mandatory = $true)][string]$SemanticArtifactPath,
        [Parameter(Mandatory = $true)][string]$FrontendArtifactSha256,
        [Parameter(Mandatory = $true)][string]$GuestCompilerPath,
        [Parameter(Mandatory = $true)][string]$DotNetPath,
        [Parameter(Mandatory = $true)][string]$ModuleId,
        [Parameter(Mandatory = $true)][string]$Configuration,
        [Parameter(Mandatory = $true)][string]$DataLaneFusion,
        [Parameter(Mandatory = $true)][string]$DebugInstrumentation,
        [AllowNull()][object]$AuthorizationPackage,
        [AllowNull()][object]$RuntimePackage
    )

    $ProjectRootFullPath = Get-AvidScriptBindingFullPath $ProjectRoot
    $CacheNamespace = Join-Path $ProjectRootFullPath "Saved\AvidScript"
    if ([string]::IsNullOrWhiteSpace($CacheRoot)) {
        $CacheRoot = Join-Path $CacheNamespace "CSharpCompilationCache\v1"
    }
    $CacheRootFullPath = Get-AvidScriptBindingFullPath $CacheRoot
    Assert-AvidScriptCSharpCompilationCache `
        -Condition (Test-AvidScriptBindingPathContained `
            -RootPath $CacheNamespace `
            -CandidatePath $CacheRootFullPath) `
        -Code "ASBI4601" `
        -Message "Compilation cache root must remain inside project Saved/AvidScript."
    Assert-AvidScriptCSharpCompilationCache `
        -Condition (Test-AvidScriptSemanticCachePathWithoutReparsePoint -Path $CacheRootFullPath) `
        -Code "ASBI4601" `
        -Message "Compilation cache root must not traverse a reparse point."
    Assert-AvidScriptCSharpCompilationCache `
        -Condition ((Test-Path -LiteralPath $SemanticArtifactPath -PathType Leaf) -and
            (Test-AvidScriptCompilationCacheSha256 $FrontendArtifactSha256)) `
        -Code "ASBI4601" `
        -Message "Compilation cache requires valid Frontend and Semantic artifacts."

    $SemanticSha256 = Get-AvidScriptBindingSha256Hex $SemanticArtifactPath
    Assert-AvidScriptCSharpCompilationCache `
        -Condition ((Test-Path -LiteralPath $GuestCompilerPath -PathType Leaf) -and
            (Test-Path -LiteralPath $DotNetPath -PathType Leaf)) `
        -Code "ASBI4601" `
        -Message "Compilation cache requires the resolved Guest compiler and .NET executable."
    $ToolchainSources = Get-AvidScriptCompilationCacheToolchainFingerprint -PluginRoot $PluginRoot
    $ToolchainFingerprint = Get-AvidScriptUtf8JsonSha256 ([ordered]@{
        schema_version = 1
        sources_sha256 = $ToolchainSources.Sha256
        guest_compiler_sha256 = Get-AvidScriptBindingSha256Hex $GuestCompilerPath
        dotnet_sha256 = Get-AvidScriptBindingSha256Hex $DotNetPath
    })
    $Identity = [ordered]@{
        schema_version = 1
        module_id = $ModuleId
        configuration = $Configuration
        data_lane_fusion = $DataLaneFusion
        debug_instrumentation = $DebugInstrumentation
        frontend_artifact_sha256 = $FrontendArtifactSha256
        semantic_artifact_sha256 = $SemanticSha256
        toolchain_fingerprint = $ToolchainFingerprint
        authorization = Get-AvidScriptCompilationCachePackageIdentity $AuthorizationPackage
        runtime = Get-AvidScriptCompilationCachePackageIdentity $RuntimePackage
    }
    $CacheKey = Get-AvidScriptUtf8JsonSha256 $Identity
    $EntryDirectory = Join-Path $CacheRootFullPath $CacheKey
    return [pscustomobject]@{
        CacheRoot = $CacheRootFullPath
        CacheKey = $CacheKey
        ToolchainFingerprint = $ToolchainFingerprint
        SemanticSha256 = $SemanticSha256
        ModuleId = $ModuleId
        EntryDirectory = $EntryDirectory
        EntryReportPath = Join-Path $EntryDirectory "entry.json"
    }
}

function Get-AvidScriptCompilationCacheArtifactLayout {
    return [ordered]@{
        guest_ir = "guest_ir.json"
        debug_map = "debug_map.json"
        state_schema = "state_schema.json"
        wasm = "module.wasm"
        wasm_inspection = "wasm_inspection.json"
    }
}

function Assert-AvidScriptCompilationCacheArtifactPaths {
    param(
        [Parameter(Mandatory = $true)][object]$Context,
        [Parameter(Mandatory = $true)][hashtable]$Artifacts,
        [Parameter(Mandatory = $true)][string]$Operation
    )

    $ObservedPaths = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    foreach ($Property in (Get-AvidScriptCompilationCacheArtifactLayout).GetEnumerator()) {
        Assert-AvidScriptCSharpCompilationCache `
            -Condition $Artifacts.ContainsKey($Property.Key) `
            -Code "ASBI4603" `
            -Message "Compilation cache $Operation path is missing: $($Property.Key)"
        $ArtifactPath = Get-AvidScriptBindingFullPath ([string]$Artifacts[$Property.Key])
        Assert-AvidScriptCSharpCompilationCache `
            -Condition ((Test-AvidScriptSemanticCachePathWithoutReparsePoint -Path $ArtifactPath) -and
                -not (Test-AvidScriptSemanticCachePathEqualOrContained `
                    -RootPath $Context.CacheRoot `
                    -CandidatePath $ArtifactPath) -and
                $ObservedPaths.Add($ArtifactPath)) `
            -Code "ASBI4603" `
            -Message "Compilation cache $Operation paths must be unique, reparse-point free, and outside the cache root: $ArtifactPath"
    }
}

function Read-AvidScriptCompilationCacheEntry {
    param([Parameter(Mandatory = $true)][object]$Context)

    Assert-AvidScriptCSharpCompilationCache `
        -Condition ((Test-Path -LiteralPath $Context.EntryDirectory -PathType Container) -and
            (Test-AvidScriptSemanticCachePathWithoutReparsePoint -Path $Context.EntryDirectory) -and
            (Test-Path -LiteralPath $Context.EntryReportPath -PathType Leaf)) `
        -Code "ASBI4602" `
        -Message "Compilation cache entry layout is invalid."
    try {
        $Report = Get-Content -Raw -LiteralPath $Context.EntryReportPath | ConvertFrom-Json
    }
    catch {
        Fail-AvidScriptCSharpCompilationCache `
            -Code "ASBI4602" `
            -Message "Compilation cache entry report is invalid: $($_.Exception.Message)"
    }
    Assert-AvidScriptCSharpCompilationCache `
        -Condition ([int]$Report.schema_version -eq 1 -and
            [string]$Report.key -ceq [string]$Context.CacheKey -and
            [string]$Report.toolchain_fingerprint -ceq [string]$Context.ToolchainFingerprint -and
            [string]$Report.semantic_sha256 -ceq [string]$Context.SemanticSha256 -and
            [string]$Report.module_id -ceq [string]$Context.ModuleId) `
        -Code "ASBI4602" `
        -Message "Compilation cache entry identity differs from the current build."

    $ArtifactPaths = [ordered]@{}
    foreach ($Property in (Get-AvidScriptCompilationCacheArtifactLayout).GetEnumerator()) {
        $Artifact = $Report.artifacts.PSObject.Properties[$Property.Key].Value
        $ArtifactPath = Join-Path $Context.EntryDirectory ([string]$Artifact.file)
        Assert-AvidScriptCSharpCompilationCache `
            -Condition ([string]$Artifact.file -ceq [string]$Property.Value -and
                (Test-AvidScriptCompilationCacheSha256 ([string]$Artifact.sha256)) -and
                (Test-AvidScriptBindingPathContained `
                    -RootPath $Context.EntryDirectory `
                    -CandidatePath $ArtifactPath) -and
                (Test-AvidScriptSemanticCachePathWithoutReparsePoint -Path $ArtifactPath) -and
                (Test-Path -LiteralPath $ArtifactPath -PathType Leaf) -and
                (Get-AvidScriptBindingSha256Hex $ArtifactPath) -ceq [string]$Artifact.sha256) `
            -Code "ASBI4602" `
            -Message "Compilation cache artifact is missing, relocated, or corrupt: $($Property.Key)"
        $ArtifactPaths[$Property.Key] = $ArtifactPath
    }
    return [pscustomobject]@{
        Report = $Report
        ReportSha256 = Get-AvidScriptBindingSha256Hex $Context.EntryReportPath
        ArtifactPaths = $ArtifactPaths
    }
}

function Move-AvidScriptCompilationCacheCorruptEntry {
    param([Parameter(Mandatory = $true)][object]$Context)

    if (-not (Test-Path -LiteralPath $Context.EntryDirectory)) {
        return
    }
    $CorruptRoot = Join-Path $Context.CacheRoot ".corrupt"
    New-Item -ItemType Directory -Force -Path $CorruptRoot | Out-Null
    $Destination = Join-Path $CorruptRoot (
        $Context.CacheKey + "." + [Guid]::NewGuid().ToString("N"))
    Move-Item -LiteralPath $Context.EntryDirectory -Destination $Destination
}

function Copy-AvidScriptCompilationCacheArtifactAtomic {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination
    )

    $Parent = Split-Path -Parent $Destination
    New-Item -ItemType Directory -Force -Path $Parent | Out-Null
    $Temporary = $Destination + ".tmp." + [Guid]::NewGuid().ToString("N")
    try {
        Copy-Item -LiteralPath $Source -Destination $Temporary -Force
        Move-Item -LiteralPath $Temporary -Destination $Destination -Force
    }
    finally {
        if (Test-Path -LiteralPath $Temporary) {
            Remove-Item -LiteralPath $Temporary -Force
        }
    }
}

function Import-AvidScriptCSharpCompilationCacheEntry {
    param(
        [Parameter(Mandatory = $true)][object]$Context,
        [Parameter(Mandatory = $true)][hashtable]$Destinations
    )

    Assert-AvidScriptCompilationCacheArtifactPaths `
        -Context $Context `
        -Artifacts $Destinations `
        -Operation "import destination"

    if (-not (Test-Path -LiteralPath $Context.EntryDirectory)) {
        return [pscustomobject]@{
            Status = "miss"
            EntryReportPath = ""
            EntryReportSha256 = ""
            DiagnosticCode = ""
            DiagnosticMessage = ""
        }
    }
    try {
        $Entry = Read-AvidScriptCompilationCacheEntry -Context $Context
    }
    catch {
        $Message = $_.Exception.Message
        Move-AvidScriptCompilationCacheCorruptEntry -Context $Context
        return [pscustomobject]@{
            Status = "rejected"
            EntryReportPath = ""
            EntryReportSha256 = ""
            DiagnosticCode = "ASBI4602"
            DiagnosticMessage = $Message
        }
    }

    foreach ($Property in (Get-AvidScriptCompilationCacheArtifactLayout).GetEnumerator()) {
        Assert-AvidScriptCSharpCompilationCache `
            -Condition $Destinations.ContainsKey($Property.Key) `
            -Code "ASBI4603" `
            -Message "Compilation cache import destination is missing: $($Property.Key)"
        Copy-AvidScriptCompilationCacheArtifactAtomic `
            -Source $Entry.ArtifactPaths[$Property.Key] `
            -Destination $Destinations[$Property.Key]
    }
    return [pscustomobject]@{
        Status = "hit"
        EntryReportPath = $Context.EntryReportPath
        EntryReportSha256 = $Entry.ReportSha256
        DiagnosticCode = ""
        DiagnosticMessage = ""
    }
}

function Enter-AvidScriptCompilationCacheLock {
    param([Parameter(Mandatory = $true)][object]$Context)

    $LockRoot = Join-Path $Context.CacheRoot ".locks"
    New-Item -ItemType Directory -Force -Path $LockRoot | Out-Null
    $LockPath = Join-Path $LockRoot ($Context.CacheKey + ".lock")
    $Deadline = [DateTime]::UtcNow.AddSeconds(5)
    while ($true) {
        try {
            New-Item -ItemType Directory -Path $LockPath -ErrorAction Stop | Out-Null
            return $LockPath
        }
        catch [System.IO.IOException] {
            if ([DateTime]::UtcNow -ge $Deadline) {
                Fail-AvidScriptCSharpCompilationCache `
                    -Code "ASBI4604" `
                    -Message "Timed out acquiring the compilation cache key lock."
            }
            Start-Sleep -Milliseconds 50
        }
    }
}

function Publish-AvidScriptCSharpCompilationCacheEntry {
    param(
        [Parameter(Mandatory = $true)][object]$Context,
        [Parameter(Mandatory = $true)][hashtable]$Artifacts
    )

    Assert-AvidScriptCompilationCacheArtifactPaths `
        -Context $Context `
        -Artifacts $Artifacts `
        -Operation "publication source"

    New-Item -ItemType Directory -Force -Path $Context.CacheRoot | Out-Null
    $LockPath = Enter-AvidScriptCompilationCacheLock -Context $Context
    $StagingRoot = Join-Path $Context.CacheRoot ".staging"
    $Staging = Join-Path $StagingRoot (
        $Context.CacheKey + "." + [Guid]::NewGuid().ToString("N"))
    try {
        if (Test-Path -LiteralPath $Context.EntryDirectory) {
            try {
                $Existing = Read-AvidScriptCompilationCacheEntry -Context $Context
                return [pscustomobject]@{
                    Published = $false
                    EntryReportPath = $Context.EntryReportPath
                    EntryReportSha256 = $Existing.ReportSha256
                }
            }
            catch {
                Move-AvidScriptCompilationCacheCorruptEntry -Context $Context
            }
        }

        New-Item -ItemType Directory -Force -Path $Staging | Out-Null
        $ArtifactReport = [ordered]@{}
        foreach ($Property in (Get-AvidScriptCompilationCacheArtifactLayout).GetEnumerator()) {
            Assert-AvidScriptCSharpCompilationCache `
                -Condition ($Artifacts.ContainsKey($Property.Key) -and
                    (Test-Path -LiteralPath $Artifacts[$Property.Key] -PathType Leaf)) `
                -Code "ASBI4603" `
                -Message "Compilation cache publication artifact is missing: $($Property.Key)"
            $Destination = Join-Path $Staging $Property.Value
            Copy-Item -LiteralPath $Artifacts[$Property.Key] -Destination $Destination -Force
            $ArtifactReport[$Property.Key] = [ordered]@{
                file = $Property.Value
                sha256 = Get-AvidScriptBindingSha256Hex $Destination
            }
        }
        $Report = [ordered]@{
            schema_version = 1
            key = $Context.CacheKey
            toolchain_fingerprint = $Context.ToolchainFingerprint
            semantic_sha256 = $Context.SemanticSha256
            module_id = $Context.ModuleId
            artifacts = $ArtifactReport
        }
        $StagingReportPath = Join-Path $Staging "entry.json"
        [System.IO.File]::WriteAllText(
            $StagingReportPath,
            ($Report | ConvertTo-Json -Depth 16),
            [System.Text.UTF8Encoding]::new($false))
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Context.EntryDirectory) | Out-Null
        Move-Item -LiteralPath $Staging -Destination $Context.EntryDirectory
        $PublishedEntry = Read-AvidScriptCompilationCacheEntry -Context $Context
        return [pscustomobject]@{
            Published = $true
            EntryReportPath = $Context.EntryReportPath
            EntryReportSha256 = $PublishedEntry.ReportSha256
        }
    }
    finally {
        if (Test-Path -LiteralPath $Staging) {
            Remove-Item -LiteralPath $Staging -Recurse -Force
        }
        if (Test-Path -LiteralPath $LockPath) {
            Remove-Item -LiteralPath $LockPath -Recurse -Force
        }
    }
}
