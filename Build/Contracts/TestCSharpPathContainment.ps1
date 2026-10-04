param()

$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSVersion.Major -lt 7 -or -not $IsWindows) { throw 'This contract requires PowerShell 7 on Windows.' }
[Diagnostics.Process]::GetCurrentProcess().PriorityClass = [Diagnostics.ProcessPriorityClass]::BelowNormal
$PluginRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $PluginRoot 'Build/AvidScriptCSharpSemanticCache.ps1')
. (Join-Path $PluginRoot 'Build/AvidScriptCSharpCompilationCache.ps1')
$RunId = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ') + '-' + [Guid]::NewGuid().ToString('N')
$EvidenceBase = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'AvidScript/Verification/PathContainment'
$EvidenceRoot = Join-Path $EvidenceBase $RunId
$Scratch = Join-Path $EvidenceRoot 'Scratch'
$Root = Join-Path $Scratch 'Root'
$Target = Join-Path $Scratch 'Target'
$ExistingDirectory = Join-Path $Root 'Child'
$File = Join-Path $ExistingDirectory 'file.txt'
$Junction = Join-Path $Root 'Link'
$FreshPath = Join-Path $Root 'Fresh'
$OwnedJunctions = [Collections.Generic.List[string]]::new()
$Results = [Collections.Generic.List[object]]::new()
$Utf8 = [Text.UTF8Encoding]::new($false)

