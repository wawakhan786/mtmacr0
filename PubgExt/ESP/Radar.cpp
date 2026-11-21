#include "pch.h"
#include "Radar.h"
#include "DrawSanity.h"   // [DRAW SANITY] barreira de coordenada do radar
#include "../Config/ConfigUtilities.h"
#include "../OS-ImGui/OS-ImGui.h"
#include <string>

ImVec2 Radar::RotatePoint(const Vector3& localPos, const Vector3& enemyPos,
                         float localYaw, float radarX, float radarY, float radarW,
                         float radarH, float range) {
    float dx = enemyPos.x - localPos.x;
    float dy = enemyPos.y - localPos.y;

    // Rotate based on local player's view angle
    // PUBG: Yaw 0 is X+
    float yawRad = (localYaw) * (float)(M_PI / 180.0f);
    float cosY = cosf(-yawRad);
    float sinY = sinf(-yawRad);

    float rotX = dx * cosY - dy * sinY;
    float rotY = dx * sinY + dy * cosY;

    // Scale (Range in meters, PUBG units are CM)
    float rangeUnits = range * 100.0f;
    float scale = (radarW / 2.0f) / rangeUnits;

    rotX *= scale;
    rotY *= scale;

    float centerX = radarX + radarW / 2.0f;
    float centerY = radarY + radarH / 2.0f;

    float finalX = centerX + rotY; 
    float finalY = centerY - rotX;

    float halfW = radarW / 2.0f - 5.0f;
    float halfH = radarH / 2.0f - 5.0f;
    finalX = fmaxf(centerX - halfW, fminf(finalX, centerX + halfW));
    finalY = fmaxf(centerY - halfH, fminf(finalY, centerY + halfH));

    return ImVec2(finalX, finalY);
}

void Radar::DrawArrow(ImDrawList* drawList, ImVec2 center, float yaw, float size, ImU32 color) {
    float angle = yaw * (float)(M_PI / 180.0f);

    float tipLen = size * 1.5f;
    float baseLen = size * 0.8f;
    float baseWidth = size * 0.6f;

    ImVec2 tip(center.x + tipLen * sinf(angle), center.y - tipLen * cosf(angle));
    
    float perpAngle = angle + (float)(M_PI / 2.0f);
    ImVec2 baseLeft(
        center.x - baseLen * sinf(angle) + baseWidth * sinf(perpAngle),
        center.y + baseLen * cosf(angle) - baseWidth * cosf(perpAngle));
    ImVec2 baseRight(
        center.x - baseLen * sinf(angle) - baseWidth * sinf(perpAngle),
        center.y + baseLen * cosf(angle) + baseWidth * cosf(perpAngle));

    drawList->AddTriangleFilled(tip, baseLeft, baseRight, color);
    drawList->AddTriangle(tip, baseLeft, baseRight, IM_COL32(0, 0, 0, 200), 1.0f);
}

