#pragma once
#include "pch.h"
class AimbotConfig
{
	std::string ConfigName;

public:
	AimbotConfig(const std::string& name)
	{
		ConfigName = name;
	}
	bool Enabled = false;
	int Hotkey = 1; // 0: Left Alt, 1: Right Click, 2: Shift, 3: Mouse 5, 4: Mouse 4
	int TargetBone = 0; // 0: Head, 1: Neck, 2: Chest, 3: Pelvis
	bool AutoBone = false;
	bool VisibilityCheck = true; 
	float FOV = 50.0f;
	float AR_RecoilValue = 100.0f;
	float AR_RecoilYaw = 70.0f;      // Horizontal recoil compensation (0-100%)
	float AR_SmoothValue = 3.0f;
	float SR_baseSmooth = 2.0f;
	int MaxDelta = 10;
	float Aggression = 1.2f;         // Movement speed multiplier (0.5-3.0)
	float DriverScale = 0.30f;       // Mouse-to-pixel conversion (0.10-0.50)
	bool LockOnTarget = true;        // Explicit lock-on mode
	// ---- destravar o eixo vertical (pitch) ----------------------------
	// Quando LIGADO, o aimbot (e o RCS) NAO enviam o componente VERTICAL do
	// movimento de mouse: a mira corrige so' o horizontal (yaw) e o pitch
	// fica inteiro com a mao do jogador.
	//
	// ATENCAO ao significado nesta base de codigo: este cheat NAO escreve
	// `ControlRotation_CP` (0x674) — o offset existe no Globals.h e no
	// diagnostico (DiagChain), mas nunca e' escrito. Quem move a vista sao os
	// InjectMouseMove (SendInput MOUSEEVENTF_MOVE, movimento RELATIVO), tanto
	// no aimbot quanto no RCS. "Eixo vertical" aqui e' portanto o dy dessa
	// injecao — ver o bloco de comentario em SDK/Aimbot.cpp.
	//
	// Default false = comportamento de sempre (os dois eixos).
	bool AimbotUnlockVertical = false;

	// =====================================================================
	// RCS — Recoil Control System
	//
	// O menu expoe APENAS o que o codigo le'. Qualquer campo aqui que nao
	// apareca em `Aimbot::RecoilControl` e' peso morto — foi assim que este
	// config acumulou NOVE campos invisiveis (`RCSScale`, `RCSAlways`,
	// `RCSSens`, `RCSGain`, `RCSLead`, `RCSFF`, `RCSKi`, `RCSAdaptMs`,
	// `RCSFilter`) mais o `RCSCrouchScale`, que tinha slider no menu e nao era
	// lido por ninguem. Todos sairam.
	// =====================================================================
	bool RCS = false;                // Recoil Control System (compensa recuo real)

	// Inverte o sentido da compensacao. So' serve para o caso da puxada sair
	// ao contrario (sinal da formula trocado) — nao e' ajuste de gosto.
	bool RCSInvert = false;

	// ---- feed-forward do acumulador drenado ----------------------------
	// DEGRAUS POR COUNT: quantos GRAUS a vista gira por 1 count de mouse, na
	// FOV 90. E' o UNICO ganho que sobrou: o antigo `RCSInputPitchScale` era o
	// MESMO botao uma segunda vez (a conta final era
	// `counts ∝ InputPitchScale / DegPerCount`), e foi para o codigo como
	// constante de build — ver `kInputPitchScale` em SDK/Aimbot.cpp.
	//
	// Nao precisa calibrar no olho: o botao "Calibrate" MEDE este numero
	// injetando um pulso de mouse conhecido e lendo o quanto a vista girou.
	// Manual, se preferir: mira ainda SOBE -> BAIXE; mira DESCE -> SUBA.
	float RCSDegPerCount = 0.022f;

	// Auto-calibracao continua: durante a rajada, mede o quanto a mira AINDA
	// subiu depois da compensacao e ajusta o ganho sozinho.
	// Default OFF: e' um controlador adaptativo, e controlador adaptativo com
	// leitura ruim faz coisa ruim. Ligue quando quiser que ele se acerte.
	bool RCSAutoCalibrate = false;

	// Limite de counts num unico envio. Trava de seguranca: um read ruim
	// nunca vira um flick.
	int RCSMaxCounts = 60;

