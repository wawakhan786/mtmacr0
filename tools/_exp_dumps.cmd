@echo off
set DB="C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\dumpbin.exe"
%DB% /exports C:\Windows\System32\drivers\kbdclass.sys > tools\_exp_kbdclass.txt 2>&1
%DB% /exports C:\Windows\System32\drivers\mouclass.sys > tools\_exp_mouclass.txt 2>&1
%DB% /exports C:\Windows\System32\ntoskrnl.exe > tools\_exp_ntoskrnl.txt 2>&1
echo done