function Assert-Containment([string]$Name, [string]$Candidate, [bool]$Expected) {
    $Actual = Test-AvidScriptBindingPathContained -RootPath $Root -CandidatePath $Candidate
    if ($Actual -ne $Expected) { throw "$Name expected=$Expected actual=$Actual" }
    $Results.Add([pscustomobject]@{ name=$Name; passed=$true; accepted=$Actual })
}
function Write-Fixture([string]$Path, [string]$Text) {
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Path)) | Out-Null
    [IO.File]::WriteAllText($Path, $Text, $Utf8)
}
function Remove-OwnedJunction([string]$Path) {
    if (-not $OwnedJunctions.Contains($Path) -or
        -not $Path.StartsWith($Root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Unexpected junction cleanup path.'
    }
    $Entry = Get-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue
    if ($null -ne $Entry) {
        if (-not ($Entry.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw 'Owned junction was replaced.'
        }
        # Removing this link is non-recursive and cannot remove its target.
        Remove-Item -LiteralPath $Path -Force
    }
}

try {
    [IO.Directory]::CreateDirectory($ExistingDirectory) | Out-Null
    [IO.Directory]::CreateDirectory($Target) | Out-Null
    Write-Fixture $File 'preserved input'
    $OriginalHash = Get-AvidScriptBindingSha256Hex $File
    Assert-Containment 'existing_file' $File $true
    Assert-Containment 'existing_directory' $ExistingDirectory $true
    Assert-Containment 'directory_trailing_separator' ($ExistingDirectory + '\') $true
    Assert-Containment 'missing_leaf' (Join-Path $ExistingDirectory 'future.json') $true
    Assert-Containment 'missing_tree' (Join-Path $Root 'Missing/Nested/future.json') $true
    Assert-Containment 'case_insensitive_windows' ($File.ToUpperInvariant()) $true
    Assert-Containment 'forward_slashes' ($File.Replace('\', '/')) $true
    Assert-Containment 'normalized_dot' (Join-Path $Root 'Child/./file.txt') $true
    Assert-Containment 'root_itself' $Root $false
    Assert-Containment 'same_name_prefix' ($Root + 'Other/file.txt') $false
    Assert-Containment 'parent_escape' (Join-Path $Root '../Target/file.txt') $false
    Assert-Containment 'outside_file' (Join-Path $Target 'file.txt') $false
    Assert-Containment 'file_as_parent' (Join-Path $File 'future.json') $false
    Assert-Containment 'invalid_component' (Join-Path $Root 'bad<name') $false

    $ExclusiveStream = [IO.File]::Open($File, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
    try { Assert-Containment 'exclusive_file_metadata' $File $true }
    finally { $ExclusiveStream.Dispose() }

    New-Item -ItemType Junction -Path $Junction -Target $Target | Out-Null
    $OwnedJunctions.Add($Junction)
    Assert-Containment 'junction_directory' $Junction $false
    Assert-Containment 'junction_missing_leaf' (Join-Path $Junction 'missing.txt') $false
    Assert-Containment 'junction_missing_tree' (Join-Path $Junction 'missing/nested/file.txt') $false
    $DriveRoot = [IO.Path]::GetPathRoot($Root)
    foreach ($Case in @(
        [pscustomobject]@{name='drive_root_file';path=$File;expected=$true},
        [pscustomobject]@{name='drive_root_missing_tree';path=(Join-Path $Root 'Missing/Nested/file.txt');expected=$true},
        [pscustomobject]@{name='drive_root_junction';path=(Join-Path $Junction 'missing.txt');expected=$false}
    )) {
        $Actual = Test-AvidScriptBindingPathContained -RootPath $DriveRoot -CandidatePath $Case.path
        if ($Actual -ne $Case.expected) { throw "$($Case.name) expected=$($Case.expected) actual=$Actual" }
        $Results.Add([pscustomobject]@{name=$Case.name;passed=$true;accepted=$Actual})
    }
    Assert-Containment 'fresh_missing_path' (Join-Path $FreshPath 'file.txt') $true
    New-Item -ItemType Junction -Path $FreshPath -Target $Target | Out-Null
    $OwnedJunctions.Add($FreshPath)
    Assert-Containment 'fresh_junction_rejected' (Join-Path $FreshPath 'file.txt') $false
    Remove-OwnedJunction $FreshPath
    Assert-Containment 'removed_junction_missing_path' (Join-Path $FreshPath 'file.txt') $true
    $BrokenTarget = Join-Path $Scratch 'BrokenTarget'
    $BrokenJunction = Join-Path $Root 'BrokenLink'
    [IO.Directory]::CreateDirectory($BrokenTarget) | Out-Null
    New-Item -ItemType Junction -Path $BrokenJunction -Target $BrokenTarget | Out-Null
    $OwnedJunctions.Add($BrokenJunction)
    Remove-Item -LiteralPath $BrokenTarget
    Assert-Containment 'broken_junction' $BrokenJunction $false
    Assert-Containment 'broken_junction_child' (Join-Path $BrokenJunction 'missing.txt') $false
    if ((Get-AvidScriptBindingSha256Hex $File) -cne $OriginalHash) { throw 'Negative checks changed the existing file.' }

    $Toolchain = Join-Path $Scratch 'Toolchain'
    Write-Fixture (Join-Path $Toolchain 'global.json') '{"sdk":{"version":"8.0.416"}}'
    foreach ($Name in @('InvokeCSharpFrontend', 'InvokeCSharpSemantic', 'InvokeCSharpGuestCompiler',
        'AvidScriptCSharpCompilerWorker', 'AvidScriptCSharpPreparedSemantic', 'AvidScriptCSharpBindingPackage',
        'AvidScriptCSharpSemanticCache', 'AvidScriptCSharpCompilationCache', 'BuildCSharpActorLifecycle')) {
        Write-Fixture (Join-Path $Toolchain "Build/$Name.ps1") "# $Name fixture"
    }
    foreach ($Name in @('AvidScript.CSharpFrontend', 'AvidScript.CSharpSemantic', 'AvidScript.CSharpGuest',
        'AvidScript.GuestIr', 'AvidScript.WasmBackend', 'AvidScript.CSharpCompilerWorker')) {
        Write-Fixture (Join-Path $Toolchain "Tools/$Name/Source.cs") 'class Fixture {}'
        Write-Fixture (Join-Path $Toolchain "Tools/$Name/Source.csproj") '<Project />'
    }
    $BeforeSemantic = Get-AvidScriptCSharpToolchainFingerprint -PluginRoot $Toolchain
    $BeforeCompilation = Get-AvidScriptCompilationCacheToolchainFingerprint -PluginRoot $Toolchain
    $HelperId = 'Build/AvidScriptCSharpBindingPackage.ps1'
    foreach ($Fingerprint in @($BeforeSemantic, $BeforeCompilation)) {
        if (@($Fingerprint.Files | Where-Object path -CEQ $HelperId).Count -ne 1) { throw 'Binding path owner is absent or duplicated in toolchain identity.' }
    }
    Write-Fixture (Join-Path $Toolchain $HelperId) '# changed path admission owner'
    if ((Get-AvidScriptCSharpToolchainFingerprint -PluginRoot $Toolchain).Sha256 -ceq $BeforeSemantic.Sha256) {
        throw 'Binding path owner did not invalidate semantic fingerprint.'
    }
    $Results.Add([pscustomobject]@{name='semantic_path_owner_identity';passed=$true})
    if ((Get-AvidScriptCompilationCacheToolchainFingerprint -PluginRoot $Toolchain).Sha256 -ceq $BeforeCompilation.Sha256) {
        throw 'Binding path owner did not invalidate compilation fingerprint.'
    }
    $Results.Add([pscustomobject]@{name='compilation_path_owner_identity';passed=$true})
}
finally {
    foreach ($Path in $OwnedJunctions) { Remove-OwnedJunction $Path }
    $ResolvedScratch = [IO.Path]::GetFullPath($Scratch)
    $ExpectedScratch = [IO.Path]::GetFullPath((Join-Path (Join-Path $EvidenceBase $RunId) 'Scratch'))
    if ($ResolvedScratch -cne $ExpectedScratch -or $RunId -cnotmatch '^\d{8}T\d{9}Z-[0-9a-f]{32}$') { throw 'Unexpected scratch cleanup path.' }
    if (Test-Path -LiteralPath $ResolvedScratch) {
        if ((Get-Item -LiteralPath $ResolvedScratch -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw 'Scratch root was replaced by a reparse point; preserve scratch.'
        }
        $Entries = @(Get-ChildItem -LiteralPath $ResolvedScratch -Recurse -Force)
        if (@($Entries | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) {
            throw 'Unexpected scratch reparse point; preserve scratch.'
        }
        foreach ($Entry in @($Entries | Sort-Object { $_.FullName.Length } -Descending)) {
            $AbsolutePath = [IO.Path]::GetFullPath($Entry.FullName)
            if (-not $AbsolutePath.StartsWith($ResolvedScratch + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
                throw 'Scratch cleanup escaped its namespace.'
            }
            Remove-Item -LiteralPath $AbsolutePath -Force
        }
        Remove-Item -LiteralPath $ResolvedScratch -Force
    }
}

$Report = [ordered]@{schema_version=1;passed=$Results.Count;total=$Results.Count;cases=$Results.ToArray();scratch_removed=(-not (Test-Path -LiteralPath $Scratch));input_preserved=$true}
[IO.File]::WriteAllText((Join-Path $EvidenceRoot 'results.json'), ($Report | ConvertTo-Json -Depth 10), $Utf8)
Write-Output "AvidScript.CSharpPathContainment: $($Results.Count)/$($Results.Count) passed; evidence=$EvidenceRoot"
