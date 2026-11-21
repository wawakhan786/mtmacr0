#include "pch.h"
#include <cmath>
#include "ConfigUtilities.h"
#include "Camera.h"
#include "../OS-ImGui/OS-ImGui.h"
#include "../Globals.h"

static ViewMatrix CreateMatrix(Vector3 rot, Vector3 origin) {
	const float DEG_TO_RAD = static_cast<float>(3.14159265358979323846) / 180.f;
	const float radPitch = rot.x * DEG_TO_RAD;
	const float radYaw = rot.y * DEG_TO_RAD;
	const float radRoll = rot.z * DEG_TO_RAD;

	const float SP = sinf(radPitch);
	const float CP = cosf(radPitch);
	const float SY = sinf(radYaw);
	const float CY = cosf(radYaw);
	const float SR = sinf(radRoll);
	const float CR = cosf(radRoll);

	ViewMatrix matrix;
	matrix.matrix[0][0] = CP * CY;
	matrix.matrix[0][1] = CP * SY;
	matrix.matrix[0][2] = SP;
	matrix.matrix[0][3] = 0.f;

	matrix.matrix[1][0] = SR * SP * CY - CR * SY;
	matrix.matrix[1][1] = SR * SP * SY + CR * CY;
	matrix.matrix[1][2] = -SR * CP;
	matrix.matrix[1][3] = 0.f;

	matrix.matrix[2][0] = -(CR * SP * CY + SR * SY);
	matrix.matrix[2][1] = CY * SR - CR * SP * SY;
	matrix.matrix[2][2] = CR * CP;
	matrix.matrix[2][3] = 0.f;

	matrix.matrix[3][0] = origin.x;
	matrix.matrix[3][1] = origin.y;
	matrix.matrix[3][2] = origin.z;
	matrix.matrix[3][3] = 1.f;

	return matrix;
}

Vector2 Camera::WorldToScreen(const MinimalViewInfo& viewinfo, Vector3 world)
{
	// -----------------------------------------------------------------------
	// [GUARDA] PROJECAO NAO-FINITA -> (0,0)
	//
	// O 0xC0000005 do dump RuntimeBroker.exe.15084 nasceu AQUI: uma projecao
	// nao-finita (inf/NaN) atravessou o ESP inteiro porque o unico filtro que
	// existia era `Vector2::IsZero()` (`!x && !y`, que so' barra o ZERO exato) e
	// terminou em `CircleSegmentCounts[INT_MIN]` dentro do ImGui.
	//
	// `(0,0)` e' o MESMO sentinela de "invalido" que esta funcao ja' usava para
	// "atras da camera", e TODOS os call sites (ESP e aimbot) ja' testam ele
	// (`if (screen.IsZero()) continue;` / `if (x == 0 && y == 0) continue;`).
	// Ou seja: rejeitar aqui nao muda comportamento valido nenhum — so' troca
	// "coordenada absurda" por "nao desenha".
	//
	// CASOS TRATADOS, com o porque:
	//   world nao-finito   -> leitura de osso/ator lixo (o log tem loot em
	//                         pos=-796602,-20178,0; osso lixo chega a inf/NaN).
	//   camera nao-finita  -> POV lido de memoria reciclando (troca de mapa).
	//   rotacao nao-finita -> matriz viraria NaN e contaminaria vTransformed.
	//   FOV 0/nao-finito   -> `centrex / tanf(0)` = +inf (divisao por zero) e a
	//                         tela inteira vira inf/NaN. Acontece de verdade:
	//                         Camera::GetCurrentView devolve a struct ZERADA
	//                         quando o BatchRead falha (FOV=0), e o RenderFrame
	//                         grava esse zero por cima do POV bom.
	// -----------------------------------------------------------------------
	if (!std::isfinite(world.x) || !std::isfinite(world.y) || !std::isfinite(world.z))
		return Vector2(0, 0);
	if (!std::isfinite(viewinfo.Location.X) || !std::isfinite(viewinfo.Location.Y) ||
	    !std::isfinite(viewinfo.Location.Z))
		return Vector2(0, 0);
	if (!std::isfinite(viewinfo.Rotation.Pitch) || !std::isfinite(viewinfo.Rotation.Yaw) ||
	    !std::isfinite(viewinfo.Rotation.Roll))
		return Vector2(0, 0);
	if (!std::isfinite(viewinfo.FOV) || fabsf(viewinfo.FOV) < 0.1f)
		return Vector2(0, 0);

	Vector3 Screenlocation(0, 0, 0);
	Vector3 rot = Vector3(viewinfo.Rotation.Pitch, viewinfo.Rotation.Yaw, viewinfo.Rotation.Roll);
	Vector3 campos = Vector3(viewinfo.Location.X, viewinfo.Location.Y, viewinfo.Location.Z);
	const ViewMatrix tempMatrix = CreateMatrix(rot, Vector3(0, 0, 0));

	Vector3 vAxisX(tempMatrix.matrix[0][0], tempMatrix.matrix[0][1], tempMatrix.matrix[0][2]);
	Vector3 vAxisY(tempMatrix.matrix[1][0], tempMatrix.matrix[1][1], tempMatrix.matrix[1][2]);
	Vector3 vAxisZ(tempMatrix.matrix[2][0], tempMatrix.matrix[2][1], tempMatrix.matrix[2][2]);

	Vector3 vDelta = world - campos;
	Vector3 vTransformed = Vector3(Vector3::Dot(vDelta,vAxisY), Vector3::Dot(vDelta,vAxisZ), Vector3::Dot(vDelta,vAxisX));

	if (vTransformed.z < 1.0f)
		return Vector2(0, 0);

	const float FOV_DEG_TO_RAD = static_cast<float>(3.14159265358979323846) / 360.f;
	
	float screenWidth = Gui.Window.Size.x;
	float screenHeight = Gui.Window.Size.y;
	
	if (screenWidth < 10.0f || screenHeight < 10.0f) {
		screenWidth = (float)GetSystemMetrics(SM_CXSCREEN);
		screenHeight = (float)GetSystemMetrics(SM_CYSCREEN);
	}

	float centrex = screenWidth / 2.0f;
	float centrey = screenHeight / 2.0f;

	Screenlocation.x = centrex + vTransformed.x * (centrex / tanf(viewinfo.FOV * FOV_DEG_TO_RAD)) / vTransformed.z;
	Screenlocation.y = centrey - vTransformed.y * (centrex / tanf(viewinfo.FOV * FOV_DEG_TO_RAD)) / vTransformed.z;

	// [GUARDA] A saida tambem pode virar inf/NaN mesmo com entrada finita:
	// basta `vTransformed.z` ser um denormal/valor minusculo (o check acima e'
	// so' `z < 1.0f`, e NaN nao satisfaz esse teste) ou a multiplicacao
	// estourar. Sem esta checagem o lixo seguiria para o DrawList.
	if (!std::isfinite(Screenlocation.x) || !std::isfinite(Screenlocation.y))
		return Vector2(0, 0);

	return Vector2(Screenlocation.x, Screenlocation.y);
}
Vector2 Camera::WorldToScreen(const MinimalViewInfo& viewinfo, UERotator customRotation, Vector3 world)
{
	Vector3 localPos = Vector3(viewinfo.Location.X, viewinfo.Location.Y, viewinfo.Location.Z);
	Vector3 rotation = Vector3(customRotation.Pitch, customRotation.Yaw, customRotation.Roll);
	return WorldToScreenCustomRot(localPos, rotation, viewinfo.FOV, world);
}

