#include "contiki.h"
#include "net/netstack.h"
#include "net/nullnet/nullnet.h"
#include <string.h>
#include <stdio.h> /* For printf() */
#include "lib/random.h"
#include "net/linkaddr.h"
#include <random.h>
#include <stdlib.h>

//according to the paper for LEACH-C, the number of cluster k will be 5
static int num_cluster_head = 0;
static int round = 0;
static uint8_t advertisement_byte;

#define P 0.3

/* Log configuration */
#include "sys/log.h"
#define LOG_MODULE "Sink"
#define LOG_LEVEL LOG_LEVEL_INFO

#define SIZE_CLUSTER 2

/* Configuration */
#define SEND_INTERVAL (8 * CLOCK_SECOND)
#define ADV_INTERVAL (600 * CLOCK_SECOND)
#define WAIT_INTERVAL (1000 * CLOCK_SECOND)
#define ROUND_INTERVAL (6000 * CLOCK_SECOND)
#define DATA_INTERVAL (300 * CLOCK_SECOND)

#define MAX_NODE 5

#if MAC_CONF_WITH_TSCH
#include "net/mac/tsch/tsch.h"
static linkaddr_t coordinator_addr =  {{ 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }};
#endif /* MAC_CONF_WITH_TSCH */

static uint8_t msg;
static linkaddr_t can_be_ch[MAX_NODE];

struct NodeInfo{
    linkaddr_t address[MAX_NODE];
    uint32_t energest_time[MAX_NODE];
    uint32_t received_unicast;
    bool is_ch[MAX_NODE];
};

static struct NodeInfo node_info;
static uint32_t average_time;
/*---------------------------------------------------------------------------*/
PROCESS(leach_c_sink_process, "NullNet broadcast example");
PROCESS(c_sink_adv_process, "Sink advertisement Process");
PROCESS(c_sink_ch_process, "Sink cluster head Process");
PROCESS(c_sink_end_process, "Sink end Process");
AUTOSTART_PROCESSES(&leach_c_sink_process);

/*---------------------------------------------------------------------------*/
void input_callback(const void *data, uint16_t len,
  const linkaddr_t *src, const linkaddr_t *dest)
{
    average_time = 0;
    //receive the unicast only here
    if(dest->u8[0] == 0 && dest->u8[1] == 0){
        //broadcast
    } else {
        //unicast
        uint32_t received_energy = *((uint32_t *)data); // Dereference properly
        LOG_INFO("Received data from cluster head: %d, %lu\n", src->u8[0], (unsigned long)received_energy); // Use the correct format specifier
        node_info.energest_time[node_info.received_unicast] = received_energy;
        node_info.address[node_info.received_unicast] = *src;
        node_info.received_unicast++;
        average_time += received_energy;
    }
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(leach_c_sink_process, ev, data)
{
   // define the etimers
    static struct etimer round_timer; //for every one round
    static struct etimer adv_leach_timer;
    static struct etimer data_leach_timer;
    static struct etimer data_sending_timer;
    PROCESS_BEGIN();
    node_info.received_unicast = 0;

// set tsch? I don't understand this part, but without this, the unicast and the broadcast does not work
#if MAC_CONF_WITH_TSCH
  tsch_set_coordinator(linkaddr_cmp(&coordinator_addr, &linkaddr_node_addr));
#endif /* MAC_CONF_WITH_TSCH */

    /* Initialize NullNet */
    nullnet_set_input_callback(input_callback);

    etimer_set(&round_timer, ROUND_INTERVAL);

    while(1){
        PROCESS_WAIT_UNTIL(etimer_expired(&round_timer));
        //advertise itself to the other nodes
        process_start(&c_sink_adv_process, NULL);
        
        //receive the energy information from the nodes
        etimer_set(&data_leach_timer, DATA_INTERVAL);
        PROCESS_WAIT_UNTIL(etimer_expired(&data_leach_timer));
  
        //send back the cluster head information to the nodes
        process_start(&c_sink_ch_process, NULL);
        etimer_set(&adv_leach_timer, ADV_INTERVAL);
        PROCESS_WAIT_UNTIL(etimer_expired(&adv_leach_timer));

        //are the same from below
        //cluster heads start sending advertisements
        etimer_set(&data_sending_timer, WAIT_INTERVAL);
        PROCESS_WAIT_UNTIL(etimer_expired(&data_sending_timer));

        //cluster nodes send back the unicast to the CH
        etimer_reset(&adv_leach_timer);
        PROCESS_WAIT_UNTIL(etimer_expired(&adv_leach_timer));
        
        //CH make tdma and send broadcast
        etimer_reset(&adv_leach_timer);
        PROCESS_WAIT_UNTIL(etimer_expired(&adv_leach_timer));

        //nodes send data using TDMA schedule
        etimer_reset(&adv_leach_timer);
        PROCESS_WAIT_UNTIL(etimer_expired(&adv_leach_timer));

        //CH fuse data and send to sink
        etimer_reset(&data_sending_timer);
        PROCESS_WAIT_UNTIL(etimer_expired(&data_sending_timer));

        process_start(&c_sink_end_process, NULL);
        // reset timer
        etimer_reset(&round_timer);
        etimer_reset(&adv_leach_timer);
        etimer_reset(&data_leach_timer);

    }
    PROCESS_END();
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(c_sink_adv_process, ev, data){
    PROCESS_BEGIN();
    LOG_INFO("SINK ADVERTISEMENT\n");
    msg = 1;
    nullnet_buf = &msg;
    nullnet_len = sizeof(msg);
    NETSTACK_NETWORK.output(NULL);
    PROCESS_END();
}

void shuffle(linkaddr_t *array, int n) {
    for (int i = n - 1; i > 0; i--) {
        int j = rand() % (i + 1); // Generate a random index between 0 and i
        // Swap array[i] and array[j]
        linkaddr_t temp = array[i];
        array[i] = array[j];
        array[j] = temp;
    }
}

PROCESS_THREAD(c_sink_ch_process, ev, data){
    PROCESS_BEGIN();
    int ch_count = 0;
    LOG_INFO("SINK CLUSTER HEAD ASSIGNMENT\n");
    average_time /= node_info.received_unicast;
    for(int i=0; i<MAX_NODE; i++){
        if(node_info.energest_time[i] < average_time){
            // can attend the cluster head assignment
            can_be_ch[ch_count++] = node_info.address[i];
        }
    }
    shuffle(can_be_ch, node_info.received_unicast);
    printf("Randomly chosen x elements:\n");
    for (int i = 0; i < 2; i++) {
        printf("%d\n", can_be_ch[i].u8[0]); // Print the value of the first byte (modify as needed)
        nullnet_buf = &advertisement_byte;
        nullnet_len = sizeof(advertisement_byte);
        NETSTACK_NETWORK.output(&can_be_ch[i]); 
    }
    PROCESS_END();
}

PROCESS_THREAD(c_sink_end_process, ev, data){
    PROCESS_BEGIN();

    for(int i=0; i<MAX_NODE; i++){
        node_info.address[i].u8[0]= 0;
        node_info.address[i].u8[1]= 0;
        node_info.energest_time[i]= 0;
        node_info.is_ch[i] = false;
    }
    node_info.received_unicast = 0;
    num_cluster_head = 0;

    LOG_INFO("SINK END\n");
    round++;
    PROCESS_END();
}
