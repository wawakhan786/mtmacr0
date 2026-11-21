@echo off
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VS=%%i"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cl.exe /nologo /c /O2 /W3 /GS- /Z7 /D_AMD64_ /DAMD64 /D_KERNEL_MODE /I"C:\Program Files (x86)\Windows Kits\10\Include\10.0.28000.0\km" /I"C:\Program Files (x86)\Windows Kits\10\Include\10.0.28000.0\shared" /Fo"SharedProto\out\obj\\" tools\probe_icp.c
exit /b %ERRORLEVEL%
