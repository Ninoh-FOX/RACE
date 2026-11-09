/* Neo Geo Pocket link cable: serial channel 0 and the BIOS COM calls.
 * See ngp_link.h.
 *
 * Adapted from Beetle NeoPop Dual's hb_link.c (handheld-link-cores,
 * GPL-2.0-or-later), with the fixes it gathered from games:
 *  - KOF R-2, SvC Match of the Millennium, Card Fighters put their own
 *    serial handlers at the user vectors 0x6FE4 (INTRX0) and 0x6FE8 (INTTX0)
 *    right after COMINIT; Pocket Tennis, Tsunagete Pon, Magical Drop rely on
 *    the BIOS ones, which COMINIT installs (as the real BIOS does).
 *  - INTTX0 comes when SC0BUF moves to the shift register (the buffer is
 *    free again), not when the byte is out (Delta Warp's time packet).
 *  - COMRECIVESTART turns RTS on (Card Fighters' "either side, A" screen).
 *  - COMCREATEDATA answers 0xFF when the send ring is full (Card Fighters
 *    puts the byte again after sending).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include <string.h>

#include "ngp_link.h"
#include "race-memory.h"

extern unsigned char *allregsB[256];
extern unsigned short *allregsW[256];
extern unsigned int *allregsL[256];
void tlcs_interrupt_wrapper(int irq);

#define rB(r) (*allregsB[r])
#define rW(r) (*allregsW[r])
#define rL(r) (*allregsL[r])

#define INT_RX0 0x0E   /* tlcs_interrupt: INTRX0 */
#define INT_TX0 0x0F   /* tlcs_interrupt: INTTX0 */

uint8_t ngp_link_on;
uint8_t ngp_link_cts;

/* Serial channel 0 */
static uint8_t sio_mod = 0x69, sio_br = 0x15, sio_cr = 0x20, sio_err;
static uint8_t sio_rxbuf, sio_rxfull;
static uint8_t sio_txbyte, sio_txnext, sio_txhasnext, sio_txwait, sio_txint;
static int32_t sio_txleft = -1;            /* Cycles left for the byte on the line, -1: idle */
static uint8_t sio_out[16];                /* Bytes sent, for the other end */
static uint8_t sio_outn;

/* BIOS work RAM of the COM calls (where the SNK serial handlers keep it) */
#define COM_TXBUF  0x6C80
#define COM_RXBUF  0x6CC0
#define COM_TXCNT  0x6D00
#define COM_RXCNT  0x6D01
#define COM_TXHEAD 0x6D02
#define COM_RXHEAD 0x6D03
#define COM_TXBUSY 0x6D04
#define COM_RXSTOP 0x6D05
#define COM_ERR    0x6D06

#define COM_BUF_OK    0
#define COM_BUF_EMPTY 1
#define COM_BUF_OVER  2

#define COMMStatus cpuram[0xB2]            /* bit 0: RTS, 0 = on */

static uint8_t loadB(uint32_t a)
{
   if (a >= 0x50 && a <= 0x53)
   {
      uint8_t v = 0;
      ngp_sio_io_read(a, &v);
      return v;
   }
   return tlcsMemReadB(a);
}

static void storeB(uint32_t a, uint8_t v)
{
   if (a >= 0x50 && a <= 0x53)
      ngp_sio_io_write(a, v);
   else
      tlcsMemWriteB(a, v);
}

static void storeL(uint32_t a, uint32_t v)
{
   storeB(a, (uint8_t)v);
   storeB(a + 1, (uint8_t)(v >> 8));
   storeB(a + 2, (uint8_t)(v >> 16));
   storeB(a + 3, (uint8_t)(v >> 24));
}

void ngp_sio_reset(void)
{
   sio_mod = 0x69;
   sio_br = 0x15;
   sio_cr = 0x20;
   sio_err = 0;
   sio_rxbuf = 0;
   sio_rxfull = 0;
   sio_txbyte = sio_txnext = sio_txhasnext = sio_txwait = sio_txint = 0;
   sio_txleft = -1;
   sio_outn = 0;
   ngp_link_cts = 0;
}

/* One byte on the line, in system clock cycles (6.144 MHz). BR0CR 0x15
 * (phiT2 / 5) is 19200 bps, 320 cycles a bit; the prescaler inputs
 * (phiT0, T2, T8, T32 = 1 : 4 : 16 : 64) and the divider scale it. Start,
 * 8 bits and stop: 10 bits (11 in 9 bit mode). */
static int32_t byte_cycles(void)
{
   static const int32_t pre[4] = { 1, 4, 16, 64 };
   int32_t n = sio_br & 0x0F;
   int32_t bit;
   if (!n)
      n = 16;
   bit = 320 * pre[(sio_br >> 4) & 3] * n / 20;
   if (bit < 16)
      bit = 16;
   return bit * ((((sio_mod >> 2) & 3) == 3) ? 11 : 10);
}

