#include "spu.h"
#include "scheduler.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ---- constants ---- */
#define ENVELOPE_COUNTER_MAX (1u << (33 - 11))
#define SPU_SAMPLE_RATE 44100u
#define SPU_MAX_AUDIO_QUEUE_BYTES (SPU_SAMPLE_RATE / 10u * 2u * sizeof(int16_t))

static const int16_t FIR_FILTER[FIR_LEN] = {
    -0x0001,
    0x0000,
    0x0002,
    0x0000,
    -0x000A,
    0x0000,
    0x0023,
    0x0000,
    -0x0067,
    0x0000,
    0x010A,
    0x0000,
    -0x0268,
    0x0000,
    0x0534,
    0x0000,
    -0x0B90,
    0x0000,
    0x2806,
    0x4000,
    0x2806,
    0x0000,
    -0x0B90,
    0x0000,
    0x0534,
    0x0000,
    -0x0268,
    0x0000,
    0x010A,
    0x0000,
    -0x0067,
    0x0000,
    0x0023,
    0x0000,
    -0x000A,
    0x0000,
    0x0002,
    0x0000,
    -0x0001,
};

/* ---- helpers ---- */
static int16_t apply_volume(int16_t sample, int16_t volume)
{
    return (int16_t)(((int32_t)sample * (int32_t)volume) >> 15);
}

static int16_t clamp16(int32_t v)
{
    if (v < -0x8000)
        return -0x8000;
    if (v > 0x7FFF)
        return 0x7FFF;
    return (int16_t)v;
}

static void spu_store16(Spu *spu, uint32_t offset, uint16_t val)
{
    spu->sound_ram[offset] = (uint8_t)(val);
    spu->sound_ram[offset + 1] = (uint8_t)(val >> 8);
}

static int16_t spu_loadi16(const Spu *spu, uint32_t offset)
{
    uint16_t b0 = spu->sound_ram[offset];
    uint16_t b1 = spu->sound_ram[offset + 1];
    return (int16_t)(b0 | (b1 << 8));
}

/* ---- FIR buffer ---- */
static void fir_push(FirBuf *fb, int16_t sample)
{
    if (fb->len < FIR_LEN)
    {
        fb->buf[fb->len++] = sample;
    }
    else
    {
        fb->buf[fb->head] = sample;
        fb->head = (fb->head + 1) % FIR_LEN;
    }
}

static int16_t fir_apply(const FirBuf *fb)
{
    int32_t acc = 0;
    for (int i = 0; i < fb->len; i++)
    {
        int idx = (fb->head + i) % FIR_LEN;
        acc += (int32_t)FIR_FILTER[i] * (int32_t)fb->buf[idx];
    }
    return clamp16(acc >> 15);
}

/* ---- Reverb addr ---- */
static uint32_t reverb_relative_addr(const Spu *spu, uint32_t offset)
{
    uint32_t addr = offset + spu->reverb_write_address;
    addr = addr % SOUND_RAM_SIZE;
    if (addr < spu->reverb_start_address)
        return spu->reverb_start_address + addr;
    return addr;
}

/* ---- ADPCM decode ---- */
static void decode_adpcm_block(const uint8_t *block, int16_t decoded[28],
                               int16_t *old_sample, int16_t *older_sample)
{
    uint8_t shift = block[0] & 0x0F;
    if (shift > 12)
        shift = 9;
    uint8_t filter = (block[0] >> 4) & 0x07;
    if (filter > 4)
        filter = 4;

    for (int i = 0; i < 28; i++)
    {
        uint8_t byte = block[2 + i / 2];
        uint8_t nibble = (byte >> (4 * (i % 2))) & 0x0F;
        int32_t raw = (int32_t)(int8_t)((nibble << 4)) >> 4;
        int32_t shifted = raw * (1 << (12 - shift));

        int32_t old_ = (int32_t)*old_sample;
        int32_t older_ = (int32_t)*older_sample;
        int32_t filtered;
        switch (filter)
        {
        case 0:
            filtered = shifted;
            break;
        case 1:
            filtered = shifted + (60 * old_ + 32) / 64;
            break;
        case 2:
            filtered = shifted + (115 * old_ - 52 * older_ + 32) / 64;
            break;
        case 3:
            filtered = shifted + (98 * old_ - 55 * older_ + 32) / 64;
            break;
        case 4:
            filtered = shifted + (122 * old_ - 60 * older_ + 32) / 64;
            break;
        default:
            filtered = shifted;
            break;
        }
        int16_t clamped = clamp16(filtered);
        decoded[i] = clamped;
        *older_sample = *old_sample;
        *old_sample = clamped;
    }
}

