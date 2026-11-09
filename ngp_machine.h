/* The machine as the dual link drives it: see ngp_machine.c. The hidden
 * instance B has the same functions, named ngpb_ngpm_*. */
#ifndef NGP_MACHINE_H
#define NGP_MACHINE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NGPM_API(name) name

#define NGPM_DECLARE(p) \
   void p##ngpm_config(int console_type, int language, int accurate_audio); \
   int p##ngpm_load_rom(const uint8_t *rom, size_t size, const char *name);  \
   void p##ngpm_reset(void);                                                 \
   size_t p##ngpm_state_size(void);                                          \
   int p##ngpm_save(void *buf);                                              \
   int p##ngpm_load(const void *buf);                                        \
   void p##ngpm_frame_begin(uint8_t keys);                                   \
   void p##ngpm_run(int cycles, int render);                                 \
   uint32_t p##ngpm_hash(void);                                              \
   void p##ngpm_link(int on);                                                \
   int p##ngpm_take(uint8_t *data);                                          \
   void p##ngpm_give(uint8_t data);                                          \
   uint8_t p##ngpm_rts(void);                                                \
   void p##ngpm_cts(uint8_t on);                                             \
   uint8_t *p##ngpm_rom(void);                                               \
   size_t p##ngpm_rom_size(void);

NGPM_DECLARE()
#ifndef RACE_INSTANCE_B
NGPM_DECLARE(ngpb_)
#endif

#ifdef __cplusplus
}
#endif

#endif
