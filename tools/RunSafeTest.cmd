@echo off
REM ===========================================================================
REM  RunSafeTest.cmd
REM
REM  Um comando so': carrega o driver, roda o harness de validacao, e (opcional)
REM  DESCARREGA o driver — sem reboot.
REM
REM  NAO e' um carregador proprio. O carregamento continua sendo feito pelo
REM  kdmapper, dentro do mtmacr0.exe: para entrar em ring0 sem assinatura o
REM  BYOVD e' o unico caminho por software, e escrever um carregador proprio nao
REM  muda isso. O que da' pra eliminar e' a digitacao, e e' o que este script faz.
REM
REM  ---------------------------------------------------------------------------
REM  O QUE MUDOU NA 1.2
REM
REM  O nome do device deixou de ser um literal. Antes este script chamava o
REM  kdmapper direto num "PUBG_KM_safe.sys" e o harness abria "\\.\Ndu2" — os
REM  dois nomes estavam escritos aqui. Agora o driver embutido e' mapeado pelo
REM  mtmacr0.exe, o nome sai do SEED da sessao, e o harness LE esse seed do
REM  logs.txt. Por isso o passo 2 e' rodar o cheat em modo map-only.
REM
REM  Uso:   tools\RunSafeTest.cmd
REM         tools\RunSafeTest.cmd --selfdestruct     (testa a descarga tambem)
REM ===========================================================================

setlocal

set RAIZ=%~dp0..
set BIN=%RAIZ%\Build\Release\PubgExt
cd /d "%BIN%"

set SELFDESTRUCT=
if /I "%~1"=="--selfdestruct" set SELFDESTRUCT=--selfdestruct

echo.
echo ==========================================================
echo  1. o jogo esta fechado?
echo ==========================================================
tasklist /FI "IMAGENAME eq TslGame.exe" 2>nul | find /I "TslGame.exe" >nul
if not errorlevel 1 (
    echo   [!] TslGame.exe esta' RODANDO.
    echo.
    echo   Feche o jogo antes. Com o jogo aberto o BattlEye esta' ativo, o
    echo   gate da 1.2 BLOQUEIA o mapeamento, e o teste perde a unica vantagem
    echo   que tem: risco de ban zero.
    echo.
    pause
    exit /b 1
)
echo   ok - nenhum TslGame.exe rodando
echo.

echo ==========================================================
echo  2. carregando o driver (modo map-only, sem overlay)
echo ==========================================================
if not exist "mtmacr0.exe" (
    echo   [!] mtmacr0.exe nao encontrado em %BIN%
    pause
    exit /b 1
)

REM O modo map-only mapeia o driver e SAI. E' o que permite obter o seed sem
REM abrir o cheat inteiro.
set PUBGEXT_MAP_ONLY=1
mtmacr0.exe
set PUBGEXT_MAP_ONLY=

REM ---------------------------------------------------------------------------
REM Le' o seed do log. As ferramentas de teste (harness, probe) derivam o nome
REM do device a partir dele — o mesmo algoritmo do driver e do user-mode.
REM
REM POR QUE ISTO CHAMA O POWERSHELL EM VEZ DE PARSEAR AQUI:
REM a primeira versao usava `for /f "tokens=5 delims=:"`, e o resultado foi
REM "[ =]" — o for /f fatia o HEX no caractere errado e ainda trata o espaco
REM depois da virgula. Fazer aritmetica/parsing de hex em batch e' o caminho
REM errado; o DeviceName.ps1 ja' faz isso certo, e tem teste proprio
REM (tools\devname_check.ps1). Entao o batch DELEGA.
REM ---------------------------------------------------------------------------
for /f "usebackq delims=" %%S in (`powershell -NoProfile -ExecutionPolicy Bypass -File "%RAIZ%\tools\probe_driver.ps1" -PrintDevice`) do set SEED=%%S
if "%SEED%"=="" (
    echo   [!] nao consegui obter o seed a partir do logs.txt.
    echo       Veja o log para saber por que o driver nao mapeou:
    echo         type "%BIN%\logs.txt"
    pause
    exit /b 1
)
echo   device desta sessao: \\.\%SEED%
echo.

echo ==========================================================
echo  3. log do USER-MODE  (o log do KERNEL esta' desligado)
echo ==========================================================
REM O C:\Windows\PkmSafe.txt NAO existe numa build normal: o log do driver esta'
REM desligado por padrao (KM_LOG_HABILITADO = 0), porque um arquivo texto em
REM C:\Windows descrevendo o driver era o artefato mais facil de achar.
REM O que diz o que aconteceu e' o logs.txt do user-mode.
if exist "%BIN%\logs.txt" (
    echo   ultimas linhas de logs.txt:
    powershell -NoProfile -Command "Get-Content '%BIN%\logs.txt' -Tail 12"
) else (
    echo   [!] logs.txt nao existe - o cheat nem chegou a abrir.
)
echo.

echo ==========================================================
echo  4. harness de validacao
echo ==========================================================
if not exist "TestHarness.exe" (
    echo   [!] TestHarness.exe nao encontrado. Compile com:
    echo         tools\build_harness.ps1
    pause
    exit /b 1
)

REM O harness roda DENTRO do diretorio do exe, entao o "logs.txt" relativo do
REM fallback de seed funciona.
TestHarness.exe --seed %SEED% %SELFDESTRUCT%

echo.
echo ==========================================================
echo  5. fim
echo ==========================================================
echo   O driver CONTINUA carregado (a menos que voce tenha usado
echo   --selfdestruct). Para descarregar sem reboot:
echo.
echo     powershell -NoProfile -ExecutionPolicy Bypass -File ^
echo       "%RAIZ%\tools\probe_driver.ps1" -Seed %SEED% -Deactivate
echo.
echo   Ou simplesmente feche e rode este script de novo: na 1.2 o driver se
echo   descarrega pelo DEACTIVATE antes de mapear de novo.

endlocal
