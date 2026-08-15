

#include "contiki.h"
#include "sys/node-id.h"
#include "sys/log.h"
#include "net/ipv6/uip-ds6.h"
#include "net/ipv6/uip-ds6-route.h"
#include "net/ipv6/uip-udp-packet.h"
#include "net/mac/tsch/tsch.h"
#include "net/mac/tsch/tsch-packet.h"
#include "net/routing/routing.h"
#include "lib/random.h"
#include "sys/ctimer.h"
#include "sys/energest.h"
#include "project-conf.h"
#include <stdint.h>
#include <string.h>

#define DEBUG DEBUG_PRINT
#include "net/ipv6/uip-debug.h"

#ifndef TSCH_DEFAULT_TIMESLOT_LENGTH
#define TSCH_DEFAULT_TIMESLOT_LENGTH 10000
#endif

#define DATA_MAGIC              0xca
#define PRICE_MAGIC             0xcb
#define DATA_HDR_LEN            7
#define PRICE_MSG_LEN           9

#define SHORT_TERM_WINDOW       3   /* recent-min window for d_stand (congestion signal) */
#define LONG_TERM_WINDOW        50  /* sample count between d_min epoch rotations */
#define SLOT_US                 TSCH_DEFAULT_TIMESLOT_LENGTH
#define SERVICE_TIME_US         (10UL * SLOT_US)
#define CONTROL_INTERVAL        (CLOCK_SECOND * 2)

#define PRICE_VALIDITY          (CLOCK_SECOND * 240)

#define COPA_DELTA_X1000        50000UL
#define COPA_ETA_NUM            3UL
#define COPA_ETA_DEN            10UL


#define MAX_CONTROL_PRICE_US    6000000UL
#define MAX_CONTROL_DQ_US       (MAX_CONTROL_PRICE_US - SERVICE_TIME_US)


#define STALE_THRESHOLD         (CLOCK_SECOND * 120)
#define STALE_RAMP              (CLOCK_SECOND * 120)

/* Baseline (WITH_COPA==0) fixed application send interval. */
#define COPA_BASELINE_INTERVAL  ((uint32_t)COPA_BASELINE_INTERVAL_S * CLOCK_SECOND)

#ifndef COPA_SEND_INTERVAL_MIN_S
#define COPA_SEND_INTERVAL_MIN_S COPA_BASELINE_INTERVAL_S
#endif
#ifndef COPA_SEND_INTERVAL_MAX_S

#define COPA_SEND_INTERVAL_MAX_S (COPA_BASELINE_INTERVAL_S * 4)
#endif

#define SEND_INTERVAL_MIN       ((uint32_t)COPA_SEND_INTERVAL_MIN_S * CLOCK_SECOND)
#define SEND_INTERVAL_MAX       ((uint32_t)COPA_SEND_INTERVAL_MAX_S * CLOCK_SECOND)

#define SEND_INTERVAL_INIT      COPA_BASELINE_INTERVAL


#define ENERGEST_LOG_INTERVAL   (CLOCK_SECOND * 60)

#define PRICE_REBROADCAST_JITTER_MAX  (CLOCK_SECOND / 5)


#define FWD_PRICE_QUEUE_THRESHOLD   (TSCH_QUEUE_NUM_PER_NEIGHBOR / 2)

#define LOCAL_PRICE_INTERVAL        (CLOCK_SECOND * 12)

PROCESS(node_process, "ALICE ASN-Copa node");
AUTOSTART_PROCESSES(&node_process);

struct source_delay {
  uint8_t used;
  uint8_t index;
  uint8_t count;
  uint8_t long_count;  
  uint32_t hist[SHORT_TERM_WINDOW];
  uint32_t d_min_cur;  
  uint32_t d_min_prev; 
  uint32_t d_min_us;    
  uint32_t d_q_us;
  uint16_t parent_id;
  uint16_t last_seq;
  uint16_t delivered;
  uint16_t lost;
  clock_time_t last_sample_time;  
};

