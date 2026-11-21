# ===========================================================================
# DeviceName.ps1 — derivacao do nome do device, para as FERRAMENTAS DE TESTE.
#
# ---------------------------------------------------------------------------
# POR QUE ISTO EXISTE
#
# Na 1.1 o nome do device era um literal: "\\.\Ndu2". O probe_driver.ps1 e o
# TestHarness.exe simplesmente abriam esse nome. Isso funcionava — e era
# exatamente o problema: o literal vivia dentro do .sys, que vai embutido no
# .exe. Uma varredura de strings achava o ponto de entrada do driver.
#
# Na 1.2 o nome e "\\.\<16 hex>" derivado do SEED da sessao. Nada no binario
# diz qual vai ser. O preco e este arquivo: as ferramentas de teste precisam
# do MESMO algoritmo para saber em quem apontar.
#
# ---------------------------------------------------------------------------
# O ALGORITMO — COPIA DE km_safe_source.c (KmMixSeed/KmHex16) E DE
#              driver_interface_v3.cpp (KmMixSeedCpp/DeriveDeviceName)
#
# ==================== NAO MEXA EM UM LADO SO ====================
#
# Tres implementacoes, uma regra: se uma mudar, as outras duas mudam junto.
# Divergir em um unico bit faz o CreateFile nao achar o device, e o sintoma e
# "o driver nao carregou" — o lugar errado pra procurar um bug de nome.
#
# ---------------------------------------------------------------------------
# ==================== AS TRES ARMADILHAS DO POWERSHELL 5.1 ====================
#
# Este arquivo era 20 linhas obvias, e as TRES primeiras versoes dele estavam
# ERRADAS. Cada armadilha abaixo da um resultado silenciosamente diferente do
# C++ — nao um erro. Um nome derivado errado nao trava nada: ele simplesmente
# nao acha o device, e a investigacao vai pro lugar errado.
#
# ARMADILHA 1 — literal hex >= 2^63 nao converte.
#
#     [uint64]0x9E3779B97F4A7C15
#     -> "Nao e possivel converter o valor -7046029254386353131 para UInt64"
#
#   O PowerShell le o literal como Int64 (negativo) e SO DEPOIS tenta converter
#   para uint64. Como o valor ja e negativo, ele recusa.
#   Solucao: [uint64]::Parse("9E3779B97F4A7C15", HexNumber).
#
# ARMADILHA 2 — `*` de uint64 TRUNCA PARA 32 BITS.
#
#     $k = [uint64]::Parse('FFFFFFFFFFFFFFFF', HexNumber)
#     $k = $k * [uint64]3        -> 4294967293  (0xFFFFFFFD), NAO o valor certo
#
#   O operador `*` escolhe o tipo pelo resultado da aritmetica interna, e em
#   contexto de uint64 ele passa por 32 bits. O valor sai "plausivel" — foi o
#   que tornou o bug dificil de ver: a saida parecia um nome valido.
#
# ARMADILHA 3 — `-band` COM BigInteger NAO FAZ NADA.
#
#     $prod = [BigInteger]22801429638646396970
#     $prod -band $mask64        -> 22801429638646396970   (inalterado!)
#
#   Medido: o operador `-band` nao aplica a operacao binaria quando um dos lados
#   e BigInteger — ele devolve o operando ESQUERDO. O resultado parece um numero
#   valido e nao esta mascarado, entao a conversao final estoura com "valor
#   muito grande para UInt64" longe da causa.
#
#   Solucao: NAO use operadores com BigInteger. Chame os METODOS:
#       [BigInteger]::op_Multiply($a, $b)
#       [BigInteger]::Remainder($p, 2^64)
#   O Remainder por 2^64 e exatamente o wraparound natural do C++.
#
# A verificacao esta em tools\devname_check.ps1: ele compara esta implementacao
# com a do C++ (tools\devname_check.cpp) em 8 seeds. Foi assim que as tres
# armadilhas apareceram — e e o que garante que elas nao voltem.
# ===========================================================================

Add-Type -AssemblyName System.Numerics

