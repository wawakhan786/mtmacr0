<#
    check_lang.ps1
    -------------------------------------------------------------------------
    Confere se TODO texto passado por L(...) tem traducao em Config\Lang.h.

    POR QUE ISTO EXISTE
    "100% em ingles, nao deixe nada para tras" nao pode depender de eu ter
    lembrado de cada string. Este script extrai os literais de L(...) direto do
    codigo, extrai as chaves da tabela, e lista a DIFERENCA.

    Se ele diz "tudo traduzido", entao o build em ingles nao tem nenhum texto
    em portugues escapando pelo fallback.

    Uso:
        powershell -ExecutionPolicy Bypass -File tools\check_lang.ps1
#>

[CmdletBinding()]
param(
    [string] $Raiz = ''
)

$ErrorActionPreference = 'Stop'
if (-not $Raiz) { $Raiz = Split-Path -Parent $PSScriptRoot }

# ---------------------------------------------------------------------------
# LEITURA EM UTF-8, SEMPRE.
#
# `Get-Content -Raw` decide o encoding pelo BOM: arquivo COM BOM vira UTF-8,
# arquivo SEM BOM vira ANSI (codepage do sistema). Os fontes deste projeto sao
# um misto. Lendo os dois do jeito errado, o MESMO caractere vira dois textos
# diferentes e nada casa — foi o que fez este checker acusar 162 falsos
# "faltando" na primeira execucao.
# ---------------------------------------------------------------------------
function Read-Utf8 {
    param([string] $caminho)
    return [System.IO.File]::ReadAllText($caminho, [System.Text.Encoding]::UTF8)
}

function Unescape-Cpp {
    param([string] $s)
    $out = New-Object System.Text.StringBuilder
    for ($i = 0; $i -lt $s.Length; $i++) {
        if ($s[$i] -eq '\' -and ($i + 1) -lt $s.Length) {
            $i++
            switch ($s[$i]) {
                'n'  { [void]$out.Append("`n") }
                't'  { [void]$out.Append("`t") }
                'r'  { [void]$out.Append("`r") }
                '\'  { [void]$out.Append('\') }
                '"'  { [void]$out.Append('"') }
                default { [void]$out.Append($s[$i]) }
            }
        } else {
            [void]$out.Append($s[$i])
        }
    }
    return $out.ToString()
}

# Junta os pedacos de um literal/concatenacao e desescapa. Os DOIS lados
# (tabela e uso) passam por AQUI: a versao anterior desescapava o uso sem as
# aspas e a tabela com as aspas, entao nada casava nunca.
function ExtrairTexto {
    param([string] $bruto)
    $pedacos = [regex]::Matches($bruto, '"(?:[^"\\]|\\.)*"')
    $sb = New-Object System.Text.StringBuilder
    foreach ($p in $pedacos) {
        $inner = $p.Value.Substring(1, $p.Value.Length - 2)
        [void]$sb.Append((Unescape-Cpp $inner))
    }
    return $sb.ToString()
}

# Um literal OU uma concatenacao de literais (o compilador junta os pedacos).
$LIT = '"(?:[^"\\]|\\.)*"(?:\s*"(?:[^"\\]|\\.)*")*'

# ---------------------------------------------------------------------------
# 1. CHAVES DA TABELA
# ---------------------------------------------------------------------------
$langH = Join-Path $Raiz 'PubgExt\Config\Lang.h'
if (-not (Test-Path $langH)) { throw "Lang.h nao encontrado: $langH" }

$txtLang = Read-Utf8 $langH
$chaves  = New-Object 'System.Collections.Generic.HashSet[string]'
foreach ($m in [regex]::Matches($txtLang, '\{\s*(' + $LIT + ')\s*,')) {
    [void]$chaves.Add((ExtrairTexto $m.Groups[1].Value))
}

Write-Host ""
Write-Host "=== TABELA ===" -ForegroundColor Cyan
Write-Host ("  Config\Lang.h : {0} entradas" -f $chaves.Count)

# ---------------------------------------------------------------------------
# 2. LITERAIS DE L(...) NO CODIGO
#
# Lang.h fica FORA: ele e' a tabela. Os exemplos de L(...) que aparecem nos
# comentarios dele nao sao consumidores.
# ---------------------------------------------------------------------------
$fontes = Get-ChildItem -Path (Join-Path $Raiz 'PubgExt') -Recurse -File -Include *.cpp,*.h |
          Where-Object { $_.FullName -notmatch '\\imgui\\|\\stb\\|\\json\\|\\Mapper\\' -and $_.Name -ne 'Lang.h' }

