# Testes de host (opcionais)

Verificacao dos modulos puros do firmware **fora do ESP8266**, sem device e sem
broker. Nada aqui entra no firmware: o PlatformIO compila apenas `src/`.

## `hosttest.cpp` — teste em C++

Exercita `MovingAverage`, `volume::compute` e `publish_policy::decide` usando os
fontes reais do projeto. Precisa de `g++`/`clang++` com biblioteca padrao:

```sh
cd tools/host_test
g++ -std=c++17 -O2 -Wall -Wextra -I../../include hosttest.cpp \
    ../../src/moving_average.cpp ../../src/volume.cpp -o hosttest
./hosttest
```

Ultima linha esperada: `TODOS OS TESTES PASSARAM (0 falha(s))`.

## `check_logic.py` — verificador numerico (sem compilador C++)

Reproduz passo a passo a mesma logica com precisao `float` de 32 bits. Serve
para as maquinas em que nao ha toolchain C++ nativa (o caso do PC onde as
etapas 1 e 2 foram compiladas: ha apenas o cross-compiler xtensa e um clang sem
biblioteca padrao).

```sh
python check_logic.py
```

Saida: mesmas baterias do teste C++ (media movel, cadeia de calibracao, limite
de nivel em zero, politica de publicacao e a resolucao offline da politica,
onde a decisao nao depende do transporte).

## `check_queue.py` — verificador da fila offline

Valida o mecanismo de persistencia portado do Ecowitt2TB sem device. O script
**le as constantes reais de `include/queue_record.h`** (falha se o layout C++
mudar sem atualizar o verificador), reproduz o formato binario e usa uma flash
simulada em diretorio temporario, o que permite simular reboots.

```sh
python check_queue.py
```

Cobre: layout de registro/segmento, CRC32, header/footer, 3 segmentos com 344
registros, persistencia e ordem cronologica apos reboot, memoria cheia (recusa
sem descartar), corrupcao em quarentena, cauda parcial por queda de energia e
retentativa de envio sem duplicar.

Limitacao: nao executa o C++. Nao ha toolchain C++ nativa nesta maquina, entao
`src/offline_queue.cpp` e validado pela compilacao do PlatformIO e pela
verificacao de layout acima; a confirmacao em hardware exige `pio run -t
uploadfs` uma vez (o firmware nunca formata o LittleFS sozinho) e um teste de
reboot fisico.

## `check_history.py` — verificador do historico diario

Valida o formato e as regras do historico **sem device**. O script le as
constantes reais de `include/history_record.h` e de `include/config.h` (falha se
o layout C++ mudar sem atualizar o verificador), reproduz o formato byte a byte
e simula a flash e o proprio modulo (gate de 10 s, virada de dia, snapshot A/B,
tabela A/B, dia pendente).

```sh
python check_history.py
```

Cobre:

| Bloco | Casos |
|---|---|
| layout | tabela de 1600 B (28 + 30 x 52 + 12), rodape em 1588, 8640 amostras nominais |
| registro | ida e volta de min/max (float32), soma (float64), n, gaps, flags |
| slots | 52 bytes zero = VAZIO; um byte nao zero = CORROMPIDO; registro no slot errado = CORROMPIDO; CRC errado = CORROMPIDO |
| cabecalho/rodape | ida e volta, `count` divergente nao invalida a tabela, CRC errado rejeitado |
| dia civil BRT | meia-noite local muda o `dayIndex`; `dayStart` devolve exatamente a meia-noite |
| anel de 30 dias | rotacao completa, ordem logica crescente, ordem fisica **nao** crescente, deslizamento da janela, slot reaproveitado |
| A/B | escolha pela maior geracao, versao corrompida ignorada, nenhuma versao valida = historico vazio sem apagar nada |
| cobertura | dia completo/meio dia/lacuna/reinicio/menos de 99,5%/sem dados; hoje comparado com o tempo decorrido; periodo de 7 dias agregado sem cruzar dias |
| periodos fechados | 7 e 30 dias terminando em ontem (hoje nunca entra), virada de meia-noite BRT, dias faltantes, dias parciais/degradados, extremos e media ponderada, historico insuficiente, recusa de 31 dias e `periodReport()` comparada com o codigo congelado |
| simulado | 8640 amostras em 24 h (287 snapshots), chamadas repetidas sem duplicar, virada com gravacao, reboot retomando o snapshot, falha de gravacao com dia pendente, bit degradado e limiar de espaco |

## `check_telegram.py` — verificador do modulo Telegram

