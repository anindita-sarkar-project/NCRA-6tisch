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

/**
 * \author Simon Duquennoy <simonduq@sics.se>
 */

#ifndef PROJECT_CONF_H_
#define PROJECT_CONF_H_

/*******************************************************/
/*********** ALICE vs ALICE+Copa A/B toggle ************/
/*******************************************************/
/* WITH_COPA=1  : full ALICE+Copa congestion control (adaptive send rate).
 * WITH_COPA=0  : baseline ALICE, fixed send interval = COPA_BASELINE_INTERVAL_S
 *                seconds, congestion-control loop disabled.  Root-side delay
 *                measurement and the COPA send/sample logs stay ON in both
 *                builds so the same analyzer compares them directly.
 *
 * The comparison harness (compare/run_compare.sh) writes copa_scenario.h to
 * select the scenario per run; absent that file the defaults below apply. */
#if defined(__has_include)
#if __has_include("copa_scenario.h")
#include "copa_scenario.h"
#endif
#endif

#ifndef WITH_COPA
#define WITH_COPA 1
#endif

#ifndef COPA_BASELINE_INTERVAL_S
#define COPA_BASELINE_INTERVAL_S 6
#endif

/* Set to enable TSCH security */
#ifndef WITH_SECURITY
#define WITH_SECURITY 0
#endif /* WITH_SECURITY */

/* USB serial takes space, free more space elsewhere */
#define SICSLOWPAN_CONF_FRAG 0
#define UIP_CONF_BUFFER_SIZE 160

/* Single channel REQUIRED for this Cooja ContikiMote + UDGM setup.
 * Cooja's ContikiMote radio does not support TSCH channel hopping: with a
 * multi-channel sequence, EBs are spread across channels the unsynchronized
 * joining nodes never hear, so NO node joins (root nbr count stays 0, all
 * nodes stuck at parent=0).  Keep single-channel so the radio model works. */
#undef TSCH_CONF_DEFAULT_HOPPING_SEQUENCE
#define TSCH_CONF_DEFAULT_HOPPING_SEQUENCE TSCH_HOPPING_SEQUENCE_1_1


#define UIP_CONF_IPV6 1
#define NETSTACK_CONF_WITH_IPV6 1

#define ENERGEST_CONF_ON 1


/*******************************************************/
/******************* Configure Node Num ********************/  //ksh..
/*******************************************************/


#define MAX_NODE_NUM 75  /* headroom for up to 70-node experiments */
#undef UIP_CONF_MAX_ROUTES
#define UIP_CONF_MAX_ROUTES MAX_NODE_NUM

#undef NBR_TABLE_CONF_MAX_NEIGHBORS
#define NBR_TABLE_CONF_MAX_NEIGHBORS MAX_NODE_NUM

/* More links needed for large ALICE schedules:
 * each node holds up to 4 links per RPL neighbour × many neighbours */
#define TSCH_SCHEDULE_CONF_MAX_LINKS 256


/*******************************************************/
/******************* Configure RPL ********************/
/*******************************************************/


#undef NETSTACK_CONF_ROUTING
#define NETSTACK_CONF_ROUTING       rpl_classic_driver

#define RPL_CONF_WITH_NON_STORING    0
#define RPL_CONF_MOP                RPL_MOP_STORING_NO_MULTICAST
#define RPL_CONF_WITH_DAO_ACK       1

#undef ROUTING_CONF_RPL_LITE
#define ROUTING_CONF_RPL_LITE        0
#define ROUTING_CONF_RPL_CLASSIC     1

/* --- RPL stability and convergence tuning for 30-node multi-hop --- */

/* DIO trickle: 2^10 = 1024 ms min for fast DODAG discovery. */
#define RPL_CONF_DIO_INTERVAL_MIN        10

/* Cap doublings at 6 so the max DIO interval is 2^16 ~65 s, not 17 min.
 * Keeps topology fresh as nodes join late in a deep multi-hop tree. */
#define RPL_CONF_DIO_INTERVAL_DOUBLINGS  6

/* DAO retransmissions: increase from default 5 to 10 so DAOs survive
 * lossy multi-hop paths and still reach root even after several drops. */
#define RPL_CONF_DAO_MAX_RETRANSMISSIONS 10

/* Retry a lost DAO after 3 s instead of 5 s for faster route registration. */
#define RPL_CONF_DAO_RETRANSMISSION_TIMEOUT (3 * CLOCK_SECOND)

/* Parent hysteresis: use the RFC default of 256.  Higher values (512)
 * made loops harder to break because trapped nodes couldn't find an
 * escape path that was good enough to trigger a switch. */
#define RPL_CONF_MIN_HOPRANKINC          256

/* Loop detector: depart the DODAG after 2 rank-inflation cycles.
 * 4×MIN = 1024 shifted the loop to nodes 13<->22 which are more central,
 * causing 0 root samples vs 4 with 2×MIN.  Restored to 2×MIN = 512. */
#define RPL_CONF_MAX_RANKINC             (2 * RPL_CONF_MIN_HOPRANKINC)






/*******************************************************/
/******************* Configure TSCH ********************/
/*******************************************************/

/* IEEE802.15.4 PANID */
#define IEEE802154_CONF_PANID 0x81a5

