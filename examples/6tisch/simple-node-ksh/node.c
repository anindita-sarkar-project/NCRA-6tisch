/*
 * Copyright (c) 2015, SICS Swedish ICT.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Institute nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

#include "contiki.h"
#include "sys/node-id.h"
#include "sys/log.h"
#include "net/ipv6/uip-ds6-route.h"
#include "net/ipv6/uip-sr.h"
#include "net/mac/tsch/tsch.h"
#include "net/routing/routing.h"
#include "lib/random.h"
#include "sys/ctimer.h"
#include "net/ipv6/uip-ds6.h"
#include "net/ipv6/uip-udp-packet.h"
#include "project-conf.h"
#include <stdint.h>
#include <string.h>

#define DEBUG DEBUG_PRINT
#include "net/ipv6/uip-debug.h"

/*---------------------------------------------------------------------------*/
/* TSCH timeslot length in microseconds (default 10 ms = 10000 us)          */
#ifndef TSCH_DEFAULT_TIMESLOT_LENGTH
#define TSCH_DEFAULT_TIMESLOT_LENGTH 10000
#endif

/*---------------------------------------------------------------------------*/
/* Phase 1: one-way delay estimation (root only)                             */
/*---------------------------------------------------------------------------*/
#define SHORT_TERM_WINDOW   10      /* samples in the recent window          */

/* BUG 2 FIX: initialise all slots to INT32_MAX so they are ignored until
   real data arrives.  Done in node_process before the event loop. */
static int32_t  delay_history[SHORT_TERM_WINDOW];
static uint8_t  history_index  = 0;
/* BUG 3 FIX: track maximum over the window (standing queue), not minimum */
static int32_t  short_term_max = 0;            /* max over recent window    */
static int32_t  long_term_min  = 0x7FFFFFFF;  /* absolute minimum (= propagation delay baseline) */
static uint16_t total_samples  = 0;
static uint8_t  history_full   = 0;           /* set once window is filled  */

/*---------------------------------------------------------------------------*/
/* Phase 2: congestion-price dissemination                                   */
/*---------------------------------------------------------------------------*/
#define UDP_CONGESTION_PORT  6789
static struct etimer  cong_etimer;
#define CONG_INTERVAL        (CLOCK_SECOND * 2)   /* broadcast every 2 s    */

/* BUG 5 FIX: persistent connections allocated once at startup              */
static struct uip_udp_conn *mcast_tx_conn;    /* root → multicast TX        */
static struct uip_udp_conn *cong_rx_conn;     /* leaves: listen for price   */

/*---------------------------------------------------------------------------*/
/* Phase 3: rate adaptation (leaves only)                                    */
/*---------------------------------------------------------------------------*/
/* Copa-style parameters (integer arithmetic, no floats — BUG 7 FIX)        */

/* COPA_DELTA: larger = lower delay target, smaller = higher throughput.
   5 means the target rate is 1/(5 * dq) packets/s.
   Stored as fixed-point * 1000 to avoid float: COPA_DELTA_x1000 = 5000. */
#define COPA_DELTA_x1000     5000

/* Minimum and maximum send intervals in clock ticks */
#define SEND_INTERVAL_MIN    (CLOCK_SECOND / 5)    /* 200 ms                */
#define SEND_INTERVAL_MAX    (CLOCK_SECOND * 10)   /* 10 s                  */
#define SEND_INTERVAL_BASE   (CLOCK_SECOND * 2)    /* initial: 2 s          */

/* EWMA: new = (7*old + 1*new_target) / 8  (alpha ≈ 0.125, BUG 7 FIX) */
#define EWMA_SHIFT           3    /* right-shift by 3 = divide by 8         */

static uint32_t app_send_time  = SEND_INTERVAL_BASE;
static int32_t  congestion_price = 0;   /* latest queueing delay in µs      */
static struct etimer send_etimer;

/*---------------------------------------------------------------------------*/
PROCESS(node_process, "RPL Node");
AUTOSTART_PROCESSES(&node_process);

static struct uip_udp_conn *server_conn;
static struct uip_udp_conn *client_conn;
static uip_ipaddr_t server_ipaddr;


static int is_coordinator;

int sent     = 0;
int received = 0;

/*---------------------------------------------------------------------------*/
static void
send_packet(void *ptr)
{
  sent++;

  if(!NETSTACK_ROUTING.get_root_ipaddr(&server_ipaddr)) {
    /* RPL root not known yet; skip silently */
    return;
  }

  printf("client-server addr : "); PRINT6ADDR(&server_ipaddr); printf("\n");

  /*
   * ASN stamp: embed the lower 16 bits of the current ASN into the
   * payload so the root can compute one-way transit time.
   * We use ls4b (the low 32 bits of the 5-byte ASN struct).
   */
  uint16_t asn_l = (uint16_t)(tsch_current_asn.ls4b & 0xFFFF);
  uint8_t  buf[2];
  buf[0] = (asn_l >> 8) & 0xFF;
  buf[1] =  asn_l       & 0xFF;
  uip_udp_packet_sendto(client_conn, buf, sizeof(buf),
                        &server_ipaddr, UIP_HTONS(UDP_SERVER_PORT));
}

