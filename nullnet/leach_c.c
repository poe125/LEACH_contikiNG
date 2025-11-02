#include "contiki.h"
#include "net/netstack.h"
#include "net/nullnet/nullnet.h"
#include "net/packetbuf.h"
#include "sys/energest.h"

#include <string.h>
#include <stdio.h> /* For printf() */
#include <stdlib.h>
#include <etimer.h>
#include <random.h>
#include <process.h>

/* Log configuration */
#include "sys/log.h"
#define LOG_MODULE "LEACH_Node"
#define LOG_LEVEL LOG_LEVEL_INFO

/* Configuration */
//addition of all the intervals should not go over the round interval
#define ADV_INTERVAL (600 * CLOCK_SECOND)
#define WAIT_INTERVAL (1000 * CLOCK_SECOND)
#define ROUND_INTERVAL (6000 * CLOCK_SECOND)
#define DATA_INTERVAL (300 * CLOCK_SECOND)
#define P 0.3
#define MAX_NODE 100
#define DATA_NUM 10
#define ADDR_LENGTH sizeof(linkaddr_t)

/* Configuration */
#define SEND_INTERVAL (8 * CLOCK_SECOND)
static linkaddr_t dest_addr;

#if MAC_CONF_WITH_TSCH
#include "net/mac/tsch/tsch.h"
static linkaddr_t coordinator_addr;
#endif /* MAC_CONF_WITH_TSCH */

#define SLOT_DURATION (CLOCK_SECOND * 5)  // Adjust the slot duration as needed

/*---------------------------------------3
------------------------------------*/
static bool is_ch = false;
static int round = 0;
static int round_ch = -1/P;
static bool from_sink = true;


// non cluster head
static bool new_broadcast_received = false;
static bool receiving_tdma_slots = false;

// cluster head
static bool receiving_tdma_data = false;
static uint8_t advertisement_byte;

static linkaddr_t sink_addr;

struct NeighborInfo{
    linkaddr_t address;
    int16_t rssi;
};

typedef struct {
    linkaddr_t address[MAX_NODE];
    uint8_t cluster_size;
} TdmaPacket;

typedef struct {
    int round;
    int values[DATA_NUM];
} DataPacket;

static struct NeighborInfo strongest_neighbor;
static TdmaPacket tdma_packet;
static DataPacket data_packet;
static DataPacket mean_packet;

/*---------------------------------------------------------------------------*/
PROCESS(leach_c_process, "LEACH-C Process");
PROCESS(send_to_sink_process, "Sending to sink process");
PROCESS(c_ch_process, "Choosing cluster head");
PROCESS(c_adv_process, "Advertisement Process");
PROCESS(c_response_process, "Response Process");
PROCESS(c_tdma_make_process, "Creating TDMA Process");
PROCESS(c_send_tdma_process, "Sending with TDMA Process");
PROCESS(c_data_fuse_process, "Data fusion Process");

PROCESS(c_wait_for_broadcast_process, "Receive Data Process");
PROCESS(c_free_process, "freeing process");

AUTOSTART_PROCESSES(&leach_c_process);