$script:KmNameMix = [uint64]::Parse('9E3779B97F4A7C15', [System.Globalization.NumberStyles]::HexNumber)
$script:KmMul1    = [uint64]::Parse('FF51AFD7ED558CCD', [System.Globalization.NumberStyles]::HexNumber)
$script:KmMul2    = [uint64]::Parse('C4CEB9FE1A85EC53', [System.Globalization.NumberStyles]::HexNumber)

# 2^64 — o modulo que reproduz o wraparound de 64 bits do C++.
$script:Mod64 = [System.Numerics.BigInteger]::Pow([System.Numerics.BigInteger]::new(2), 64)

function script:KmMul64 {
    <#
        Multiplicacao de 64 bits SEM perda. Devolve [uint64].

        Usa os METODOS do BigInteger, e nao operadores: `*` trunca
        (ARMADILHA 2) e `-band` com BigInteger nao faz nada (ARMADILHA 3).
        O Remainder por 2^64 e o wraparound do C++.
    #>
    param([uint64] $A, [uint64] $B)

    $x = [System.Numerics.BigInteger]::new($A)
    $y = [System.Numerics.BigInteger]::new($B)
    $p = [System.Numerics.BigInteger]::op_Multiply($x, $y)
    $r = [System.Numerics.BigInteger]::Remainder($p, $script:Mod64)
    return [uint64]$r
}

function script:KmXorShift {
    # `k ^ (k >> n)` — os dois lados ja sao uint64, entao -bxor e -shr servem.
    param([uint64] $K, [int] $N)
    return [uint64]($K -bxor ($K -shr $N))
}

function Get-KmMixSeed {
    <#
        Mistura o seed com o tipo (0 = driver object, 1 = symlink/device).
        Mesma avalanche do murmur3 — os multiplicadores sao os do finalizador.
    #>
    param(
        [Parameter(Mandatory)][uint64] $Seed,
        [Parameter(Mandatory)][uint32] $Kind
    )

    # k = seed ^ (KM_NAME_MIX * (kind + 1))
    $k = $Seed -bxor (script:KmMul64 -A $script:KmNameMix -B ([uint64]($Kind + 1)))

    $k = script:KmXorShift -K $k -N 33
    $k = script:KmMul64 -A $k -B $script:KmMul1
    $k = script:KmXorShift -K $k -N 33
    $k = script:KmMul64 -A $k -B $script:KmMul2
    $k = script:KmXorShift -K $k -N 33

    return $k
}

function Get-KmDeviceName {
    <#
        Devolve "\\.\<16 hex>" para o seed dado.
        E o mesmo valor que o cheat imprime no log como "device desta sessao".
    #>
    param([Parameter(Mandatory)][uint64] $Seed)

    $v = Get-KmMixSeed -Seed $Seed -Kind 1

    $sb = New-Object System.Text.StringBuilder
    [void]$sb.Append('\\.\')
    for ($i = 15; $i -ge 0; $i--) {
        $nib = [int](($v -shr ($i * 4)) -band 0xF)
        [void]$sb.Append('0123456789abcdef'[$nib])
    }
    return $sb.ToString()
}

function Get-KmSeedFromLog {
    <#
        Le o seed da ultima execucao do cheat no logs.txt.

        O cheat TRUNCA o logs.txt a cada execucao, entao a ultima linha de seed
        do arquivo e a da sessao mais recente — que e a que tem um driver
        carregado.
    #>
    param([string] $LogPath = '')

    if (-not $LogPath) {
        $raiz = Split-Path -Parent $PSScriptRoot
        $LogPath = Join-Path $raiz 'Build\Release\PubgExt\logs.txt'
    }

    if (-not (Test-Path $LogPath)) { return $null }

    $linhas = Select-String -Path $LogPath -Pattern 'seed desta sessao: 0x([0-9A-Fa-f]{1,16})' -AllMatches
    if (-not $linhas) { return $null }

    $ultima = $linhas[-1].Matches[0].Groups[1].Value
    return [uint64]::Parse($ultima, [System.Globalization.NumberStyles]::HexNumber)
}
