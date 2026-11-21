# RCS (no-recoil) — arquitetura, matemática e pontos de revisão

**Arquivo principal:** `PubgExt/SDK/Aimbot.cpp` → `Aimbot::RecoilControl()` (566 linhas)
**Apoio:** `PubgExt/SDK/RcsNoise.h` (`RcsServo`, geradores) · `PubgExt/Config/AimbotConfig.h` (parâmetros) · `PubgExt/driver/driver_interface_v3.cpp` (`InjectMouseMove`)
**Executa:** a cada frame da thread de memória (`MemoryLoop`), 144 Hz por padrão

---

## 1. O que este RCS faz — e como difere de "spray pattern"

A maioria dos RCS grava um **padrão de recuo** (uma tabela de deslocamento por tiro) e reproduz. Isso quebra quando muda a arma, o acessório, a postura ou a build do jogo.

Aqui o sensor é o **estado do próprio jogo**: a arma carrega um acumulador de recuo que o motor **consome** ao empurrar a câmera.

```
RecoilValueVector (weapon + 0x…)   → recuo que o jogo AINDA VAI aplicar
VerticalRecovery   (weapon + 0x…)  → componente que o motor devolve
```

A cada frame:

```
drop = valor_anterior − valor_atual        (só a DIMINUIÇÃO conta)
```

Diminuição = o motor consumiu, ou seja, **aquilo acabou de ser aplicado na câmera**. Aumento = tiro novo que ainda não chegou na câmera, e não é movimento dela — ignorar isso é o que impede o RCS de "puxar duas vezes".

Não existe tabela por arma: o número vem do jogo. Trocar de arma, ou de acessório, não exige recalibrar nada.

---

## 2. Cadeia completa (do sensor ao pixel)

```
1. SENSOR        rvv.y, rvv.x, vr  (3 leituras por frame, offsets da arma)
                 sanidade: |valor| > 100 → descarta o frame
                 âncora: primeiro frame após (re)aquisição só guarda o valor

2. DIFERENÇA     dropY = max(prevY − y, 0)
                 dropR = max(prevR − r, 0)
                 dropX = |x| < |prevX| ? prevX − x : 0

3. GRAUS         degY = kInputPitchScale · (dropY − dropR)
                 degX = kInputPitchScale · dropX

4. COUNTS        degPerCount = RCSDegPerCount · (FOV / 90)
                 cy = sinal · degY / degPerCount · autoMul · (AR_RecoilValue/100)
                 cx = −sinal · degX / degPerCount · autoMul · (AR_RecoilYaw/100)

5. HUMANIZAÇÃO   latência sorteada · sub-ganho inicial · servo de drenagem
                 · ruído correlacionado (OU) · cadência em ms reais
                 · acumulador de fração (o que sobra não é perdido)

6. INJEÇÃO       InjectMouseMove(int, int) → KEVENT KMP_OP_MOUSE
                 (divisão em pacotes com fronteira aleatória p/ movimento grande)
```

### 2.1 Por que `degPerCount` depende da FOV

Com FOV menor (mira), o **mesmo** movimento de mouse gira **mais** graus. Sem a correção, a compensação fica fraca exatamente mirando — que é quando ela importa.

O clamp de sanidade era `fov < 20 → 90`, e isso era um bug: no 4x a FOV do PUBG cai para ~10–15°, o clamp jogava o valor certo fora e forçava 90 → `degPerCount` 4× maior → compensação 4× menor. No 4x a mira subia inteira, enquanto red dot/2x/3x funcionavam. Agora só rejeita lixo: `fov < 1 || fov > 170 → 90`.

### 2.2 Auto-calibração (opcional, desligada por padrão)

O sinal de controle é a **própria deriva da câmera**: se a compensação está certa, o pitch não sobe durante a rajada.

```
janela de 500 ms:
    acumula Δpitch  (só com botão pressionado E rvv.y > 0.05 E |Δ| < 5°)
    se acumulado > deadzone  → multiplicador *= 1.04
    se acumulado < −deadzone → multiplicador *= 0.96
    limitado a [0.20×, 5.00×]
```

Dois cuidados que estão no código e importam:

- só acumula com o **acumulador cheio** (`rvv.y > 0`): exclui a fase de recuperação, em que a câmera desce sozinha — contar aquilo derrubaria o ganho sem motivo;
- `s_autoMul` é da **sessão**, não vai para o config: é correção de arma/acessório daquele momento.

**Limite conhecido:** o controlador não separa o mouse do jogador do recuo. Se ele puxar a mira na mão durante a rajada, o controlador lê como sobre-compensação e reduz o ganho.

---

## 3. Humanização — o modelo de mão

Um RCS que puxa exatamente o teórico, em todo frame, no mesmo instante, é **estatisticamente distinguível** de um humano. Este aplica quatro deformações intencionais:

| estágio | o que faz | por quê |
|---|---|---|
| **latência** (`RCSReactionMs`, 90 ms) | nos primeiros ms da rajada a compensação é **zero**; depois sobe por `exp` até 1 | humano não reage no tiro 1 |
| **sub-ganho** (até 12% × força) | a rajada começa compensando **menos** que o teórico e fecha a diferença | mão não acha o ponto de cara |
| **servo de drenagem** | a dívida (`owed`) é liberada por um servo de 1ª ordem com lag em **ms reais** | cadência de correção humana, não "tudo de uma vez" |
| **ruído correlacionado** (OU) | jitter que se mantém entre frames, não ruído branco | ruído branco é fácil de filtrar; ruído correlacionado parece mão |

A garantia que **não pode quebrar**: `owed` preserva o total. O servo muda *quando* o count sai, nunca *quanto* sai ao longo da rajada — fora o sub-ganho, que é sub-compensação deliberada e fecha sozinha.

### 3.1 Latência sorteada por rajada

```c
const float jan = (float)Configs.Aimbot.RCSReactionMs;
s_hzLagMs = jan * (0.30f + 0.40f * RandUnit(rng));   // 30%..70% da janela
```

