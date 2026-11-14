# Arquitetura do Pestana Estúdio (iOS)

Documento de referência para quem implementa (dev), quem testa (QA) e quem aprova (PO).
Qualquer código novo segue este modelo. Desvios só com aprovação do PO, registrados aqui.

## 1. Visão geral

**Por funcionalidade + MVVM**, com SOLID aplicado onde traz ganho real (sem cerimônia).

```
Afinador/
  App/                  entrada do app, abas, ciclo de vida (segundo plano)
  Features/
    Tuner/              TunerView, TunerEngine
    Chords/             ChordsView, Chords
    Metronome/          MetronomeView, Metronome
    Studio/
      Views/            telas e componentes do Estúdio
      Engine/           StudioEngine (coordena) + serviços (gravação, reprodução, afinação automática…)
      Models/           StudioModels (contrato Codable)
    Practice/           Treino (player, separação Demucs)
    Loop/               LoopView, LooperEngine
  Core/
    Audio/              sessão de áudio (AudioSessionService), cadeia de efeitos, medidores
    AudioUnits/         wrappers Objective-C++ finos (PestanaGuitarAU, PestanaLooperAU, PestanaNoiseAU)
    DSP/                C++20 puro: GuitarFXCore, LooperCore, NoiseReducer (sem APIs da Apple)
    Storage/            ProjectStore (protocolo) e implementação em disco
  Shared/
    UI/                 cores, componentes reutilizáveis (LevelBar, seções, sliders)
  Models/               modelo Core ML baixado no CI (não mexer no caminho)
tests/                  testes C++ (CMake + CTest), rodam no CI Linux a cada push
```

## 2. Regras de dependência

1. `App → Features → Core/Shared`. **Core nunca importa Features.** Shared/UI não conhece motores.
2. **Features não se conhecem diretamente.** Quando uma precisa da outra (ex.: Treino manda a música
   para o Estúdio), a dependência é um **protocolo** declarado em Core (ex.: `StudioImporting`) e a
   implementação é injetada pelo App (inversão de dependência).
3. **Core/DSP** é C++20 puro: nada de `<AVFoundation>`, Objective-C ou Swift. Tudo nele tem teste em `tests/`.
4. **Core/AudioUnits** só traduz chamadas (Swift ↔ C++) e o render; nenhuma regra de negócio ali.

## 3. MVVM

- **View** (SwiftUI): só apresentação e intenção do usuário. Sem `AVAudioEngine`, sem disco.
- **ViewModel = Engine** (`ObservableObject`, `@MainActor`): estado publicado e orquestração.
- **Serviços**: fazem o trabalho pesado (gravar, exportar, processar, ler/gravar disco). O Engine os
  coordena; não reimplementa o que um serviço faz.

## 4. SOLID, na medida

- **S** — um arquivo/tipo, uma responsabilidade. Alvo: **até ~600 linhas por arquivo** Swift. O
  `StudioEngine` vira coordenador; gravação, reprodução/transporte, afinação automática e roteamento
  de entrada vão para serviços/arquivos próprios.
- **O** — a cadeia de efeitos é uma lista de blocos; um efeito novo entra como bloco, sem mexer nos outros.
- **L** — implementações de protocolo cumprem o contrato inteiro (sem `fatalError("não suportado")`).
- **I** — protocolos pequenos e de borda: `AudioSessionManaging`, `ProjectStore`, `StudioImporting`.
  **Não** criar protocolo para tudo: só onde há troca de implementação ou teste.
- **D** — Engines recebem serviços/protocolos no `init` (com padrão de produção), não criam singletons escondidos.

## 5. Tempo real (thread de áudio)

Na thread de áudio (render de AU, `process()` do DSP): **sem alocar memória, sem lock bloqueante,
sem Objective-C/Swift, sem log, sem E/S**. Comunicação com a principal só por filas sem lock (SPSC),
`std::atomic` ou `try_lock` (nunca `lock`). Memória é alocada e liberada na thread principal (ou em
fila de fundo, devolvida à principal).

## 6. Estilo

- Identificadores em inglês (como hoje); **comentários e textos da interface em português do Brasil**.
- Só APIs do iOS 16. Views grandes quebradas em propriedades (evita "unable to type-check").
- Sem `!` forçado, `try!` ou `fatalError` em código de produção.

## 7. Testes e CI

- `tests/` com CMake/CTest: `LooperCore` (modo manual e automático, camadas, memória), `NoiseReducer`
  (transparência, redução, troca de perfil), `GuitarFXCore` (drive sustenta a nota). Rodam com
  AddressSanitizer/UBSan em `.github/workflows/testes.yml` (Ubuntu) a cada push.
- Swift não compila no ambiente do agente: o CI "Gerar IPA" é a validação do Swift.
- Workflows com **permissões mínimas** e ações de terceiros **fixadas por SHA** (com a tag em comentário).

## 8. Processo (dev / QA / PO)

- **Dev** implementa a etapa conforme a especificação do PO. Não faz commit nem push.
- **QA** escreve/roda testes e confere este documento; reporta defeitos e desvios com arquivo e linha.
- **PO** inspeciona entre etapas, integra, faz commit/push (padrão do `CLAUDE.md`) e dá o aceite com o CI verde.

## 9. Etapas

1. **Motor C++ + testes**: orçamento de memória e junção de camadas fora da thread principal no
   `LooperCore`; troca de perfil sem disputa no `NoiseReducer`; `tests/` + workflow de testes.
2. **App + CI**: aviso de memória no Loop, junção assíncrona ligada ao motor, desligar motor ocioso em
   segundo plano, validação de `.nam`, permissões e SHAs dos workflows.
3. **Estrutura**: pastas da seção 1, renomes (`RootView.swift`→`App/MainTabView.swift` com
   `MainTabView`; `ContentView.swift`→`Features/Tuner/TunerView.swift`), `project.yml` e bridging header.
4. **Serviços**: `AudioSessionService` único (hoje a configuração da sessão está repetida em 5 lugares),
   `ProjectStore`, `StudioImporting`, divisão do `StudioEngine`.
