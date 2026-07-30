# Chiprune

**Ferramenta de diagnóstico de DRAM para Nintendo Switch, feita para
diferenciar se um defeito de tela preta/tela azul está na memória RAM
ou no SoC (processador).**

## Pra que serve

Se o seu Switch trava com tela preta ou tela azul, existem várias causas
possíveis (SoC, DRAM, eMMC, alimentação...). Esse payload existe
especificamente pra **isolar a DRAM como suspeita ou descartá-la**, sem
precisar de nenhum arquivo no SD card — é injetado direto via RCM/Fusée
com um modchip.

Ele faz isso em duas etapas bem separadas e visíveis na tela:

- **Etapa 1 — prova o SoC:** o programa inicia rodando inteiramente na
  memória interna do próprio chip (IRAM), sem tocar na DRAM. Se essa
  etapa aparecer na tela, já é evidência de que o SoC, a alimentação e o
  vídeo estão funcionando — porque nada disso depende da DRAM.
- **Etapa 2 — testa a DRAM:** só depois disso o programa treina o
  controlador de DRAM e roda um teste de escrita/leitura com vários
  padrões. Se o console travar exatamente aqui (e não antes), a suspeita
  passa a ser a DRAM/EMC — não mais o SoC.

**Como interpretar o resultado:**

| O que aparece na tela | Onde suspeitar |
|---|---|
| Tela preta total, nunca aparece nada | SoC / alimentação / vídeo — não é a DRAM |
| Aparece "Etapa 1" mas trava ao entrar/durante a "Etapa 2" | DRAM / EMC |
| Completa o teste e mostra PASS ou FAIL | DRAM confirmada (com ou sem erro específico) — se o console ainda não ligar normalmente depois disso, a suspeita passa para o eMMC/NAND, que este teste não cobre |

Se o teste encontrar erro, ele mostra **em qual pino/sinal exato** (ex.
`DQ12_A`, bola `F9`) o problema foi detectado — mas **não é capaz de
confirmar em qual dos dois módulos físicos de DRAM** (`N14856` ou
`N14857`) o defeito realmente está. O programa desenha os dois módulos
lado a lado, na posição real das bolas do encapsulamento BGA-200, com o
pino destacado em ambos — mas essa duplicação ainda é uma limitação, não
uma confirmação de que os dois estão com defeito.

Ou seja: no estado atual, o Chiprune serve para **confirmar que o
problema está relacionado à RAM (e não ao SoC)**, e para indicar qual
sinal específico apresentou erro — mas ainda não diferencia com certeza
qual dos dois módulos físicos é o culpado. Essa parte do desenho ainda
precisa de ajuste.

Compatível com Nintendo Switch V1, V2, Lite e OLED — o tipo de chip e de
painel são detectados automaticamente.

## Limitação conhecida

A associação entre "metade baixa/alta do barramento de 64 bits" e os
módulos físicos reais de uma placa específica não é confirmada só por
software — ver `g_module_order` em `src/main.c`. Trate essa identificação
de módulo como uma hipótese até validar fisicamente (osciloscópio nas
linhas DQ/CS, ou uma placa com defeito localizado já conhecido).

## Baixando e usando

Não precisa compilar nada — é só baixar o `chiprune.bin` já pronto (na
seção de Releases ou direto do repositório).

1. Baixa o arquivo `chiprune.bin`
2. Renomeia pra `payload.bin`
3. Injeta do jeito que preferir:
   - Direto via **TegraRcmGUI** (ou outro injetor de payload de RCM), sem precisar de SD card
   - Ou colocando `payload.bin` no SD card, se o seu método de injeção
     depender disso (varia de acordo com o modchip/injetor usado)

## Compilando a partir do código-fonte (opcional)

Se preferir compilar você mesmo em vez de usar o `.bin` pronto, precisa de
um toolchain bare-metal ARMv4T/Thumb (o payload roda no coprocessador
BPMP do Tegra X1 no boot inicial, não nos núcleos principais da CPU) —
`arm-none-eabi-gcc` funciona:
Make

Saída: `output/chiprune.bin` — mesma coisa, renomeia pra `payload.bin` e
injeta do mesmo jeito.

## Créditos

Construído sobre um kit de drivers de baixo nível (bdk) para bootloaders
via RCM do Tegra X1, disponível abertamente e descendente do projeto
Hekate, de CTCaer. O código de treinamento de DRAM/EMC em `bdk/mem/` está
inalterado dessa base; a lógica de diagnóstico, visualização e o
mapeamento de pinos específico da placa em `src/main.c` são o que este
projeto adiciona.