90 ms fixo é um número, e número fixo é padrão. A janela total continua sendo o slider; o que muda entre rajadas é a divisão entre "parado" e "subindo".

### 3.2 `dt` medido, não suposto

```c
dt = duração real desde o frame anterior (µs)
clamp: [1 ms, 50 ms]
```

Sem isso o servo rodaria com `dt` fantasia: a 60 Hz liberaria a mesma fração por frame que a 144 Hz, ou seja **2,4× mais devagar em tempo de parede**. O lag tem que ser em milissegundos, não em frames.

### 3.3 Soltar o gatilho drena o que sobrou

Se a rajada termina com saldo pendente, sair zerado deixaria uma puxada órfã esperando a próxima rajada — a vista fica torta e o próximo spray começa já compensado. O código drena com o **mesmo servo** e desiste quando o resto é sub-count. E `s_hzAtirandoAntes = false` (e **não** `atirando`): é o **sensor** que define rajada, não o botão; senão a rajada seguinte não veria a borda de subida.

---

## 4. Injeção do movimento

`TargetProcess.driver.InjectMouseMove(x, y)` → backend KEVENT → `KMP_OP_MOUSE` → `Mulclass` (`MouseClassServiceCallback`) com `KeRaiseIrql(DISPATCH_LEVEL)`.

Isso evita `SendInput`, que marca o pacote com `LLMHF_INJECTED` — bit que qualquer hook `WH_MOUSE_LL` lê.

Movimento grande é **subdividido** em pacotes com fronteira aleatória (~1 ms entre eles):

```
abs ≤ kDivergirAcimaDe  → 1 pacote
abs > limite            → N pacotes de tamanho sorteado, restante sempre exato no fim
```

O ponto final é sempre exato — o restante sai no último pacote.

---

## 5. Parâmetros (`AimbotConfig.h`)

| campo | padrão | efeito |
|---|---|---|
| `RCS` | `false` | liga/desliga |
| `AR_RecoilValue` | `100` | ganho vertical (%) |
| `AR_RecoilYaw` | `70` | ganho horizontal (%) |
| `RCSInvert` | `false` | inverte o sentido do eixo vertical |
| `RCSDegPerCount` | `0.022` | sensibilidade base; corrigida por FOV |
| `RCSAutoCalibrate` | `false` | auto-ganho (§2.2) |
| `RCSMaxCounts` | `60` | teto de counts por passo |
| `RCSHumanize` | `true` | liga o modelo de mão |
| `RCSHumanizeStrength` | `35` | 0..100, escala latência/sub-ganho/ruído |
| `RCSReactionMs` | `90` | janela de reação (0..300) |
| `RCSLagMs` | — | lag do servo de drenagem |

---

## 6. Limites conhecidos (para a revisão)

1. **Sensibilidade do jogador entra na conta por uma constante** (`RCSDegPerCount`). Se ele muda a sensibilidade no jogo, a compensação fica proporcionalmente errada — a auto-calibração existe para absorver isso, mas está **desligada por padrão**.
2. **O controlador não distingue mouse humano de recuo** (§2.2): puxada manual durante a rajada é lida como sobre-compensação.
3. **FOV lida da câmera pode ser de um frame atrasado** em relação ao recuo aplicado — a diferença é pequena, mas existe.
4. **Depende de 3 offsets da arma** (`RecoilValueVector`, `VerticalRecovery`, `WeaponProcessor`); offset errado não gera erro alto, gera "RCS fraco/forte" — falha silenciosa por natureza.
5. **Injeção por kernel:** se o `KMP_OP_MOUSE` falhar, cai em `SendInput` (marcado). Não há retry no caminho do kernel.
6. **Uma rajada = uma "mão"**: os acumuladores são zerados na borda de subida, então tiro único e spray longo não compartilham estado — o que é correto para humanização, mas significa que o RCS otimiza a rajada, não o engajamento inteiro.

---

## 7. Checklist de revisão

- [ ] `dropX` usa `|x| < |prevX|` — está correto para recuo lateral (que pode ser negativo)?
- [ ] A âncora de primeiro frame (`s_rcsValid`) pode mascarar um tiro real se a arma for trocada no meio da rajada?
- [ ] `degPerCount` com FOV: a fórmula `base · FOV/90` está na direção certa? (testar 4x/6x/8x)
- [ ] O servo (`RcsServo::Step`) preserva a soma sob `dt` variável? Há prova numérica em `tools/rcs_hand_check.cpp`.
- [ ] Ruído OU: o tempo de correlação está em ms ou em frames?
- [ ] Na borda de soltar, `s_hzAtirandoAntes = false` é o estado certo (e não `atirando`)?
- [ ] `mul` (auto-calibração) pode acumular deriva em sessão longa?
- [ ] `kInputPitchScale` está coerente com a escala real do jogo, ou é um ajuste empírico?
- [ ] O teto `RCSMaxCounts` cobre recuo de LMG (M249/MG3) em spray longo?
- [ ] Se `InjectMouseMove` falhar por N frames, o acumulador `owed` cresce sem limite?

---

## Apêndice A — `Aimbot::RecoilControl()` (código integral)