Valida a camada de texto do Telegram (`include/telegram_text.h`) e as invariantes
de seguranca do projeto, **sem device**. Le as constantes reais de
`include/config.h` e a capacidade dos buffers de `src/telegram.cpp`, falhando se
o firmware mudar sem que o verificador acompanhe.

```sh
python check_telegram.py
```

Cobre: classificacao de comandos (`/help`/`/h`, `/status`/`/s`, `/warnings`/`/w`,
maiusculas, espacos, comandos inexistentes e **rejeicao de qualquer texto com
`@`**, mesmo enderecado a outro bot), uptime acumulado (continuo na passagem pelo
rollover de `millis()` e acima de 49,7 dias, com formatacao em minutos/horas/dias),
texto exato do aviso de boot (`Reiniciado.`, acentuado) e do `/help` e
`/status` (nivel de AGORA, litros em digitos-emoji, taxa recente de 1 h e a
tabela de minimos e maximos em fonte fixa, nenhuma informacao interna, separador
de milhar), tamanho/URL-encoding do POST, `setInsecure()` ausente do
codigo, CA validada nos dois transportes TLS, buffers do MQTT preservados
(3072/512, com fallback automatico para 4096 quando o broker envia um registro TLS
maior que o buffer), credenciais apenas em `include/secrets.h`, SHA-256 da ancora
Go Daddy G2, uso de `dayReport()` / `closedPeriodReport()` sem recalcular agregados
e ausencia de `getMe`/username.
Cobre tambem a sessao TLS persistente: liberacao da sessao com socket morto
ANTES da medicao de heap, renovacao preventiva aos 50 min (o servidor fecha a
conexao com ~1 h) e a guarda de bloqueio que reinicia o ESP quando Telegram ou
MQTT ficam presos por tempo demais.
E, na secao 19, os **alertas de nivel**: referencia dos percentuais (teto do
sensor, 116.651 L), `/w` sem argumento e `/w lr lc hist`, validacao dos limites
(ordem e histerese), as quatro transicoes com histerese de um lado so, transicoes
em cadeia, o texto exato das quatro mensagens e a ligacao com o Telegram (uma
mensagem por consulta, antes do `getUpdates`, com `peek`/`confirm`).
O **aviso de boot** tem quatro linhas - `Reiniciado.`, `Motivo`, `Versão` e
`Volume atual: <emoji> <NÍVEL>` (ou `Volume atual: sem leitura`) - com o estado de
AGORA, nao o persistido, para dar contexto ao reboot, e os litros do instante na
ultima linha, em **digitos-emoji** (keycap), sem a unidade e separados do nivel
por uma linha em branco.

Na secao 19b, o que sustenta a memoria: as mensagens do Telegram vao para a
**flash** com `TG_TEXT()`/`PSTR()` (no ESP8266 o `.rodata` vive na DRAM, e cada
byte de texto custa 1 byte de heap), a **primeira consulta** do Telegram sai
`TELEGRAM_FIRST_POLL_DELAY_MS` (1 s) apos o boot - esperando Wi-Fi **e** NTP - e o
`Telegram` roda **antes** do `ThingsBoard` no `loop()`, para a sessao TLS nascer
com o heap inteiro (medido: `.rodata` caiu 2 948 B, o que reabriu o gate de heap
de 14 080 B que estava fechando o Telegram).

## `check_alerts.py` — verificador dos alertas de nivel (persistencia)

Valida o que sustenta a preservacao de dados do `/w` **sem device**: le o layout
real de `include/alerts.h` (falha se o C++ mudar sem o verificador acompanhar),
reproduz o registro byte a byte e simula o LittleFS, incluindo **reboots**.

```sh
python check_alerts.py
```

Cobre: layout de 32 B (magic `WCFG`, versao, limites, estado, 2 pendentes com
tipo + volume float32, geracao e CRC32 sobre os 28 primeiros bytes), recusa de
registro corrompido (CRC, magic, versao, estado ou limites incoerentes), dois
slots A/B com geracao crescente e escolha do mais novo no boot, gravacao que
falha sem sobrescrever o slot bom, **pendente que sobrevive ao reboot e e
reentregue sem duplicar** (e sem reenviar depois de confirmado), recuperacao com
histerese preservando o estado, transicoes em cadeia, fila cheia descartando o
alerta mais antigo, settle antes da primeira avaliacao, ausencia de LittleFS (opera
em RAM e volta aos defaults no reboot) e limites invalidos que nao gravam nada.