static void tx_start(uint8_t data)
{
   sio_txbyte = data;
   if ((sio_mod & 0x40) && !ngp_link_cts)
   {
      sio_txwait = 1;            /* CTSE: wait for the other end's RTS */
      sio_txleft = -1;
   }
   else
   {
      sio_txwait = 0;
      sio_txleft = byte_cycles();
      sio_txint = 1;             /* SC0BUF is free again */
   }
}

int ngp_sio_io_read(uint32_t address, uint8_t *out)
{
   if (!ngp_link_on)
      return 0;
   switch (address)
   {
      case 0x50:
         *out = sio_rxbuf;
         sio_rxfull = 0;
         return 1;
      case 0x51:
         *out = (uint8_t)((sio_cr & 0xE3) | sio_err);
         sio_err = 0;            /* Reading clears the error bits */
         return 1;
      case 0x52:
         *out = sio_mod;
         return 1;
      case 0x53:
         *out = sio_br;
         return 1;
   }
   return 0;
}

int ngp_sio_io_write(uint32_t address, uint8_t data)
{
   if (!ngp_link_on)
      return 0;
   switch (address)
   {
      case 0x50:
         if (sio_txleft < 0 && !sio_txwait)
            tx_start(data);
         else
         {
            sio_txnext = data;   /* Waits while the line is busy */
            sio_txhasnext = 1;
         }
         return 1;
      case 0x51:
         sio_cr = data;
         return 1;
      case 0x52:
         sio_mod = data;
         return 1;
      case 0x53:
         sio_br = data;
         return 1;
   }
   return 0;
}

static void sio_tick(int32_t cycles)
{
   if (sio_txwait)
   {
      if (!(sio_mod & 0x40) || ngp_link_cts)
      {
         sio_txwait = 0;
         sio_txleft = byte_cycles();
         sio_txint = 1;
      }
      else
         return;
   }
   if (sio_txleft < 0)
      return;
   sio_txleft -= cycles;
   if (sio_txleft > 0)
      return;
   /* The byte is out: to the other end */
   sio_txleft = -1;
   if (sio_outn < sizeof(sio_out))
      sio_out[sio_outn++] = sio_txbyte;
   if (sio_txhasnext)
   {
      sio_txhasnext = 0;
      tx_start(sio_txnext);      /* (its INTTX0 comes from tx_start) */
   }
}

void ngp_sio_tick(int32_t cycles)
{
   if (!ngp_link_on)
      return;
   sio_tick(cycles);
   if (sio_txint)
   {
      sio_txint = 0;
      tlcs_interrupt_wrapper(INT_TX0);
   }
}

void ngp_sio_receive(uint8_t data)
{
   if (!ngp_link_on || !(sio_mod & 0x20))
      return;                    /* Receiving off: lost, as on the machine */
   if (sio_rxfull)
      sio_err |= 0x10;           /* Overrun */
   sio_rxbuf = data;
   sio_rxfull = 1;
   tlcs_interrupt_wrapper(INT_RX0);
}

int ngp_sio_take(uint8_t *data)
{
   if (!sio_outn)
      return 0;
   *data = sio_out[0];
   memmove(sio_out, sio_out + 1, --sio_outn);
   return 1;
}

uint8_t ngp_sio_rts(void)
{
   return (COMMStatus & 1) == 0;
}

/* ---- BIOS COM calls -------------------------------------------------- */

static void com_kick_tx(void)
{
   uint8_t cnt, head;
   if (loadB(COM_TXBUSY))
      return;                    /* Sending: the INTTX0 handler goes on */
   cnt = loadB(COM_TXCNT);
   if (!cnt)
      return;
   head = loadB(COM_TXHEAD);
   storeB(COM_TXBUSY, 0xFF);
   storeB(COM_TXCNT, (uint8_t)(cnt - 1));
   storeB(COM_TXHEAD, (uint8_t)(head + 1));
   storeB(0x50, loadB(COM_TXBUF + (head & 0x3F)));
}

static int com_put(uint8_t data)
{
   uint8_t cnt = loadB(COM_TXCNT);
   if (cnt >= 64)
      return 0;
   storeB(COM_TXBUF + ((loadB(COM_TXHEAD) + cnt) & 0x3F), data);
   storeB(COM_TXCNT, (uint8_t)(cnt + 1));
   return 1;
}

static int com_get(uint8_t *data)
{
   uint8_t cnt = loadB(COM_RXCNT), head;
   if (!cnt)
      return 0;
   head = loadB(COM_RXHEAD);
   *data = loadB(COM_RXBUF + (head & 0x3F));
   storeB(COM_RXHEAD, (uint8_t)(head + 1));
   storeB(COM_RXCNT, (uint8_t)(cnt - 1));
   /* The receive handler stopped RTS when the ring was nearly full: on
    * again once there is room */
   if (loadB(COM_RXSTOP) == 0xFF && cnt - 1 < 0x20)
   {
      storeB(COM_RXSTOP, 0);
      COMMStatus &= ~1;
   }
   return 1;
}

/* The BIOS serial handlers (what the SNK library ones do too). RETI
 * follows in the BIOS. */
