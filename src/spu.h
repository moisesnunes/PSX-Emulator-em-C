#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <SDL2/SDL.h>

#define SOUND_RAM_SIZE (512 * 1024)
#define NUM_VOICES 24
#define FIR_LEN 39

typedef enum { ADSR_ATTACK, ADSR_DECAY, ADSR_SUSTAIN, ADSR_RELEASE } AdsrPhase;
typedef enum { DIR_INCREASING, DIR_DECREASING } EnvDirection;
typedef enum { RATE_LINEAR, RATE_EXPONENTIAL } ChangeRate;

typedef struct {
    uint32_t   volume;
    int16_t    level;
    uint32_t   counter;
    AdsrPhase  phase;
    uint16_t   sustain_level;
} AdsrEnvelope;

typedef struct {
    uint32_t     start_address;
    uint32_t     repeat_address;
    uint32_t     current_address;
    uint16_t     pitch_counter;
    int16_t      decode_buffer[28];
    AdsrEnvelope envelope;
    uint16_t     sample_rate;
    uint8_t      current_buffer_idx;
    int16_t      current_sample;
    bool         keyed_on;
    int16_t      volume_l;
    int16_t      volume_r;
    bool         enable_sweep_l;
    bool         enable_sweep_r;
    uint16_t     sweep_l;
    uint16_t     sweep_r;
    uint16_t     adsr1;
    uint16_t     adsr2;
    bool         reverb_enabled;
} Voice;

/* circular buffer for FIR filter */
typedef struct {
    int16_t  buf[FIR_LEN];
    int      head;
    int      len;
} FirBuf;

typedef struct {
    Voice     voices[NUM_VOICES];
    SDL_AudioDeviceID device;

    uint8_t   sound_ram[SOUND_RAM_SIZE];
    uint32_t  sound_ram_start_address;
    int16_t   main_volume_l;
    int16_t   main_volume_r;
    uint32_t  write_count;

    uint32_t  reverb_start_address;
    uint32_t  reverb_write_address;
    int16_t   reverb_output_volume_l;
    int16_t   reverb_output_volume_r;
    int16_t   reverb_input_volume_l;
    int16_t   reverb_input_volume_r;
    bool      reverb_left;

    uint32_t  mlsame, dlsame, mrsame, drsame;
    uint32_t  mrdiff, dldiff, mldiff, drdiff;
    int16_t   vwall, viir;
    int16_t   vcomb1, vcomb2, vcomb3, vcomb4;
    uint32_t  mlcomb1, mlcomb2, mlcomb3, mlcomb4;
    uint32_t  mrcomb1, mrcomb2, mrcomb3, mrcomb4;
    uint32_t  dapf1, dapf2;
    int16_t   vapf1, vapf2;
    uint32_t  mlapf1, mlapf2, mrapf1, mrapf2;

    FirBuf    far_input_l;
    FirBuf    far_input_r;
} Spu;

int      spu_init(Spu *spu);
uint16_t spu_load(const Spu *spu, uint32_t abs_addr, uint32_t offset);
void     spu_store(Spu *spu, uint32_t abs_addr, uint32_t offset, uint16_t val);
void     spu_clock(Spu *spu);
void     spu_destroy(Spu *spu);
