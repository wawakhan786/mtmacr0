@echo off
REM ===========================================================================
REM perf_flag.cmd -- liga/desliga a medicao de desempenho
REM
REM   perf_flag.cmd on      liga
REM   perf_flag.cmd off     desliga
REM   perf_flag.cmd         mostra o estado
REM ===========================================================================
setlocal
set "FLAG=%LOCALAPPDATA%\RenderCache\perf.flag"
if /i "%~1"=="on"  goto ligar
if /i "%~1"=="off" goto desligar
if exist "%FLAG%" (echo Medicao: LIGADA  ^(%FLAG%^)) else (echo Medicao: desligada)
goto fim
:ligar
echo ligado> "%FLAG%"
echo Medicao LIGADA. Rode o mtmacr0 normalmente -- nao precisa de variavel.
goto fim
:desligar
if exist "%FLAG%" del /q "%FLAG%"
echo Medicao DESLIGADA.
goto fim
:fim
endlocal
