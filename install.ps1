<#
.SYNOPSIS
    Install, upgrade or remove DisplayWakeUp for the current user (no admin needed).

.DESCRIPTION
    Install:
      1. Gets the exe. It uses -ExePath if given, else the exe next to this script,
         else builds from source with MSBuild, else downloads the latest GitHub release.
      2. Removes any previous install: the scheduled task and old Run-key or Startup-folder entries.
      3. Copies the exe to %LOCALAPPDATA%\DisplayWakeUp.
      4. Registers the "DisplayWakeUp" scheduled task. It runs at log on (+10 s), unlock and resume from sleep (+5 s).
      5. Runs the task once and reports the result.

    The exe hides the HDMI-extractor ghost monitor, wakes its audio, then exits.
    Nothing stays running.

.EXAMPLE
    .\install.ps1                      # install or upgrade
.EXAMPLE
    .\install.ps1 -TaskArgs "--min"    # wake at the lowest resolution
.EXAMPLE
    .\install.ps1 -Uninstall           # remove the task and files
.EXAMPLE
    .\install.ps1 -Uninstall -Restore  # also put hidden displays back on the desktop
.EXAMPLE
    irm https://raw.githubusercontent.com/gamer043/DisplayWakeUp/master/install.ps1 | iex
#>
[CmdletBinding()]
param(
    [switch]$Uninstall,
    [switch]$Restore,
    [string]$ExePath,
    [string]$TaskArgs = ''
)

$DWU_Repo     = 'gamer043/DisplayWakeUp'
$DWU_TaskName = 'DisplayWakeUp'
$DWU_Dir      = Join-Path $env:LOCALAPPDATA 'DisplayWakeUp'
$DWU_Exe      = Join-Path $DWU_Dir 'DisplayWakeUp.exe'
$DWU_Src      = if ($PSScriptRoot) { $PSScriptRoot } else { '' }   # empty when piped into iex

function Write-Step([string]$n, [string]$msg) { Write-Host "[$n] " -ForegroundColor Cyan -NoNewline; Write-Host $msg }
function Write-Ok([string]$msg)   { Write-Host "    $msg" -ForegroundColor Green }
function Write-Info([string]$msg) { Write-Host "    $msg" -ForegroundColor DarkGray }
function Write-Bad([string]$msg)  { Write-Host "    $msg" -ForegroundColor Yellow }

function Get-ResultText([int]$code) {
    switch ($code) {
        0 { 'ok: ghost hidden, audio display woken' }
        1 { 'no SpecialPurpose display found: no HDMI ghost detected (is the extractor connected and powered?)' }
        2 { 'a step failed (run: DisplayWakeUp.exe --verbose > log.txt)' }
        3 { 'WinRT / DisplayManager init failed' }
        4 { 'UAC prompt declined' }
        0x41301 { 'still running' }
        default { 'exit code 0x{0:X}' -f $code }
    }
}

function Find-MSBuild {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $p = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
                        -find 'MSBuild\**\Bin\MSBuild.exe' 2>$null | Select-Object -First 1
        if ($p) { return $p }
    }
    $c = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    return $null
}