/*---------------------------------------------------------------------------*/
static void
tcpip_handler(void)
{
  received++;

  if(!uip_newdata()) {
    return;
  }

  /* ------------------------------------------------------------------ */
  /* ROOT: receives 2-byte ASN-stamped data packet from a leaf           */
  /* ------------------------------------------------------------------ */
  if(is_coordinator && uip_datalen() == 2) {

    uint16_t tx_asn_l = (uint16_t)(((uint8_t *)uip_appdata)[0] << 8 |
                                    ((uint8_t *)uip_appdata)[1]);
    uint16_t rx_asn_l = (uint16_t)(tsch_current_asn.ls4b & 0xFFFF);

    /*
     * BUG 1 FIX: one-way delay = arrival_ASN - departure_ASN.
     * Cast to int16_t first for correct modular wraparound over the
     * 16-bit window; then widen to int32_t.
     * Result is positive when the packet took time to travel (normal case).
     */
    int16_t  diff16   = (int16_t)(rx_asn_l - tx_asn_l);
    int32_t  slot_diff = (int32_t)diff16;
    int32_t  delay_us  = slot_diff * (int32_t)TSCH_DEFAULT_TIMESLOT_LENGTH;

    printf("OWD: %ld us (slot_diff: %ld)\n",
           (long)delay_us, (long)slot_diff);

    /* Only use plausible samples (reject negative or implausibly large) */
    if(delay_us <= 0 || delay_us > 2000000L) {
      printf("OWD: sample out of range, discarded\n");
      /* Still send the ACK so the leaf knows the root is alive */
      goto send_ack;
    }

    /* --- Phase 1: update delay history -------------------------------- */
    delay_history[history_index] = delay_us;
    history_index = (history_index + 1) % SHORT_TERM_WINDOW;
    if(history_index == 0) {
      history_full = 1;
    }
    total_samples++;

    /* Update long-term minimum (propagation delay baseline) */
    if(delay_us < long_term_min) {
      long_term_min = delay_us;
    }

    /*
     * BUG 3 FIX: short_term_max = MAXIMUM over the valid window.
     * Copa's RTTstanding is the minimum in a short window, but there
     * the signal is RTT (two-way).  For one-way delay in a convergecast,
     * the *maximum* in the recent window captures the standing (worst-case
     * persistent) queue, which is what we want to control against.
     */
    {
      uint8_t  valid_slots = history_full ? SHORT_TERM_WINDOW : history_index;
      int32_t  max_val     = 0;
      uint8_t  i;
      for(i = 0; i < valid_slots; i++) {
        if(delay_history[i] > max_val) {
          max_val = delay_history[i];
        }
      }
      short_term_max = max_val;
    }

    int32_t queueing_delay = short_term_max - long_term_min;
    if(queueing_delay < 0) queueing_delay = 0;

    printf("dq: %ld us  (win_max: %ld  baseline: %ld  n=%u)\n",
           (long)queueing_delay, (long)short_term_max,
           (long)long_term_min,  total_samples);

    /* Phase 2: update price for downlink dissemination */
    congestion_price = queueing_delay;

send_ack:
    /* Send a 1-byte ACK so the leaf knows the packet arrived */
    {
      uint8_t ack = 0xAC;
      uip_ipaddr_copy(&server_conn->ripaddr, &UIP_IP_BUF->srcipaddr);
      uip_udp_packet_send(server_conn, &ack, 1);
      uip_create_unspecified(&server_conn->ripaddr);
    }
    sent++;
    return;
  }

  /* ------------------------------------------------------------------ */
  /* LEAF: receives 4-byte congestion price from root multicast          */
  /* ------------------------------------------------------------------ */
  /*
   * BUG 4 FIX: match only on destport and payload length.
   * The root's ephemeral TX socket may have any srcport, so the original
   * srcport == UDP_CONGESTION_PORT check never matched.
   */
  if(!is_coordinator &&
     UIP_UDP_BUF->destport == UIP_HTONS(UDP_CONGESTION_PORT) &&
     uip_datalen() == 4) {

    int32_t price =
        (int32_t)(((uint8_t *)uip_appdata)[0] << 24 |
                  ((uint8_t *)uip_appdata)[1] << 16 |
                  ((uint8_t *)uip_appdata)[2] << 8  |
                  ((uint8_t *)uip_appdata)[3]);
    if(price < 0) price = 0;
    congestion_price = price;
    printf("Rx price: %ld us\n", (long)congestion_price);

    /* --- Phase 3: Copa-style rate adaptation -------------------------- */
    /*
     * BUG 6 FIX: target interval = CLOCK_SECOND / (dq_s / delta)
     *          = CLOCK_SECOND * delta / dq_s
     *          = CLOCK_SECOND * COPA_DELTA_x1000 / (dq_us / 1000)
     *          = CLOCK_SECOND * COPA_DELTA_x1000 * 1000 / dq_us
     *
     * All arithmetic in integer clock ticks; no float used.
     *
     * When dq ≈ 0 (no congestion) we fall back to SEND_INTERVAL_MAX.
     */
    uint32_t target_ticks;
    if(congestion_price <= 0) {
      target_ticks = SEND_INTERVAL_MAX;
    } else {
      /* target_ticks = CLOCK_SECOND * COPA_DELTA_x1000 * 1000 / dq_us
         Guard against overflow: dq_us > 0, typical range 10 ms – 2 s   */
      uint32_t numerator = (uint32_t)CLOCK_SECOND *
                           (uint32_t)COPA_DELTA_x1000;   /* e.g. 128*5000 */
      /* numerator is at most 128*5000 = 640000; safe in uint32_t */
      /* multiply by 1000 before dividing to preserve precision   */
      if(numerator > 0xFFFFFFFFUL / 1000UL) {
        /* overflow guard: cap numerator */
        target_ticks = SEND_INTERVAL_MAX;
      } else {
        numerator *= 1000UL;
        target_ticks = numerator / (uint32_t)congestion_price;
      }
    }

    /* Clamp to [MIN, MAX] */
    if(target_ticks < SEND_INTERVAL_MIN) target_ticks = SEND_INTERVAL_MIN;
    if(target_ticks > SEND_INTERVAL_MAX) target_ticks = SEND_INTERVAL_MAX;

    /*
     * BUG 7 FIX: integer EWMA with alpha = 1/8.
     *   new = old - (old >> EWMA_SHIFT) + (target >> EWMA_SHIFT)
     *       = (7/8)*old + (1/8)*target
     */
    app_send_time = app_send_time
                    - (app_send_time >> EWMA_SHIFT)
                    + (target_ticks  >> EWMA_SHIFT);

    printf("Rate adapt: interval=%lu ms  (target=%lu ms  dq=%ld us)\n",
           (unsigned long)(app_send_time * 1000UL / CLOCK_SECOND),
           (unsigned long)(target_ticks  * 1000UL / CLOCK_SECOND),
           (long)congestion_price);
  }
}

