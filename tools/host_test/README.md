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
de nivel em zero e politica de publicacao).

## Cobertura

| Bloco | Casos |
|---|---|
| `MovingAverage` | buffer vazio, janela parcial, janela cheia, descarte da amostra mais antiga, equivalencia com a media ingenua em 20000 amostras, `reset()`, janela menor que 50 |
| `volume::compute` | ADC 0, ADC 1023 (3,3 V / 3000 mm / 116651,4 L), clamp abaixo do offset, ida-e-volta `levelMmToAdc`, comparacao com a cadeia inteira do programa original |
| `publish_policy::decide` | sem leitura valida, primeira leitura (`FIRST`), sem mudanca (`NONE`), variacao abaixo do delta, delta antes do intervalo minimo, delta apos o intervalo, heartbeat de 60 min, reconexao (`RECONNECT`) |
| display | formato do volume: `830`, `1.234`, `12.345`, `123.456` |
