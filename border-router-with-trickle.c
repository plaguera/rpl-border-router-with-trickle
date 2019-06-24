/*
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
 * This file is part of the Contiki operating system.
 *
 */
/**
 * \file
 *         border-router
 * \author
 *         Niclas Finne <nfi@sics.se>
 *         Joakim Eriksson <joakime@sics.se>
 *         Nicolas Tsiftes <nvt@sics.se>
 */

#include "contiki.h"
#include "contiki-lib.h"
#include "contiki-net.h"
#include "net/ip/uip.h"
#include "net/ipv6/uip-ds6.h"
#include "net/rpl/rpl.h"
#include "net/rpl/rpl-private.h"
#if RPL_WITH_NON_STORING
#include "net/rpl/rpl-ns.h"
#endif /* RPL_WITH_NON_STORING */
#include "net/netstack.h"
#include "dev/button-sensor.h"
#include "dev/slip.h"
#include "lib/trickle-timer.h" //Incluir librería trickle-timer.h

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define DEBUG DEBUG_NONE
#include "net/ip/uip-debug.h"

static uip_ipaddr_t prefix;
static uint8_t prefix_set;

//====================================================
/* Trickle variables and constants */
static struct trickle_timer tt;

#define IMIN               16   /* ticks */
#define IMAX               10   /* doublings */
#define REDUNDANCY_CONST    2

#define NSAMPLES 3
#define SAMPLEPERIOD1 300
#define SAMPLEPERIOD2 600

/* Networking */
#define TRICKLE_PROTO_PORT 30001
static struct uip_udp_conn *trickle_conn; // Estructura que describe una conexión udp
static uip_ipaddr_t ipaddr;     /* destination: link-local all-nodes multicast */

/*
 * For this 'protocol', nodes exchange a token (1 byte) at a frequency
 * governed by trickle. A node detects an inconsistency when it receives a
 * token different than the one it knows.
 * In this case, either:
 * - 'they' have a 'newer' token and we also update our own value, or
 * - 'we' have a 'newer' token, in which case we trigger an inconsistency
 *   without updating our value.
 * In this context, 'newer' is defined in serial number arithmetic terms.
 *
 * Every NEW_TOKEN_INTERVAL clock ticks each node will generate a new token
 * with probability 1/NEW_TOKEN_PROB. This is controlled by etimer et.
 */
#define NEW_TOKEN_INTERVAL  10 * CLOCK_SECOND
#define NEW_TOKEN_PROB      2
static uint8_t token;
static struct etimer et; /* Used to periodically generate inconsistencies */
/*---------------------------------------------------------------------------*/
//================================================================
//================================================================

/* La función tcpip_handler se encarga de manejar los eventos tcpip
que en este caso consisten en la llegada de un paquete anunciando
un nuevo token que habrá que examinar para determinar la consistencia
o inconsistencia*/

static void
tcpip_handler(void)
{
//  leds_on(LEDS_GREEN);
  if(uip_newdata()) { // nuevos datos en paquete ip
    PRINTF("At %lu (I=%lu, c=%u): ", (unsigned long)clock_time(), (unsigned long)tt.i_cur, tt.c);
    PRINTF("Our token=0x%02x, theirs=0x%02x\n", token, ((uint8_t *)uip_appdata)[0]);
    if(token == ((uint8_t *)uip_appdata)[0]) { // consistencia
      PRINTF("Consistent RX\n");
      trickle_timer_consistency(&tt); // el trickle timer se actualiza para consistencia
    } else { // se determina inconsistencia
      if((signed char)(token - ((uint8_t *)uip_appdata)[0]) < 0) {
        PRINTF("Theirs is newer. Update\n");
        token = ((uint8_t *)uip_appdata)[0]; // el token del mote se actualiza solo si hay incosistencia y el que llega es más nuevo
      } else {
        PRINTF("They are behind\n");
      }
      trickle_timer_inconsistency(&tt); // Actualizar timer ante inconsistencia

      /*
       * Here tt.ct.etimer.timer.{start + interval} points to time t in the
       * current interval. However, between t and I it points to the interval's
       * end so if you're going to use this, do so with caution.
       */
      PRINTF("At %lu: Trickle inconsistency. Scheduled TX for %lu\n",
             (unsigned long)clock_time(),
             (unsigned long)(tt.ct.etimer.timer.start +
                             tt.ct.etimer.timer.interval));
    }
  }
  //leds_off(LEDS_GREEN);
  return;
}
/*---------------------------------------------------------------------------*/
/* trickle_tx es la función que sirve para que el mote transmita a sus vecinos su token*/
static void
trickle_tx(void *ptr, uint8_t suppress)
{
  /* *ptr is a pointer to the trickle_timer that triggered this callback. In
   * his example we know that ptr points to tt. However, we pretend that we did
   * not know (which would be the case if we e.g. had multiple trickle timers)
   * and cast it to a local struct trickle_timer* */
  struct trickle_timer *loc_tt = (struct trickle_timer *)ptr;

  if(suppress == TRICKLE_TIMER_TX_SUPPRESS) {
    return;
  }

  //leds_on(LEDS_RED);

  PRINTF("At %lu (I=%lu, c=%u): ", (unsigned long)clock_time(), (unsigned long)loc_tt->i_cur, loc_tt->c);
  PRINTF("Trickle TX token 0x%02x\n", token);

  /* Instead of changing ->ripaddr around by ourselves, we could have used
   * uip_udp_packet_sendto which would have done it for us. However it puts an
   * extra ~20 bytes on stack and the cc2x3x micros hate it, so we stick with
   * send() */

  /* Destination IP: link-local all-nodes multicast */
  uip_ipaddr_copy(&trickle_conn->ripaddr, &ipaddr);
  uip_udp_packet_send(trickle_conn, &token, sizeof(token));

  /* Restore to 'accept incoming from any IP' */
  uip_create_unspecified(&trickle_conn->ripaddr);

  //leds_off(LEDS_RED);
}
/*---------------------------------------------------------------------------*/