/*---------------------------------------------------------------------------*/
static void
set_global_address(void)
{
  uip_ipaddr_t ipaddr;
  uip_ip6addr(&ipaddr, UIP_DS6_DEFAULT_PREFIX, 0, 0, 0, 0, 0, 0, 0);
  uip_ds6_set_addr_iid(&ipaddr, &uip_lladdr);
  uip_ds6_addr_add(&ipaddr, 0, ADDR_AUTOCONF);
  printf("my addr : "); PRINT6ADDR(&ipaddr); printf("\n");
}

/*---------------------------------------------------------------------------*/
PROCESS_THREAD(node_process, ev, data)
{
  PROCESS_BEGIN();

  /* BUG 2 FIX: fill history with INT32_MAX so initial window scans are
     harmless before real samples arrive */
  {
    uint8_t i;
    for(i = 0; i < SHORT_TERM_WINDOW; i++) {
      delay_history[i] = 0x7FFFFFFF;
    }
  }

  is_coordinator = 0;

#if CONTIKI_TARGET_COOJA || CONTIKI_TARGET_Z1
  is_coordinator = (node_id == 1);
#endif

  /* ------------------------------------------------------------------ */
  /* Network setup                                                        */
  /* ------------------------------------------------------------------ */
  if(!is_coordinator) {

    set_global_address();

    client_conn = udp_new(NULL, UIP_HTONS(UDP_SERVER_PORT), NULL);
    if(client_conn == NULL) {
      PRINTF("No UDP client conn available!\n");
      PROCESS_EXIT();
    }
    udp_bind(client_conn, UIP_HTONS(UDP_CLIENT_PORT));

    PRINTF("Created client conn :: local/remote port %u/%u\n",
           UIP_HTONS(client_conn->lport), UIP_HTONS(client_conn->rport));

    /*
     * BUG 5 FIX: persistent congestion-price listener on leaves.
     * Bind to UDP_CONGESTION_PORT so tcpip_event fires when the root
     * multicasts the price.
     */
    cong_rx_conn = udp_new(NULL, 0, NULL);
    if(cong_rx_conn != NULL) {
      udp_bind(cong_rx_conn, UIP_HTONS(UDP_CONGESTION_PORT));
      PRINTF("Congestion RX listener bound on port %u\n", UDP_CONGESTION_PORT);
    } else {
      PRINTF("WARNING: no conn for congestion RX (pool exhausted)\n");
    }

  } else {

    /* Root: fixed address ::1 */
    uip_ipaddr_t ipaddr;
    uip_ip6addr(&ipaddr, UIP_DS6_DEFAULT_PREFIX, 0, 0, 0, 0, 0, 0, 1);
    uip_ds6_addr_add(&ipaddr, 0, ADDR_AUTOCONF);

    PRINTF("SERVER ADDR: "); PRINT6ADDR(&ipaddr); PRINTF("\n");

    server_conn = udp_new(NULL, UIP_HTONS(UDP_CLIENT_PORT), NULL);
    if(server_conn == NULL) {
      PRINTF("No UDP server conn available!\n");
      PROCESS_EXIT();
    }
    udp_bind(server_conn, UIP_HTONS(UDP_SERVER_PORT));

    PRINTF("Created server conn :: local/remote port %u/%u\n",
           UIP_HTONS(server_conn->lport), UIP_HTONS(server_conn->rport));

    /*
     * BUG 5 FIX: allocate the multicast TX connection once at startup
     * rather than creating+destroying it on every broadcast timer expiry.
     */
    {
      uip_ipaddr_t mcast_addr;
      uip_ip6addr(&mcast_addr, 0xff02, 0, 0, 0, 0, 0, 0, 1);
      mcast_tx_conn = udp_new(&mcast_addr, UIP_HTONS(UDP_CONGESTION_PORT), NULL);
      if(mcast_tx_conn != NULL) {
        udp_bind(mcast_tx_conn, UIP_HTONS(UDP_CONGESTION_PORT));
        PRINTF("Multicast TX conn ready on port %u\n", UDP_CONGESTION_PORT);
      } else {
        PRINTF("WARNING: no conn for multicast TX (pool exhausted)\n");
      }
    }
  }

  /* ------------------------------------------------------------------ */
  /* RPL / TSCH startup                                                   */
  /* ------------------------------------------------------------------ */
  if(is_coordinator) {
    NETSTACK_ROUTING.root_start();
  }
  tsch_set_coordinator(is_coordinator);
  NETSTACK_MAC.on();

  /* ------------------------------------------------------------------ */
  /* Timer initialisation                                                  */
  /* ------------------------------------------------------------------ */
  if(is_coordinator) {
    etimer_set(&cong_etimer, CONTIKI_NG_INTERVAL);
  } else {
    etimer_set(&send_etimer, app_send_time);
  }

#if WITH_PERIODIC_ROUTES_PRINT
  {
    static struct etimer et;
    etimer_set(&et, SEND_INTERVAL);

    while(1) {
      PROCESS_YIELD();

      /* ---- incoming packet ---- */
      if(ev == tcpip_event) {
        tcpip_handler();
      }

      /* ---- root: broadcast congestion price every CONG_INTERVAL ---- */
      if(is_coordinator && etimer_expired(&cong_etimer)) {
        if(mcast_tx_conn != NULL) {
          uint8_t buffer[4];
          buffer[0] = (uint8_t)((congestion_price >> 24) & 0xFF);
          buffer[1] = (uint8_t)((congestion_price >> 16) & 0xFF);
          buffer[2] = (uint8_t)((congestion_price >>  8) & 0xFF);
          buffer[3] = (uint8_t)( congestion_price        & 0xFF);
          uip_udp_packet_send(mcast_tx_conn, buffer, sizeof(buffer));
          printf("Broadcast price: %ld us\n", (long)congestion_price);
        }
        etimer_reset(&cong_etimer);
      }

      /* ---- leaf: adaptive packet transmission ---- */
      if(!is_coordinator && etimer_expired(&send_etimer)) {
        send_packet(NULL);
        etimer_set(&send_etimer, app_send_time);
      }

      /* ---- periodic stats print ---- */
      if(etimer_expired(&et)) {
#if (UIP_MAX_ROUTES != 0)
        PRINTF("Routing entries: %u\n", uip_ds6_route_num_routes());
#endif
#if (UIP_SR_LINK_NUM != 0)
        PRINTF("Routing links: %u\n", uip_sr_num_nodes());
#endif
        PRINTF("srsr %d/%d\n", sent, received);
        etimer_restart(&et);
      }
    }
  }
#endif /* WITH_PERIODIC_ROUTES_PRINT */

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