	// =====================================================================
	// HUMANIZACAO DO RCS
	//
	// O RCS puxa EXATAMENTE a compensacao teorica, TODO frame, no mesmo
	// instante — e isso e' o oposto do que uma mao faz. Tres caracteristicas
	// humanas sao modeladas aqui, e as tres mudam o PADRAO sem mudar o total
	// compensado (menos a reacao, que e' deliberadamente incompleta):
	//
	//   1. REACAO (`RCSReactionMs`): nos primeiros ~90 ms da rajada a pessoa
	//      ainda esta' percebendo o coice — a compensacao entra em rampa, nao
	//      no talo. E' o "puxao inicial" que todo jogador tem.
	//   2. JITTER (`RCSHumanizeStrength`): cada frame compensa um pouco mais
	//      ou um pouco menos que o teorico. Mao nao tem ganho constante.
	//   3. DITHERING / TREMOR: em alguns frames o envio e' ADIADO (o valor
	//      fica no acumulador e sai no frame seguinte — nada se perde), e um
	//      tremor de fracao de count entra no acumulador. O pacote deixa de
	//      ter tamanho e cadencia perfeitamente iguais entre frames.
	//
	// O QUE ISSO **NAO** RESOLVE, dito sem enfeite: deteccao por PADRAO DE
	// RAJADA no servidor olha a trajetoria agregada da mira (variacao de
	// pitch/yaw ao longo de N ms), e a humanizacao muda o ruido fino, nao a
	// assinatura. Quem quiser atacar aquilo precisa de comportamento — curva
	// de aprendizado, erro de leitura por arma, over-flick ocasional — que e'
	// outro desenho, nao um slider.
	//
	// Defaults: LIGADO com forca 35% e reacao de 90 ms. Desligar devolve o
	// comportamento antigo byte a byte (os tres valores viram 1.0/false).
	// =====================================================================
	bool RCSHumanize = true;

	// Forca da humanizacao (0 = so' a reacao, 100 = bem visivel). Escala o
	// jitter, a chance de adiar o envio e o tremor.
	int RCSHumanizeStrength = 35;    // 0 .. 100

	// Janela de reacao no inicio da rajada (ms). 0 = compensa no talo desde o
	// primeiro frame (comportamento antigo).
	//
	// ATENCAO ao sentido na v2: isto e' a janela TOTAL ate' a compensacao
	// chegar no maximo, e a LATENCIA dentro dela e' SORTEADA por rajada
	// (30% a 70% da janela). Duas rajadas nunca tem a mesma reacao — que era
	// justamente o defeito de ter um 90 ms fixo.
	int RCSReactionMs = 90;          // 0 .. 300

	// ---------------------------------------------------------------------
	// SERVO LAG (`RCSLagMs`) — a correcao que mais importa
	//
	// O QUE ESTAVA ERRADO: o RCS aplicava o valor teorico no MESMO frame em
	// que o recuo era drenado do acumulador. Efeito colateral que ninguem
	// olha: durante a rajada a vista NUNCA sobe. Quem amostrar o pitch de um
	// jogador durante um spray ve DENTE DE SERRA (sobe no tiro, e' puxado de
	// volta); no RCS antigo ve uma LINHA RETA. Isso e' assinatura de
	// telemetria, e nenhuma quantidade de jitter apaga — jitter e' ruido em
	// cima de um sinal que continua reto.
	//
	// O CONSERTO: o valor devido e' ACUMULADO (`s_hzOwedY/X`) e liberado por
	// um filtro de 1a ordem, `alpha = dt / (lag + dt)`. Nada se perde — o
	// total compensado ao longo da rajada continua o mesmo — mas ele sai
	// ESPALHADO ao longo de ~RCSLagMs. A vista sobe a cada tiro e e'
	// corrigida em seguida.
	//
	// O `dt` e' MEDIDO, nao suposto: roda a 144 Hz e a 60 Hz com o mesmo
	// comportamento. Hitch (dt > 50 ms) e' cortado, senao um travamento de
	// 300 ms viraria um flick de 300 ms de compensacao num pacote unico.
	//
	// 0 = desligado, caminho antigo (aplica tudo no frame).
	int RCSLagMs = 45;               // 0 .. 250 ms

	// ---------------------------------------------------------------------
	// RUIDO CORRELACIONADO (`RCSNoiseCounts`)
	//
	// O jitter antigo era ruido BRANCO: ±18% do ganho, media zero, sorteado
	// de novo a cada frame. Ruido branco em cima do valor exato DESAPARECE
	// quando alguem faz media numa janela de 100-300 ms — ou seja, incomodava
	// o jogador (a mira treme) sem esconder nada de quem analisa em janela.
	//
	// Este e' um processo de ORNSTEIN-UHLENBECK: passeia com tempo de
	// correlacao de ~200 ms. O desvio tem INERCIA — nao troca de sinal a cada
	// 7 ms — entao uma media em janela AINDA ve o desvio. E' o que a mao faz:
	// o erro de compensacao de um humano e' um passeio, nao um chiado.
	//
	// Em COUNTS. 0 = sem ruido. Escalado tambem por `RCSHumanizeStrength`,
	// entao forca 0 desliga mesmo com este em 4.0.
	float RCSNoiseCounts = 1.5f;     // 0.0 .. 4.0

