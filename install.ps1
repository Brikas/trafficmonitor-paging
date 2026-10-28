# Builds PagingMonitor.dll with MSVC, copies it into TrafficMonitor's plugins folder, and restarts TrafficMonitor.
# Usage: powershell -File modules/trafficmonitor-paging/install.ps1
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$build = Join-Path $here "build"
New-Item -ItemType Directory -Force $build | Out-Null

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "MSVC x64 build tools not found" }
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"

cmd /c "call `"$vcvars`" >nul && cd /d `"$build`" && cl /nologo /LD /O2 /EHsc /std:c++17 /utf-8 /DUNICODE /D_UNICODE `"$here\PagingMonitor.cpp`" /Fe:PagingMonitor.dll"
if ($LASTEXITCODE -ne 0) { throw "build failed" }

# TrafficMonitor loads every DLL in the plugins folder next to its exe.
$exe = (Get-Process TrafficMonitor -ErrorAction SilentlyContinue | Select-Object -First 1).Path
if (-not $exe) {
    $exe = (Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\zhongyang219.TrafficMonitor*\TrafficMonitor\TrafficMonitor.exe" | Select-Object -First 1).FullName
}
if (-not $exe) { throw "TrafficMonitor.exe not found" }
$plugins = Join-Path (Split-Path $exe) "plugins"
New-Item -ItemType Directory -Force $plugins | Out-Null

Stop-Process -Name TrafficMonitor -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500
Copy-Item (Join-Path $build "PagingMonitor.dll") $plugins -Force
Start-Process $exe
Write-Host "Installed to $plugins"