```cpp
void Aimbot::RecoilControl() {
    // A calibracao tem PRIORIDADE: enquanto ela roda, o RCS fica parado. Ela
    // esta' movendo a vista de proposito, e qualquer puxada em cima disso
    // envenena a medida.
    if (RunSensitivityCalibration()) return;

    if (!Configs.Aimbot.RCS) return;
    if (!EngineInstance) return;

    // O botao esquerdo e' lido AQUI (subiu de lugar): o caminho de "soltar o
    // gatilho" la' embaixo precisa saber se a mao ainda esta' apertando, e o
    // estado da borda (`s_hzAtirandoAntes`) tem que ser mantido nos DOIS
    // caminhos. Se so' o caminho principal atualizasse, sair por ali deixaria
    // `s_hzAtirandoAntes = true` preso e a rajada SEGUINTE nao detectaria a
    // borda de subida â€” ela comecaria sem latencia e sem sub-ganho, que e'
    // exatamente o comportamento que a humanizacao existe para remover.
    const bool atirando = HotkeyHook::IsDown(HotkeyHook::KEY_LBUTTON);
    g_rcsFiring.store(atirando, std::memory_order_relaxed);

    const uint64_t pawn = EngineInstance->AcknowledgedPawn;
    if (pawn < 0x1000000) return;

    // -----------------------------------------------------------------
    // ARMA EQUIPADA: WeaponProcessor -> EquippedWeapons[idx]
    // -----------------------------------------------------------------
    uint64_t weapon = 0;
    {
        uint64_t wp = 0;
        if (!TargetProcess.Read(pawn + SDK.WeaponProcessor, &wp, sizeof(wp), "WP") ||
            !IsCanonicalPtr(wp)) {
            s_rcsValid = false; return;
        }

        uint8_t idx = 0;
        uint64_t ew = 0;
        TargetProcess.Read(wp + SDK.CurrentWeaponIndex, &idx, sizeof(idx), "WIdx");
        TargetProcess.Read(wp + SDK.EquippedWeapons, &ew, sizeof(ew), "EW");
        if (!IsCanonicalPtr(ew) || idx > 4) { s_rcsValid = false; return; }

        TargetProcess.Read(ew + (uint64_t)idx * 8, &weapon, sizeof(weapon), "Weapon");
        if (!IsCanonicalPtr(weapon)) { s_rcsValid = false; return; }
    }

    // -----------------------------------------------------------------
    // OS DOIS ACUMULADORES  (o sensor direto do recuo)
    // -----------------------------------------------------------------
    Vector2 rvv = Vector2(0.f, 0.f);
    float   vr  = 0.0f;

    if (!TargetProcess.Read(weapon + SDK.RecoilValueVector, &rvv, sizeof(rvv), "RVV")) {
        s_rcsValid = false; return;
    }
    TargetProcess.Read(weapon + SDK.VerticalRecovery, &vr, sizeof(vr), "VR");

    // sanidade: o post avisa que read ruim tem que ser descartado
    if (std::abs(rvv.x) > 100.0f || std::abs(rvv.y) > 100.0f || std::abs(vr) > 100.0f) {
        s_rcsValid = false; return;
    }

    // nao atirando / totalmente recuperado -> zera tudo
    if (rvv.y == 0.0f && vr == 0.0f) {
        s_rcsValid = false;
        s_ffRvvY = 0.0f; s_ffVr = 0.0f; s_ffRvvX = 0.0f;

        // -----------------------------------------------------------------
        // SOLTAR O GATILHO â€” drenar o que o servo ainda devia.
        //
        // O servo lag guarda counts ENTRE frames de proposito. Se a rajada
        // acabar com saldo pendente, sair daqui zerado deixaria uma puxada
        // orfa esperando a proxima rajada â€” a vista ficaria torta e o proximo
        // spray comecaria ja' compensado. Drena com o MESMO filtro e desiste
        // quando o resto for sub-count.
        //
        // E' o comportamento certo do ponto de vista de assinatura tambem: um
        // jogador que solta o gatilho AINDA tem a mao na mira por um instante.
        // -----------------------------------------------------------------
        if (Configs.Aimbot.RCSHumanize && Configs.Aimbot.RCSLagMs > 0) {
            const float lagS  = (float)Configs.Aimbot.RCSLagMs * 1e-3f;
            const float dtNom = 1.0f / 144.0f;               // cadencia nominal
            const float mc    = (float)Configs.Aimbot.RCSMaxCounts;

            // O MESMO servo, sem duplicar a formula.
            const float dy = RcsServo::Step(s_hzOwedY, dtNom, lagS, mc);
            const float dx = RcsServo::Step(s_hzOwedX, dtNom, lagS, mc);

            // Mao soltando o gatilho nao injeta tremor.
            s_hzOUy = 0.0f; s_hzOUx = 0.0f;

            int sy = (int)dy;
            int sx = (int)dx;
            // O eixo vertical respeita o mesmo toggle do caminho normal.
            if (Configs.Aimbot.AimbotUnlockVertical) { sy = 0; s_hzOwedY = 0.0f; }
            if (sy != 0 || sx != 0) TargetProcess.driver.InjectMouseMove(sx, sy);

            // FECHA A RAJADA. O estado da borda vira `false` aqui, e NAO
            // `atirando`: e' o SENSOR que define rajada, nao o botao. Se
            // atribuisse `atirando` e o jogador ja' estivesse com o gatilho
            // apertado, a rajada seguinte nunca veria a borda de subida e
            // comecaria sem latencia e sem sub-ganho â€” exatamente o
            // comportamento que a humanizacao existe para remover.
            s_hzAtirandoAntes = false;
            if (!atirando && std::fabs(s_hzOwedY) < 0.5f && std::fabs(s_hzOwedX) < 0.5f) {
                s_hzOwedY = 0.0f; s_hzOwedX = 0.0f;
            }
            return;
        }

        s_hzOwedY = 0.0f; s_hzOwedX = 0.0f;
        s_hzOUy   = 0.0f; s_hzOUx   = 0.0f;
        s_hzAtirandoAntes = false;
        return;
    }

    if (!s_rcsValid) {
        s_ffRvvY = rvv.y; s_ffVr = vr; s_ffRvvX = rvv.x;
        s_rcsValid = true;
        return;                       // ancora e pula 1 frame
    }

    // -----------------------------------------------------------------
    // O DRENO: o que sumiu do acumulador = o que a camera acabou de levar
    //
    // SO a diminuicao conta. Um AUMENTO significa tiro novo que ainda nao
    // chegou na camera â€” nao e' movimento dela.
    // -----------------------------------------------------------------
    float dropY = (s_ffRvvY - rvv.y > 0.0f) ? (s_ffRvvY - rvv.y) : 0.0f;
    float dropR = (s_ffVr   - vr   > 0.0f) ? (s_ffVr   - vr  ) : 0.0f;
    float dropX = (std::abs(rvv.x) < std::abs(s_ffRvvX)) ? (s_ffRvvX - rvv.x) : 0.0f;

    s_ffRvvY = rvv.y; s_ffVr = vr; s_ffRvvX = rvv.x;

    // -----------------------------------------------------------------
    // GRAUS -> COUNTS  (o unico ganho que sobrou)
    //
    //     graus  = kInputPitchScale * (coice - recuperacao)
    //     counts = graus / degPerCount
    //
    // `degPerCount` varia com a FOV de proposito: mirar reduz a FOV, e com a
    // FOV menor o mesmo movimento de mouse gira MAIS graus. Sem essa correcao a
    // compensacao fica fraca justamente mirando â€” que e' quando ela importa.
    // -----------------------------------------------------------------
    const float degY = kInputPitchScale * (dropY - dropR);
    const float degX = kInputPitchScale * dropX;

    float fov = EngineInstance->GetCameraCache().POV.FOV;

    // -----------------------------------------------------------------
    // FAIXA DE FOV  (este clamp era o bug do 4x)
    //
    // Antes era ov < 20 -> 90. No 4x a FOV do PUBG cai pra ~10-15 graus,
    // entao o clamp JOGAVA FORA o valor certo e forcava 90 â€” o que deixava
    // degPerCount 4x maior e a compensacao 4x menor. Resultado: no 4x a mira
    // subia inteira, enquanto red dot / 2x / 3x (acima de 20) funcionavam.
    //
    // Agora so' rejeita o que e' claramente lixo de leitura.
    // -----------------------------------------------------------------
    if (fov < 1.0f || fov > 170.0f) fov = 90.0f;

    float degPerCount = Configs.Aimbot.RCSDegPerCount * (fov / 90.0f);
    if (degPerCount < 0.001f) degPerCount = 0.022f;   // config zerada ou lixo

    // =====================================================================
    // AUTO-CALIBRACAO â€” mede a sobra e corrige o ganho sozinho.
    //
    // O sensor ja' entrega o que o jogo empurrou ("graus"). Se a compensacao
    // estiver CERTA, a camera nao sobe. Curta -> ela sobe. Longa -> ela desce.
    // Entao o sinal de controle e' a PROPRIA DERIVA DA CAMERA, e o alvo e' 0.
    //
    // O que isto NAO consegue separar: o mouse do proprio jogador. Durante a
    // rajada, se ele puxar a mira na mao, o controlador le como
    // sobre-compensacao e reduz o ganho. Por isso:
    //
    //   - so' acumula com o BOTAO ESQUERDO pressionado E o acumulador ainda
    //     cheio (`rvv.y > 0`). Isso exclui a fase de RECUPERACAO, em que a
    //     camera desce sozinha â€” contar aquilo derrubaria o ganho sem motivo;
    //   - passo pequeno (4% por janela de 500 ms) e deriva baixada em deadzone;
    //   - limitado a [0.20x, 5.00x];
    //   - a opcao vem DESLIGADA por padrao.
    //
    // `s_autoMul` e' da SESSAO, nao vai para o config: e' correcao de
    // arma/acessorio daquele momento, e deve reaprender a cada partida.
    // =====================================================================
    // =====================================================================
    // `atirando` e `g_rcsFiring` foram lidos no TOPO do RecoilControl (o
    // caminho de soltar o gatilho precisa deles antes daqui). Nao redeclarar.
    // =====================================================================

    float mul = 1.0f;
    if (Configs.Aimbot.RCSAutoCalibrate) {
        const float camPitch = EngineInstance->GetCameraCache().POV.Rotation.Pitch;
        if (s_autoHadPitch) {
            const float d = AngleDiff(camPitch, s_autoPrevPitch);
            // Deriva absurda = teleporte de camera (troca de mapa, espectador,
            // read ruim), nao recuo. Descarta em vez de somar.
            if (atirando && rvv.y > 0.05f && std::fabs(d) < 5.0f) {
                s_autoAccum += d;
            }
        }
        s_autoPrevPitch = camPitch;
        s_autoHadPitch  = true;

        const auto agora = std::chrono::steady_clock::now();
        if (s_autoWindowStart.time_since_epoch().count() == 0) s_autoWindowStart = agora;

        const int64_t decorrido = std::chrono::duration_cast<std::chrono::milliseconds>(
            agora - s_autoWindowStart).count();

        if (decorrido >= kAutoWindowMs) {
            s_autoWindowStart = agora;
            if (atirando) {
                if (s_autoAccum > kAutoDeadzone) {
                    s_autoMul *= (1.0f + kAutoStep);   // mira SOBE -> puxa mais
                } else if (s_autoAccum < -kAutoDeadzone) {
                    s_autoMul *= (1.0f - kAutoStep);   // mira DESCE -> puxa menos
                }
                if (s_autoMul < kAutoMinMul) s_autoMul = kAutoMinMul;
                if (s_autoMul > kAutoMaxMul) s_autoMul = kAutoMaxMul;
            }
            g_rcsResidual.store(s_autoAccum, std::memory_order_relaxed);
            s_autoAccum = 0.0f;
        }
        mul = s_autoMul;
    } else {
        // Desligada: zera a adaptacao inteira para nao guardar estado velho de
        // uma sessao em que ela esteve ligada.
        s_autoMul = 1.0f;
        s_autoAccum = 0.0f;
        s_autoHadPitch = false;
        g_rcsResidual.store(0.0f, std::memory_order_relaxed);
    }

    const float sgn = Configs.Aimbot.RCSInvert ? -1.0f : 1.0f;

    float cy =  sgn * (degY / degPerCount) * mul * (Configs.Aimbot.AR_RecoilValue / 100.0f);
    float cx = -sgn * (degX / degPerCount) * mul * (Configs.Aimbot.AR_RecoilYaw   / 100.0f);

    // =====================================================================
    // HUMANIZACAO v2 â€” MODELO DE MAO
    //
    // A explicacao longa esta' no topo do arquivo, junto das variaveis
    // (`s_hzOwedY`, `OUUpdate`). Aqui fica so' a conta.
    //
    // ORDEM: o teorico (cy/cx) vira "o que uma mao deveria puxar agora" e
    // passa por quatro estagios antes de virar count inteiro:
    //
    //   teorico -> x ganho humano -> + divida (servo) -> libera fracao
    //           -> + ruido correlacionado -> x cadencia -> acumulador
    //
    // A garantia que NAO pode quebrar: `owed` preserva o total. O servo so'
    // muda QUANDO o count sai, nunca QUANTO sai ao longo da rajada â€” fora o
    // sub-ganho, que e' sub-compensacao DELIBERADA e fecha sozinha.
    // =====================================================================
    const float cap = (float)Configs.Aimbot.RCSMaxCounts;

    float hzRampa  = 1.0f;
    float hzJit    = 1.0f;
    bool  hzAdiar  = false;
    float hzForca  = 0.0f;

    if (Configs.Aimbot.RCSHumanize) {
        hzForca = (float)Configs.Aimbot.RCSHumanizeStrength / 100.0f;
        if (hzForca < 0.0f) hzForca = 0.0f;
        if (hzForca > 1.0f) hzForca = 1.0f;

        const auto hzAgora = std::chrono::steady_clock::now();

        // ---- dt REAL deste frame (medido, nao suposto) ----
        // Sem isto o servo roda com um dt fantasia: a 60 Hz ele liberaria a
        // mesma fracao por frame que a 144 Hz, ou seja 2.4x mais devagar em
        // tempo de parede. O lag tem que ser em MILISSEGUNDOS, nao em frames.
        float dt = 1.0f / 144.0f;
        if (s_hzPrevFrame.time_since_epoch().count() != 0) {
            dt = (float)std::chrono::duration_cast<std::chrono::microseconds>(
                     hzAgora - s_hzPrevFrame).count() * 1e-6f;
        }
        s_hzPrevFrame = hzAgora;
        if (!(dt > 0.001f)) dt = 0.001f;
        if (dt > 0.050f)    dt = 0.050f;   // hitch nao vira flick

        // ---- BORDA DE SUBIDA: cada rajada e' uma mao NOVA ----
        if (atirando && !s_hzAtirandoAntes) {
            s_hzBurstIni = hzAgora;

            // divida, ruido e servo comecam do zero: nada atravessa rajada
            s_hzOwedY = 0.0f; s_hzOwedX = 0.0f;
            s_hzOUy   = 0.0f; s_hzOUx   = 0.0f;

            // LATENCIA sorteada: 30%..70% da janela. Duas rajadas nunca tem a
            // mesma reacao â€” 90 ms fixo e' um numero, e numero fixo e' padrao.
            const float jan = (float)Configs.Aimbot.RCSReactionMs;
            s_hzLagMs = jan * (0.30f + 0.40f * RandUnit(rng));

            // SUB-GANHO inicial: a mao nao acha o ponto de cara. Teto de 12%
            // de recuo NAO compensado no comeco, escalado pela forca.
            const float subMax = 0.12f * hzForca;
            s_hzUnderGain = 1.0f - subMax * (0.55f + 0.45f * RandUnit(rng));
        }
        s_hzAtirandoAntes = atirando;

        // ---- 1) LATENCIA + RECUPERACAO ----
        //
        // `hzRampa` = quanto da compensacao ja' esta' disponivel. Zero durante
        // a latencia (a mao aceita o coice), depois sobe por `exp` ate' 1.
        // A janela e' o que SOBRA de `RCSReactionMs` depois do sorteio, entao
        // o tempo total ate' 100% continua sendo o slider â€” o que muda entre
        // rajadas e' a divisao entre "parado" e "subindo".
        float hzGain = 1.0f;
        float tMsBurst = 0.0f;
        if (atirando) {
            tMsBurst = (float)std::chrono::duration_cast<std::chrono::microseconds>(
                           hzAgora - s_hzBurstIni).count() * 1e-3f;

            if (s_hzLagMs > 0.5f) {
                const float passou = tMsBurst - s_hzLagMs;
                if (passou <= 0.0f) {
                    hzRampa = 0.0f;      // ainda nao reagiu: aceita o coice
                } else {
                    float resto = (float)Configs.Aimbot.RCSReactionMs - s_hzLagMs;
                    if (resto < 15.0f) resto = 15.0f;
                    hzRampa = 1.0f - std::exp(-passou / (resto / 3.0f));
                }
            }

            // ---- 2) SUB-GANHO que fecha conforme a mao aprende o padrao ----
            // tau ~420 ms: no fim de uma rajada longa o ganho ja' e' ~1. O que
            // fica de fora e' o COMECO â€” e o comeco e' o que a analise olha.
            const float aprendeu = 1.0f - std::exp(-tMsBurst / 420.0f);
            hzGain = s_hzUnderGain + (1.0f - s_hzUnderGain) * aprendeu;
            hzGain *= hzRampa;
        }

        // ---- 3) SERVO LAG ----
        //
        // O teorico vira DIVIDA; o filtro de 1a ordem libera uma fracao por
        // frame: `alpha = dt / (lag + dt)`. Com `RCSLagMs = 0`, `alpha = 1` e
        // sai tudo no frame â€” comportamento antigo.
        s_hzOwedY += cy * hzGain;
        s_hzOwedX += cx * hzGain;

        // Trava da divida. Sem isto, um hitch de 50 ms com `cy` no cap vira um
        // pacote de varios caps de uma vez. `4 * cap` e' generoso e ainda
        // impede o flick. O que a trava corta e' a UNICA coisa que o servo
        // pode perder â€” e e' por seguranca, nao por estetica.
        {
            const float owedMax = cap * 4.0f;
            if (s_hzOwedY >  owedMax) s_hzOwedY =  owedMax;
            if (s_hzOwedY < -owedMax) s_hzOwedY = -owedMax;
            if (s_hzOwedX >  owedMax) s_hzOwedX =  owedMax;
            if (s_hzOwedX < -owedMax) s_hzOwedX = -owedMax;
        }

        const float lagS = (float)Configs.Aimbot.RCSLagMs * 1e-3f;
        // O cap e' aplicado DENTRO do servo, antes de debitar a divida: o que
        // a trava corta continua devendo e sai no frame seguinte. Cortar
        // depois de debitar perderia counts em silencio.
        s_hzStepY = RcsServo::Step(s_hzOwedY, dt, lagS, cap);
        s_hzStepX = RcsServo::Step(s_hzOwedX, dt, lagS, cap);

        // ---- 4) RUIDO CORRELACIONADO (passeio, nao chiado) ----
        //
        // tau 200 ms no pitch, 170 no yaw: o desvio tem inercia, entao uma
        // media em janela ainda o ve. Amplitude em counts, escalada pela
        // forca â€” forca 0 desliga mesmo com `RCSNoiseCounts` alto.
        float nY = 0.0f, nX = 0.0f;
        if (atirando && hzForca > 0.0f && Configs.Aimbot.RCSNoiseCounts > 0.0f) {
            const float ampY = Configs.Aimbot.RCSNoiseCounts * hzForca;
            const float ampX = Configs.Aimbot.RCSNoiseCounts * 0.65f * hzForca;
            nY = OUUpdate(s_hzOUy, 0.200f, ampY, dt, rng);
            nX = OUUpdate(s_hzOUx, 0.170f, ampX, dt, rng);
        } else {
            s_hzOUy = 0.0f; s_hzOUx = 0.0f;
        }
        s_hzNoiseY = nY;
        s_hzGainHz = hzGain;

        // ---- 5) CADENCIA IRREGULAR ----
        // O jitter virou FINO (Â±6%, era Â±18%): o trabalho de esconder o padrao
        // agora esta' no servo e no passeio, que sobrevivem a media. Um Â±18%
        // aqui so' estragava a compensacao sem esconder nada.
        if (atirando && hzForca > 0.0f) {
            hzJit   = 1.0f + RandSym(rng) * (0.06f * hzForca);
            hzAdiar = (RandUnit(rng) < 0.10f * hzForca);
        }

        // O que entra no acumulador fracionario.
        cy = (s_hzStepY + nY) * hzJit;
        cx = (s_hzStepX + nX) * hzJit;
    } else {
        // Desligado: caminho antigo e limpeza de TODO o estado, para religar
        // sem herdar divida pendente nem um `s_hzBurstIni` de minutos atras.
        s_hzAtirandoAntes = atirando;
        s_hzOwedY = 0.0f; s_hzOwedX = 0.0f;
        s_hzOUy   = 0.0f; s_hzOUx   = 0.0f;
        s_hzStepY = 0.0f;
        s_hzStepX = 0.0f;
        s_hzGainHz = 1.0f;
        s_hzNoiseY = 0.0f;
        hzRampa = 1.0f;
        hzJit   = 1.0f;
    }

    s_hzRampa = hzRampa;
    s_hzJit   = hzJit;
    s_hzAdiou = hzAdiar;

    if (cy >  cap) cy =  cap;
    if (cy < -cap) cy = -cap;
    if (cx >  cap) cx =  cap;
    if (cx < -cap) cx = -cap;

    g_rcsGainMul.store(mul, std::memory_order_relaxed);
    g_rcsCountsPerDeg.store(degPerCount > 0.0f ? (1.0f / degPerCount) * mul : 0.0f,
                            std::memory_order_relaxed);
    g_rcsDropY.store(dropY, std::memory_order_relaxed);
    g_rcsDropR.store(dropR, std::memory_order_relaxed);
    g_rcsDegY.store(degY, std::memory_order_relaxed);

    // -----------------------------------------------------------------
    // ACUMULADOR FRACIONARIO  (o post: sem isto perde 20-40% em sens baixa)
    // -----------------------------------------------------------------
    static float accX = 0.0f, accY = 0.0f;

    // ---------------------------------------------------------------------
    // DESTRAVAR O EIXO VERTICAL â€” este e' o SEGUNDO ponto que move a vista.
    //
    // O RCS e' quem mais puxa o pitch (ele existe justamente para compensar o
    // recuo que sobe a mira). Se a opcao valesse so' no MoveMouseTowards, o
    // recuo voltaria a subir a mira por aqui e o usuario acharia que o toggle
    // nao funciona â€” por isso o gate e' aplicado AQUI TAMBEM.
    //
    // `cy` e' a puxada vertical (ja' em contagens de mouse -> dy da injecao) e
    // `accY` e' o acumulador fracionario dela. Os dois zeram juntos: zerar so'
    // `cy` deixaria a fracao guardada para o proximo frame, que da' no mesmo
    // que nao ter zerado.
    //
    // O sensor continua LENDO e atualizando `s_ffRvvY` normalmente (nao
    // congela nada), entao desligar o toggle retoma a compensacao sem pico.
    // O eixo horizontal (`cx` / `accX`) segue intocado.
    // ---------------------------------------------------------------------
    if (Configs.Aimbot.AimbotUnlockVertical) {
        cy   = 0.0f;
        accY = 0.0f;
        // A DIVIDA do servo tambem zera. Sem isto a divida vertical ficaria
        // crescendo a cada frame (o teorico continua entrando em `owedY`) e a
        // trava de `4 * cap` comecaria a cortar. Zerar aqui deixa o eixo
        // vertical literalmente com a mao do jogador, que e' o que o toggle
        // promete.
        s_hzOwedY = 0.0f;
    }

    accY += cy;
    accX += cx;

    // -----------------------------------------------------------------
    // TREMOR BRANCO â€” REMOVIDO na v2.
    //
    // Aqui somava `+-0.35 count` de ruido BRANCO no acumulador. Ele fazia
    // companhia ao jitter branco e tinha o mesmo defeito: media zero, novo a
    // cada frame, some quando alguem analisa em janela. O papel dele â€”
    // "impedir que a sequencia de inteiros injetados seja periodica" â€” agora
    // e' feito pelo ruido CORRELACIONADO do estagio 4, que sobrevive a media
    // e ao mesmo tempo nao precisa ser cortado para nao virar pacote.
    // ---------------------------------------------------------------------

    int iy = 0, ix = 0;

    // -----------------------------------------------------------------
    // DITHERING â€” o envio adiado.
    //
    // `hzAdiar` foi decidido acima (junto do jitter). Adiar = NAO extrair a
    // parte inteira neste frame: o acumulador guarda tudo e o pacote sai no
    // proximo, maior. E' o oposto de "perder movimento" â€” o total compensado
    // continua exato, o que muda e' o instante.
    //
    // O teto e' necessario: dois frames adiados seguidos com `cy` no cap
    // somariam 2x o cap num pacote unico. `2 * cap` e' o maximo plausivel
    // (um frame de atraso), e o driver ainda subdivide em pacotes de 3-9 px.
    // -----------------------------------------------------------------
    if (hzAdiar) {
        const float teto = (float)Configs.Aimbot.RCSMaxCounts * 2.0f;
        if (accY >  teto) accY =  teto;
        if (accY < -teto) accY = -teto;
        if (accX >  teto) accX =  teto;
        if (accX < -teto) accX = -teto;
    } else {
        iy = (int)accY; accY -= (float)iy;
        ix = (int)accX; accX -= (float)ix;
    }

    // Publica o que SAIU de fato (os inteiros, depois do acumulador
    // fracionario) â€” e' o numero que o menu mostra, nao o `cy`/`cx` teoricos.
    g_rcsCy.store(iy, std::memory_order_relaxed);
    g_rcsCx.store(ix, std::memory_order_relaxed);

    // ---- humanizacao v2: o que o painel "RCS ao vivo" mostra ----
    // `owedY` e' o numero que denuncia o servo funcionando: durante a rajada
    // ele fica ALTO e serpenteia; se ficar cravado em 0 com `RCSLagMs` > 0, o
    // servo nao esta' sendo alimentado (sensor morto ou toggle errado).
    g_rcsLagMs .store(Configs.Aimbot.RCSHumanize ? s_hzLagMs  : 0.0f, std::memory_order_relaxed);
    g_rcsGainHz.store(Configs.Aimbot.RCSHumanize ? s_hzGainHz : 1.0f, std::memory_order_relaxed);
    g_rcsOwedY .store(Configs.Aimbot.RCSHumanize ? s_hzOwedY  : 0.0f, std::memory_order_relaxed);
    g_rcsStepY .store(Configs.Aimbot.RCSHumanize ? s_hzStepY  : 0.0f, std::memory_order_relaxed);
    g_rcsNoiseY.store(Configs.Aimbot.RCSHumanize ? s_hzNoiseY : 0.0f, std::memory_order_relaxed);

    {
        static auto lastLog = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        // 1x/s so' com `Debug logs` ligado (aba DEBUG). Antes rodava SEMPRE:
        // eram 2 linhas por segundo ([RCS] + [CAM]) em toda sessao, ~7 mil
        // linhas por hora de jogo. Erro/aviso continua saindo sem a flag.
        if (Configs.Survivor.DebugLogs &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - lastLog).count() > 1000) {
            lastLog = now;
            LOG("[RCS] weapon=0x%llX rvv=(%.3f,%.3f) vr=%.3f | dropY=%.4f dropR=%.4f dropX=%.4f | "
                "degY=%.3f degX=%.3f | fov=%.1f dpc=%.4f (%.1f counts/grau) mul=%.2f -> (%d,%d) | "
                "hz: rampa=%.2f jit=%.2f adiou=%d | "
                "mao: lat=%.0fms ganho=%.3f passo=%.2f devendo=%.2f ruido=%.2f | "
                "inj: k=%llu f=%llu err=%u\n",
                (unsigned long long)weapon, rvv.x, rvv.y, vr,
                dropY, dropR, dropX, degY, degX, fov, degPerCount,
                (degPerCount > 0.0f ? 1.0f / degPerCount : 0.0f), mul, ix, iy,
                s_hzRampa, s_hzJit, (int)s_hzAdiou,
                s_hzLagMs, s_hzGainHz, s_hzStepY, s_hzOwedY, s_hzNoiseY,
                (unsigned long long)g_mouseInjOk.load(std::memory_order_relaxed),
                (unsigned long long)g_mouseInjFail.load(std::memory_order_relaxed),
                (unsigned)g_mouseInjLastErr.load(std::memory_order_relaxed));

            // -------------------------------------------------------------
            // CAMERA: CACHE x FRESCA, NA MESMA LINHA.
            //
            // O aimbot mira com `Engine::CameraEntry` (via GSharedData) â€” um
            // cache escrito por `RefreshViewMatrix` na thread de memoria.
            // O ESP NAO usa ele: rele' a camera por conta propria com
            // `Camera::GetCurrentView`, na thread de render.
            //
            // Um cache parado explica sozinho "ESP desenha, aimbot morto": o
            // aimbot projeta contra uma camera que nao anda e acha que esta'
            // tudo certo, porque o FOV dela continua plausivel (os diagnosticos
            // existentes so' olham o FOV â€” e 80.0 passa em qualquer guarda).
            //
            // As duas leem OS MESMOS offsets, entao:
            //   diferentes -> e' cache velho (RefreshViewMatrix/frequencia)
            //   iguais E parados com voce jogando -> nao e' cache: e' o
            //                PlayerCameraManager/offset, ou o jogo nao publica
            //   mudando     -> esta' tudo vivo, e o problema do aimbot e' outro
            //
            // 1x/s, junto do log do RCS que ja' existe â€” sem spam novo.
            // -------------------------------------------------------------
            if (EngineInstance) {
                const MinimalViewInfo fresca = Camera::GetCurrentView(EngineInstance->PlayerCameraManager);
                LOG("[CAM] cache=(%.4f,%.4f) fov=%.1f | fresca=(%.4f,%.4f) fov=%.1f | %s\n",
                    GSharedData.CameraCache.POV.Rotation.Pitch,
                    GSharedData.CameraCache.POV.Rotation.Yaw,
                    GSharedData.CameraCache.POV.FOV,
                    fresca.Rotation.Pitch, fresca.Rotation.Yaw, fresca.FOV,
                    (std::fabs(fresca.Rotation.Pitch - GSharedData.CameraCache.POV.Rotation.Pitch) > 0.01f ||
                     std::fabs(fresca.Rotation.Yaw   - GSharedData.CameraCache.POV.Rotation.Yaw)   > 0.01f)
                        ? "DIFEREM -> cache atrasado em relacao a leitura viva"
                        : "iguais");
            }
        }
    }

    if (ix == 0 && iy == 0) return;
    TargetProcess.driver.InjectMouseMove(ix, iy);
}
```

