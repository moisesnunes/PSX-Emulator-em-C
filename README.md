# PSX Emulator em C

Emulador do PlayStation 1 escrito em C11, baseado no [psx-guide](https://github.com/simias/psx-guide).

## Dependências

- GCC (C11)
- SDL2
- OpenGL / GLEW

```bash
# Ubuntu/Debian
sudo apt install gcc libsdl2-dev libglew-dev
```

## Build e execução

```bash
make              # compila
make run          # compila e executa (usa bios/BIOS.ROM)
make smoke        # roda 500k instruções sem janela e sai (CI)
make test-cdrom   # roda testes unitários do controlador CD-ROM
make test-sio     # roda testes unitários de SIO/joypad
make test-gte     # roda testes unitários de registradores básicos do GTE
make debug        # compila com ASan/UBSan e executa
make clean        # remove artefatos
```

Opções do binário:

```text
./ps1_boot [--bios <path>] [--exe <path>] [--disc <path>] [--headless] [--max-instructions <N>]
```

A BIOS não está incluída no repositório. Coloque o arquivo em `bios/BIOS.ROM`
ou passe o caminho via `--bios`.

## Logging

Ative logs por subsistema com a variável `PS1_LOG`:

```bash
PS1_LOG=IRQ,DMA,GPU make run
```

Subsistemas: `CPU`, `DMA`, `GPU`, `IRQ`, `CDROM`, `SPU`, `SIO`.

## Estado atual

| Subsistema | Status |
| ---------- | ------ |
| CPU MIPS R3000A (67 opcodes) | funcional |
| Interconnect / mapa de memória | funcional |
| RAM 2 MB / BIOS 512 KB | funcional |
| DMA (block + linked-list) | funcional |
| GPU GP0/GP1 (rasterização software + VRAM texture) | parcial |
| SPU 24 vozes ADPCM | parcial |
| IRQ control (I_STAT/I_MASK) | implementado |
| Scheduler / VBlank | implementado |
| Timers (root counters) | implementado |
| CD-ROM (.bin raw + comandos básicos + DMA3) | parcial |
| SIO / Joypad digital | parcial |
| GTE / COP2 | parcial |

## Referências

- [psx-spx](http://problemkaputt.de/psx-spx.htm) — especificação completa do hardware
- [psx-guide](https://github.com/simias/psx-guide) — guia de implementação
- [rustation](https://github.com/simias/rustation) — emulador Rust de referência