/* ---- Voice ---- */
static void voice_init(Voice *v)
{
    memset(v, 0, sizeof(*v));
    v->keyed_on = false;
    v->volume_l = 0x7FFF;
    v->volume_r = 0x7FFF;
    v->envelope.phase = ADSR_RELEASE;
}

static void adsr_key_on(AdsrEnvelope *e)
{
    e->level = 0;
    e->phase = ADSR_ATTACK;
    e->counter = ENVELOPE_COUNTER_MAX;
}

static void adsr_key_off(AdsrEnvelope *e)
{
    e->phase = ADSR_RELEASE;
}

static void adsr_check_transition(AdsrEnvelope *e)
{
    if (e->phase == ADSR_ATTACK && e->level == 0x7FFF)
        e->phase = ADSR_DECAY;
    if (e->phase == ADSR_DECAY && (uint16_t)e->level <= e->sustain_level)
        e->phase = ADSR_SUSTAIN;
}

static void adsr_update(AdsrEnvelope *e, EnvDirection dir, ChangeRate rate,
                        uint8_t shift, uint8_t step)
{
    int32_t s = (int32_t)(7 - step);
    if (dir == DIR_DECREASING)
        s = ~s;
    uint8_t sat = (shift > 11) ? 0 : (11 - shift);
    s *= (1 << sat);
    int32_t cur = (int32_t)e->level;
    if (dir == DIR_DECREASING && rate == RATE_EXPONENTIAL)
        s = (s * cur) >> 15;
    int32_t next = cur + s;
    if (next < 0)
        next = 0;
    if (next > 0x7FFF)
        next = 0x7FFF;
    e->level = (int16_t)next;
}

static void adsr_clock(AdsrEnvelope *e, EnvDirection dir, ChangeRate rate,
                       uint8_t shift, uint8_t step)
{
    uint32_t decrement = ENVELOPE_COUNTER_MAX >> (shift > 11 ? 0 : (11 - shift));
    if (dir == DIR_INCREASING && rate == RATE_EXPONENTIAL && e->level > 0x6000)
        decrement >>= 2;
    if (e->counter < decrement)
        e->counter = 0;
    else
        e->counter -= decrement;
    if (e->counter == 0)
    {
        e->counter = ENVELOPE_COUNTER_MAX;
        adsr_update(e, dir, rate, shift, step);
        adsr_check_transition(e);
    }
}

static void voice_decode_next_block(Voice *v, const uint8_t *sound_ram)
{
    const uint8_t *block = &sound_ram[v->current_address];
    int16_t old = v->decode_buffer[27];
    int16_t older = v->decode_buffer[26];
    decode_adpcm_block(block, v->decode_buffer, &old, &older);

    bool loop_end = (block[1] & (1 << 0)) != 0;
    bool loop_repeat = (block[1] & (1 << 1)) != 0;
    bool loop_start = (block[1] & (1 << 2)) != 0;

    if (loop_start)
        v->repeat_address = v->current_address;

    if (loop_end)
    {
        v->current_address = v->repeat_address;
        if (!loop_repeat)
        {
            v->envelope.volume = 0;
            adsr_key_off(&v->envelope);
        }
    }
    else
    {
        v->current_address += 16;
    }
}

static void voice_key_on(Voice *v, const uint8_t *sound_ram)
{
    adsr_key_on(&v->envelope);
    v->current_address = v->start_address;
    v->pitch_counter = 0;
    voice_decode_next_block(v, sound_ram);
    v->keyed_on = true;
}

static void voice_key_off(Voice *v)
{
    adsr_key_off(&v->envelope);
    v->keyed_on = false;
}

