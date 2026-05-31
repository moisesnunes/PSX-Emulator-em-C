#pragma once
#include <stdint.h>
#include <stdbool.h>

/*
 * PS1 GTE (COP2) — Geometry Transformation Engine
 *
 * 32 data registers  (cop2 data, accessed via MFC2/MTC2/LWC2/SWC2)
 * 32 control registers (cop2 ctrl, accessed via CFC2/CTC2)
 * FLAGS register (ctrl[31]) — overflow/clamp flags
 */

typedef struct
{
    uint32_t dr[32];   /* COP2 data  registers d0..d31  */
    uint32_t cr[32];   /* COP2 ctrl  registers c0..c31  */
} Gte;

void     gte_init(Gte *gte);

/* Register access */
uint32_t gte_read_data(Gte *gte, uint32_t reg);
void     gte_write_data(Gte *gte, uint32_t reg, uint32_t val);
uint32_t gte_read_ctrl(Gte *gte, uint32_t reg);
void     gte_write_ctrl(Gte *gte, uint32_t reg, uint32_t val);

/* Memory ops (LWC2 / SWC2 equivalent — indexed by GTE data reg number) */
void     gte_load(Gte *gte, uint32_t reg, uint32_t val);   /* same as write_data */
uint32_t gte_store(Gte *gte, uint32_t reg);                /* same as read_data  */

/* Execute a GTE command (bits 24..0 of the COP2 instruction) */
void     gte_execute(Gte *gte, uint32_t cmd);
