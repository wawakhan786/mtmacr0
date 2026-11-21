@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cl.exe /nologo /O2 /W3 "C:\Users\Bruno\PubgExt\tools\dump_driver\copy_sys.c" /Fo"C:\Users\Bruno\PubgExt\tools\dump_driver\out\\" /Fe"C:\Users\Bruno\PubgExt\tools\dump_driver\out\copy_sys.exe"
exit /b %ERRORLEVEL%