static void com_rx_isr(void)
{
   uint8_t rts = COMMStatus & 1;
   uint8_t err = (uint8_t)(loadB(0x51) & 0x1C);
   uint8_t cnt = loadB(COM_RXCNT);
   if (err)
      storeB(COM_ERR, (uint8_t)(loadB(COM_ERR) | err));
   storeB(COM_RXCNT, (uint8_t)(cnt + 1));
   storeB(COM_RXBUF + ((cnt + loadB(COM_RXHEAD)) & 0x3F), loadB(0x50));
   if ((int8_t)(cnt + 1) > 0x37)
   {
      storeB(COM_RXSTOP, 0xFF);
      COMMStatus |= 1;
   }
   else
   {
      storeB(COM_RXSTOP, 0);
      COMMStatus = (uint8_t)((COMMStatus & ~1) | rts);
   }
}

static void com_tx_isr(void)
{
   uint8_t cnt = loadB(COM_TXCNT);
   if (cnt)
   {
      uint8_t head = loadB(COM_TXHEAD);
      storeB(COM_TXBUSY, 0xFF);
      storeB(COM_TXCNT, (uint8_t)(cnt - 1));
      storeB(COM_TXHEAD, (uint8_t)(head + 1));
      storeB(0x50, loadB(COM_TXBUF + (head & 0x3F)));
   }
   else
      storeB(COM_TXBUSY, 0);
}

int ngp_com_hle(unsigned char nr, int *cycles)
{
   uint8_t data;
   if (!ngp_link_on)
      return 0;
   *cycles = 100;
   switch (nr)
   {
      case 0x10: /* COMINIT */
      {
         uint32_t a;
         sio_mod = 0x69;         /* 8 bit UART, receiving on, CTS handshake */
         sio_br = 0x15;
         sio_err = 0;
         sio_rxfull = 0;
         sio_txleft = -1;
         sio_txwait = 0;
         sio_txhasnext = 0;
         sio_txint = 0;
         for (a = COM_TXCNT; a <= COM_ERR; a++)
            storeB(a, 0);
         /* The default handlers, and both serial interrupts at level 6 */
         storeL(0x6FE4, NGP_COM_RXISR);
         storeL(0x6FE8, NGP_COM_TXISR);
         storeB(0x77, 0x66);
         rB(0x30) = COM_BUF_OK;
         return 1;
      }
      case 0x11: /* COMSENDSTART */
         com_kick_tx();
         return 1;
      case 0x12: /* COMRECIVESTART: ready to take bytes (RTS on), unless
                    the ring is full */
         sio_mod |= 0x20;
         if (loadB(COM_RXSTOP) != 0xFF)
            COMMStatus &= ~1;
         return 1;
      case 0x13: /* COMCREATEDATA */
         rB(0x30) = com_put(rB(0x35)) ? COM_BUF_OK : 0xFF;
         return 1;
      case 0x14: /* COMGETDATA */
         if (com_get(&data))
         {
            rB(0x35) = data;
            rB(0x30) = COM_BUF_OK;
         }
         else
            rB(0x30) = COM_BUF_EMPTY;
         return 1;
      case 0x15: /* COMONRTS */
         COMMStatus &= ~1;
         return 1;
      case 0x16: /* COMOFFRTS */
         COMMStatus |= 1;
         return 1;
      case 0x17: /* COMSENDSTATUS */
         rW(0x30) = loadB(COM_TXCNT);
         return 1;
      case 0x18: /* COMRECIVESTATUS */
         rW(0x30) = loadB(COM_RXCNT);
         return 1;
      case 0x19: /* COMCREATEBUFDATA */
         while (rB(0x35) > 0)
         {
            if (!com_put(loadB(rL(0x3C))))
               break;
            rL(0x3C)++;
            rB(0x35)--;
         }
         rB(0x30) = rB(0x35) ? COM_BUF_OVER : COM_BUF_OK;
         return 1;
      case 0x1A: /* COMGETBUFDATA */
         while (rB(0x35) > 0)
         {
            if (!com_get(&data))
               break;
            storeB(rL(0x3C), data);
            rL(0x3C)++;
            rB(0x35)--;
         }
         rB(0x30) = rB(0x35) ? COM_BUF_EMPTY : COM_BUF_OK;
         return 1;
      case NGP_BIOS_RXISR_NR:
         com_rx_isr();
         *cycles = 40;
         return 1;
      case NGP_BIOS_TXISR_NR:
         com_tx_isr();
         *cycles = 40;
         return 1;
   }
   return 0;
}

/* BIOS call (C8 1A nr) then RETI (07) */
void ngp_link_install_bios(unsigned char *rom)
{
   const uint32_t rx = NGP_COM_RXISR - 0xFF0000;
   const uint32_t tx = NGP_COM_TXISR - 0xFF0000;
   rom[rx + 0] = 0xC8;
   rom[rx + 1] = 0x1A;
   rom[rx + 2] = NGP_BIOS_RXISR_NR;
   rom[rx + 3] = 0x07;
   rom[tx + 0] = 0xC8;
   rom[tx + 1] = 0x1A;
   rom[tx + 2] = NGP_BIOS_TXISR_NR;
   rom[tx + 3] = 0x07;
}