struct parent_price {
  uint8_t used;
  uint32_t d_q_max_us;
  uint32_t price_us;
  uint16_t epoch;
};

static struct source_delay sources[MAX_NODE_NUM + 1];
static struct parent_price parents[MAX_NODE_NUM + 1];

static struct uip_udp_conn *server_conn;
static struct uip_udp_conn *client_conn;
static struct uip_udp_conn *price_rx_conn;
static struct uip_udp_conn *price_tx_conn;
static uip_ipaddr_t server_ipaddr;
static uint8_t is_coordinator;

static struct etimer send_timer;
static struct etimer control_timer;
static struct etimer energest_timer;

static uint32_t app_send_time = SEND_INTERVAL_INIT;
static clock_time_t last_price_time;
static uint16_t stored_price_epoch;
static uint16_t stored_price_subtree;
static uint16_t mac_price_epoch;
static uint16_t mac_price_subtree;
static uint16_t next_parent_advertise;
static uint16_t seqno;

static uint16_t sent;
static uint16_t received;

/* Jitter state for deferred price rebroadcast */
static struct ctimer jitter_timer;
static struct {
  uint16_t subtree_id;
  uint32_t price_us;
  uint16_t epoch;
} jitter_pending;

/* Local forwarding-price state */
static struct ctimer local_price_timer;
static uint16_t local_price_epoch;
static uint32_t stored_price_us;   /* last root price received, for max comparison */