Vector2 Camera::WorldToScreenCustomRot(Vector3 localPos, Vector3 customRotation, float fov, Vector3 world)
{
	// [GUARDA] Mesma barreira da WorldToScreen — ver a nota grande la' em cima.
	// Esta variante e' usada pelo aimbot (posicao PREDITA, que soma
	// `Velocity * flyTime` e por isso tambem herda lixo de leitura).
	if (!std::isfinite(world.x) || !std::isfinite(world.y) || !std::isfinite(world.z))
		return Vector2(0, 0);
	if (!std::isfinite(localPos.x) || !std::isfinite(localPos.y) || !std::isfinite(localPos.z))
		return Vector2(0, 0);
	if (!std::isfinite(customRotation.x) || !std::isfinite(customRotation.y) ||
	    !std::isfinite(customRotation.z))
		return Vector2(0, 0);
	if (!std::isfinite(fov) || fabsf(fov) < 0.1f)
		return Vector2(0, 0);

	const ViewMatrix tempMatrix = CreateMatrix(customRotation, Vector3(0, 0, 0));

	Vector3 vAxisX(tempMatrix.matrix[0][0], tempMatrix.matrix[0][1], tempMatrix.matrix[0][2]);
	Vector3 vAxisY(tempMatrix.matrix[1][0], tempMatrix.matrix[1][1], tempMatrix.matrix[1][2]);
	Vector3 vAxisZ(tempMatrix.matrix[2][0], tempMatrix.matrix[2][1], tempMatrix.matrix[2][2]);

	Vector3 vDelta = world - localPos;
	Vector3 vTransformed = Vector3(Vector3::Dot(vDelta, vAxisY), Vector3::Dot(vDelta, vAxisZ), Vector3::Dot(vDelta, vAxisX));

	if (vTransformed.z < 1.0f)
		return Vector2(0, 0);
	if (vTransformed.z / 100.0f > 1000.0f)
		return Vector2(0, 0);

	const float FOV_DEG_TO_RAD = static_cast<float>(3.14159265358979323846) / 360.f;

	float screenWidth = Gui.Window.Size.x;
	float screenHeight = Gui.Window.Size.y;
	if (screenWidth < 10.0f || screenHeight < 10.0f) {
		screenWidth = (float)GetSystemMetrics(SM_CXSCREEN);
		screenHeight = (float)GetSystemMetrics(SM_CYSCREEN);
	}

	float centrex = screenWidth / 2.0f;
	float centrey = screenHeight / 2.0f;

	float screenX = centrex + vTransformed.x * (centrex / tanf(fov * FOV_DEG_TO_RAD)) / vTransformed.z;
	float screenY = centrey - vTransformed.y * (centrex / tanf(fov * FOV_DEG_TO_RAD)) / vTransformed.z;

	// [GUARDA] Saida nao-finita -> mesmo sentinela de "invalido" (ver a nota na
	// WorldToScreen). Sem isto o valor absurdo iria para o DrawList.
	if (!std::isfinite(screenX) || !std::isfinite(screenY))
		return Vector2(0, 0);

	return Vector2(screenX, screenY);
}

MinimalViewInfo Camera::GetCurrentView(uintptr_t managerPtr) {
	MinimalViewInfo view{};
	if (!managerPtr) return view;

	float camFov = 0.0f;
	UEVector camPos = {};
	UERotator camRot = {};

	DriverInterfaceV3::BatchReadEntry camBatch[3] = {
		{ managerPtr + SDK.CameraFov, sizeof(float), &camFov },
		{ managerPtr + SDK.CameraPos, sizeof(UEVector), &camPos },
		{ managerPtr + SDK.CameraRot, sizeof(UERotator), &camRot }
	};

	if (TargetProcess.BatchRead(camBatch, 3)) {
		view.FOV = camFov;
		view.Location = camPos;
		view.Rotation = camRot;
	}
	return view;
}