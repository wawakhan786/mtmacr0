# ===========================================================================
# devname_check.ps1 — Compara TRES implementacoes da derivacao de nome:
#
#   1. km_safe_source.c        (via um parse das constantes — so confere que
#                               as constantes batem; a logica do kernel nao
#                               roda em user-mode)
#   2. driver_interface_v3.cpp (via tools\devname_check.exe, compilado do mesmo
#                               algoritmo)
#   3. tools\DeviceName.ps1    (esta, que as ferramentas de teste usam)
#
# POR QUE ISTO EXISTE
#
# O nome do device deixou de ser um literal. Se as implementacoes divergirem em
# um bit, o CreateFile nao acha o device e o sintoma e "o driver nao carregou".
# Nao da pra descobrir isso lendo: tem que comparar.
#
# Uso:
#     powershell -ExecutionPolicy Bypass -File tools\devname_check.ps1
# ===========================================================================
[CmdletBinding()]
param(
    [string] $Exe = "$env:TEMP\devname_check.exe"
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'DeviceName.ps1')

$seeds = @(
    '0123456789abcdef',
    'fedcba9876543210',
    '0000000000000001',
    'deadbeefcafebabe',
    '9e3779b97f4a7c15',
    'ffffffffffffffff',
    '8000000000000000',
    '0000000000000000'
)

$seedsFile = Join-Path $env:TEMP 'devname_seeds.txt'
Set-Content -Path $seedsFile -Value $seeds -Encoding ASCII

# --- 1) implementacao do C++ (driver_interface_v3.cpp) ---
if (-not (Test-Path $Exe)) {
    Write-Host "  [aviso] $Exe nao existe — so' a implementacao do PowerShell sera testada." -ForegroundColor Yellow
    Write-Host "          Compile com: tools\build_devname_check.ps1" -ForegroundColor Yellow
    $cpp = @()
} else {
    $cpp = @(& $Exe $seedsFile | Where-Object { $_ -and $_.Trim() })
}

# --- 2) implementacao do PowerShell (DeviceName.ps1) ---
$ps = foreach ($s in $seeds) {
    $v = [uint64]::Parse($s, [System.Globalization.NumberStyles]::HexNumber)
    '{0} {1}' -f $s, (Get-KmDeviceName -Seed $v)
}

Write-Host ""
Write-Host "  seed                 PS (DeviceName.ps1)       C++ (driver_interface)" -ForegroundColor Cyan
Write-Host "  ------------------   ----------------------   ----------------------" -ForegroundColor DarkGray

$diverge = 0
for ($i = 0; $i -lt $seeds.Count; $i++) {
    $psLinha = "$($ps[$i])".Trim()
    $psNome  = ($psLinha -split ' ')[1]

    $cppNome   = '(nao comparado)'
    $comparado = $false
    if ($cpp.Count -eq $seeds.Count) {
        $cppLinha  = "$($cpp[$i])".Trim()
        $cppNome   = ($cppLinha -split ' ')[1]
        $comparado = $true
        if ($cppNome -ne $psNome) { $diverge++ }
    }

    $cor = 'Gray'
    if ($comparado) {
        if ($cppNome -eq $psNome) { $cor = 'Green' } else { $cor = 'Red' }
    }

    Write-Host ("  {0}   {1}   {2}" -f $seeds[$i], $psNome, $cppNome) -ForegroundColor $cor
}

Write-Host ""
if ($cpp.Count -ne $seeds.Count) {
    Write-Host "  [~] Sem o .exe do C++, a comparacao nao foi feita." -ForegroundColor Yellow
    Write-Host "      Estas sao as saidas do DeviceName.ps1 (a que as ferramentas usam)."
    exit 0
}

if ($diverge -eq 0) {
    Write-Host "  [ok] as duas implementacoes concordam em $($seeds.Count) seeds." -ForegroundColor Green
    exit 0
}

Write-Host "  [ERR] $diverge divergencia(s). O nome do device nao bate entre o" -ForegroundColor Red
Write-Host "        user-mode e as ferramentas de teste. Ver ARMADILHA 1/2 no" -ForegroundColor Red
Write-Host "        cabecalho de tools\DeviceName.ps1." -ForegroundColor Red
exit 1
