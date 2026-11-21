#pragma once
#include "ConfigInstance.h"
#include <filesystem>

void SaveConfig(const std::wstring& config);
void LoadConfig(const std::wstring& config);
void AutoSaveConfig();   // salva sozinho (menu fecha / encerra)
std::vector<std::wstring> GetAllConfigs();

// ---------------------------------------------------------------------------
// Pasta de artefatos do processo — config, logs e o .ini da UI.
//
// Existe para que TODOS os arquivos que este programa cria no disco fiquem num
// lugar so', com nome neutro, e nenhum deles caia ao lado do .exe (onde
// `logs.txt` + `imgui.ini` + o binario formam um conjunto facil de reconhecer).
//
// A politica (por que %LOCALAPPDATA% e nao Documents, e por que o nome mudou)
// esta' comentada em ConfigUtilities.cpp, junto da implementacao.
//
// Devolve vazio se LOCALAPPDATA nao existir no ambiente — quem chama deve
// tratar isso e cair num caminho relativo.
// ---------------------------------------------------------------------------
std::filesystem::path PastaDeArtefatos();

extern ConfigInstances Configs;
extern void SetUpConfig();