void Radar::Render(const SharedData& data) {
    auto& config = Configs.Survivor;
    if (!config.Radar) return;

    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    
    // Posicao vinda do config: assim da' pra encaixar o radar em cima do
    // minimapa do jogo (cada um usa uma escala/posicao de HUD diferente).
    float x = Configs.Survivor.RadarX;
    float y = Configs.Survivor.RadarY;
    float w = config.RadarSize;
    float h = config.RadarSize;

    // [DRAW SANITY] RadarX/RadarY/RadarSize vem do config (JSON). Os tres
    // viram AddRectFilled/AddRect com canto ARREDONDADO nas linhas abaixo —
    // exatamente o caminho que estoura com NaN/+-inf dentro do ImGui.
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(w) || !std::isfinite(h))
        return;

    drawList->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), IM_COL32(0, 0, 0, 120), 5.0f);
    drawList->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), IM_COL32(255, 255, 255, 50), 5.0f, 0, 1.0f);

    float centerX = x + w / 2.0f;
    float centerY = y + h / 2.0f;
    drawList->AddLine(ImVec2(x, centerY), ImVec2(x + w, centerY), IM_COL32(255, 255, 255, 30));
    drawList->AddLine(ImVec2(centerX, y), ImVec2(centerX, y + h), IM_COL32(255, 255, 255, 30));

    drawList->AddCircleFilled(ImVec2(centerX, centerY), 3.0f, IM_COL32(0, 255, 0, 255));

    Vector3 localPos = Vector3(data.CameraCache.POV.Location.X, data.CameraCache.POV.Location.Y, data.CameraCache.POV.Location.Z);

    // -----------------------------------------------------------------------
    // NORTH-UP  (estilo minimapa do PUBG)
    //
    // O radar girava junto com a camera: quando voce virava, TODOS os pontinhos
    // rodavam em volta do centro. No minimapa do jogo e' o contrario — o mapa
    // fica parado (norte pra cima) e so' o INDICADOR de direcao gira.
    //
    // Entao: os pontos sao posicionados com yaw = 0 (norte fixo), e o yaw real
    // e' usado so' pra desenhar o cone de visao.
    // -----------------------------------------------------------------------
    const float yawReal = data.CameraCache.POV.Rotation.Yaw;
    const float localYaw = Configs.Survivor.RadarNorthUp ? 0.0f : yawReal;

    // Recorta tudo ao retangulo do radar (o ImGui nao recorta sozinho, entao
    // o circulo da zona azul — que e' enorme — vazava por cima da tela toda).
    drawList->PushClipRect(ImVec2(x, y), ImVec2(x + w, y + h), true);

    // -----------------------------------------------------------------------
    // ZONA AZUL / ZONA SEGURA  (GameState)
    //
    // O GameState da' o centro e o raio em unidades de mundo (UU). No radar a
    // escala e'  half/RadarRange , entao o raio vira  radius * scale.
    // -----------------------------------------------------------------------
    if (data.HasGameState) {
        float half  = w / 2.0f;
        float range = config.RadarRange > 1.0f ? config.RadarRange : 100000.0f;
        float scale = half / range;

        auto drawZone = [&](const Vector3& c, float radiusUU, ImU32 col, float thick) {
            if (radiusUU <= 1.0f) return;
            ImVec2 p = RotatePoint(localPos, c, localYaw, x, y, w, h, range);
            float r = radiusUU * scale;
            if (r < 1.0f || r > 5000.0f) return;
            drawList->AddCircle(p, r, col, 96, thick);
        };

        // zona segura (branco) primeiro, zona azul (azul) por cima
        drawZone(Vector3(data.SafetyZonePos.x, data.SafetyZonePos.y, data.SafetyZonePos.z),
                 data.SafetyZoneRadius, IM_COL32(255, 255, 255, 170), 1.5f);
        drawZone(Vector3(data.BlueZonePos.x, data.BlueZonePos.y, data.BlueZonePos.z),
                 data.BlueZoneRadius, IM_COL32(80, 160, 255, 200), 2.0f);
    }

    for (auto& entity : data.Actors) {
        // `isStale` junto: o radar nao pode manter um ponto na ultima posicao
        // conhecida de quem ja' saiu (mesmo motivo do filtro do ESP de tela).
        if (!entity || entity->isDie || entity->isStale || entity->PlayerType == 0) continue;
        if (entity->Class == data.AcknowledgedPawn) continue;

        Vector3 enemyPos = entity->GetPosition();
        // [DRAW SANITY] O RotatePoint ate' clampa NaN (fminf/fmaxf devolvem o
        // operando valido), mas o resultado seria um ponto "colado" na borda do
        // radar, que e' pior que nao desenhar: mentira visual. Posicao de mundo
        // lixo (ver [LOOT] pos=-796602,-20178,0 no log da partida que crashou)
        // e' descartada aqui.
        if (!DrawSanity::MundoOk(enemyPos, "radar")) continue;

        ImVec2 radarPos = RotatePoint(localPos, enemyPos, localYaw, x, y, w, h, config.RadarRange);
        if (!DrawSanity::TelaOk(radarPos.x, radarPos.y, "radar", enemyPos)) continue;

        ImU32 color = IM_COL32(255, 0, 0, 200); 
        if (entity->PlayerType == 1) color = IM_COL32(255, 165, 0, 200); // Orange for bots

        drawList->AddCircleFilled(radarPos, 4.0f, color);
        
        // Directional Line (Placeholder for viewangles if we find them later, currently dots)
    }

    // -----------------------------------------------------------------------
    // INDICADOR DE DIRECAO (o "cone" do minimapa)
    //
    // Em north-up o mapa nao gira, entao a direcao que voce olha tem que
    // aparecer aqui.
    //
    // IMPORTANTE: a seta e' desenhada PASSANDO PELA MESMA RotatePoint que
    // posiciona os pontinhos. Antes eu fazia trigonometria propria e as duas
    // podiam divergir; usando a mesma funcao a seta nunca mais pode apontar
    // diferente do resto do radar.
    //
    // O angulo e' (yaw + offset): a matematica esta' verificada contra
    // Camera::CreateMatrix, entao qualquer desvio residual e' convencao de
    // eixo do mapa — e o offset resolve isso no jogo, sem recompilar.
    // -----------------------------------------------------------------------
    {
        const float ang = (yawReal + Configs.Survivor.RadarArrowOffset) * 3.14159265f / 180.0f;

        // forward em mundo (mesma convencao do Camera::CreateMatrix):
        //     forward = (cos(yaw), sin(yaw))
        // 50 m a frente cai dentro do radar em qualquer range razoavel
        Vector3 frente(
            localPos.x + cosf(ang) * 5000.0f,
            localPos.y + sinf(ang) * 5000.0f,
            localPos.z);

        ImVec2 ponta = RotatePoint(localPos, frente, localYaw, x, y, w, h, config.RadarRange);
        ImVec2 centro(centerX, centerY);

        drawList->AddLine(centro, ponta, IM_COL32(0, 255, 255, 220), 2.0f);

        // seta na ponta (perpendicular a' linha)
        ImVec2 dirv(ponta.x - centro.x, ponta.y - centro.y);
        const float len = sqrtf(dirv.x * dirv.x + dirv.y * dirv.y);
        if (len > 1.0f) {
            dirv.x /= len; dirv.y /= len;
            const float perp = 6.0f, recuo = 10.0f;
            ImVec2 base(ponta.x - dirv.x * recuo, ponta.y - dirv.y * recuo);
            ImVec2 esq(base.x - dirv.y * perp, base.y + dirv.x * perp);
            ImVec2 dir(base.x + dirv.y * perp, base.y - dirv.x * perp);
            drawList->AddTriangleFilled(ponta, esq, dir, IM_COL32(0, 255, 255, 240));
        }
    }

    drawList->PopClipRect();
}
