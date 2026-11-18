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

## Cobertura

| Bloco | Casos |
|---|---|
| `MovingAverage` | buffer vazio, janela parcial, janela cheia, descarte da amostra mais antiga, equivalencia com a media ingenua em 20000 amostras, `reset()`, janela menor que 50 |
| `volume::compute` | ADC 0, ADC 1023 (3,3 V / 3000 mm / 116651,4 L), clamp abaixo do offset, ida-e-volta `levelMmToAdc`, comparacao com a cadeia inteira do programa original |
| `publish_policy::decide` | sem leitura valida, primeira leitura (`FIRST`), sem mudanca (`NONE`), variacao abaixo do delta, delta antes do intervalo minimo, delta apos o intervalo, heartbeat de 60 min, reconexao (`RECONNECT`) |
| politica offline (decisao x transporte) | `FIRST` sem conexao vai para a fila, `RECONNECT` vira um unico registro e deixa de ser estado continuo, ~10 s offline sem variacao nao enfileira a cada 250 ms, `DELTA` offline respeita o intervalo minimo, `HEARTBEAT` offline, falha de gravacao nao avanca o baseline nem libera `RECONNECT`, backlog pendente mantem a leitura nova na fila e a publicacao direta volta com a fila vazia |
| display | formato do volume: `830`, `1.234`, `12.345`, `123.456` |
| fila offline (`check_queue.py`) | layout binario lido do header C++, CRC32, header/footer, reboot com 344 registros em 3 segmentos, ordem cronologica, memoria cheia, corrupcao em quarentena, cauda parcial, retentativa sem duplicar |
