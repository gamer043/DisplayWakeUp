One-shot tool for HDMI audio extractors. It removes the "ghost" monitor from the Windows desktop and keeps its HDMI audio endpoint alive. **Then it exits. Nothing stays running.**

- 14 KB exe: no CRT, no C++/WinRT, no exceptions, no console window. Takes milliseconds.
- Static imports: `kernel32`, `ntdll`, and the WinRT API sets. `user32` and `shell32` load only on the run that actually hides a display.

## Install

Per-user install. No admin needed. Takes about 5 seconds.

**One-liner (PowerShell):**

```powershell
irm https://raw.githubusercontent.com/gamer043/DisplayWakeUp/master/install.ps1 | iex
```

**Or** download `DisplayWakeUp.zip` from [Releases](https://github.com/gamer043/DisplayWakeUp/releases/latest), extract it, and double-click `install.cmd`.

**Or** from a clone: `powershell -ExecutionPolicy Bypass -File install.ps1`. If MSBuild is installed, it builds from source first.

The installer:

1. Copies the exe to `%LOCALAPPDATA%\DisplayWakeUp\`.
2. Removes any older install, including old Run-key and Startup-folder entries.
3. Registers the scheduled task **DisplayWakeUp**: at log on (+10 s), on unlock, and on resume from sleep (+5 s).
4. Prints what it detected and runs the task once.

The task is in Task Scheduler, so it won't show in Task Manager → Startup apps.

| Installer option | Effect |
|---|---|
| `-TaskArgs "--min"` | Pass extra args to the task |
| `-ExePath <file>` | Install a specific exe |
| `-Uninstall` | Remove the task, files and startup entries |
| `-Uninstall -Restore` | Same, and put hidden displays back on the desktop |

Uninstall via the one-liner:

```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/gamer043/DisplayWakeUp/master/install.ps1))) -Uninstall
```

## What it does

1. **Detect.** It looks for an HDMI display whose EDID (vendor + serial) matches another connected display on a different connector. That's an extractor sitting in front of a monitor that is already connected over DisplayPort.
2. **Hide.** It makes the same call as Settings → Advanced display → **"Remove display from desktop"**. Windows remembers the setting, so later runs skip this step.
3. **Wake.** It activates every removed-from-desktop (SpecialPurpose) display at its lowest mode, so the audio endpoint comes up.

## Usage

| Arg | Effect |
|---|---|
| *(none)* | Detect + hide the ghost, then wake it (preferred resolution, lowest refresh rate) |
| `--min` | Wake at the lowest resolution instead |
| `--dry-run` | Show what it detects. Change nothing |
| `--restore` | Undo: put hidden displays back on the desktop |
| `--no-hide` | Only wake displays that are already hidden |
| `--verbose` / `-v` | Print to the calling console, or to stdout if redirected (`> log.txt`) |

Exit code: `0` ok · `1` no SpecialPurpose display · `2` a step failed · `3` WinRT init failed · `4` UAC declined.

From cmd, use `start /wait DisplayWakeUp.exe --dry-run` to see output (it's a GUI-subsystem exe).

## Caveat

A display reconfiguration can turn off a SpecialPurpose display whose activating process is gone. Examples: a resolution, refresh or HDR change, a hotplug, or a driver reset. If audio drops, run the exe again. The installed task already reruns it at log on, unlock and resume.

## How the hide works (RE notes)

1. `SettingsHandlers_PCDisplay.dll!DisplaySettingsManager::SetMonitorSpecialized` runs the admin flow `SystemSettingsAdminFlows.exe SpecializeDisplay <luidLo> <luidHi> <targetId> <enable> <hash>`.
2. That calls `DisplayConfigSetDeviceInfo` with this packet:

| Offset | Field |
|---|---|
| +0x00 | header: `type = -23` (undocumented), `size = 48`, adapter LUID, target id (`DisplayTarget.AdapterRelativeId`) |
| +0x14 | `GUID_MONITOR_OVERRIDE_PSEUDO_SPECIALIZED` `{F196C02F-F86F-4F9A-AA15-E9CEBDFE3B96}` |
| +0x24 | `enable` |
| +0x28 | `hash`: `RtlHashUnicodeString` (X65599, case-insensitive) of `StableMonitorId + "{GUID}"`. First half of the chars → low dword, second half → high dword |

On a test machine (Windows 11 26300) the call succeeded **without** elevation. If it returns `ERROR_ACCESS_DENIED`, the tool relaunches itself elevated (one UAC prompt).

## Build

Needs VS Build Tools: MSVC + Windows SDK.

```
msbuild DisplayWakeUp.sln -p:Configuration=Release -p:Platform=x64
```

Output: `x64\Release\DisplayWakeUp.exe`. CI (`.github/workflows/msbuild.yml`) builds every push. Pushing a tag publishes a release.

## Credits

This is a rewrite of [O-Robotic/DisplayWakeUp](https://github.com/O-Robotic/DisplayWakeUp), which found the original SpecialPurpose-display wake approach. Licensed GPL-3.0 (see `LICENSE`).