    json ToJson()
    {
        json j;
        j[ConfigName][LIT("Enabled")] = Enabled;
        j[ConfigName][LIT("Hotkey")] = Hotkey;
        j[ConfigName][LIT("TargetBone")] = TargetBone;
        j[ConfigName][LIT("AutoBone")] = AutoBone;
        j[ConfigName][LIT("VisibilityCheck")] = VisibilityCheck;
        j[ConfigName][LIT("FOV")] = FOV;
        j[ConfigName][LIT("AR_RecoilValue")] = AR_RecoilValue;
        j[ConfigName][LIT("AR_RecoilYaw")] = AR_RecoilYaw;
        j[ConfigName][LIT("AR_SmoothValue")] = AR_SmoothValue;
        j[ConfigName][LIT("SR_baseSmooth")] = SR_baseSmooth;
        j[ConfigName][LIT("MaxDelta")] = MaxDelta;
        j[ConfigName][LIT("Aggression")] = Aggression;
        j[ConfigName][LIT("DriverScale")] = DriverScale;
        j[ConfigName][LIT("LockOnTarget")] = LockOnTarget;
        // 2a das TRES PARTES do campo novo: declaracao (topo), ToJson (aqui)
        // e FromJson (mais abaixo). Faltar uma faz o valor sumir em silencio.
        j[ConfigName][LIT("AimbotUnlockVertical")] = AimbotUnlockVertical;
        j[ConfigName][LIT("RCS")] = RCS;
        j[ConfigName][LIT("RCSInvert")] = RCSInvert;
        j[ConfigName][LIT("RCSDegPerCount")] = RCSDegPerCount;
        j[ConfigName][LIT("RCSAutoCalibrate")] = RCSAutoCalibrate;
        j[ConfigName][LIT("RCSMaxCounts")] = RCSMaxCounts;
        // Humanizacao do RCS — as TRES partes (declaracao acima, aqui e no
        // FromJson): faltar uma e' o bug que ja' voltou campo pro default em
        // silencio neste projeto.
        j[ConfigName][LIT("RCSHumanize")] = RCSHumanize;
        j[ConfigName][LIT("RCSHumanizeStrength")] = RCSHumanizeStrength;
        j[ConfigName][LIT("RCSReactionMs")] = RCSReactionMs;
        // v2 da humanizacao — servo lag + ruido correlacionado. As TRES
        // partes de novo: declaracao (topo), aqui e no FromJson. Sem o ToJson
        // o slider volta no default a cada boot; sem o FromJson nunca aplica.
        j[ConfigName][LIT("RCSLagMs")] = RCSLagMs;
        j[ConfigName][LIT("RCSNoiseCounts")] = RCSNoiseCounts;

        return j;
    }
    void FromJson(json j)
    {
        // SECAO RENOMEADA "Aimbot" -> "Motion" (hardening-p3): o rotulo
        // "Aimbot" no JSON era autoevidente; "Motion" so' diz o que e' para
        // quem ja' sabe o que o app faz. Config de build antiga continua valendo:
        // se a secao nova nao existe e a legenda sim, o conteudo migra para o
        // nome novo ANTES da leitura — e o proximo autosave grava ja' com
        // "Motion". Passagem por valor: apenas esta classe copia, uma vez por
        // load (o JSON do config tem poucos KB; o custo e' irrelevante).
        if (!j.contains(ConfigName) && j.contains(LIT("Aimbot")))
            j[ConfigName] = std::move(j[LIT("Aimbot")]);
        if (!j.contains(ConfigName))
            return;
        if (j[ConfigName].contains(LIT("Enabled")))
            Enabled = j[ConfigName][LIT("Enabled")];
        if (j[ConfigName].contains(LIT("Hotkey")))
            Hotkey = j[ConfigName][LIT("Hotkey")];
        if (j[ConfigName].contains(LIT("TargetBone")))
            TargetBone = j[ConfigName][LIT("TargetBone")];
        if (j[ConfigName].contains(LIT("AutoBone")))
            AutoBone = j[ConfigName][LIT("AutoBone")];
        if (j[ConfigName].contains(LIT("VisibilityCheck")))
            VisibilityCheck = j[ConfigName][LIT("VisibilityCheck")];
        if (j[ConfigName].contains(LIT("FOV")))
            FOV = j[ConfigName][LIT("FOV")];
        if (j[ConfigName].contains(LIT("AR_RecoilValue")))
            AR_RecoilValue = j[ConfigName][LIT("AR_RecoilValue")];
        if (j[ConfigName].contains(LIT("AR_RecoilYaw")))
            AR_RecoilYaw = j[ConfigName][LIT("AR_RecoilYaw")];
        if (j[ConfigName].contains(LIT("AR_SmoothValue")))
            AR_SmoothValue = j[ConfigName][LIT("AR_SmoothValue")];
        if (j[ConfigName].contains(LIT("SR_baseSmooth")))
            SR_baseSmooth = j[ConfigName][LIT("SR_baseSmooth")];
        if (j[ConfigName].contains(LIT("MaxDelta")))
            MaxDelta = j[ConfigName][LIT("MaxDelta")];
        if (j[ConfigName].contains(LIT("Aggression")))
            Aggression = j[ConfigName][LIT("Aggression")];
        if (j[ConfigName].contains(LIT("DriverScale")))
            DriverScale = j[ConfigName][LIT("DriverScale")];
        if (j[ConfigName].contains(LIT("LockOnTarget")))
            LockOnTarget = j[ConfigName][LIT("LockOnTarget")];
        // 3a das TRES PARTES: a leitura de volta. Sem este bloco o checkbox
        // voltaria desmarcado a cada boot (foi o bug de 15 campos do projeto).
        if (j[ConfigName].contains(LIT("AimbotUnlockVertical")))
            AimbotUnlockVertical = j[ConfigName][LIT("AimbotUnlockVertical")];
        if (j[ConfigName].contains(LIT("RCS")))
            RCS = j[ConfigName][LIT("RCS")];
        if (j[ConfigName].contains(LIT("RCSInvert")))
            RCSInvert = j[ConfigName][LIT("RCSInvert")];
        if (j[ConfigName].contains(LIT("RCSDegPerCount")))
            RCSDegPerCount = j[ConfigName][LIT("RCSDegPerCount")];
        if (j[ConfigName].contains(LIT("RCSAutoCalibrate")))
            RCSAutoCalibrate = j[ConfigName][LIT("RCSAutoCalibrate")];
        if (j[ConfigName].contains(LIT("RCSMaxCounts")))
            RCSMaxCounts = j[ConfigName][LIT("RCSMaxCounts")];
        if (j[ConfigName].contains(LIT("RCSHumanize")))
            RCSHumanize = j[ConfigName][LIT("RCSHumanize")];
        if (j[ConfigName].contains(LIT("RCSHumanizeStrength")))
            RCSHumanizeStrength = j[ConfigName][LIT("RCSHumanizeStrength")];
        if (j[ConfigName].contains(LIT("RCSReactionMs")))
            RCSReactionMs = j[ConfigName][LIT("RCSReactionMs")];
        if (j[ConfigName].contains(LIT("RCSLagMs")))
            RCSLagMs = j[ConfigName][LIT("RCSLagMs")];
        if (j[ConfigName].contains(LIT("RCSNoiseCounts")))
            RCSNoiseCounts = j[ConfigName][LIT("RCSNoiseCounts")];
        // Clamp: forca/reação vem de JSON editado a mao. Forca 0 e' valida
        // (desliga os efeitos sem desmarcar o checkbox) — 0 NAO vira 1.
        if (RCSHumanizeStrength < 0)   RCSHumanizeStrength = 0;
        if (RCSHumanizeStrength > 100) RCSHumanizeStrength = 100;
        if (RCSReactionMs < 0)         RCSReactionMs = 0;
        if (RCSReactionMs > 300)       RCSReactionMs = 300;
        if (RCSLagMs < 0)              RCSLagMs = 0;
        if (RCSLagMs > 250)            RCSLagMs = 250;
        // NaN nao passa em nenhuma comparacao: `x == x` e' falso para NaN, e um
        // JSON com `null`/`1e999` vira NaN aqui. Sem esta linha o servo lag
        // receberia NaN e o filtro inteiro viraria NaN — a mira simplesmente
        // pararia, sem erro e sem log.
        if (!(RCSNoiseCounts == RCSNoiseCounts)) RCSNoiseCounts = 1.5f;
        if (RCSNoiseCounts < 0.0f)     RCSNoiseCounts = 0.0f;
        if (RCSNoiseCounts > 4.0f)     RCSNoiseCounts = 4.0f;
        if (RCSMaxCounts < 1)          RCSMaxCounts = 1;
    }
};
