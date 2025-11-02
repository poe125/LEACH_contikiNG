#include "contiki.h"
#include "net/netstack.h"
#include "net/nullnet/nullnet.h"
#include <string.h>
#include <stdio.h> /* For printf() */

/* Log configuration */
#include "sys/log.h"
#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO

/* Configuration */
#define SEND_INTERVAL (8 * CLOCK_SECOND)
#define ADV_INTERVAL (600 * CLOCK_SECOND)
#define WAIT_INTERVAL (1000 * CLOCK_SECOND)
#define ROUND_INTERVAL (6000 * CLOCK_SECOND)
#define DATA_INTERVAL (300 * CLOCK_SECOND)

#if MAC_CONF_WITH_TSCH
#include "net/mac/tsch/tsch.h"
static linkaddr_t coordinator_addr =  {{ 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }};
#endif /* MAC_CONF_WITH_TSCH */

static uint8_t msg;

/*---------------------------------------------------------------------------*/
PROCESS(nullnet_example_process, "NullNet broadcast example");
PROCESS(sink_adv_process, "Sink advertisement Process");
PROCESS(sink_end_process, "Sink end Process");
AUTOSTART_PROCESSES(&nullnet_example_process);

/*---------------------------------------------------------------------------*/
void input_callback(const void *data, uint16_t len,
  const linkaddr_t *src, const linkaddr_t *dest)
{
  //receive the unicast only here
  if(dest->u8[0] == 0 && dest->u8[1] == 0){
  //broadcast
  } else {
    LOG_INFO("Received data from cluster head: %d\n", src->u8[0]);
  }
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(nullnet_example_process, ev, data)
{
   // define the etimers
    static struct etimer round_timer; //for every one round
    static struct etimer adv_leach_timer;
    static struct etimer data_leach_timer;
    static struct etimer data_sending_timer;
    PROCESS_BEGIN();

// set tsch? I don't understand this part, but without this, the unicast and the broadcast does not work
#if MAC_CONF_WITH_TSCH
  tsch_set_coordinator(linkaddr_cmp(&coordinator_addr, &linkaddr_node_addr));
#endif /* MAC_CONF_WITH_TSCH */

    /* Initialize NullNet */
    nullnet_set_input_callback(input_callback);

    process_start(&sink_adv_process, NULL);

    etimer_set(&data_leach_timer, DATA_INTERVAL);
    PROCESS_WAIT_UNTIL(etimer_expired(&data_leach_timer));

    etimer_set(&round_timer, ROUND_INTERVAL);

    while(1){
        PROCESS_WAIT_UNTIL(etimer_expired(&round_timer));
        
        etimer_set(&data_leach_timer, DATA_INTERVAL);
        PROCESS_WAIT_UNTIL(etimer_expired(&data_leach_timer));
  
        etimer_set(&adv_leach_timer, ADV_INTERVAL);
        PROCESS_WAIT_UNTIL(etimer_expired(&adv_leach_timer));

        etimer_reset(&adv_leach_timer);
        PROCESS_WAIT_UNTIL(etimer_expired(&adv_leach_timer));
        
        etimer_reset(&adv_leach_timer);
        PROCESS_WAIT_UNTIL(etimer_expired(&adv_leach_timer));

        etimer_set(&data_sending_timer, WAIT_INTERVAL);
        PROCESS_WAIT_UNTIL(etimer_expired(&data_sending_timer));

        process_start(&sink_end_process, NULL);
        // reset timer
        etimer_reset(&round_timer);
        etimer_reset(&adv_leach_timer);
        etimer_reset(&data_leach_timer);

    }
    PROCESS_END();
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(sink_adv_process, ev, data){
    PROCESS_BEGIN();
    LOG_INFO("SINK ADVERTISEMENT\n");
    msg = 1;
    nullnet_buf = &msg;
    nullnet_len = sizeof(msg);
    NETSTACK_NETWORK.output(NULL);
    PROCESS_END();
}


PROCESS_THREAD(sink_end_process, ev, data){
    PROCESS_BEGIN();
    LOG_INFO("SINK END\n");
    PROCESS_END();
}