/* Do not start TSCH at init, wait for NETSTACK_MAC.on() */
#define TSCH_CONF_AUTOSTART 0

/* 6TiSCH minimal schedule length.
 * Larger values result in less frequent active slots: reduces capacity and saves energy. */
//#define TSCH_SCHEDULE_CONF_DEFAULT_LENGTH 16

/* TSCH Hopping Sequence: Set to 16 channels for better diversity */
//#define TSCH_CONF_DEFAULT_HOPPING_SEQUENCE TSCH_HOPPING_SEQUENCE_16_16

#if WITH_SECURITY

/* Enable security */
#define LLSEC802154_CONF_ENABLED 1

#endif /* WITH_SECURITY */



/*******************************************************/
/******************* Orchestra Scheduler ***************/
/*******************************************************/

#define SEND_INTERVAL   	((6) * (CLOCK_SECOND))
#define SEND_TIME		(random_rand() % (SEND_INTERVAL)) //backoff 

#define UDP_CLIENT_PORT	8765
#define UDP_SERVER_PORT	5678
#define UDP_CONGESTION_PORT 6789






/* Slotframes for 30-node ALICE (mutually prime to avoid alignment). */
#define ORCHESTRA_CONF_COMMON_SHARED_PERIOD 47
#define ORCHESTRA_CONF_UNICAST_PERIOD       47
#define ORCHESTRA_CONF_EBSF_PERIOD          151




#define CURRENT_TSCH_SCHEDULER 3 //MAKE_WITH_SCHEDULER,  1: orchestra 2:mc-orchestra 3:ALICE



#if CURRENT_TSCH_SCHEDULER > 1
#define ORCHESTRA_CONF_RULES { &eb_per_time_source, &default_common , &unicast_per_neighbor_rpl_storing}
#define ALICE_UNICAST_SF_ID 2 //slotframe handle of unicast slotframe
#define ALICE_BROADCAST_SF_ID 1 //slotframe handle of broadcast/default slotframe
#ifndef MULTIPLE_CHANNEL_OFFSETS
#define MULTIPLE_CHANNEL_OFFSETS 1 //ksh.. allow multiple channel offsets.
#endif
#endif



/**********************************************************************/
/*******   orchestra sender-based  vs. receiver-based    **************/
#define ORCHESTRA_CONF_UNICAST_SENDER_BASED 1 //1:sender-based 0:receiver-based
#define ORCHESTRA_ONE_CHANNEL_OFFSET 0 //mc-orchestra -> 1:single channel offset, 0:multiple channel offsets
/**********************************************************************/

/* ---------------- TSCH EB ---------------- */
#undef TSCH_CONF_EB_WITH_SF
#define TSCH_CONF_EB_WITH_SF 0

/* Path-2 overhead reduction: EB period 2s -> 4s halves EB airtime on the
 * shared channel.  Not raised further (e.g. 8s) because COPA's congestion
 * price is piggybacked on EBs — too-rare EBs would starve price dissemination
 * and PRICE_VALIDITY would expire before updates arrive. */
#undef TSCH_CONF_EB_PERIOD
#define TSCH_CONF_EB_PERIOD (2 * CLOCK_SECOND)

#undef TSCH_CONF_MAX_EB_PERIOD
#define TSCH_CONF_MAX_EB_PERIOD (2 * CLOCK_SECOND)  /* for RPL mapping */

#if CURRENT_TSCH_SCHEDULER == 3 //ALICE
#define WITH_ALICE   1//KSH
#undef ORCHESTRA_CONF_UNICAST_SENDER_BASED
#define ORCHESTRA_CONF_UNICAST_SENDER_BASED 1 //1:sender-based 0:receiver-based

#else //CURRENT_TSCH_SCHEDULER != 3
#define WITH_ALICE  0 //KSH
#endif


#if WITH_ALICE
#define ALICE_CALLBACK_PACKET_SELECTION alice_callback_packet_selection //ksh. alice packet selection
#define ALICE_TSCH_CALLBACK_SLOTFRAME_START alice_callback_slotframe_start //ksh. alice time varying slotframe schedule
#endif
/**********************************************************************/
/**********************************************************************/

#define TSCH_PACKET_CONF_EB_WITH_SLOTFRAME_AND_LINK 1


/*******************************************************/
/************* Other system configuration **************/
/*******************************************************/

/* Logging */
#define LOG_CONF_LEVEL_RPL                        LOG_LEVEL_DBG
#define LOG_CONF_LEVEL_TCPIP                     0//  LOG_LEVEL_WARN
#define LOG_CONF_LEVEL_IPV6                      0//  LOG_LEVEL_WARN
#define LOG_CONF_LEVEL_6LOWPAN                   0//  LOG_LEVEL_WARN
#define LOG_CONF_LEVEL_MAC                       0//  LOG_LEVEL_INFO
#define LOG_CONF_LEVEL_FRAMER                    0//  LOG_LEVEL_DBG
#define TSCH_LOG_CONF_PER_SLOT                   0//  1
#define LOG_CONF_LEVEL_TSCH                       LOG_LEVEL_DBG



#endif /* PROJECT_CONF_H_ */