A partir da secao 8, o **rebaseline pos-boot**: MELHORA e silenciosa (o aparelho
nao viu a recuperacao e o volume de agora nao e o da travessia) e PIORA vira um
unico alerta com o nivel de agora, sem duplicar quando o estado nao mudou - inclusive
o caso de campo de 2026-11-20, em que duas "recuperacoes" espurias de reboot
estouravam a fila de 2 e descartavam o alerta critico mais antigo. A secao 9
confere a geometria do cenario de bancada `CYCLE` (`SIM_SCENARIO`, esvazia, enche e
recomeca): os QUATRO avisos saem em um ciclo (42/86/161/205 s com os limites de
fabrica), com margem sobre os limiares, e no settle do boot o tanque ainda esta
Normal - o rebaseline de um reboot no meio da queda sai silencioso.

Limite: nao executa o C++. O comportamento do `src/alert_monitor.cpp` e confirmado
pela compilacao do PlatformIO e pelos espelhos acima; a confirmacao em hardware
exige `pio run -t upload` e observar as mensagens no chat de TESTE.

## `check_rate.py` — verificador da taxa de variacao recente

Valida a serie CURTA em RAM de `include/rate.h` (12 pontos de 5 min = 1 h) sem
device: le as constantes reais de `include/rate.h`, reproduz o anel e a
inclinacao por minimos quadrados e confere a base (`sem base` antes de 7 pontos,
`estável` na banda de 150 L/h), as rampas em L/h, as lacunas por amostra
invalida, a rotacao do anel, o **rollover de `millis()`** e a ligacao no `loop()`
(a serie e alimentada pela MESMA media movel publicada).

```sh
python check_rate.py
```

Saida: `TODAS AS VERIFICACOES PASSARAM (0 falha)`.

## Cobertura