function Get-SourceExe {
    if ($ExePath) {
        if (-not (Test-Path $ExePath)) { throw "ExePath not found: $ExePath" }
        Write-Info "using $ExePath"
        return (Resolve-Path $ExePath).Path
    }

    if ($DWU_Src) {
        # Release zip layout: exe next to this script
        $local = Join-Path $DWU_Src 'DisplayWakeUp.exe'
        if (Test-Path $local) { Write-Info "using $local"; return $local }

        # Source checkout: build a fresh Release exe
        $sln = Join-Path $DWU_Src 'DisplayWakeUp.sln'
        $built = Join-Path $DWU_Src 'x64\Release\DisplayWakeUp.exe'
        if (Test-Path $sln) {
            $msb = Find-MSBuild
            if ($msb) {
                Write-Info "building with $msb"
                & $msb $sln -p:Configuration=Release -p:Platform=x64 -m -nologo -v:minimal | Out-Host
                if ($LASTEXITCODE -ne 0) { throw "build failed (msbuild exit $LASTEXITCODE)" }
                return $built
            }
            if (Test-Path $built) { Write-Bad "MSBuild not found, using existing build $built"; return $built }
            Write-Info 'MSBuild not found, downloading the release instead'
        }
    }

    # Download the latest release
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    $url = "https://github.com/$DWU_Repo/releases/latest/download/DisplayWakeUp.exe"
    $tmp = Join-Path ([IO.Path]::GetTempPath()) ("DisplayWakeUp-{0}.exe" -f [guid]::NewGuid().ToString('N'))
    Write-Info "downloading $url"
    $ProgressPreference = 'SilentlyContinue'
    Invoke-WebRequest -Uri $url -OutFile $tmp -UseBasicParsing -ErrorAction Stop
    return $tmp
}

function Remove-Previous {
    $t = Get-ScheduledTask -TaskName $DWU_TaskName -ErrorAction SilentlyContinue
    if ($t) {
        Stop-ScheduledTask -TaskName $DWU_TaskName -ErrorAction SilentlyContinue
        Unregister-ScheduledTask -TaskName $DWU_TaskName -Confirm:$false
        Write-Info 'removed scheduled task'
    }
    # Older setups: Run key / Startup folder shortcuts (avoid a double run at logon)
    $run = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
    $props = Get-ItemProperty -Path $run -ErrorAction SilentlyContinue
    if ($props) {
        foreach ($p in $props.PSObject.Properties) {
            if ($p.Name -like '*DisplayWakeUp*' -or "$($p.Value)" -like '*DisplayWakeUp.exe*') {
                Remove-ItemProperty -Path $run -Name $p.Name
                Write-Info "removed Run entry '$($p.Name)'"
            }
        }
    }
    $startup = [Environment]::GetFolderPath('Startup')
    Get-ChildItem -Path $startup -Filter '*DisplayWakeUp*' -ErrorAction SilentlyContinue | ForEach-Object {
        Remove-Item $_.FullName -Force
        Write-Info "removed Startup shortcut '$($_.Name)'"
    }
}