/*---------------------------------------------------------------------------*/
static uint16_t
node_id_from_ipaddr(const uip_ipaddr_t *addr)
{
  uint16_t id;

  id = addr->u8[15];
  if(id == 0) {
    id = ((uint16_t)addr->u8[14] << 8) | addr->u8[15];
  }
  return id;
}
/*---------------------------------------------------------------------------*/
static uint16_t
current_parent_id(void)
{
  const uip_ipaddr_t *parent = uip_ds6_defrt_choose();

  return parent == NULL ? 0 : node_id_from_ipaddr(parent);
}
/*---------------------------------------------------------------------------*/
static uint16_t
root_next_hop_id(const uip_ipaddr_t *src)
{
  uip_ds6_route_t *route;
  const uip_ipaddr_t *nexthop;

  route = uip_ds6_route_lookup(src);
  nexthop = route == NULL ? src : uip_ds6_route_nexthop(route);
  return nexthop == NULL ? node_id_from_ipaddr(src) : node_id_from_ipaddr(nexthop);
}
/*---------------------------------------------------------------------------*/
static uint32_t
recent_min_delay(const struct source_delay *s)
{
  uint8_t i;
  uint32_t min = 0xffffffffUL;

  for(i = 0; i < s->count; i++) {
    if(s->hist[i] < min) {
      min = s->hist[i];
    }
  }
  return min == 0xffffffffUL ? 0 : min;
}
/*---------------------------------------------------------------------------*/
static void
recompute_parent_price(uint16_t parent_id)
{
  uint16_t i;
  uint8_t found = 0;
  uint32_t max_q = SLOT_US;

  if(parent_id == 0 || parent_id > MAX_NODE_NUM) {
    return;
  }

  for(i = 1; i <= MAX_NODE_NUM; i++) {
    if(sources[i].used && sources[i].parent_id == parent_id) {
      found = 1;
      if(sources[i].d_q_us > max_q) {
        max_q = sources[i].d_q_us;
      }
    }
  }

  if(!found) {
    memset(&parents[parent_id], 0, sizeof(parents[parent_id]));
    return;
  }

  parents[parent_id].used = 1;
  if(max_q > MAX_CONTROL_DQ_US) {
    max_q = MAX_CONTROL_DQ_US;
  }

  parents[parent_id].d_q_max_us = max_q;
  parents[parent_id].price_us = max_q + SERVICE_TIME_US;
}
/*---------------------------------------------------------------------------*/
static void
update_source_delay(uint16_t source_id, uint16_t parent_id,
                    uint16_t seq, uint32_t delay_us)
{
  struct source_delay *s;
  uint32_t d_stand_us;
  uint16_t old_parent_id;

  if(source_id == 0 || source_id > MAX_NODE_NUM ||
     parent_id == 0 || parent_id > MAX_NODE_NUM) {
    return;
  }

  s = &sources[source_id];
  old_parent_id = s->used ? s->parent_id : 0;
  if(!s->used) {
    s->used = 1;
    s->long_count = 0;
    s->d_min_cur = 0xffffffffUL;
    s->d_min_prev = 0xffffffffUL;
    s->d_min_us = 0xffffffffUL;
    s->last_seq = seq;
  } else if(seq != s->last_seq) {
    uint16_t seq_gap = seq - s->last_seq;

    if(seq_gap > 1 && seq_gap < 32768) {
      s->lost += seq_gap - 1;
    }
    s->last_seq = seq;
  }
  s->delivered++;
  s->last_sample_time = clock_time();

  if(old_parent_id != 0 && old_parent_id != parent_id) {
    s->index = 0;
    s->count = 0;
    s->long_count = 0;
    s->d_min_cur = 0xffffffffUL;
    s->d_min_prev = 0xffffffffUL;
    s->d_min_us = 0xffffffffUL;
    s->d_q_us = SLOT_US;
  }

  s->parent_id = parent_id;
  s->hist[s->index] = delay_us;
  s->index = (s->index + 1) % SHORT_TERM_WINDOW;
  if(s->count < SHORT_TERM_WINDOW) {
    s->count++;
  }

  /* Update current-epoch minimum */
  if(delay_us < s->d_min_cur) {
    s->d_min_cur = delay_us;
  }


  s->long_count++;
  if(s->long_count >= LONG_TERM_WINDOW) {
    s->long_count = 0;
    s->d_min_prev = s->d_min_cur;
    s->d_min_cur = 0xffffffffUL;
  }

  {
    uint32_t lt = (s->d_min_cur < s->d_min_prev) ? s->d_min_cur : s->d_min_prev;
    s->d_min_us = (lt == 0xffffffffUL) ? delay_us : lt;
  }

  d_stand_us = recent_min_delay(s);
  if(d_stand_us > s->d_min_us + SLOT_US) {
    s->d_q_us = d_stand_us - s->d_min_us;
  } else {
    s->d_q_us = SLOT_US;
  }
  if(s->d_q_us > MAX_CONTROL_DQ_US) {
    s->d_q_us = MAX_CONTROL_DQ_US;
  }

  if(old_parent_id != 0 && old_parent_id != parent_id) {
    recompute_parent_price(old_parent_id);
  }
  recompute_parent_price(parent_id);

  printf("COPA root sample src=%u parent=%u seq=%u delay=%lu dmin=%lu dq=%lu price=%lu delivered=%u lost=%u\n",
         source_id, parent_id, seq, (unsigned long)delay_us,
         (unsigned long)s->d_min_us, (unsigned long)s->d_q_us,
         (unsigned long)parents[parent_id].price_us,
         s->delivered, s->lost);
}
/*---------------------------------------------------------------------------*/
static uint32_t
target_interval_from_price(uint32_t price_us)
{
  uint64_t ticks;

  if(price_us == 0) {
    return SEND_INTERVAL_MAX;
  }

  ticks = (uint64_t)price_us * COPA_DELTA_X1000 * CLOCK_SECOND;
  ticks /= 1000000000ULL;

  if(ticks < SEND_INTERVAL_MIN) {
    ticks = SEND_INTERVAL_MIN;
  }
  if(ticks > SEND_INTERVAL_MAX) {
    ticks = SEND_INTERVAL_MAX;
  }
  return (uint32_t)ticks;
}
/*---------------------------------------------------------------------------*/
static void
adapt_leaf_rate(uint32_t price_us, uint16_t epoch)
{
  uint32_t target;

  if(!WITH_COPA) {
    return;
  }

  if(epoch == stored_price_epoch && stored_price_subtree == current_parent_id()) {
    return;
  }

  stored_price_epoch = epoch;
  stored_price_subtree = current_parent_id();
  stored_price_us = price_us;
  last_price_time = clock_time();
  target = target_interval_from_price(price_us);
  app_send_time =
    ((COPA_ETA_DEN - COPA_ETA_NUM) * app_send_time +
     COPA_ETA_NUM * target) / COPA_ETA_DEN;

  printf("COPA leaf price=%lu epoch=%u target_ms=%lu interval_ms=%lu\n",
         (unsigned long)price_us, epoch,
         (unsigned long)(target * 1000UL / CLOCK_SECOND),
         (unsigned long)(app_send_time * 1000UL / CLOCK_SECOND));
}
/*---------------------------------------------------------------------------*/
static void send_price_down(uint16_t subtree_id, uint32_t price_us,
                            uint16_t epoch);