| Bloco | Casos |
|---|---|
| `MovingAverage` | buffer vazio, janela parcial, janela cheia, descarte da amostra mais antiga, equivalencia com a media ingenua em 20000 amostras, `reset()`, janela menor que 50 |
| `volume::compute` | ADC 0, ADC 1023 (3,3 V / 3000 mm / 116651,4 L), clamp abaixo do offset, ida-e-volta `levelMmToAdc`, comparacao com a cadeia inteira do programa original |
| `publish_policy::decide` | sem leitura valida, primeira leitura (`FIRST`), sem mudanca (`NONE`), variacao abaixo do delta, delta antes do intervalo minimo, delta apos o intervalo, heartbeat de 60 min, reconexao (`RECONNECT`) |
| politica offline (decisao x transporte) | `FIRST` sem conexao vai para a fila, `RECONNECT` vira um unico registro e deixa de ser estado continuo, ~10 s offline sem variacao nao enfileira a cada 250 ms, `DELTA` offline respeita o intervalo minimo, `HEARTBEAT` offline, falha de gravacao nao avanca o baseline nem libera `RECONNECT`, backlog pendente mantem a leitura nova na fila e a publicacao direta volta com a fila vazia |
| display | formato do volume: `830`, `1.234`, `12.345`, `123.456` |
| fila offline (`check_queue.py`) | layout binario lido do header C++, CRC32, header/footer, reboot com 344 registros em 3 segmentos, ordem cronologica, memoria cheia, corrupcao em quarentena, cauda parcial, retentativa sem duplicar |
| historico diario (`check_history.py`) | layout 1600/52 B lido do header C++, slots vazio/corrompido/valido, `count` divergente, dia civil BRT, rotacao do anel de 30 dias, A/B por geracao, cobertura por dia e por periodo, 24 h simuladas (287 snapshots), reboot pelo snapshot, dia pendente e bit degradado, limiar de espaco |
| periodos fechados (`check_history.py`) | `closedPeriodReport()`: janela de 7/30 dias terminando em ontem, exclusao integral de hoje, virada de meia-noite BRT, dias faltantes, dias parciais/reiniciados/com lacuna, extremos globais, media ponderada pelo numero de amostras, historico insuficiente, recusa acima do anel/0 dias/sem relogio e `periodReport()` identica ao codigo congelado em 15 combinacoes |
| Telegram (`check_telegram.py`) | constantes lidas de `config.h`, classificacao de comandos (`/help`/`/h`, `/status`/`/s`, maiusculas, espacos, `/status@Bot` e qualquer texto com `@` **rejeitados**, `/reboot` e `/ota` inexistentes, texto vazio/absurdo), uptime acumulado (antes, durante e depois do rollover de `millis()`, acumulado acima de 49,7 dias, formatacao em minutos/horas/dias), texto exato do aviso de boot (`Reiniciado.`) e de `/help` e `/status` (nivel de AGORA, litros em digitos-emoji, taxa recente de 1 h e
tabela `<pre>`, sem media/Wi-Fi/MQTT, abreviacoes anunciadas), tamanho e URL-encoding do POST, ausencia de `setInsecure()`, CA validada nos dois transportes, buffers do MQTT preservados, credenciais fora dos arquivos versionados, SHA-256 da ancora TLS, uso de `dayReport()`/`closedPeriodReport()` sem recalcular agregados, pisos de bloco **abaixo do buraco deixado pela sessao fechada** (`TELEGRAM_SESSION_BYTES`/`TELEGRAM_SESSION_HOLE_BYTES`): sessao fechada (falha de consulta ou renovacao preventiva) -> gate reabre na consulta seguinte e a sessao e recriada (o bug de 2026-11-20 eram os pisos acima do buraco, com o bot mudo e alertas presos), liberacao da sessao TLS morta antes da medicao de heap, renovacao preventiva aos 50 min (o servidor fecha a conexao com ~1 h), RX do MQTT reduzido a 3072 B, `FIRMWARE_VERSION` unica, integracao no `loop()` e ausencia de `getMe`/username |
| guarda de bloqueio (`check_telegram.py`, secao 18) | reboot so com Wi-Fi conectado, carencia do boot, aviso unico antes do reboot, credenciais pendentes nao contam como bloqueio, limites lidos de `config.h` (TG/MQTT), **sessao MQTT de pe conta como viva** (uma conexao longa e saudavel nao reinicia a cada 30 min), **reboot so com o heap inadministravel** (maior bloco contiguo abaixo do piso de emergencia por tempo sustentado; servico bloqueado com heap intacto nao reinicia), **carimbo no futuro conta como silencio zero** (o poll do Telegram bloqueia e marca `millis()` depois do `now` do loop), rollover de `millis()`, nenhuma alocacao dinamica e ligacao no `loop()` |
| alertas de nivel (`check_telegram.py`, secao 19 + `check_alerts.py`) | `/w`/`/warnings` (mostrar e configurar `lr lc hist`), percentuais do teto do sensor (116.651 L), validacao de ordem e histerese, as quatro transicoes com histerese de um lado so (entrada no limiar, recuperacao em limiar + hist), degrau direto emitindo so o alerta critico, transicoes em cadeia, texto exato das quatro mensagens, registro de 32 B com CRC32 e dois slots A/B (geracao escolhe o mais novo), gravacao que falha sem perder o slot bom, **reboot sem alerta duplicado e com o pendente reentregue**, fila de 2 pendentes descartando o mais antigo, settle de 15 s, ausencia de LittleFS e limites invalidos que nao gravam, **rebaseline pos-boot** (melhora silenciosa / piora anunciada, sem mexer na fila) e a **geometria do cenario de bancada `CYCLE`** (os quatro avisos dentro de um ciclo) |
| taxa recente (`check_rate.py` + `check_telegram.py`) | anel de 12 pontos de 5 min lido de `include/rate.h`, base pronta com 7 pontos (span de 30 min), inclinacao por minimos quadrados (rampas de +0 a 116.651 L/h), ponto atipico que nao inverte o sentido, regime antigo saindo da janela, lacuna que nao distorce a taxa, rotacao do anel, **rollover de `millis()`**, banda "estável" de 150 L/h, arredondamento a 10 L/h com milhar por ponto, `sem base` no `/status` e linha **omitida** nos alertas |
| heap e ordem do boot (`check_telegram.py`, secao 19b) | mensagens e formatos de log na **flash** via `TG_TEXT()`/`PSTR()` (`.rodata` vive na DRAM no ESP8266), textos identicos aos esperados dentro do `TG_TEXT()`, literais de comando preservados, primeira consulta do Telegram 1 s apos o boot esperando Wi-Fi **e** NTP, e `Telegram` **antes** do `ThingsBoard` no `loop()` |
| fallback de TLS do MQTT (`check_telegram.py`, secao 10) | RX normal de 3072 B, RX de emergencia de 4096 B maior que o normal e dentro do limite do core, `BR_ERR_TOO_LARGE` (codigo 6) amplia o buffer, falta de heap (-1000) volta ao normal, tentativa que falhou libera contexto/buffers (`PubSubClient` nao chama `stop()` em falha de handshake), nova tentativa sem o backoff dobrado, TLS so com o relogio sincronizado e hora passada ao BearSSL (`setX509Time`) nos dois transportes |