$usados = New-Object 'System.Collections.Generic.Dictionary[string,string]'   # texto -> arquivo
foreach ($f in $fontes) {
    $txt = Read-Utf8 $f.FullName
    foreach ($m in [regex]::Matches($txt, 'L\(\s*(' + $LIT + ')\s*\)')) {
        $t = ExtrairTexto $m.Groups[1].Value
        if ($t.Length -gt 0 -and -not $usados.ContainsKey($t)) { $usados[$t] = $f.Name }
    }
}
Write-Host ("  literais L(...): {0} distintos" -f $usados.Count)

# ---------------------------------------------------------------------------
# 3. O QUE FALTA TRADUZIR
# ---------------------------------------------------------------------------
function Precisa-Traducao {
    param([string] $t)
    if ($t.StartsWith('##')) { return $false }
    if ($t -match '^%[-0-9.]*[a-zA-Z]+$') { return $false }
    return $true
}

$faltando = @()
foreach ($kv in $usados.GetEnumerator()) {
    if (-not (Precisa-Traducao $kv.Key)) { continue }
    if (-not $chaves.Contains($kv.Key)) {
        $faltando += [pscustomobject]@{ Texto = $kv.Key; Arquivo = $kv.Value }
    }
}

Write-Host ""
if ($faltando.Count -eq 0) {
    Write-Host "  [ok] todo literal L(...) tem traducao" -ForegroundColor Green
} else {
    Write-Host ("  [FALTAM] {0} literais sem traducao:" -f $faltando.Count) -ForegroundColor Yellow
    $faltando | Sort-Object Arquivo, Texto | ForEach-Object {
        $t = $_.Texto -replace "`n", '\n'
        if ($t.Length -gt 96) { $t = $t.Substring(0, 96) + '...' }
        Write-Host ("    {0,-16} {1}" -f $_.Arquivo, $t)
    }
}

# ---------------------------------------------------------------------------
# 4. ROTULOS DE IMGUI QUE FICARAM SEM L(...)
#
# Um rotulo SEM L() aparece igual nos dois idiomas. Se ele tiver texto em
# portugues, e' texto vazando pra dentro do build em ingles.
# ---------------------------------------------------------------------------
$semL = @()
foreach ($f in $fontes) {
    $txt = Read-Utf8 $f.FullName
    foreach ($m in [regex]::Matches($txt, 'ImGui::(?:Checkbox|SliderFloat|SliderInt|Combo|SeparatorText|TextDisabled|Button|BulletText|Text)\(\s*(' + $LIT + ')')) {
        $t = ExtrairTexto $m.Groups[1].Value
        if (-not $t.StartsWith('##')) { $semL += [pscustomobject]@{ Texto = $t; Arquivo = $f.Name } }
    }
    # TextColored tem a COR como 1o argumento: o rotulo e' o 2o.
    foreach ($m in [regex]::Matches($txt, 'ImGui::TextColored\(\s*ImVec4\([^)]*\)\s*,\s*(' + $LIT + ')')) {
        $t = ExtrairTexto $m.Groups[1].Value
        if (-not $t.StartsWith('##')) { $semL += [pscustomobject]@{ Texto = $t; Arquivo = $f.Name } }
    }
}

# ---------------------------------------------------------------------------
# EXCECOES INTENCIONAIS
#
# O rotulo do COMBO DE IDIOMA nao pode passar por L(): o ImGui usa o rotulo
# como ID do widget, entao traduzir o rotulo mudaria o ID no meio do frame — o
# combo perderia o foco exatamente quando voce trocasse o idioma. Ele fica
# bilingue fixo ("Idioma / Language") de proposito.
# ---------------------------------------------------------------------------
$excecoes = @( 'Idioma / Language' )
$semL = $semL | Where-Object { $excecoes -notcontains $_.Texto }

Write-Host ""
if ($semL.Count -eq 0) {
    Write-Host "  [ok] nenhum rotulo de ImGui ficou sem L(...)" -ForegroundColor Green
} else {
    Write-Host ("  [ATENCAO] {0} rotulos SEM L(...):" -f $semL.Count) -ForegroundColor Yellow
    $semL | Select-Object -Unique Texto,Arquivo | Sort-Object Texto | ForEach-Object {
        $t = $_.Texto -replace "`n", '\n'
        if ($t.Length -gt 96) { $t = $t.Substring(0, 96) + '...' }
        Write-Host ("    {0,-16} {1}" -f $_.Arquivo, $t)
    }
}

Write-Host ""
if ($faltando.Count -eq 0 -and $semL.Count -eq 0) {
    Write-Host "  RESULTADO: build em ingles sem texto em portugues escapando." -ForegroundColor Green
    exit 0
}
Write-Host "  RESULTADO: ainda ha texto a traduzir / envolver (ver acima)." -ForegroundColor Yellow
exit 1