## Apêndice B — `RcsServo` (o servo de drenagem)

```cpp
// `lag` = 0 significa "sem suavidade": emite tudo no frame e zera a divida,
// byte a byte o comportamento antigo.
// ===========================================================================
namespace RcsServo {

inline float Step(float& owed, float dt, float lagS, float cap) {
    if (!(lagS > 0.0005f)) {
        // Caminho antigo: aplica o teorico inteiro neste frame.
        float step = owed;
        if (step >  cap) step =  cap;
        if (step < -cap) step = -cap;
        owed = 0.0f;
        return step;
    }
    if (!(dt > 0.0f)) return 0.0f;

    const float alpha = dt / (lagS + dt);
    float step = owed * alpha;

    // O cap e' aplicado ANTES de debitar: o que a trava corta continua
    // devendo e sai no frame seguinte. Cortar depois de debitar perderia
    // counts em silencio â€” e' a diferenca entre "o servo esta' limitado" e
    // "o servo esta' roubando".
    if (step >  cap) step =  cap;
    if (step < -cap) step = -cap;
    owed -= step;
    return step;
}

} // namespace RcsServo
```

## Apêndice C — parâmetros (`AimbotConfig.h`, trecho do RCS)

```cpp
	// =====================================================================
	// RCS â€” Recoil Control System
	//
	// O menu expoe APENAS o que o codigo le'. Qualquer campo aqui que nao
	// apareca em `Aimbot::RecoilControl` e' peso morto â€” foi assim que este
	// config acumulou NOVE campos invisiveis (`RCSScale`, `RCSAlways`,
	// `RCSSens`, `RCSGain`, `RCSLead`, `RCSFF`, `RCSKi`, `RCSAdaptMs`,
	// `RCSFilter`) mais o `RCSCrouchScale`, que tinha slider no menu e nao era
	// lido por ninguem. Todos sairam.
	// =====================================================================
	bool RCS = false;                // Recoil Control System (compensa recuo real)

	// Inverte o sentido da compensacao. So' serve para o caso da puxada sair
	// ao contrario (sinal da formula trocado) â€” nao e' ajuste de gosto.
	bool RCSInvert = false;

	// ---- feed-forward do acumulador drenado ----------------------------
	// DEGRAUS POR COUNT: quantos GRAUS a vista gira por 1 count de mouse, na
	// FOV 90. E' o UNICO ganho que sobrou: o antigo `RCSInputPitchScale` era o
	// MESMO botao uma segunda vez (a conta final era
	// `counts âˆ InputPitchScale / DegPerCount`), e foi para o codigo como
	// constante de build â€” ver `kInputPitchScale` em SDK/Aimbot.cpp.
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
	// instante â€” e isso e' o oposto do que uma mao faz. Tres caracteristicas
	// humanas sao modeladas aqui, e as tres mudam o PADRAO sem mudar o total
	// compensado (menos a reacao, que e' deliberadamente incompleta):
	//
	//   1. REACAO (`RCSReactionMs`): nos primeiros ~90 ms da rajada a pessoa
	//      ainda esta' percebendo o coice â€” a compensacao entra em rampa, nao
	//      no talo. E' o "puxao inicial" que todo jogador tem.
	//   2. JITTER (`RCSHumanizeStrength`): cada frame compensa um pouco mais
	//      ou um pouco menos que o teorico. Mao nao tem ganho constante.
	//   3. DITHERING / TREMOR: em alguns frames o envio e' ADIADO (o valor
	//      fica no acumulador e sai no frame seguinte â€” nada se perde), e um
	//      tremor de fracao de count entra no acumulador. O pacote deixa de
	//      ter tamanho e cadencia perfeitamente iguais entre frames.
	//
	// O QUE ISSO **NAO** RESOLVE, dito sem enfeite: deteccao por PADRAO DE
	// RAJADA no servidor olha a trajetoria agregada da mira (variacao de
	// pitch/yaw ao longo de N ms), e a humanizacao muda o ruido fino, nao a
	// assinatura. Quem quiser atacar aquilo precisa de comportamento â€” curva
	// de aprendizado, erro de leitura por arma, over-flick ocasional â€” que e'
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
	// (30% a 70% da janela). Duas rajadas nunca tem a mesma reacao â€” que era
	// justamente o defeito de ter um 90 ms fixo.
	int RCSReactionMs = 90;          // 0 .. 300

	// ---------------------------------------------------------------------
	// SERVO LAG (`RCSLagMs`) â€” a correcao que mais importa
	//
	// O QUE ESTAVA ERRADO: o RCS aplicava o valor teorico no MESMO frame em
	// que o recuo era drenado do acumulador. Efeito colateral que ninguem
```
