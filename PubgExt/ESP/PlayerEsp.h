#pragma once
#include "../SharedState.h"
extern void DrawPlayerEsp(const SharedData& data);
void DrawGrenadeEsp(const SharedData& data);
void DrawVehicleEsp(const SharedData& data);
void DrawLootEsp(const SharedData& data);
void DrawCrosshair();

// Nome legivel a partir do nome de CLASSE do UE4.
//   'Item_Weapon_M16A4_C' -> 'M16A4'
//   'WeapBerylM762_C'     -> 'Beryl M762'
// Usado no ESP e tambem no LABEL da aba WORLD (a chave persistida continua
// sendo o nome bruto).
const std::string& CleanEntityName(const std::string& raw, bool firstTokenOnly = false);   // referencia p/ o memo (thread de render)