PROCESS(border_router_process, "Border router process");

#if WEBSERVER==0
/* No webserver */
AUTOSTART_PROCESSES(&border_router_process);
#elif WEBSERVER>1
/* Use an external webserver application */
#include "webserver-nogui.h"
AUTOSTART_PROCESSES(&border_router_process,&webserver_nogui_process);
#else
/* Use simple webserver with only one page for minimum footprint.
 * Multiple connections can result in interleaved tcp segments since
 * a single static buffer is used for all segments.
 */
#include "httpd-simple.h"
/* The internal webserver can provide additional information if
 * enough program flash is available.
 */
#define WEBSERVER_CONF_LOADTIME 0
#define WEBSERVER_CONF_FILESTATS 0
#define WEBSERVER_CONF_NEIGHBOR_STATUS 0
/* Adding links requires a larger RAM buffer. To avoid static allocation
 * the stack can be used for formatting; however tcp retransmissions
 * and multiple connections can result in garbled segments.
 * TODO:use PSOCk_GENERATOR_SEND and tcp state storage to fix this.
 */
#define WEBSERVER_CONF_ROUTE_LINKS 0
#if WEBSERVER_CONF_ROUTE_LINKS
#define BUF_USES_STACK 1
#endif

PROCESS(webserver_nogui_process, "Web server");
PROCESS_THREAD(webserver_nogui_process, ev, data)
{
  PROCESS_BEGIN();

  httpd_init();

  while(1) {
    PROCESS_WAIT_EVENT_UNTIL(ev == tcpip_event);
    httpd_appcall(data);
  }

  PROCESS_END();
}
AUTOSTART_PROCESSES(&border_router_process,&webserver_nogui_process);

static const char *TOP = "<html><head><title>ContikiRPL</title></head><body>\n";
static const char *BOTTOM = "</body></html>\n";
#if BUF_USES_STACK
static char *bufptr, *bufend;
#define ADD(...) do {                                                   \
    bufptr += snprintf(bufptr, bufend - bufptr, __VA_ARGS__);      \
  } while(0)
#else
static char buf[256];
static int blen;
#define ADD(...) do {                                                   \
    blen += snprintf(&buf[blen], sizeof(buf) - blen, __VA_ARGS__);      \
  } while(0)
#endif
/*---------------------------------------------------------------------------*/
static void
ipaddr_add(const uip_ipaddr_t *addr)
{
  uint16_t a;
  int i, f;
  for(i = 0, f = 0; i < sizeof(uip_ipaddr_t); i += 2) {
    a = (addr->u8[i] << 8) + addr->u8[i + 1];
    if(a == 0 && f >= 0) {
      if(f++ == 0) ADD("::");
    } else {
      if(f > 0) {
        f = -1;
      } else if(i > 0) {
        ADD(":");
      }
      ADD("%x", a);
    }
  }
}
/*---------------------------------------------------------------------------*/
static
PT_THREAD(generate_routes(struct httpd_state *s))
{
  static uip_ds6_route_t *r;
#if RPL_WITH_NON_STORING
  static rpl_ns_node_t *link;
#endif /* RPL_WITH_NON_STORING */
  static uip_ds6_nbr_t *nbr;
#if BUF_USES_STACK
  char buf[256];
#endif
#if WEBSERVER_CONF_LOADTIME
  static clock_time_t numticks;
  numticks = clock_time();
#endif

  PSOCK_BEGIN(&s->sout);

  SEND_STRING(&s->sout, TOP);
#if BUF_USES_STACK
  bufptr = buf;bufend=bufptr+sizeof(buf);
#else
  blen = 0;
#endif
  ADD("RPL Border Router with Trickle");


  SEND_STRING(&s->sout, buf);
  SEND_STRING(&s->sout, BOTTOM);

//=============================================
// Aquí el Border Route decide la actualización del token
       token++;
        PRINTF("At %lu: Generating a new token 0x%02x\n",
               (unsigned long)clock_time(), token);
        trickle_timer_reset_event(&tt);


//=============================================

  PSOCK_END(&s->sout);
}
/*---------------------------------------------------------------------------*/
httpd_simple_script_t
httpd_simple_get_script(const char *name)
{

  return generate_routes;
}