static void voice_clock(Voice *v, const uint8_t *sound_ram)
{
    EnvDirection dir = DIR_INCREASING;
    ChangeRate rate = RATE_LINEAR;
    uint8_t shift = 0, step = 0;

    switch (v->envelope.phase)
    {
    case ADSR_ATTACK:
        rate = (v->adsr1 & 0x8000) ? RATE_EXPONENTIAL : RATE_LINEAR;
        dir = DIR_INCREASING;
        shift = (v->adsr1 >> 10) & 0x1F;
        step = (v->adsr1 >> 8) & 0x03;
        break;
    case ADSR_DECAY:
        rate = RATE_EXPONENTIAL;
        dir = DIR_DECREASING;
        shift = (v->adsr1 >> 4) & 0x0F;
        step = 0;
        break;
    case ADSR_SUSTAIN:
        v->envelope.sustain_level = ((v->adsr1 & 0x000F) + 1) * 0x0800;
        rate = (v->adsr2 & 0x8000) ? RATE_EXPONENTIAL : RATE_LINEAR;
        dir = (v->adsr2 & 0x4000) ? DIR_DECREASING : DIR_INCREASING;
        shift = (v->adsr2 >> 8) & 0x1F;
        step = (v->adsr2 >> 6) & 0x03;
        break;
    case ADSR_RELEASE:
        rate = (v->adsr2 & 0x0020) ? RATE_LINEAR : RATE_EXPONENTIAL;
        dir = DIR_DECREASING;
        shift = v->adsr2 & 0x001F;
        step = 0;
        break;
    }
    adsr_clock(&v->envelope, dir, rate, shift, step);

    uint16_t step_val = v->sample_rate < 0x4000 ? v->sample_rate : 0x4000;
    v->pitch_counter += step_val;
    while (v->pitch_counter >= 0x1000)
    {
        v->pitch_counter -= 0x1000;
        v->current_buffer_idx++;
        if (v->current_buffer_idx == 28)
        {
            v->current_buffer_idx = 0;
            voice_decode_next_block(v, sound_ram);
        }
    }
    v->current_sample = v->decode_buffer[v->current_buffer_idx];
}

static void voice_store(Voice *v, uint32_t offset, uint16_t val)
{
    switch (offset)
    {
    case 0x00:
        v->enable_sweep_l = (val & 0x8000) != 0;
        if (v->enable_sweep_l)
            v->sweep_l = val;
        else
            v->volume_l = (int16_t)(val << 1);
        break;
    case 0x02:
        v->enable_sweep_r = (val & 0x8000) != 0;
        if (v->enable_sweep_r)
            v->sweep_r = val;
        else
            v->volume_r = (int16_t)(val << 1);
        break;
    case 0x04:
        v->sample_rate = val;
        break;
    case 0x06:
        v->start_address = (uint32_t)val << 3;
        break;
    case 0x08:
        v->adsr1 = val;
        break;
    case 0x0A:
        v->adsr2 = val;
        break;
    case 0x0E:
        v->repeat_address = (uint32_t)val << 3;
        break;
    default:
        break;
    }
}

static void voice_apply_volume(const Voice *v, int16_t adpcm, int16_t *out_l, int16_t *out_r)
{
    int16_t env = apply_volume(adpcm, v->envelope.level);
    *out_l = apply_volume(env, v->volume_l);
    *out_r = apply_volume(env, v->volume_r);
}

/* ---- Reverb processing ---- */
static void apply_same_side_reflection_inner(Spu *spu, int16_t input,
                                             uint32_t m_addr, uint32_t d_addr)
{
    int16_t a = spu_loadi16(spu, reverb_relative_addr(spu, d_addr));
    int16_t b = spu_loadi16(spu, reverb_relative_addr(spu, m_addr - 2));
    int16_t val = apply_volume(input + apply_volume(a, spu->vwall) - b, spu->viir) + b;
    spu_store16(spu, m_addr, (uint16_t)val);
}

static void apply_same_side_reflection(Spu *spu, int16_t input)
{
    apply_same_side_reflection_inner(spu, input, spu->mlsame, spu->dlsame);
    apply_same_side_reflection_inner(spu, input, spu->mrsame, spu->drsame);
    apply_same_side_reflection_inner(spu, input, spu->mrdiff, spu->dldiff);
    apply_same_side_reflection_inner(spu, input, spu->mldiff, spu->drdiff);
}

static int16_t apply_comb_filter(Spu *spu)
{
    int16_t c1 = spu_loadi16(spu, reverb_relative_addr(spu, spu->reverb_left ? spu->mlcomb1 : spu->mrcomb1));
    int16_t c2 = spu_loadi16(spu, reverb_relative_addr(spu, spu->reverb_left ? spu->mlcomb2 : spu->mrcomb2));
    int16_t c3 = spu_loadi16(spu, reverb_relative_addr(spu, spu->reverb_left ? spu->mlcomb3 : spu->mrcomb3));
    int16_t c4 = spu_loadi16(spu, reverb_relative_addr(spu, spu->reverb_left ? spu->mlcomb4 : spu->mrcomb4));
    return apply_volume(c1, spu->vcomb1) + apply_volume(c2, spu->vcomb2) + apply_volume(c3, spu->vcomb3) + apply_volume(c4, spu->vcomb4);
}