/*---------------------------------------------------------------------------*/
// callbacks for broadcast/unicast
void input_callback(const void *data, uint16_t len,
  const linkaddr_t *src, const linkaddr_t *dest)
{
    if(dest->u8[0] == 0 && dest->u8[1] == 0){ //broadcast
        if(!is_ch && !from_sink){
            int16_t rssi = packetbuf_attr(PACKETBUF_ATTR_RSSI);
            //FIRST BROADCAST (ADVERTISEMENT)
            if(!receiving_tdma_slots){
                LOG_INFO("[%d] Receiving advertisement broadcast from %d\n", round, src->u8[0]);
                if(!new_broadcast_received){
                    strongest_neighbor.address = *src;
                    strongest_neighbor.rssi = rssi;
                    new_broadcast_received = true;
                } else if (rssi > strongest_neighbor.rssi){
                    strongest_neighbor.address = *src;
                    strongest_neighbor.rssi = rssi;
                }
            } else if(linkaddr_cmp(src, &strongest_neighbor.address)){ 
                //SECOND BROADCAST (TDMA SLOT)
                LOG_INFO("[%d] Receiving TDMA broadcast from %d\n", round, src->u8[0]);
                tdma_packet = *((TdmaPacket *)data);
                uint8_t received_cluster_size = tdma_packet.cluster_size;
                LOG_INFO("[%d] Received packet: cluster size %d\n", round, received_cluster_size);
                for(int i=0; i<received_cluster_size; i++){
                    LOG_INFO("[%d] [%d] Received packet: address %d\n", round, i, tdma_packet.address[i].u8[0]);
                }
            } else {
                LOG_INFO("RECEIVED SOMETHING\n");
            }
        } else if(from_sink){
            //receive the broadcast from the sink
            from_sink = false;
            sink_addr = *src;
            LOG_INFO("[%d] Received packet from sink: address %d\n", round, sink_addr.u8[0]);
        }

    } else { //unicast
        if(is_ch){

            if(!receiving_tdma_data){

                bool node_already_present = false;
                for (int i = 0; i < tdma_packet.cluster_size; i++) {
                    if (linkaddr_cmp(src, &tdma_packet.address[i])) {
                        node_already_present = true;
                        break;
                    }
                }

                if (!node_already_present) {
                    tdma_packet.address[tdma_packet.cluster_size] = *src;
                    LOG_INFO("[%d] Received advertisement from %d cluster size %d\n",round, src->u8[0], tdma_packet.cluster_size);
                    tdma_packet.cluster_size++;
                }

            } else {
                //SECOND UNICAST (DATA)
                LOG_INFO("[%d] Received data from %d\n", round , src->u8[0]);
                
                data_packet = *((DataPacket *)data);
                for(int i=0; i<DATA_NUM; i++){
                    mean_packet.values[i] = (data_packet.values[i] + mean_packet.values[i])/2;
                }
            }
        }
    }
}

// function to reset cluster information for the next round
void free_array(){
    for(int i=0; i<DATA_NUM; i++){
        data_packet.values[i] = 0;
        mean_packet.values[i] = 0;
    }
    for(int j=0; j<MAX_NODE; j++){
        tdma_packet.address[j].u8[0] = 0;
        tdma_packet.address[j].u8[1] = 0;
    }
    tdma_packet.cluster_size = 0;
}

void generateRandomData(DataPacket *packet) {
    for (int i = 0; i < DATA_NUM; i++) {
        packet->values[i] = rand() % 1000; // Generate a random number between 0 and 999
    }
}

/*---------------------------------------------------------------------------*/

