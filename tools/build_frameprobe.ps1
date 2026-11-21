# Compila Build\frameprobe\probe.exe (prova da causa da moldura preta).
# Standalone: so' imgui + d3d11. Nao toca no cheat.
$ErrorActionPreference = 'Stop'
$Root   = Split-Path -Parent $PSScriptRoot
$Probe  = Join-Path $Root 'Build\frameprobe'
$ImGui  = Join-Path $Root 'PubgExt\OS-ImGui\imgui'
$VsPath = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$VcVars = Join-Path $VsPath 'VC\Auxiliary\Build\vcvars64.bat'

$cmd = "cl.exe /nologo /std:c++17 /EHsc /O2 /MD /DNDEBUG /I`"$ImGui`" " +
       "`"$Probe\probe.cpp`" `"$ImGui\imgui.cpp`" `"$ImGui\imgui_draw.cpp`" " +
       "`"$ImGui\imgui_widgets.cpp`" `"$ImGui\imgui_tables.cpp`" " +
       "`"$ImGui\imgui_impl_win32.cpp`" `"$ImGui\imgui_impl_dx11.cpp`" " +
       "/Fe`"$Probe\probe.exe`" /Fo`"$Probe\\`" /link d3d11.lib dxgi.lib dwmapi.lib"

$bat = Join-Path $Probe 'build.bat'
Set-Content -Path $bat -Encoding ASCII -Value @('@echo off', "call `"$VcVars`" >nul 2>&1", $cmd, 'exit /b %ERRORLEVEL%')
& cmd.exe /d /c "`"$bat`" 2>&1"
Write-Host "exit=$LASTEXITCODE"
if (Test-Path "$Probe\probe.exe") { Write-Host ("ok -> {0} bytes" -f (Get-Item "$Probe\probe.exe").Length) }
