#ifndef PROJECT_CONF_H_
#define PROJECT_CONF_H_

/* ============================ A3 + Copa ============================
 * Copa source-rate control (node.c, copied from simple-node-ksh) running on
 * top of the A3 adaptive-slot ALICE scheduler (unicast_per_neighbor_a3).
 *   A3   = scheduler/MAC: adapts the NUMBER of slots per link to load.
 *   Copa = application:  adapts the SEND RATE to one-way delay (price).
 * ================================================================== */

/* --- A3 --- */
#define A3_TSCH_HOOKS 1
#define A3_NUM_ZONES  4

/* --- Copa on by default (node.c reads WITH_COPA) --- */
#ifndef WITH_COPA
#define WITH_COPA 1
#endif
#ifndef COPA_BASELINE_INTERVAL_S
#define COPA_BASELINE_INTERVAL_S 30
#endif

#define SICSLOWPAN_CONF_FRAG 0
#define UIP_CONF_BUFFER_SIZE 160

/* Single channel so it joins under Cooja ContikiMote; A3's multi-channel
 * benefit is validated on the IoT-LAB nRF52840 path. */
#undef TSCH_CONF_DEFAULT_HOPPING_SEQUENCE
#define TSCH_CONF_DEFAULT_HOPPING_SEQUENCE TSCH_HOPPING_SEQUENCE_1_1

#define UIP_CONF_IPV6 1
#define NETSTACK_CONF_WITH_IPV6 1
#define ENERGEST_CONF_ON 1

#define MAX_NODE_NUM 35
#undef UIP_CONF_MAX_ROUTES
#define UIP_CONF_MAX_ROUTES MAX_NODE_NUM
#undef NBR_TABLE_CONF_MAX_NEIGHBORS
#define NBR_TABLE_CONF_MAX_NEIGHBORS MAX_NODE_NUM
#define TSCH_SCHEDULE_CONF_MAX_LINKS 128

#undef NETSTACK_CONF_ROUTING
#define NETSTACK_CONF_ROUTING       rpl_classic_driver
#define RPL_CONF_WITH_NON_STORING    0
#define RPL_CONF_MOP                RPL_MOP_STORING_NO_MULTICAST
#define RPL_CONF_WITH_DAO_ACK       1
#undef ROUTING_CONF_RPL_LITE
#define ROUTING_CONF_RPL_LITE        0
#define ROUTING_CONF_RPL_CLASSIC     1
#define RPL_CONF_DIO_INTERVAL_MIN        10
#define RPL_CONF_DIO_INTERVAL_DOUBLINGS  6
#define RPL_CONF_DAO_MAX_RETRANSMISSIONS 10
#define RPL_CONF_DAO_RETRANSMISSION_TIMEOUT (3 * CLOCK_SECOND)

#define IEEE802154_CONF_PANID 0x81a5
#define TSCH_CONF_AUTOSTART 0

#define SEND_INTERVAL  ((30) * (CLOCK_SECOND))
#define SEND_TIME      (random_rand() % (SEND_INTERVAL))
#define UDP_CLIENT_PORT     8765
#define UDP_SERVER_PORT     5678
#define UDP_CONGESTION_PORT 6789   /* Copa price multicast */

/* ALICE-A3 scheduler: EB + common from the alice module, A3 adaptive unicast. */
#define ORCHESTRA_CONF_COMMON_SHARED_PERIOD 31
#define ORCHESTRA_CONF_UNICAST_PERIOD       40   /* divisible by A3_NUM_ZONES=4 */
#define ORCHESTRA_CONF_EBSF_PERIOD          53
#define ORCHESTRA_CONF_UNICAST_SENDER_BASED 1
#define ORCHESTRA_ONE_CHANNEL_OFFSET        0

struct orchestra_rule;
extern struct orchestra_rule eb_per_time_source;
extern struct orchestra_rule default_common;
extern struct orchestra_rule unicast_per_neighbor_a3;
#define ORCHESTRA_CONF_RULES { &eb_per_time_source, &default_common, &unicast_per_neighbor_a3 }
/* make the (unused) ALICE unicast rule compile under -Werror; never invoked. */
#define ALICE_TSCH_CALLBACK_SLOTFRAME_START alice_callback_slotframe_start

/* Copa piggybacks its price on EBs -> keep EBs frequent. */
#undef TSCH_CONF_EB_PERIOD
#define TSCH_CONF_EB_PERIOD (2 * CLOCK_SECOND)
#undef TSCH_CONF_MAX_EB_PERIOD
#define TSCH_CONF_MAX_EB_PERIOD (2 * CLOCK_SECOND)
#define TSCH_PACKET_CONF_EB_WITH_SLOTFRAME_AND_LINK 1

#define LOG_CONF_LEVEL_RPL    LOG_LEVEL_WARN
#define LOG_CONF_LEVEL_TCPIP  0
#define LOG_CONF_LEVEL_IPV6   0
#define LOG_CONF_LEVEL_6LOWPAN 0
#define LOG_CONF_LEVEL_MAC    0
#define LOG_CONF_LEVEL_FRAMER 0
#define TSCH_LOG_CONF_PER_SLOT 0
#define LOG_CONF_LEVEL_TSCH   LOG_LEVEL_WARN

#endif /* PROJECT_CONF_H_ */