//the main process for running the leach protocol
PROCESS_THREAD(leach_c_process, ev, data){
    // define the etimers
    static struct etimer round_timer; //for every one round
    static struct etimer adv_leach_timer;
    static struct etimer data_leach_timer;
    static struct etimer data_sending_timer;
    PROCESS_BEGIN();

    LOG_INFO("Round_ch%d and round:%d\n", round_ch, round);
// set tsch? I don't understand this part, but without this, the unicast and the broadcast does not work
#if MAC_CONF_WITH_TSCH
  tsch_set_coordinator(linkaddr_cmp(&coordinator_addr, &linkaddr_node_addr));
#endif /* MAC_CONF_WITH_TSCH */

    /* Initialize NullNet */
    nullnet_set_input_callback(input_callback);

    /*Initialize Energest*/
    energest_init();

    etimer_set(&round_timer, ROUND_INTERVAL);

    //receive sink information

    while(1){
        PROCESS_WAIT_UNTIL(etimer_expired(&round_timer));


        //receive sink advertisement
        etimer_set(&data_leach_timer, DATA_INTERVAL);
        PROCESS_WAIT_UNTIL(etimer_expired(&data_leach_timer));
  
        //send energy information to sink
        process_start(&send_to_sink_process, NULL);
        etimer_set(&adv_leach_timer, ADV_INTERVAL);
        PROCESS_WAIT_UNTIL(etimer_expired(&adv_leach_timer));

        //receive cluster head information from the sink
        etimer_set(&adv_leach_timer, ADV_INTERVAL);
        PROCESS_WAIT_UNTIL(etimer_expired(&adv_leach_timer));

        //The same from here
        LOG_INFO("[%d] is_ch: %d\n", round, is_ch);
        if(is_ch){
            process_start(&c_adv_process, NULL);
            round_ch = round;
            // LOG_INFO("[%d] Sent out advertisements\n", round);
        }

        if(!is_ch){
            process_start(&c_wait_for_broadcast_process, NULL);
        }
        
        etimer_set(&data_leach_timer, DATA_INTERVAL);
        PROCESS_WAIT_UNTIL(etimer_expired(&data_leach_timer));
        
        
        
        if(!is_ch){
            process_start(&c_response_process, NULL);
            // LOG_INFO("[%d] Sent back unicast\n", round);
        }    
        etimer_set(&adv_leach_timer, ADV_INTERVAL);
        PROCESS_WAIT_UNTIL(etimer_expired(&adv_leach_timer));


        //CH make TDMA schedule and sends the broadcast
        if(is_ch){
            process_start(&c_tdma_make_process, NULL);
        } else {
            process_start(&c_wait_for_broadcast_process, NULL);
        }

        etimer_reset(&adv_leach_timer);
        PROCESS_WAIT_UNTIL(etimer_expired(&adv_leach_timer));
        
        //nodes send data using TDMA schedule
        if(!is_ch){
            process_start(&c_send_tdma_process, NULL);
        }
        
        etimer_reset(&adv_leach_timer);
        PROCESS_WAIT_UNTIL(etimer_expired(&adv_leach_timer));

        //CH fuse data and send to sink
        if(is_ch){
            process_start(&c_data_fuse_process, NULL);
        }

        etimer_set(&data_sending_timer, WAIT_INTERVAL);
        PROCESS_WAIT_UNTIL(etimer_expired(&data_sending_timer));

        // reset timer
        etimer_reset(&round_timer);
        etimer_reset(&adv_leach_timer);
        etimer_reset(&data_leach_timer);

        process_start(&c_free_process, NULL);
    }
    PROCESS_END();
}

/*---------------------------------------------------------------------------*/
PROCESS_THREAD(send_to_sink_process, ev, data) {
    uint32_t total_energy = energest_type_time(ENERGEST_TYPE_LISTEN) + energest_type_time(ENERGEST_TYPE_TRANSMIT);
    uint32_t total_energy_seconds = total_energy / CLOCK_SECOND;

    PROCESS_BEGIN();
    
    //The only unicast sending to the sink
    nullnet_buf = (uint8_t *)&total_energy_seconds;
    nullnet_len = sizeof(total_energy_seconds);
    NETSTACK_NETWORK.output(&sink_addr);

    PROCESS_END();
}

// decide the cluster heads
PROCESS_THREAD(c_ch_process, ev, data){
    PROCESS_BEGIN();

    energest_flush();

    // check whether the node has been a cluster head within the last 1/P rounds
    // if fulfilling the condition, participate in choosing the clusterhead
    LOG_INFO("[%d] Sending out broadcast\n", round);
    nullnet_buf = &advertisement_byte;
    nullnet_len = sizeof(advertisement_byte);
    NETSTACK_NETWORK.output(NULL);    

    PROCESS_END();
}

/*---------------------------------------------------------------------------*/
PROCESS_THREAD(c_wait_for_broadcast_process, ev, data){
    static struct etimer wait_timer;
    PROCESS_BEGIN();

    LOG_INFO("[%d] Waiting for messages...\n", round);
    etimer_set(&wait_timer, WAIT_INTERVAL);
    
    while (1) {
        PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&wait_timer));
        etimer_reset(&wait_timer);
    }

    PROCESS_END();
}


/*---------------------------------------------------------------------------*/
PROCESS_THREAD(c_adv_process, ev, data){
    // define the etimers
    PROCESS_BEGIN();
    // to wait for another 1/P rounds to become a cluster head
    
    LOG_INFO("[%d] Sending out broadcast\n", round);
    nullnet_buf = &advertisement_byte;
    nullnet_len = sizeof(advertisement_byte);
    NETSTACK_NETWORK.output(NULL);
    PROCESS_END();
}

