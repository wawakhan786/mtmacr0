#pragma once
// ---------------------------------------------------------------------------
// VERSAO DO APP — FONTE UNICA.
//
// Existe porque a versao estava escrita a mao em DOIS lugares (o chip do menu e
// a tabela de traducao) e o nome dos arquivos nao tinha versao nenhuma. Resultado:
// tres numeros diferentes na mesma pasta, e nenhum jeito de saber qual build o
// usuario esta rodando quando ele manda um print.
//
// Agora: muda aqui, e o chip, o build e o nome do arquivo acompanham.
//   - `MTM_VERSAO`     -> "2.1"   (texto puro, para o nome do arquivo)
//   - `MTM_VERSAO_TAG` -> "v2.1"  (com o "v", para o chip do menu)
//
// O `tools\build_all.ps1` le' este arquivo por regex e gera
//   mtmacr0_v2.1.exe  /  mtmacr0_public_v2.1.exe
// ---------------------------------------------------------------------------
#define MTM_VERSAO      "2.1"
#define MTM_VERSAO_TAG  "v" MTM_VERSAO