static int16_t apply_apf1(Spu *spu, int16_t input)
{
    uint32_t mapf = spu->reverb_left ? spu->mlapf1 : spu->mrapf1;
    int16_t a = spu_loadi16(spu, reverb_relative_addr(spu, mapf - spu->dapf1));
    int16_t buffered = input - apply_volume(a, spu->vapf1);
    spu_store16(spu, mapf, (uint16_t)buffered);
    return apply_volume(buffered, spu->vapf1) + a;
}

static int16_t apply_apf2(Spu *spu, int16_t input)
{
    uint32_t mapf = spu->reverb_left ? spu->mlapf2 : spu->mrapf2;
    int16_t a = spu_loadi16(spu, reverb_relative_addr(spu, mapf - spu->dapf2));
    int16_t buffered = input - apply_volume(a, spu->vapf2);
    spu_store16(spu, mapf, (uint16_t)buffered);
    return apply_volume(buffered, spu->vapf2) + a;
}

/* ---- Init / destroy ---- */
int spu_init(Spu *spu, bool enable_audio)
{
    memset(spu, 0, sizeof(*spu));
    for (int i = 0; i < NUM_VOICES; i++)
        voice_init(&spu->voices[i]);
    spu->main_volume_l = 0x7FFF;
    spu->main_volume_r = 0x7FFF;
    spu->reverb_left = true;

    if (!enable_audio)
        return 0;

    SDL_AudioSpec desired, obtained;
    SDL_zero(desired);
    desired.freq = 44100;
    desired.format = AUDIO_S16SYS;
    desired.channels = 2;
    desired.samples = 512;
    desired.callback = NULL;
    spu->device = SDL_OpenAudioDevice(NULL, 0, &desired, &obtained, 0);
    if (spu->device == 0)
    {
        fprintf(stderr, "SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return -1;
    }
    SDL_PauseAudioDevice(spu->device, 0);
    return 0;
}

void spu_destroy(Spu *spu)
{
    if (spu->device)
    {
        SDL_CloseAudioDevice(spu->device);
        spu->device = 0;
    }
}

void spu_push_cd_audio_frame(Spu *spu, int16_t left, int16_t right)
{
    if (spu->cd_audio_count >= SPU_CD_AUDIO_BUFFER_FRAMES)
    {
        spu->cd_audio_head = (spu->cd_audio_head + 1u) % SPU_CD_AUDIO_BUFFER_FRAMES;
        spu->cd_audio_count--;
    }

    spu->cd_audio_l[spu->cd_audio_tail] = left;
    spu->cd_audio_r[spu->cd_audio_tail] = right;
    spu->cd_audio_tail = (spu->cd_audio_tail + 1u) % SPU_CD_AUDIO_BUFFER_FRAMES;
    spu->cd_audio_count++;
}

void spu_clear_cd_audio(Spu *spu)
{
    spu->cd_audio_head = 0;
    spu->cd_audio_tail = 0;
    spu->cd_audio_count = 0;
}

static void spu_pop_cd_audio_frame(Spu *spu, int16_t *left, int16_t *right)
{
    if (spu->cd_audio_count == 0)
    {
        *left = 0;
        *right = 0;
        return;
    }

    *left = spu->cd_audio_l[spu->cd_audio_head];
    *right = spu->cd_audio_r[spu->cd_audio_head];
    spu->cd_audio_head = (spu->cd_audio_head + 1u) % SPU_CD_AUDIO_BUFFER_FRAMES;
    spu->cd_audio_count--;
}

/* ---- Load / Store ---- */
uint16_t spu_load(const Spu *spu, uint32_t abs_addr, uint32_t offset)
{
    (void)abs_addr;
    if (offset == 0x01A8)
        return 0xFFFF;

    uint16_t val = 0;
    uint32_t reg = (offset & 0x3FEu) >> 1;
    if (reg < SPU_REG_WORDS)
        val = spu->regs[reg];
    return val;
}

void spu_store(Spu *spu, uint32_t abs_addr, uint32_t offset, uint16_t val)
{
    (void)abs_addr;
    uint32_t reg = (offset & 0x3FEu) >> 1;
    if (reg < SPU_REG_WORDS)
        spu->regs[reg] = val;

    if (offset <= 0x017F)
    {
        int index = offset / 0x10;
        voice_store(&spu->voices[index], offset % 0x10, val);
        return;
    }
    switch (offset)
    {
    case 0x01A6:
        spu->sound_ram_start_address = (uint32_t)val << 3;
        spu->write_count = 0;
        break;
    case 0x01A8:
        spu_store16(spu, spu->sound_ram_start_address, val);
        spu->sound_ram_start_address += 2;
        spu->write_count++;
        break;
    case 0x0188:
        for (int i = 0; i < 16; i++)
            if (val & (1 << i))
                voice_key_on(&spu->voices[i], spu->sound_ram);
        break;
    case 0x018A:
        for (int i = 0; i < 8; i++)
            if (val & (1 << i))
                voice_key_on(&spu->voices[16 + i], spu->sound_ram);
        break;
    case 0x018C:
        for (int i = 0; i < 16; i++)
            if (val & (1 << i))
                voice_key_off(&spu->voices[i]);
        break;
    case 0x018E:
        for (int i = 0; i < 8; i++)
            if (val & (1 << i))
                voice_key_off(&spu->voices[16 + i]);
        break;
    case 0x0198:
        for (int i = 0; i < 16; i++)
            spu->voices[i].reverb_enabled = (val & (1 << i)) != 0;
        break;
    case 0x019A:
        for (int i = 0; i < 8; i++)
            spu->voices[16 + i].reverb_enabled = (val & (1 << i)) != 0;
        break;
    case 0x0180:
        if (!(val & 0x8000))
            spu->main_volume_l = (int16_t)(val << 1);
        break;
    case 0x0182:
        if (!(val & 0x8000))
            spu->main_volume_r = (int16_t)(val << 1);
        break;
    case 0x0184:
        spu->reverb_output_volume_l = (int16_t)val;
        break;
    case 0x0186:
        spu->reverb_output_volume_r = (int16_t)val;
        break;
    case 0x01A2:
        spu->reverb_start_address = (uint32_t)val << 3;
        spu->reverb_write_address = spu->reverb_start_address;
        break;
    case 0x01FC:
        spu->reverb_input_volume_l = (int16_t)val;
        break;
    case 0x01FE:
        spu->reverb_input_volume_r = (int16_t)val;
        break;
    case 0x01D4:
        spu->mlsame = (uint32_t)val << 3;
        break;
    case 0x01E0:
        spu->dlsame = (uint32_t)val << 3;
        break;
    case 0x01D6:
        spu->mrsame = (uint32_t)val << 3;
        break;
    case 0x01E2:
        spu->drsame = (uint32_t)val << 3;
        break;
    case 0x01E6:
        spu->mrdiff = (uint32_t)val << 3;
        break;
    case 0x01F0:
        spu->dldiff = (uint32_t)val << 3;
        break;
    case 0x01E4:
        spu->mldiff = (uint32_t)val << 3;
        break;
    case 0x01F2:
        spu->drdiff = (uint32_t)val << 3;
        break;
    case 0x01C4:
        spu->viir = (int16_t)val;
        break;
    case 0x01CE:
        spu->vwall = (int16_t)val;
        break;
    case 0x01C6:
        spu->vcomb1 = (int16_t)val;
        break;
    case 0x01C8:
        spu->vcomb2 = (int16_t)val;
        break;
    case 0x01CA:
        spu->vcomb3 = (int16_t)val;
        break;
    case 0x01CC:
        spu->vcomb4 = (int16_t)val;
        break;
    case 0x01D8:
        spu->mlcomb1 = (uint32_t)val << 3;
        break;
    case 0x01DA:
        spu->mrcomb1 = (uint32_t)val << 3;
        break;
    case 0x01DC:
        spu->mlcomb2 = (uint32_t)val << 3;
        break;
    case 0x01DE:
        spu->mrcomb2 = (uint32_t)val << 3;
        break;
    case 0x01E8:
        spu->mlcomb3 = (uint32_t)val << 3;
        break;
    case 0x01EA:
        spu->mrcomb3 = (uint32_t)val << 3;
        break;
    case 0x01EC:
        spu->mlcomb4 = (uint32_t)val << 3;
        break;
    case 0x01EE:
        spu->mrcomb4 = (uint32_t)val << 3;
        break;
    case 0x01C0:
        spu->dapf1 = (uint32_t)val << 3;
        break;
    case 0x01C2:
        spu->dapf2 = (uint32_t)val << 3;
        break;
    case 0x01D0:
        spu->vapf1 = (int16_t)val;
        break;
    case 0x01D2:
        spu->vapf2 = (int16_t)val;
        break;
    case 0x01F4:
        spu->mlapf1 = (uint32_t)val << 3;
        break;
    case 0x01F6:
        spu->mrapf1 = (uint32_t)val << 3;
        break;
    case 0x01F8:
        spu->mlapf2 = (uint32_t)val << 3;
        break;
    case 0x01FA:
        spu->mrapf2 = (uint32_t)val << 3;
        break;
    default:
        break;
    }
}

/* ---- Clock ---- */
void spu_clock(Spu *spu)
{
    int32_t mixed_l = 0, mixed_r = 0, reverb = 0;

    for (int i = 0; i < NUM_VOICES; i++)
    {
        Voice *v = &spu->voices[i];
        if (!v->keyed_on)
            continue;
        voice_clock(v, spu->sound_ram);
        int16_t sl, sr;
        voice_apply_volume(v, v->current_sample, &sl, &sr);
        mixed_l += sl;
        mixed_r += sr;
        if (v->reverb_enabled)
            reverb += (spu->reverb_left ? sl : sr);
    }

    int16_t clamped_l = clamp16(mixed_l);
    int16_t clamped_r = clamp16(mixed_r);
    int16_t clamped_reverb = clamp16(reverb);

    int16_t input_reverb = apply_volume(clamped_reverb,
                                        spu->reverb_left ? spu->reverb_input_volume_l : spu->reverb_input_volume_r);
    spu_store16(spu, spu->reverb_write_address, (uint16_t)input_reverb);
    spu->reverb_write_address += 2;
    if (spu->reverb_write_address > 0x7FFFF)
        spu->reverb_write_address = spu->reverb_start_address;

    int16_t input_sample = spu->reverb_left ? clamped_l : clamped_r;
    apply_same_side_reflection(spu, input_sample);
    int16_t comb_out = apply_comb_filter(spu);
    int16_t apf1_out = apply_apf1(spu, comb_out);
    int16_t apf2_out = apply_apf2(spu, apf1_out);
    if (spu->reverb_left)
        fir_push(&spu->far_input_l, apf2_out);
    else
        fir_push(&spu->far_input_r, apf2_out);

    int16_t reverb_l = fir_apply(&spu->far_input_l);
    int16_t reverb_r = fir_apply(&spu->far_input_r);

    int16_t out_reverb_l = apply_volume(reverb_l, spu->reverb_output_volume_l);
    int16_t out_reverb_r = apply_volume(reverb_r, spu->reverb_output_volume_r);

    spu->reverb_left = !spu->reverb_left;

    int16_t with_l = clamped_l + out_reverb_l;
    int16_t with_r = clamped_r + out_reverb_r;
    int16_t cd_l, cd_r;
    spu_pop_cd_audio_frame(spu, &cd_l, &cd_r);
    with_l = clamp16((int32_t)with_l + cd_l);
    with_r = clamp16((int32_t)with_r + cd_r);
    int16_t out_l = apply_volume(with_l, spu->main_volume_l);
    int16_t out_r = apply_volume(with_r, spu->main_volume_r);

    if (spu->device)
    {
        if (SDL_GetQueuedAudioSize(spu->device) > SPU_MAX_AUDIO_QUEUE_BYTES)
            SDL_ClearQueuedAudio(spu->device);
        int16_t samples[2] = {out_l, out_r};
        SDL_QueueAudio(spu->device, samples, sizeof(samples));
    }
}

void spu_step(Spu *spu, uint32_t cpu_cycles)
{
    uint64_t accum = (uint64_t)spu->sample_cycle_accum +
                     (uint64_t)cpu_cycles * SPU_SAMPLE_RATE;
    while (accum >= PS1_CPU_HZ)
    {
        accum -= PS1_CPU_HZ;
        spu_clock(spu);
    }
    spu->sample_cycle_accum = (uint32_t)accum;
}

void spu_dma_write(Spu *spu, uint32_t word)
{
    uint32_t addr = spu->sound_ram_start_address & (SOUND_RAM_SIZE - 1u);
    spu->sound_ram[addr] = (uint8_t)(word & 0xFF);
    spu->sound_ram[addr + 1] = (uint8_t)((word >> 8) & 0xFF);
    uint32_t addr2 = (addr + 2u) & (SOUND_RAM_SIZE - 1u);
    spu->sound_ram[addr2] = (uint8_t)((word >> 16) & 0xFF);
    spu->sound_ram[addr2 + 1] = (uint8_t)((word >> 24) & 0xFF);
    spu->sound_ram_start_address = (addr + 4u) & (SOUND_RAM_SIZE - 1u);
}
