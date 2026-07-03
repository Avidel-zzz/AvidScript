param()

$ErrorActionPreference = "Stop"

function Get-ToolVersionLine {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )

    try {
        $Output = & $Path @Arguments 2>&1
        foreach ($Line in $Output) {
            $Text = "$Line".Trim()
            if ($Text.Length -gt 0) {
                return $Text
            }
        }
    }
    catch {
        return "version_error=$($_.Exception.Message)"
    }

    return "version_unknown"
}

function Write-ToolProbe {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string[]]$VersionArguments
    )

    $Command = Get-Command $Name -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $Command) {
        Write-Output "[AvidScript][DGuest][Tool] $Name=MISSING"
        return
    }

    $Version = Get-ToolVersionLine -Path $Command.Source -Arguments $VersionArguments
    Write-Output "[AvidScript][DGuest][Tool] $Name=FOUND path=$($Command.Source) version=$Version"
}

Write-Output "[AvidScript][DGuest][Probe] start"
Write-ToolProbe -Name "ldc2" -VersionArguments @("--version")
Write-ToolProbe -Name "dub" -VersionArguments @("--version")
Write-ToolProbe -Name "wasm-ld" -VersionArguments @("--version")
Write-ToolProbe -Name "clang" -VersionArguments @("--version")
Write-Output "[AvidScript][DGuest][Probe] result=complete"

exit 0
