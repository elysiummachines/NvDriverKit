#Requires -RunAsAdministrator
<#
.SYNOPSIS
    After an NVCleanstall clean install: reinstalls the Microsoft Store NVIDIA Control Panel,
    checks that it works, and restores your saved driver settings.

.DESCRIPTION
    ONE-TIME SETUP (do this now, while your settings are the way you like them):
        .\NvPostInstall.ps1 -Capture
    This saves every customized driver setting (Global/Base Profile + any per-game profiles)
    to Documents\NvPostInstall\MySettings.nip, and your DSR Factors + Smoothness (which the
    driver keeps per monitor in the registry) to Documents\NvPostInstall\DsrSettings.json.
    "Documents" is your real Documents folder, even if you moved it to another drive.

    NVIDIA Profile Inspector is downloaded on first use and updated automatically
    whenever GitHub has a newer release.

    AFTER EVERY NVCLEANSTALL CLEAN INSTALL:
        .\NvPostInstall.ps1
    Installs/validates the Control Panel, makes sure the NVIDIA Display Container service is
    running, imports MySettings.nip, then re-reads the driver and confirms every value took.
    Then writes your DSR bytes back for each saved monitor and offers to reboot so DSR applies.

    Run from an elevated PowerShell. If scripts are blocked:
        powershell -ExecutionPolicy Bypass -File .\NvPostInstall.ps1

.PARAMETER Capture
    Save your current driver settings instead of restoring them.
.PARAMETER SkipControlPanel
    Only restore settings; don't touch the Control Panel install.
.PARAMETER OpenControlPanel
    Launch the Control Panel at the end so you can eyeball it.
#>
[CmdletBinding()]
param(
    [switch]$Capture,
    [switch]$SkipControlPanel,
    [switch]$OpenControlPanel
)

$ErrorActionPreference = 'Stop'

# ---------------- config ----------------
$DocsDir      = [Environment]::GetFolderPath('MyDocuments')   # follows a relocated Documents folder
$WorkDir      = Join-Path $DocsDir 'NvPostInstall'
$LegacyDir    = Join-Path $env:USERPROFILE 'Documents\NvPostInstall'   # where older versions saved
$InspectorDir = Join-Path $WorkDir 'ProfileInspector'
$SavedProfile = Join-Path $WorkDir 'MySettings.nip'
$StoreId      = '9NF8H0H7WMLT'                      # NVIDIA Control Panel on the Microsoft Store
$PackageName  = 'NVIDIACorp.NVIDIAControlPanel'
$ContainerSvc = 'NVDisplay.ContainerLocalSystem'    # "NVIDIA Display Container LS"
$SavedDsr     = Join-Path $WorkDir 'DsrSettings.json'
$DisplayDb    = 'HKLM:\SYSTEM\CurrentControlSet\Services\nvlddmkm\State\DisplayDatabase'
$DsrValues    = @('SmoothScalingData', 'SmoothScalingMultiplierData')   # per-monitor DSR smoothness + factors

# Friendly names for settings Profile Inspector exports without one
$KnownSettings = @{
    '390467' = 'Low Latency Mode'     # 0x0005F543 "Ultra Low Latency - CPL State": 0 Off, 1 On, 2 Ultra
}
$KnownValues = @{
    '390467' = @{ '0' = 'Off'; '1' = 'On'; '2' = 'Ultra' }
}
# ----------------------------------------

$script:Problems = New-Object System.Collections.Generic.List[string]

function Write-Step([string]$m) { Write-Host "`n==> $m" -ForegroundColor Cyan }
function Write-Ok  ([string]$m) { Write-Host "    [OK]   $m" -ForegroundColor Green }
function Write-Note([string]$m) { Write-Host "    [!!]   $m" -ForegroundColor Yellow }
function Write-Bad ([string]$m) { Write-Host "    [FAIL] $m" -ForegroundColor Red; $script:Problems.Add($m) }

New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null

# One-time move from the old C:\Users\...\Documents location to your real Documents folder
if ($LegacyDir -ne $WorkDir -and (Test-Path $LegacyDir)) {
    foreach ($f in 'MySettings.nip', 'DsrSettings.json') {
        $old = Join-Path $LegacyDir $f
        $new = Join-Path $WorkDir $f
        if ((Test-Path $old) -and -not (Test-Path $new)) {
            Copy-Item $old $new
            Write-Host "    Copied $f from the old location to $WorkDir" -ForegroundColor DarkGray
        }
    }
}

