# RTU Workshop

Questo repository contiene una mini implementazione di un Remote Terminal Unit (RTU) in C++, sviluppata in due versioni parallele:

- `cpp23/`: versione basata su C++23
- `cpp17/`: versione adattata per compilare con C++17

Le due directory contengono lo stesso insieme di esempi (`main_00` ... `main_07`), ma con differenti requisiti del compilatore e delle librerie standard.

## Prerequisiti

- CMake (versione 3.20+)
- Un compilatore C++ moderno, preferibilmente `g++` 13/14/15
- Linux / ambiente Unix-like

Per verificare la versione del compilatore:

```bash
g++ --version
cmake --version
```

Se usi un compilatore non standard, puoi specificarlo in fase di configurazione, ad esempio:

```bash
cmake -S cpp17 -B cpp17/build -DCMAKE_CXX_COMPILER=g++-15
```

## Struttura del progetto

```text
.
├── README.md
├── compile.sh
├── cpp23/
│   ├── CMakeLists.txt
│   ├── main_00_console.cpp
│   ├── main_01_value.cpp
│   ├── ...
│   └── build/
├── cpp17/
│   ├── CMakeLists.txt
│   ├── main_00_console.cpp
│   ├── main_01_value.cpp
│   ├── ...
│   └── build/
└── build/   # eventuale build da root
```

## Build della versione C++23

Dalla cartella root del progetto:

```bash
cmake -S cpp23 -B cpp23/build
cmake --build cpp23/build -j
```

Oppure usa lo script già presente:

```bash
./compile.sh
```

Lo script `compile.sh` configura e compila la directory `cpp23` usando `g++-15`:

```bash
#!/bin/bash

rm -rf build
cmake -B build -S cpp23 -DCMAKE_CXX_COMPILER=g++-15
cmake --build build
```

## Build della versione C++17

Dalla cartella root del progetto:

```bash
cmake -S cpp17 -B cpp17/build
cmake --build cpp17/build -j
```

In alternativa, con compilatore esplicito:

```bash
cmake -S cpp17 -B cpp17/build -DCMAKE_CXX_COMPILER=g++-15
cmake --build cpp17/build -j
```

## Esecuzione dei binari

Dopo la build, i file eseguibili vengono generati nelle rispettive cartelle `build`:

```bash
./cpp23/build/rtu_00
./cpp23/build/rtu_01
./cpp17/build/rtu_00
./cpp17/build/rtu_01
```

Per eseguire un esempio specifico:

```bash
./cpp17/build/rtu_02
```

## Pulizia

Per rimuovere i file di build:

```bash
rm -rf cpp23/build cpp17/build build
```

## Note

- `cpp23` rappresenta la versione moderna, con feature più avanzate e librerie standard più recenti.
- `cpp17` è la variante compatibile con lo standard più ampio e più semplice da usare su toolchain meno recenti.
- I nomi `rtu_00`, `rtu_01`, ..., `rtu_07` corrispondono ai diversi programmi di esempio all'interno del workshop.

## Sviluppo consigliato

Se vuoi lavorare su una versione specifica:

```bash
cd cpp17
cmake -S . -B build
cmake --build build -j
```

oppure:

```bash
cd cpp23
cmake -S . -B build
cmake --build build -j
```
