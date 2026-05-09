# PSX Emulator em C

Emulador do PlayStation 1 escrito em C11, portado a partir da implementação de referência
em Rust baseada no [psx-guide](https://github.com/simias/psx-guide)

---

## Progresso do Desenvolvimento

> Atualize esta tabela conforme cada item for concluído.

| Etapa | Componente | Status | Observações |
| ----- | --------- | ------ | ----------- |
| 1 | CPU R3000A — fetch/decode/execute | ✅ Feito | `cpu.c` |
| 1 | Banco de 32 registradores | ✅ Feito | `cpu.c` — `RegisterFile` |
| 1 | BIOS loader (512 KB, little-endian) | ✅ Feito | `bios.c` |
| 1 | RAM 2 MB | ✅ Feito | `ram.c` |
| 1 | Mapa de memória + `mask_region()` | ✅ Feito | `map.h` |
| 1 | Interconnect (barramento) | ✅ Feito | `interconnect.c` |
| 2 | 67 opcodes MIPS completos | ✅ Feito | `cpu.c` |
| 2 | Branch delay slot | ✅ Feito | campo `branch` na CPU |
| 2 | Load delay slot | ✅ Feito | campos `load_delay_reg/val` |
| 2 | Overflow trapping (ADD/SUB/ADDI) | ✅ Feito | `__builtin_add/sub_overflow` |
| 2 | Unaligned loads LWL/LWR | ✅ Feito | |
| 2 | Unaligned stores SWL/SWR | ✅ Feito | |
| 3 | COP0 — SR, Cause, EPC, BadVAddr | ✅ Feito | `cop0.c` |
| 3 | Entrada de exceção | ✅ Feito | pilha de modo KU/IE |
| 3 | RFE (Return From Exception) | ✅ Feito | |
| 3 | Cache isolada (bit 16 do SR) | ✅ Feito | |
| 4 | GPU — registradores GP0/GP1/GPUSTAT | 🔲 Pendente | stubs em `interconnect.c` |
| 4 | VRAM 1 MB (512×1024 px 16-bit) | 🔲 Pendente | criar `gpu.c` |
| 4 | Renderização — triângulos flat/Gouraud | 🔲 Pendente | |
| 4 | Renderização — retângulos e sprites | 🔲 Pendente | |
| 4 | Texturas e CLUT | 🔲 Pendente | |
| 4 | Janela via SDL2 | 🔲 Pendente | |
| 5 | DMA — 7 canais | 🔲 Pendente | criar `dma.c` |
| 5 | Modo LinkedList (GPU canal 2) | 🔲 Pendente | |
| 5 | Modo Request (CDROM canal 3) | 🔲 Pendente | |
| 6 | Timers — 3 root counters 16-bit | 🔲 Pendente | criar `timers.c` |
| 6 | IRQ de timer (VBlank / HBlank) | 🔲 Pendente | |
| 6 | Controlador de interrupções (IRQ) | 🔲 Pendente | |
| 7 | CD-ROM controller | 🔲 Pendente | criar `cdrom.c` |
| 7 | Comandos: GetStat, SetLoc, ReadN | 🔲 Pendente | |
| 7 | Formato de setor (2048 bytes) | 🔲 Pendente | |
| 8 | SPU — 24 vozes ADPCM | 🔲 Pendente | criar `spu.c` |
| 8 | Envelope ADSR por voz | 🔲 Pendente | |
| 8 | XA-ADPCM (áudio do CD) | 🔲 Pendente | |
| 8 | Saída de áudio via SDL2 | 🔲 Pendente | |
| — | GTE (COP2 — geometria 3D) | 🔲 Pendente | necessário para jogos |
| — | Debugger / disassembler MIPS | 🔲 Pendente | criar `debug.c` |

**Legenda:** ✅ Feito · 🔲 Pendente · 🚧 Em andamento · ❌ Bloqueado

---

## Resultado Esperado por Etapa

| Após etapa | O que acontece ao rodar |
| --------- | ---------------------- |
| 1–3 | CPU executa a BIOS; stubs imprimem `[GPU]`, `[DMA]`... no terminal; para no primeiro COP2 ou endereço não mapeado |
| 4 | Logo da Sony e tela do BIOS aparecem na janela SDL2 |
| 5 | GPU recebe dados via DMA sem travar a CPU |
| 6 | VBlank e HSync corretos; BIOS não trava em loop de espera |
| 7 | Jogos em formato `.bin/.cue` carregam e executam |
| 8 | Áudio funciona (músicas e efeitos) |
| GTE | Jogos 3D renderizam corretamente |

---

## Arquitetura do Hardware

```
┌──────────────────────────────────────────────────────────┐
│                      PlayStation 1                       │
│                                                          │
│  ┌──────────┐    ┌──────────┐    ┌──────────┐           │
│  │ CPU MIPS │    │   GPU    │    │   SPU    │           │
│  │ R3000A   │    │ (Custom) │    │ 24 vozes │           │
│  │ 33.8 MHz │    │ 1MB VRAM │    │ 512KB RAM│           │
│  └────┬─────┘    └────┬─────┘    └────┬─────┘           │
│       │               │               │                  │
│  ┌────▼───────────────▼───────────────▼───────────────┐  │
│  │              System Bus (Interconnect)              │  │
│  └──────┬────────┬────────┬────────┬───────────────────┘  │
│         │        │        │        │                      │
│  ┌──────▼┐  ┌────▼──┐  ┌──▼───┐  ┌▼──────┐              │
│  │  RAM  │  │ BIOS  │  │ DMA  │  │ CDROM │              │
│  │  2 MB │  │ 512KB │  │ 7 ch │  │       │              │
│  └───────┘  └───────┘  └──────┘  └───────┘              │
└──────────────────────────────────────────────────────────┘
```

### Mapa de Memória Físico

| Região (física) | Tamanho | Dispositivo |
| -------------- | ------- | ---------- |
| `0x00000000` | 2 MB | RAM principal |
| `0x1f000000` | 8 MB | Expansion 1 |
| `0x1f801000` | 36 B | Memory Control |
| `0x1f801060` | 4 B | RAM Size |
| `0x1f801070` | 8 B | IRQ Control |
| `0x1f801080` | 128 B | DMA |
| `0x1f801100` | 48 B | Timers |
| `0x1f801800` | 4 B | CD-ROM |
| `0x1f801810` | 8 B | GPU (GP0/GP1/GPUSTAT) |
| `0x1f801c00` | 640 B | SPU |
| `0x1f802000` | 66 B | Expansion 2 (porta serial de debug) |
| `0x1fc00000` | 512 KB | BIOS ROM |
| `0xfffe0130` | 4 B | Cache Control |

### Conversão de Regiões Virtuais

```
KUSEG  0x00000000–0x7fffffff  →  & 0xffffffff  (já físico)
KSEG0  0x80000000–0x9fffffff  →  & 0x7fffffff
KSEG1  0xa0000000–0xbfffffff  →  & 0x1fffffff  (uncached)
KSEG2  0xc0000000–0xffffffff  →  & 0xffffffff
```

---

## Conjunto de Instruções MIPS implementado

### Grupo SPECIAL (opcode 0x00)

| funct | Instrução | Descrição |
| ----- | --------- | --------- |
| 0x00 | SLL | Shift Left Logical |
| 0x02 | SRL | Shift Right Logical |
| 0x03 | SRA | Shift Right Arithmetic |
| 0x04 | SLLV | SLL Variable |
| 0x06 | SRLV | SRL Variable |
| 0x07 | SRAV | SRA Variable |
| 0x08 | JR | Jump Register |
| 0x09 | JALR | Jump and Link Register |
| 0x0c | SYSCALL | System Call → exceção |
| 0x0d | BREAK | Breakpoint → exceção |
| 0x10 | MFHI | Move From HI |
| 0x11 | MTHI | Move To HI |
| 0x12 | MFLO | Move From LO |
| 0x13 | MTLO | Move To LO |
| 0x18 | MULT | Multiply (signed) |
| 0x19 | MULTU | Multiply Unsigned |
| 0x1a | DIV | Divide (signed) |
| 0x1b | DIVU | Divide Unsigned |
| 0x20 | ADD | Add (dispara exceção em overflow) |
| 0x21 | ADDU | Add Unsigned (sem exceção) |
| 0x22 | SUB | Subtract (dispara exceção em overflow) |
| 0x23 | SUBU | Subtract Unsigned |
| 0x24 | AND | Bitwise AND |
| 0x25 | OR | Bitwise OR |
| 0x26 | XOR | Bitwise XOR |
| 0x27 | NOR | Bitwise NOR |
| 0x2a | SLT | Set Less Than (signed) |
| 0x2b | SLTU | Set Less Than Unsigned |

### Grupo principal

| opcode | Instrução | Descrição |
| ------ | --------- | --------- |
| 0x01 | BcondZ | BLTZ / BGEZ / BLTZAL / BGEZAL |
| 0x02 | J | Jump |
| 0x03 | JAL | Jump and Link |
| 0x04 | BEQ | Branch if Equal |
| 0x05 | BNE | Branch if Not Equal |
| 0x06 | BLEZ | Branch if ≤ Zero |
| 0x07 | BGTZ | Branch if > Zero |
| 0x08 | ADDI | Add Immediate (com overflow) |
| 0x09 | ADDIU | Add Immediate Unsigned |
| 0x0a | SLTI | Set Less Than Immediate (signed) |
| 0x0b | SLTIU | Set Less Than Immediate Unsigned |
| 0x0c | ANDI | AND Immediate |
| 0x0d | ORI | OR Immediate |
| 0x0e | XORI | XOR Immediate |
| 0x0f | LUI | Load Upper Immediate |
| 0x10 | COP0 | MFC0 / MTC0 / RFE |
| 0x12 | COP2 | GTE (stub) |
| 0x20 | LB | Load Byte (signed) |
| 0x21 | LH | Load Halfword (signed) |
| 0x22 | LWL | Load Word Left (unaligned) |
| 0x23 | LW | Load Word |
| 0x24 | LBU | Load Byte Unsigned |
| 0x25 | LHU | Load Halfword Unsigned |
| 0x26 | LWR | Load Word Right (unaligned) |
| 0x28 | SB | Store Byte |
| 0x29 | SH | Store Halfword |
| 0x2a | SWL | Store Word Left (unaligned) |
| 0x2b | SW | Store Word |
| 0x2e | SWR | Store Word Right (unaligned) |

---

## Estrutura do Projeto

```
psx_c/
├── Makefile
├── README.md
│
├── map.h              ← Ranges de memória + mask_region()
│
├── bios.h / bios.c    ← Loader da BIOS ROM (512 KB, little-endian)
├── ram.h  / ram.c     ← RAM principal (2 MB)
│
├── cop0.h / cop0.c    ← COP0: SR, Cause, EPC, BadVAddr, exceções
│
├── interconnect.h     ← System bus: despacha loads/stores
├── interconnect.c
│
├── cpu.h  / cpu.c     ← CPU R3000A: 67 opcodes, delay slots
│
└── main.c             ← Entry point
```

### Arquivos a criar nas próximas etapas

```
gpu.h  / gpu.c         ← Etapa 4: GP0/GP1, VRAM, SDL2
dma.h  / dma.c         ← Etapa 5: 7 canais DMA
timers.h / timers.c    ← Etapa 6: 3 root counters + IRQ
cdrom.h / cdrom.c      ← Etapa 7: protocolo CD-ROM
spu.h  / spu.c         ← Etapa 8: 24 vozes ADPCM + SDL2 Audio
gte.h  / gte.c         ← GTE: 63 comandos de geometria 3D
debug.h / debug.c      ← Disassembler MIPS + dump de registradores
```

---

## Decisões da Implementação em C

### Comparado ao Rust original

| Aspecto | Rust | C |
| ------- | ---- | - |
| Overflow em ADD/ADDI/SUB | `i32::checked_add()` | `__builtin_add/sub_overflow()` |
| Aritmética sem overflow | `.wrapping_add/sub()` | unsigned wrap nativo |
| `Option<u32>` em contains() | `if let Some(off) = ...` | `memrange_contains()` com ponteiro |
| Segurança de memória | borrow checker | responsabilidade do dev |
| `panic!()` | unwind + mensagem | `fprintf(stderr) + abort()` |
| `unreachable!()` | trap em debug | `abort()` |
| Módulos | `mod cpu { ... }` | arquivos `.h / .c` separados |

### Convenções adotadas

- **Endianness:** leitura e escrita manuais byte a byte — portável em qualquer arquitetura host.
- **Registrador $zero:** `reg_set()` sempre força `regs.r[0] = 0` após qualquer escrita.
- **Load delay slot:** dois campos (`load_delay_reg`, `load_delay_val`) substituem a tupla Rust.
- **Sem alocação dinâmica:** todos os componentes (BIOS 512 KB, RAM 2 MB) ficam em structs na stack/BSS. O executável ocupa ~2.5 MB de BSS.

---

## Como Compilar e Executar

```bash
# Compilar (requer GCC ou Clang com suporte a C11)
make

# Compilar em modo debug (sem otimização, com sanitizers)
make CFLAGS="-std=c11 -Wall -Wextra -g -fsanitize=address,undefined"

# Executar — necessita da BIOS SCPH1001.BIN
./psx roms/SCPH1001.BIN

# BIOS esperada
# Nome:   SCPH1001.BIN
# SHA-1:  10155d8d6e6e832d6ea66db9bc098321fb5e8ebf
# MD5:    924e392ed05558ffdb115408c263dccf
# Tamanho: 524288 bytes (512 KB)
```

### Saída atual (Etapas 1–3 concluídas)

```
PSX Emulator - Starting
Loading BIOS: roms/SCPH1001.BIN
CPU initialized. PC = 0xbfc00000
Starting emulation loop...

[IRQ] Store32 0x00000000 at offset 0x0 (stub)
[GPU] GP1 command: 0x00000000
[GPU] GP1 command: 0x01000000
[SPU] Store16 0x0000 at offset 0x... (stub)
...
[CPU] GTE (COP2) not implemented: ...     ← ponto de parada atual
```

O emulador para no primeiro uso do GTE, necessário para a animação do logo da Sony.
A próxima parada natural, após implementar o GTE, seria a ausência de janela gráfica (Etapa 4).

---

## Referências

- [psx-guide](https://github.com/simias/psx-guide) — Lionel Flandrin (referência principal)
- [rustation](https://github.com/simias/rustation) — emulador Rust completo de referência
- [Nocash PSX Specs](http://problemkaputt.de/psx-spx.htm) — especificação completa do hardware
- [MIPS R3000 Manual](https://cgi.cse.unsw.edu.au/~cs3231/doc/R3000.pdf) — manual do processador
- [PlayStation Architecture](https://www.copetti.org/writings/consoles/playstation/) — visão geral acessível