#endif /* WEBSERVER */

/*---------------------------------------------------------------------------*/
static void
print_local_addresses(void)
{
  int i;
  uint8_t state;

  PRINTA("Server IPv6 addresses:\n");
  for(i = 0; i < UIP_DS6_ADDR_NB; i++) {
    state = uip_ds6_if.addr_list[i].state;
    if(uip_ds6_if.addr_list[i].isused &&
       (state == ADDR_TENTATIVE || state == ADDR_PREFERRED)) {
      PRINTA(" ");
      uip_debug_ipaddr_print(&uip_ds6_if.addr_list[i].ipaddr);
      PRINTA("\n");
    }
  }
}
/*---------------------------------------------------------------------------*/
void
request_prefix(void)
{
  /* mess up uip_buf with a dirty request... */
  uip_buf[0] = '?';
  uip_buf[1] = 'P';
  uip_len = 2;
  slip_send();
  uip_len = 0;
  //uip_clear_buf();
}
/*---------------------------------------------------------------------------*/
void
set_prefix_64(uip_ipaddr_t *prefix_64)
{
  rpl_dag_t *dag;
  uip_ipaddr_t ipaddr;
  memcpy(&prefix, prefix_64, 16);
  memcpy(&ipaddr, prefix_64, 16);
  prefix_set = 1;
  uip_ds6_set_addr_iid(&ipaddr, &uip_lladdr);
  uip_ds6_addr_add(&ipaddr, 0, ADDR_AUTOCONF);

  dag = rpl_set_root(RPL_DEFAULT_INSTANCE, &ipaddr);
  if(dag != NULL) {
    rpl_set_prefix(dag, &prefix, 64);
    PRINTF("created a new RPL dag\n");
  }
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(border_router_process, ev, data)
{
  static struct etimer et;

  PROCESS_BEGIN();

  //=================================================================
  // INICIALIZACIÓN DE TRICKLE
  PRINTF("Trickle protocol started\n");
  // Crea en ipaddr la dirección ipv6 FF02::1 (link local all nodes multicast). 
  //El destino serán todos los nodos identificados con direcciones link local
  uip_create_linklocal_allnodes_mcast(&ipaddr); /* Store for later */

  // Inicialización de una conexión UDP. Todos los motes en el mismo puerto.
  //Crea una conexión UDP.
  trickle_conn = udp_new(NULL, UIP_HTONS(TRICKLE_PROTO_PORT), NULL);
  // Bind asocia la conexión a un puerto.
  udp_bind(trickle_conn, UIP_HTONS(TRICKLE_PROTO_PORT));

  PRINTF("Connection: local/remote port %u/%u\n", UIP_HTONS(trickle_conn->lport), UIP_HTONS(trickle_conn->rport));

  // Inicialización del token
  token = 0;

  // Configuración del trickle_timer
  // Primer paso configuración con la estructura trickle_timer y parámetros del algoritmo
  trickle_timer_config(&tt, IMIN, IMAX, REDUNDANCY_CONST);
  // Segundo paso: Se establece la función callback para el envio del token
  // El tercer argumento es un puntero que se pasa a la función callback
  // En este caso es la propia estructura trickle_timer
  trickle_timer_set(&tt, trickle_tx, &tt);
  /*
   * At this point trickle is started and is running the first interval. All
   * nodes 'agree' that token == 0. This will change when one of them randomly
   * decides to generate a new one
   */


  //==================================================================
  /* While waiting for the prefix to be sent through the SLIP connection, the future
  * border router can join an existing DAG as a parent or child, or acquire a default
  * router that will later take precedence over the SLIP fallback interface.
  * Prevent that by turning the radio off until we are initialized as a DAG root.
  */
  prefix_set = 0;
  NETSTACK_MAC.off(0);

  PROCESS_PAUSE();

  SENSORS_ACTIVATE(button_sensor);

  PRINTF("RPL-Border router started\n");
#if 0
   /* The border router runs with a 100% duty cycle in order to ensure high
     packet reception rates.
     Note if the MAC RDC is not turned off now, aggressive power management of the
     cpu will interfere with establishing the SLIP connection */
  NETSTACK_MAC.off(1);
#endif

  /* Request prefix until it has been received */
  while(!prefix_set) {
    etimer_set(&et, CLOCK_SECOND);
    request_prefix();
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));
  }

  /* Now turn the radio on, but disable radio duty cycling.
   * Since we are the DAG root, reception delays would constrain mesh throughbut.
   */
  NETSTACK_MAC.off(1);

#if DEBUG || 1
  print_local_addresses();
#endif

  while(1) {
    PROCESS_YIELD();
    //========================================================
    // Si llega un evento tcpip llamamos al handler que determina
    // a partir del token recibido si hay consistencia o no.
    if(ev == tcpip_event) tcpip_handler();
  }
//======================================================
  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