function Register-DwuTask {
    $user = [Security.Principal.WindowsIdentity]::GetCurrent().Name
    $ns = 'ROOT\Microsoft\Windows\TaskScheduler'

    $action = if ($TaskArgs) { New-ScheduledTaskAction -Execute $DWU_Exe -Argument $TaskArgs }
              else           { New-ScheduledTaskAction -Execute $DWU_Exe }

    # 1) log on (+10 s, lets the display stack settle)
    $logon = New-ScheduledTaskTrigger -AtLogOn -User $user
    $logon.Delay = 'PT10S'
    # 2) workstation unlock (StateChange 8 = SessionUnlock)
    $unlock = New-CimInstance -ClientOnly -CimClass (Get-CimClass -Namespace $ns -ClassName MSFT_TaskSessionStateChangeTrigger) `
                              -Property @{ StateChange = 8; UserId = $user; Enabled = $true }
    # 3) resume from sleep (System log, Power-Troubleshooter event 1)
    $q = '<QueryList><Query Id="0" Path="System"><Select Path="System">' +
         "*[System[Provider[@Name='Microsoft-Windows-Power-Troubleshooter'] and EventID=1]]" +
         '</Select></Query></QueryList>'
    $resume = New-CimInstance -ClientOnly -CimClass (Get-CimClass -Namespace $ns -ClassName MSFT_TaskEventTrigger) `
                              -Property @{ Enabled = $true; Delay = 'PT5S'; Subscription = $q }

    $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
                    -ExecutionTimeLimit (New-TimeSpan -Minutes 1) -MultipleInstances IgnoreNew
    $principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited

    Register-ScheduledTask -TaskName $DWU_TaskName -Action $action -Trigger $logon, $unlock, $resume `
        -Settings $settings -Principal $principal `
        -Description 'Hide the HDMI audio-extractor ghost monitor and wake its audio, then exit. https://github.com/gamer043/DisplayWakeUp' `
        -ErrorAction Stop | Out-Null
}

function Invoke-Install {
    $ErrorActionPreference = 'Stop'

    Write-Step '1/5' 'Getting DisplayWakeUp.exe'
    $src = Get-SourceExe

    Write-Step '2/5' 'Removing previous install'
    Remove-Previous

    Write-Step '3/5' "Copying to $DWU_Dir"
    New-Item -ItemType Directory -Force -Path $DWU_Dir | Out-Null
    if ((Resolve-Path $src).Path -ne $DWU_Exe) { Copy-Item $src $DWU_Exe -Force }
    if ($src -like (Join-Path ([IO.Path]::GetTempPath()) 'DisplayWakeUp-*')) { Remove-Item $src -Force -ErrorAction SilentlyContinue }
    Unblock-File $DWU_Exe -ErrorAction SilentlyContinue
    Write-Ok ('{0:N0} KB' -f ((Get-Item $DWU_Exe).Length / 1KB))
    $scan = & $DWU_Exe --dry-run | Out-String
    foreach ($line in ($scan -split "`r?`n" | Where-Object { $_ })) { Write-Info $line }

    Write-Step '4/5' "Registering scheduled task '$DWU_TaskName' (log on +10 s, unlock, resume +5 s)"
    Register-DwuTask
    Write-Ok 'registered'

    Write-Step '5/5' 'Running it once now'
    Start-ScheduledTask -TaskName $DWU_TaskName
    $deadline = (Get-Date).AddSeconds(60)   # long enough for a one-time UAC prompt
    do { Start-Sleep -Milliseconds 300 } while ((Get-ScheduledTask -TaskName $DWU_TaskName).State -eq 'Running' -and (Get-Date) -lt $deadline)
    $code = [int](Get-ScheduledTaskInfo -TaskName $DWU_TaskName).LastTaskResult
    if ($code -eq 0) { Write-Ok (Get-ResultText $code) } else { Write-Bad (Get-ResultText $code) }

    Write-Host ''
    Write-Host 'Installed. It runs at every sign-in, unlock and wake, then exits.' -ForegroundColor Green
    Write-Host "Uninstall: run install.ps1 -Uninstall   (add -Restore to put hidden displays back)"
}

function Invoke-Uninstall {
    $ErrorActionPreference = 'Stop'
    $total = if ($Restore) { 3 } else { 2 }
    $i = 0

    if ($Restore) {
        $i++; Write-Step "$i/$total" 'Putting hidden displays back on the desktop'
        if (Test-Path $DWU_Exe) {
            $out = & $DWU_Exe --restore --verbose | Out-String
            $rc = $LASTEXITCODE
            foreach ($line in ($out -split "`r?`n" | Where-Object { $_ })) { Write-Info $line }
            if ($rc -eq 0) { Write-Ok 'restored' } else { Write-Bad (Get-ResultText $rc) }
        } else { Write-Bad "$DWU_Exe not found, skipped" }
    }

    $i++; Write-Step "$i/$total" 'Removing scheduled task and startup entries'
    Remove-Previous

    $i++; Write-Step "$i/$total" "Deleting $DWU_Dir"
    if (Test-Path $DWU_Dir) { Remove-Item $DWU_Dir -Recurse -Force; Write-Ok 'deleted' } else { Write-Info 'not present' }

    Write-Host ''
    Write-Host 'Uninstalled.' -ForegroundColor Green
    if (-not $Restore) { Write-Host 'Hidden displays stay hidden (Windows remembers). To undo: Settings > Display > Advanced display, or reinstall and run -Uninstall -Restore.' }
}

try {
    if ($Uninstall) { Invoke-Uninstall } else { Invoke-Install }
} catch {
    Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
    if ($PSCommandPath) { exit 1 }   # never exit when piped into iex (it would close the shell)
}
