/* Neo Geo Pocket link cable: serial channel 0 of the TLCS-900H and the
 * BIOS COM calls, for the dual link (libretro/ngp_dual.cpp).
 *
 * Adapted from Beetle NeoPop Dual's hb_link.c (handheld-link-cores,
 * GPL-2.0-or-later) to RACE's memory map and HLE BIOS.
 *
 * Everything here only acts while ngp_link_on is set (the dual link is
 * up); otherwise RACE behaves as it always did (the COM calls answer that
 * nothing is there).
 */
#ifndef NGP_LINK_H
#define NGP_LINK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern uint8_t ngp_link_on;   /* The cable is plugged (dual link up) */
extern uint8_t ngp_link_cts;  /* Our CTS: the other end's RTS */

/* Where the default serial handlers of the HLE BIOS live (COMINIT puts
 * them at the user vectors, as the real BIOS does) */
#define NGP_COM_RXISR 0xFFFC00
#define NGP_COM_TXISR 0xFFFC10
#define NGP_BIOS_RXISR_NR 0x1B
#define NGP_BIOS_TXISR_NR 0x1C

void ngp_sio_reset(void);
/* I/O registers 0x50-0x53 (SC0BUF, SC0CR, SC0MOD, BR0CR): 1 if handled */
int ngp_sio_io_read(uint32_t address, uint8_t *out);
int ngp_sio_io_write(uint32_t address, uint8_t data);
/* Time goes on (system clock cycles) */
void ngp_sio_tick(int32_t cycles);
/* A byte from the other end */
void ngp_sio_receive(uint8_t data);
/* A byte we finished sending, for the other end: 1 if there was one */
int ngp_sio_take(uint8_t *data);
/* Our RTS (ready to take a byte), for the other end's CTS */
uint8_t ngp_sio_rts(void);
/* BIOS calls 0x10-0x1A (COM*) and the default serial handlers 0x1B and
 * 0x1C: 1 if handled (*cycles: what it took) */
int ngp_com_hle(unsigned char nr, int *cycles);
/* Puts the default serial handlers in the HLE BIOS */
void ngp_link_install_bios(unsigned char *cpurom);

#ifdef __cplusplus
}
#endif

#endif
