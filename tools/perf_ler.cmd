@echo off
REM ===========================================================================
REM perf_ler.cmd -- le as linhas [PERF] do log
REM
REM POR QUE ESTE .cmd EXISTE
REM
REM O .ps1 nao roda direto: a politica de execucao do Windows bloqueia script
REM .ps1 por padrao ("a execucao de scripts foi desabilitada neste sistema").
REM Em vez de mandar voce mexer na politica da sua maquina -- o que afeta
REM TODOS os scripts do sistema -- este .cmd chama o PowerShell com Bypass
REM SO' para esta execucao. Nada muda no seu sistema.
REM ===========================================================================
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0perf_ler.ps1"
pause