# ======================================================================
#  NVIDIA Profile Inspector (does the actual settings import/export)
# ======================================================================
function Find-InspectorExe {
    Get-ChildItem $InspectorDir -Recurse -Filter 'nvidiaProfileInspector.exe' -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName
}

# Returns the Profile Inspector path, updating it first if GitHub has a newer release.
# Checked once per run; if GitHub can't be reached, the copy already on disk is used.
$script:InspectorExe = $null
function Get-InspectorExe {
    if ($script:InspectorExe) { return $script:InspectorExe }

    Write-Step 'NVIDIA Profile Inspector'
    $versionFile = Join-Path $InspectorDir 'version.txt'
    $have     = if (Test-Path $versionFile) { (Get-Content $versionFile -Raw).Trim() } else { $null }
    $existing = Find-InspectorExe

    $rel = $null
    try {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        $rel = Invoke-RestMethod 'https://api.github.com/repos/Orbmu2k/nvidiaProfileInspector/releases/latest' `
                                 -Headers @{ 'User-Agent' = 'NvPostInstall' } -TimeoutSec 15
    } catch {
        if ($existing) {
            Write-Note "Couldn't check GitHub for updates - using the copy already here ($(if ($have) { $have } else { 'unknown version' }))"
            $script:InspectorExe = $existing
            return $existing
        }
        throw "Couldn't download Profile Inspector from GitHub: $($_.Exception.Message)"
    }

    if ($existing -and $have -eq $rel.tag_name) {
        Write-Ok "Up to date ($have)"
        $script:InspectorExe = $existing
        return $existing
    }

    $asset = $rel.assets | Where-Object { $_.name -like '*.zip' } | Select-Object -First 1
    if (-not $asset) { throw 'No .zip found on the latest Profile Inspector release.' }

    if ($existing) { Write-Host "    Updating $(if ($have) { $have } else { 'old copy' }) -> $($rel.tag_name)..." }
    else           { Write-Host "    Downloading $($rel.tag_name)..." }

    $zip = Join-Path $env:TEMP $asset.name
    Invoke-WebRequest $asset.browser_download_url -OutFile $zip -UseBasicParsing
    if (Test-Path $InspectorDir) { Remove-Item $InspectorDir -Recurse -Force }   # clean swap, no leftover files
    New-Item -ItemType Directory -Force -Path $InspectorDir | Out-Null
    Expand-Archive $zip -DestinationPath $InspectorDir -Force
    Remove-Item $zip -Force
    Get-ChildItem $InspectorDir -Recurse | Unblock-File
    Set-Content -Path $versionFile -Value $rel.tag_name

    $exe = Find-InspectorExe
    if (-not $exe) { throw 'nvidiaProfileInspector.exe not found in the downloaded archive.' }
    Write-Ok "Profile Inspector $($rel.tag_name) ready"
    $script:InspectorExe = $exe
    return $exe
}

# Runs "-exportCustomized" and returns the path of the .nip it wrote
function Export-CurrentSettings([string]$exe) {
    $dir    = Split-Path $exe
    $before = @(Get-ChildItem $dir -Filter '*.nip' -ErrorAction SilentlyContinue | ForEach-Object FullName)
    Start-Process -FilePath $exe -ArgumentList '-exportCustomized' -Wait -WindowStyle Hidden
    $new = Get-ChildItem $dir -Filter '*.nip' |
           Where-Object { $before -notcontains $_.FullName } |
           Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $new) { throw 'Profile Inspector did not produce an export file.' }
    return $new.FullName
}

# Reads a .nip into a list of objects: Profile, Id, Name, Value
function Read-Nip([string]$path) {
    [xml]$x = Get-Content -LiteralPath $path -Raw
    foreach ($p in @($x.ArrayOfProfile.Profile)) {
        foreach ($s in @($p.Settings.ProfileSetting)) {
            if ($null -eq $s) { continue }
            $id  = [string]$s.SettingID
            $val = [string]$s.SettingValue
            [pscustomobject]@{
                Profile = $p.ProfileName
                Id      = $id
                Name    = if ($s.SettingNameInfo) { $s.SettingNameInfo }
                          elseif ($KnownSettings.ContainsKey($id)) { $KnownSettings[$id] }
                          else { "ID $id" }
                Value   = $val
                Display = if ($KnownValues.ContainsKey($id) -and $KnownValues[$id].ContainsKey($val)) {
                              "$val ($($KnownValues[$id][$val]))"
                          } else { $val }
            }
        }
    }
}

# ======================================================================
#  DSR (stored per monitor in the registry, not in the driver profile)
# ======================================================================
# Returns one object per monitor/value: Monitor, Name, Bytes
function Get-DsrValues {
    if (-not (Test-Path $DisplayDb)) { return }
    foreach ($mon in Get-ChildItem $DisplayDb) {
        foreach ($name in $DsrValues) {
            $data = $mon.GetValue($name, $null)
            if ($data -is [byte[]]) {
                [pscustomobject]@{ Monitor = $mon.PSChildName; Name = $name; Bytes = [byte[]]$data }
            }
        }
    }
}

# Human-readable summary of one monitor's DSR blobs (display only; the restore copies raw bytes)
function Format-Dsr($smooth, $mult) {
    $parts = @()
    if ($smooth -and $smooth.Length -ge 16) {
        $parts += "Smoothness $([BitConverter]::ToUInt32($smooth, 12))%"
    }
    if ($mult) {
        $factors = @()
        for ($o = 0; $o + 28 -le $mult.Length; $o += 28) {
            $root = [BitConverter]::ToUInt32($mult, $o + 8)        # sqrt(factor) x 10000
            if ($root -gt 0) { $factors += ('{0:N2}x' -f [math]::Round([math]::Pow($root / 10000.0, 2), 2)) }
        }
        $parts += 'Factors ' + $(if ($factors) { $factors -join ', ' } else { 'none' })
    }
    return ($parts -join ' | ')
}

function Show-DsrSummary($entries) {
    foreach ($g in ($entries | Group-Object Monitor)) {
        $s = ($g.Group | Where-Object Name -eq 'SmoothScalingData').Bytes
        $m = ($g.Group | Where-Object Name -eq 'SmoothScalingMultiplierData').Bytes
        Write-Host "    Monitor $($g.Name): $(Format-Dsr $s $m)"
    }
}

# ======================================================================
#  CAPTURE MODE
# ======================================================================
if ($Capture) {
    $exe = Get-InspectorExe
    Write-Step 'Saving your current driver settings'
    $export = Export-CurrentSettings $exe
    Copy-Item $export $SavedProfile -Force
    Remove-Item $export -Force

    $settings = @(Read-Nip $SavedProfile)
    $base     = @($settings | Where-Object Profile -eq 'Base Profile')
    Write-Ok "Saved $($settings.Count) customized setting(s) to $SavedProfile"
    Write-Host ''
    $base | Sort-Object Name | Format-Table @{l='Global setting';e={$_.Name}}, @{l='Value';e={$_.Display}} -AutoSize | Out-Host

    Write-Step 'Saving DSR Factors and Smoothness'
    $dsr = @(Get-DsrValues)
    if ($dsr.Count -eq 0) {
        Write-Note 'No DSR data found. Set DSR once in the Control Panel, click Apply, then run -Capture again.'
    } else {
        ConvertTo-Json -InputObject @($dsr | ForEach-Object {
            [pscustomobject]@{ Monitor = $_.Monitor; Name = $_.Name; Data = [Convert]::ToBase64String($_.Bytes) }
        }) | Set-Content -Path $SavedDsr -Encoding UTF8
        Write-Ok "Saved DSR for $(@($dsr | Select-Object -ExpandProperty Monitor -Unique).Count) monitor(s) to $SavedDsr"
        Show-DsrSummary $dsr
    }
    Write-Host "`nDone. After each NVCleanstall install, run this script without -Capture." -ForegroundColor Cyan
    return
}

# ======================================================================
#  RESTORE MODE
# ======================================================================

# ---- 1. NVIDIA Control Panel from the Microsoft Store ----
$pkg = $null
if (-not $SkipControlPanel) {
    Write-Step 'NVIDIA Control Panel (Microsoft Store)'
    $pkg = Get-AppxPackage -Name $PackageName -ErrorAction SilentlyContinue

    if ($pkg) {
        Write-Ok "Already installed (v$($pkg.Version))"
    } else {
        $winget = (Get-Command winget.exe -ErrorAction SilentlyContinue).Source
        if (-not $winget) {
            $winget = Get-ChildItem "$env:ProgramFiles\WindowsApps\Microsoft.DesktopAppInstaller_*_x64__8wekyb3d8bbwe\winget.exe" `
                        -ErrorAction SilentlyContinue | Sort-Object FullName -Descending |
                        Select-Object -First 1 -ExpandProperty FullName
        }
        if (-not $winget) { throw 'winget not found. Install "App Installer" from the Microsoft Store first.' }

        Write-Host '    Installing via winget...'
        & $winget install --id $StoreId --source msstore --silent `
              --accept-package-agreements --accept-source-agreements | Out-Host

        # Store installs can register a few seconds after winget returns
        for ($i = 0; $i -lt 30 -and -not $pkg; $i++) {
            Start-Sleep -Seconds 2
            $pkg = Get-AppxPackage -Name $PackageName -ErrorAction SilentlyContinue
        }
        if ($pkg) { Write-Ok "Installed (v$($pkg.Version))" }
        else      { Write-Bad 'Control Panel did not install. Try installing it from the Store app manually.' }
    }

    if ($pkg) {
        if ($pkg.Status -ne 'Ok') { Write-Bad "Package status is '$($pkg.Status)'. Try Settings > Apps > NVIDIA Control Panel > Repair." }
        else { Write-Ok 'Package status OK' }

        if ($pkg.InstallLocation -notlike "$env:SystemDrive*") {
            Write-Bad "Installed on $($pkg.InstallLocation.Substring(0,2)) - it must be on C: or it won't open."
            Write-Note 'Fix: Settings > System > Storage > Where new content is saved > New apps = C:, then reinstall.'
        } else { Write-Ok 'Installed on the system drive' }
    }
}

# ---- 2. NVIDIA Display Container service (Control Panel needs it) ----
Write-Step 'NVIDIA Display Container LS service'
$svc = Get-Service -Name $ContainerSvc -ErrorAction SilentlyContinue
if (-not $svc) {
    Write-Bad 'Service missing. Your NVCleanstall run probably had "Disable NVIDIA Container" ticked - untick it next time.'
} else {
    if ($svc.StartType -ne 'Automatic') { Set-Service $ContainerSvc -StartupType Automatic; Write-Ok 'Startup type set to Automatic' }
    if ($svc.Status -ne 'Running')      { Start-Service $ContainerSvc; Write-Ok 'Service started' }
    else                                { Write-Ok 'Running' }
}

# ---- 3. Restore driver settings ----
Write-Step 'Restoring driver settings'
if (-not (Test-Path $SavedProfile)) {
    Write-Bad "No saved settings at $SavedProfile. Configure everything once, then run: .\NvPostInstall.ps1 -Capture"
} else {
    $exe = Get-InspectorExe
    Start-Process -FilePath $exe -ArgumentList '-silentImport', "`"$SavedProfile`"" -Wait -WindowStyle Hidden
    Write-Ok 'Imported MySettings.nip'

    # ---- 4. Verify: read settings back from the driver and compare ----
    Write-Step 'Verifying settings were applied'
    $check   = Export-CurrentSettings $exe
    $wanted  = @(Read-Nip $SavedProfile)
    $actual  = @(Read-Nip $check)
    Remove-Item $check -Force

    $lookup = @{}
    foreach ($a in $actual) { $lookup["$($a.Profile)|$($a.Id)"] = $a.Value }

    $bad = 0
    foreach ($w in $wanted) {
        $got = $lookup["$($w.Profile)|$($w.Id)"]
        if ($got -ne $w.Value) {
            $bad++
            Write-Bad "$($w.Profile) / $($w.Name): wanted $($w.Display), driver has $(if ($got) { $got } else { 'default' })"
        }
    }
    if ($bad -eq 0) { Write-Ok "All $($wanted.Count) setting(s) match what you saved" }
}

# ---- 4b. Restore DSR Factors / Smoothness (raw registry bytes, per monitor) ----
$dsrWritten = $false
Write-Step 'Restoring DSR Factors and Smoothness'
if (-not (Test-Path $SavedDsr)) {
    Write-Note 'No saved DSR settings. Run -Capture once with DSR set the way you like it.'
} else {
    # Windows PowerShell 5.1 returns a JSON array as ONE object; piping it again unrolls it into entries
    $saved = @((Get-Content $SavedDsr -Raw | ConvertFrom-Json) | ForEach-Object { $_ })

    # Work out which values actually need writing (reading works fine as admin)
    $toWrite = @()
    foreach ($e in $saved) {
        $key = Join-Path $DisplayDb $e.Monitor
        if (-not (Test-Path $key)) {
            Write-Bad "Monitor $($e.Monitor) not in the driver's display list yet. Reboot once with that monitor connected, then run this script again."
            continue
        }
        $current = (Get-Item $key).GetValue($e.Name, $null)
        if ($current -is [byte[]] -and [Convert]::ToBase64String($current) -eq $e.Data) {
            Write-Ok "$($e.Monitor) / $($e.Name) already correct"
        } else {
            $toWrite += $e
        }
    }

    if ($toWrite.Count -gt 0) {
        # Only the SYSTEM account may write under nvlddmkm\State, so hand the writes to a
        # one-shot SYSTEM scheduled task, wait for it, then delete the task and its files.
        $tmpDir  = Join-Path $env:ProgramData 'NvPostInstall'
        $writer  = Join-Path $tmpDir 'dsr-write.ps1'
        $logFile = Join-Path $tmpDir 'dsr-write.log'
        $task    = 'NvPostInstall-DSR-Restore'
        New-Item -ItemType Directory -Force -Path $tmpDir | Out-Null
        Remove-Item $logFile -Force -ErrorAction SilentlyContinue

        $lines = @("`$ErrorActionPreference = 'Stop'", 'try {')
        foreach ($e in $toWrite) {
            $k = (Join-Path $DisplayDb $e.Monitor) -replace "'", "''"
            $lines += "    Set-ItemProperty -LiteralPath '$k' -Name '$($e.Name)' -Type Binary -Value ([Convert]::FromBase64String('$($e.Data)'))"
        }
        $lines += "    'OK' | Set-Content -LiteralPath '$logFile'"
        $lines += '} catch {'
        $lines += "    ('ERROR: ' + `$_.Exception.Message) | Set-Content -LiteralPath '$logFile'"
        $lines += '}'
        Set-Content -LiteralPath $writer -Value $lines -Encoding UTF8

        Write-Host '    Writing DSR as SYSTEM (temporary task)...'
        try {
            $action    = New-ScheduledTaskAction -Execute "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" `
                                                 -Argument "-NoProfile -ExecutionPolicy Bypass -File `"$writer`""
            $principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
            Register-ScheduledTask -TaskName $task -Action $action -Principal $principal -Force | Out-Null
            Start-ScheduledTask -TaskName $task
            for ($i = 0; $i -lt 30 -and -not (Test-Path $logFile); $i++) { Start-Sleep -Seconds 1 }
        } finally {
            Unregister-ScheduledTask -TaskName $task -Confirm:$false -ErrorAction SilentlyContinue
        }

        $result = if (Test-Path $logFile) { (Get-Content -LiteralPath $logFile -Raw).Trim() } else { 'no response from the SYSTEM task within 30 seconds' }
        Remove-Item $writer, $logFile -Force -ErrorAction SilentlyContinue
        if ($result -ne 'OK') { Write-Bad "SYSTEM write failed: $result" }

        # Verify every value by reading it back
        foreach ($e in $toWrite) {
            $check = (Get-Item (Join-Path $DisplayDb $e.Monitor)).GetValue($e.Name, $null)
            if ($check -is [byte[]] -and [Convert]::ToBase64String($check) -eq $e.Data) {
                Write-Ok "$($e.Monitor) / $($e.Name) restored"
                $dsrWritten = $true
            } else {
                Write-Bad "$($e.Monitor) / $($e.Name) did not write correctly"
            }
        }
    }
    $now = @(Get-DsrValues | Where-Object { $saved.Monitor -contains $_.Monitor })
    if ($now) { Show-DsrSummary $now }
}

# ---- 5. Optionally open the Control Panel ----
if ($OpenControlPanel -and $pkg) {
    Write-Step 'Opening NVIDIA Control Panel'
    $appId = (Get-AppxPackageManifest $pkg).Package.Applications.Application | Select-Object -First 1 -ExpandProperty Id
    Start-Process "shell:AppsFolder\$($pkg.PackageFamilyName)!$appId"
    Start-Sleep -Seconds 6
    if (Get-Process nvcplui -ErrorAction SilentlyContinue) { Write-Ok 'Control Panel is running' }
    else { Write-Bad 'Control Panel did not start (nvcplui.exe not running).' }
}

# ---- summary ----
Write-Host ''
if ($script:Problems.Count -eq 0) {
    Write-Host 'All done - everything checked out.' -ForegroundColor Green
} else {
    Write-Host "Finished with $($script:Problems.Count) problem(s) - see [FAIL] lines above." -ForegroundColor Red
}
if ($dsrWritten) {
    Write-Host ''
    Write-Note 'DSR was rewritten in the registry. The driver only reads it at startup, so a reboot is needed.'
    $answer = Read-Host '    Reboot now? (y/N)'
    if ($answer -match '^(y|yes)$') {
        Write-Host '    Rebooting in 5 seconds... (Ctrl+C to cancel)' -ForegroundColor Yellow
        Start-Sleep -Seconds 5
        Restart-Computer -Force
    } else {
        Write-Note 'OK - reboot when you are ready. The Control Panel shows the old DSR values until then.'
    }
}