/*---------------------------------------------------------------------------*/
PROCESS_THREAD(c_response_process, ev, data){
    // define the etimers
    PROCESS_BEGIN();
    LOG_INFO("[%d] Sending unicast response\n", round);
    dest_addr = strongest_neighbor.address;
    //this should be after receiving all the broadcasts from the ch nodes
    receiving_tdma_slots = true;

    // if a node received the broadcast
    if(new_broadcast_received){
        // send a unicast to the CH
        nullnet_buf = &advertisement_byte;
        nullnet_len = sizeof(advertisement_byte);
        NETSTACK_NETWORK.output(&dest_addr);
        LOG_INFO("[%d] sent out unicast to %d\n", round, dest_addr.u8[0]);
    } else {
        // taking out the ones that didn't receive a broadcast from the members of the cluster
        LOG_INFO("[%d] doesn't belong in any cluster\n", round);
    }
    
    PROCESS_END();
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(c_tdma_make_process, ev, data){
    // define the etimers
    
    PROCESS_BEGIN();
    LOG_INFO("[%d] Sending TDMA schedule\n", round);

    // send TDMA schedule using broadcast
    //should be finished receiving all the unicast advertisements
    receiving_tdma_data = true;
    nullnet_buf = (uint8_t *)&tdma_packet;
    nullnet_len = sizeof(tdma_packet);
    NETSTACK_NETWORK.output(NULL);
    PROCESS_END();
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(c_send_tdma_process, ev, data){
    // define the etimers
    // static struct etimer adv_timer; // for the advertisement (broadcasting) by clusterheads

    PROCESS_BEGIN();

    LOG_INFO("[%d] Sending with TDMA process\n", round);
    
    generateRandomData(&data_packet);
    LOG_INFO("[%d] packet size: %d\n", round, tdma_packet.cluster_size);

    for(int i=0; i<tdma_packet.cluster_size; i++){
        if(linkaddr_cmp(&tdma_packet.address[i], &linkaddr_node_addr)){
            // Set the destination address for NullNet
            nullnet_buf = (uint8_t *)&data_packet;
            nullnet_len = sizeof(data_packet);
            NETSTACK_NETWORK.output(&dest_addr);
        } else {
            int j=0;
            while(j<100){
                j++;
                //should be sleeping
            }
        }
    }
        
    PROCESS_END();
}

/*---------------------------------------------------------------------------*/
PROCESS_THREAD(c_data_fuse_process, ev, data){
    // define the etimers
    // static struct etimer adv_timer; // for the advertisement (broadcasting) by clusterheads
    PROCESS_BEGIN();
    LOG_INFO("[%d] Sending fused data to %d\n", round, sink_addr.u8[0]);
    mean_packet.round = round;
    nullnet_buf = (uint8_t *)&mean_packet;
    nullnet_len = sizeof(mean_packet);
    NETSTACK_NETWORK.output(&sink_addr);
    PROCESS_END();
}

/*---------------------------------------------------------------------------*/
PROCESS_THREAD(c_free_process, ev, data){
    PROCESS_BEGIN();

    LOG_INFO("[%d] Round_ch: %d\n",round, round_ch);

    LOG_INFO("[%d] Freeing cluster neighbor information\n", round);
    
    // reset parameters
    free_array();
    round++;
    new_broadcast_received = false;
    receiving_tdma_slots = false;
    receiving_tdma_data = false;
    is_ch = false;

    printf("\nEnergest:\n");
    printf(" CPU          %lus LPM      %lus DEEP LPM %lus  Total time %lus\n",
           energest_type_time(ENERGEST_TYPE_CPU),
           energest_type_time(ENERGEST_TYPE_LPM),
           energest_type_time(ENERGEST_TYPE_DEEP_LPM),
           ENERGEST_GET_TOTAL_TIME());

    printf(" Radio LISTEN %lus TRANSMIT %lus OFF      %lus\n",
           energest_type_time(ENERGEST_TYPE_LISTEN),
           energest_type_time(ENERGEST_TYPE_TRANSMIT),
           ENERGEST_GET_TOTAL_TIME() - energest_type_time(ENERGEST_TYPE_TRANSMIT) - energest_type_time(ENERGEST_TYPE_LISTEN));


    PROCESS_END();
}