static void schedule_price_down(uint16_t subtree_id, uint32_t price_us,
                                uint16_t epoch);

static int
uplink_queue_depth(void)
{
  struct tsch_neighbor *ts = tsch_queue_get_time_source();
  return (ts != NULL) ? tsch_queue_nbr_packet_count(ts) : 0;
}

static uint32_t
compute_local_fwd_price(void)
{
  int depth = uplink_queue_depth();
  if(depth < FWD_PRICE_QUEUE_THRESHOLD) {
    return 0;
  }
  return ((uint32_t)depth * MAX_CONTROL_PRICE_US) / TSCH_QUEUE_NUM_PER_NEIGHBOR;

static void
local_price_cb(void *ptr)
{
  uint32_t fwd_price;
  uint32_t relay_price;

  (void)ptr;
  ctimer_reset(&local_price_timer);

  if(!WITH_COPA || is_coordinator || current_parent_id() == 0) {
    return;
  }

  fwd_price = compute_local_fwd_price();


  if(fwd_price > stored_price_us) {
    relay_price = fwd_price;
    local_price_epoch++;
    schedule_price_down(node_id, relay_price, local_price_epoch);
    printf("COPA fwd queue=%d local_price=%lu epoch=%u\n",
           uplink_queue_depth(), (unsigned long)relay_price,
           local_price_epoch);
  }
}
/*---------------------------------------------------------------------------*/
static void
jitter_send_cb(void *ptr)
{
  (void)ptr;
  send_price_down(jitter_pending.subtree_id,
                  jitter_pending.price_us,
                  jitter_pending.epoch);
}
static void
schedule_price_down(uint16_t subtree_id, uint32_t price_us, uint16_t epoch)
{
  clock_time_t delay = random_rand() % (PRICE_REBROADCAST_JITTER_MAX + 1);
  jitter_pending.subtree_id = subtree_id;
  jitter_pending.price_us   = price_us;
  jitter_pending.epoch      = epoch;
  ctimer_set(&jitter_timer, delay, jitter_send_cb, NULL);
}
/*---------------------------------------------------------------------------*/
static void
send_price_down(uint16_t subtree_id, uint32_t price_us, uint16_t epoch)
{
  uint8_t buf[PRICE_MSG_LEN];
  uip_ipaddr_t mcast;

  if(price_tx_conn == NULL) {
    return;
  }

  buf[0] = PRICE_MAGIC;
  buf[1] = subtree_id >> 8;
  buf[2] = subtree_id;
  buf[3] = price_us >> 24;
  buf[4] = price_us >> 16;
  buf[5] = price_us >> 8;
  buf[6] = price_us;
  buf[7] = epoch >> 8;
  buf[8] = epoch;

  uip_create_linklocal_allnodes_mcast(&mcast);
  uip_udp_packet_sendto(price_tx_conn, buf, sizeof(buf),
                        &mcast, UIP_HTONS(UDP_CONGESTION_PORT));
  printf("COPA parent forward subtree=%u price=%lu epoch=%u\n",
         subtree_id, (unsigned long)price_us, epoch);
}
/*---------------------------------------------------------------------------*/
static void
handle_mac_price(void)
{
  uint16_t subtree_id;
  uint32_t price_us;
  uint16_t epoch;
  uint16_t parent_id;
  uint8_t fresh;

  if(!WITH_COPA || is_coordinator ||
     !tsch_packet_get_copa_price(&subtree_id, &price_us, &epoch) ||
     (epoch == mac_price_epoch && subtree_id == mac_price_subtree)) {
    return;
  }

  mac_price_epoch = epoch;
  mac_price_subtree = subtree_id;
  if(node_id == subtree_id) {
    uint32_t fwd_p = compute_local_fwd_price();
    uint32_t relay_p = (fwd_p > price_us) ? fwd_p : price_us;
    fresh = !(epoch == stored_price_epoch && stored_price_subtree == current_parent_id());
    adapt_leaf_rate(price_us, epoch);
    if(fresh) {
      schedule_price_down(subtree_id, relay_p, epoch);
    }
    return;
  }

  parent_id = current_parent_id();
  if(parent_id == subtree_id) {
    uint32_t fwd_p = compute_local_fwd_price();
    uint32_t relay_p = (fwd_p > price_us) ? fwd_p : price_us;
    fresh = !(epoch == stored_price_epoch && stored_price_subtree == parent_id);
    adapt_leaf_rate(price_us, epoch);
    if(fresh) {
      schedule_price_down(subtree_id, relay_p, epoch);
    }
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
static void
send_packet(void *ptr)
{
  uint8_t buf[DATA_HDR_LEN];
  uint16_t asn_low;
  uint16_t id;

  (void)ptr;

  if(!NETSTACK_ROUTING.get_root_ipaddr(&server_ipaddr)) {
    return;
  }

  asn_low = (uint16_t)(tsch_current_asn.ls4b & 0xffff);
  id = node_id;
  seqno++;

  buf[0] = DATA_MAGIC;
  buf[1] = id >> 8;
  buf[2] = id;
  buf[3] = asn_low >> 8;
  buf[4] = asn_low;
  buf[5] = seqno >> 8;
  buf[6] = seqno;

  uip_udp_packet_sendto(client_conn, buf, sizeof(buf),
                        &server_ipaddr, UIP_HTONS(UDP_SERVER_PORT));
  sent++;
  printf("COPA send seq=%u interval_ms=%lu\n", seqno,
         (unsigned long)(app_send_time * 1000UL / CLOCK_SECOND));
}
/*---------------------------------------------------------------------------*/
static void
tcpip_handler(void)
{
  uint8_t *data;

  if(!uip_newdata()) {
    return;
  }
  received++;
  data = (uint8_t *)uip_appdata;

  if(is_coordinator && UIP_UDP_BUF->destport == UIP_HTONS(UDP_SERVER_PORT) &&
     uip_datalen() >= DATA_HDR_LEN && data[0] == DATA_MAGIC) {
    uint16_t source_id;
    uint16_t tx_asn;
    uint16_t seq;
    uint16_t rx_asn;
    uint16_t diff16;
    uint32_t delay_us;
    uint16_t parent_id;

    source_id = ((uint16_t)data[1] << 8) | data[2];
    tx_asn = ((uint16_t)data[3] << 8) | data[4];
    seq = ((uint16_t)data[5] << 8) | data[6];
    rx_asn = (uint16_t)(tsch_current_asn.ls4b & 0xffff);
    diff16 = rx_asn - tx_asn;
    delay_us = (uint32_t)diff16 * SLOT_US;
    parent_id = root_next_hop_id(&UIP_IP_BUF->srcipaddr);

    if(diff16 > 0 && diff16 < 32768) {
      update_source_delay(source_id, parent_id, seq, delay_us);
    } else {
      printf("COPA root drop implausible ASN diff src=%u diff=%u\n",
             source_id, diff16);
    }
    return;
  }

  if(WITH_COPA && !is_coordinator &&
     UIP_UDP_BUF->destport == UIP_HTONS(UDP_CONGESTION_PORT) &&
     uip_datalen() == PRICE_MSG_LEN && data[0] == PRICE_MAGIC) {
    uint16_t subtree_id;
    uint32_t price_us;
    uint16_t epoch;
    uint16_t parent_id;
    uint8_t fresh;

    subtree_id = ((uint16_t)data[1] << 8) | data[2];
    price_us = ((uint32_t)data[3] << 24) |
               ((uint32_t)data[4] << 16) |
               ((uint32_t)data[5] << 8) |
               data[6];
    epoch = ((uint16_t)data[7] << 8) | data[8];

    parent_id = current_parent_id();
    if(parent_id == subtree_id) {
      uint32_t fwd_p = compute_local_fwd_price();
      uint32_t relay_p = (fwd_p > price_us) ? fwd_p : price_us;
      fresh = !(epoch == stored_price_epoch && stored_price_subtree == parent_id);
      adapt_leaf_rate(price_us, epoch);
      if(fresh) {
        schedule_price_down(subtree_id, relay_p, epoch);
      }
    }
  }

static clock_time_t
parent_last_sample(uint16_t parent_id)
{
  uint16_t i;
  clock_time_t latest = 0;

  for(i = 1; i <= MAX_NODE_NUM; i++) {
    if(sources[i].used && sources[i].parent_id == parent_id
       && sources[i].last_sample_time > latest) {
      latest = sources[i].last_sample_time;
    }
  }
  return latest;
}
/*---------------------------------------------------------------------------*/
static void
advertise_next_parent_price(void)
{
  uint16_t scanned;


  if(!WITH_COPA) {
    return;
  }

  for(scanned = 0; scanned <= MAX_NODE_NUM; scanned++) {
    next_parent_advertise++;
    if(next_parent_advertise > MAX_NODE_NUM) {
      next_parent_advertise = 1;
    }
    if(parents[next_parent_advertise].used) {
      uint16_t p = next_parent_advertise;
      /* Base price = last measured value (d_q + service time). */
      uint32_t base = parents[p].d_q_max_us + SERVICE_TIME_US;
      uint32_t adv = base;
      clock_time_t recent = parent_last_sample(p);
      uint8_t stale = 0;


      if(recent != 0) {
        clock_time_t silence = clock_time() - recent;
        if(silence > STALE_THRESHOLD) {
          uint32_t over = (uint32_t)(silence - STALE_THRESHOLD);
          stale = 1;
          if(over >= (uint32_t)STALE_RAMP || base >= MAX_CONTROL_PRICE_US) {
            adv = MAX_CONTROL_PRICE_US;
          } else {
            adv = base + (uint32_t)(((uint64_t)(MAX_CONTROL_PRICE_US - base)
                                     * over) / (uint32_t)STALE_RAMP);
          }
        }
      }

      parents[p].price_us = adv;
      parents[p].epoch++;
      tsch_packet_set_copa_price(p, adv, parents[p].epoch);
      printf("COPA root EB subtree=%u price=%lu epoch=%u stale=%u\n",
             p, (unsigned long)adv, parents[p].epoch, stale);
      return;
    }
  }
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(node_process, ev, data)
{
  PROCESS_BEGIN();

  is_coordinator = 0;
#if CONTIKI_TARGET_COOJA || CONTIKI_TARGET_Z1
  is_coordinator = (node_id == 1);
#endif

  if(is_coordinator) {
    uip_ipaddr_t ipaddr;

    uip_ip6addr(&ipaddr, UIP_DS6_DEFAULT_PREFIX, 0, 0, 0, 0, 0, 0, 1);
    uip_ds6_addr_add(&ipaddr, 0, ADDR_AUTOCONF);
    printf("SERVER ADDR: "); PRINT6ADDR(&ipaddr); printf("\n");

    server_conn = udp_new(NULL, UIP_HTONS(UDP_CLIENT_PORT), NULL);
    if(server_conn == NULL) {
      PROCESS_EXIT();
    }
    udp_bind(server_conn, UIP_HTONS(UDP_SERVER_PORT));
  } else {
    set_global_address();

    client_conn = udp_new(NULL, UIP_HTONS(UDP_SERVER_PORT), NULL);
    if(client_conn == NULL) {
      PROCESS_EXIT();
    }
    udp_bind(client_conn, UIP_HTONS(UDP_CLIENT_PORT));

    if(WITH_COPA) {
      price_rx_conn = udp_new(NULL, 0, NULL);
      if(price_rx_conn != NULL) {
        udp_bind(price_rx_conn, UIP_HTONS(UDP_CONGESTION_PORT));
      }

      price_tx_conn = udp_new(NULL, UIP_HTONS(UDP_CONGESTION_PORT), NULL);
      if(price_tx_conn != NULL) {
        udp_bind(price_tx_conn, UIP_HTONS(UDP_CONGESTION_PORT + 1));
      }
    } else {
      /* Baseline ALICE: fixed application send interval, no adaptation. */
      app_send_time = COPA_BASELINE_INTERVAL;
    }
  }

  if(is_coordinator) {
    NETSTACK_ROUTING.root_start();
  }
  tsch_set_coordinator(is_coordinator);
  NETSTACK_MAC.on();

  if(is_coordinator) {
    if(WITH_COPA) {
      etimer_set(&control_timer, CONTROL_INTERVAL);
    }
  } else {
    etimer_set(&send_timer, random_rand() % SEND_INTERVAL_INIT);
    last_price_time = clock_time();
    if(WITH_COPA) {
      ctimer_set(&local_price_timer, LOCAL_PRICE_INTERVAL, local_price_cb, NULL);
    }
  }

  /* Energest accounting runs in both builds for the comparison harness. */
  etimer_set(&energest_timer, ENERGEST_LOG_INTERVAL);

  while(1) {
    PROCESS_YIELD();

    if(ev == tcpip_event) {
      tcpip_handler();
    }

    handle_mac_price();

    if(WITH_COPA && is_coordinator && etimer_expired(&control_timer)) {
      advertise_next_parent_price();
      etimer_reset(&control_timer);
    }

    if(!is_coordinator && etimer_expired(&send_timer)) {

      if(WITH_COPA && clock_time() - last_price_time > PRICE_VALIDITY) {
        app_send_time = SEND_INTERVAL_MAX;
      }
      send_packet(NULL);
      etimer_set(&send_timer, app_send_time);
    }

    if(etimer_expired(&energest_timer)) {
      energest_flush();
      printf("ENERGEST id=%u cpu=%lu lpm=%lu tx=%lu rx=%lu total=%lu\n",
             node_id,
             (unsigned long)energest_type_time(ENERGEST_TYPE_CPU),
             (unsigned long)energest_type_time(ENERGEST_TYPE_LPM),
             (unsigned long)energest_type_time(ENERGEST_TYPE_TRANSMIT),
             (unsigned long)energest_type_time(ENERGEST_TYPE_LISTEN),
             (unsigned long)ENERGEST_GET_TOTAL_TIME());
      etimer_reset(&energest_timer);
    }

#if WITH_PERIODIC_ROUTES_PRINT
    if(ev == PROCESS_EVENT_TIMER) {
      printf("srsr %u/%u interval_ms=%lu parent=%u\n",
             sent, received,
             (unsigned long)(app_send_time * 1000UL / CLOCK_SECOND),
             current_parent_id());
    }
#endif
  }

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
