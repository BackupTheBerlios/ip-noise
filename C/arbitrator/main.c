#ifndef __KERNEL__
#include <linux/netfilter.h>
#include <libipq.h>
#include <stdio.h>
#include <sys/time.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#else
#include "k_stdlib.h"
#include "k_stdio.h"
#endif

#include "queue.h"
#include "delayer.h"
#include "verdict.h"
#include "iface.h"
#include "switcher.h"
#include "packet_logic.h"

#ifdef __KERNEL__
#include <linux/version.h>
#include "k_ipq.h"
#endif

#define DEBUG

#ifndef __KERNEL__
static void die(struct ipq_handle * h)
{
    ipq_perror("IP-Noise Arbitrator");
    ipq_destroy_handle(h);
    exit(-1);
}
#else
#define die(h) 
#endif

struct ip_noise_decide_what_to_do_with_packets_thread_context_struct
{
#ifndef __KERNEL__
    ip_noise_messages_queue_t * queue;
#endif
    int * terminate;
    struct ipq_handle * h;
    ip_noise_delayer_t * delayer;
    ip_noise_arbitrator_data_t * * data;
    ip_noise_flags_t * flags;
};

typedef struct ip_noise_decide_what_to_do_with_packets_thread_context_struct ip_noise_decide_what_to_do_with_packets_thread_context_t;

#ifndef __KERNEL__
static void * ip_noise_decide_what_to_do_with_packets_thread_func (void * void_context)
{
    ip_noise_decide_what_to_do_with_packets_thread_context_t * context;      
    ip_noise_messages_queue_t * packets_to_arbitrate_queue;
    int * terminate;
    ip_noise_message_t * msg_with_time;
    int status;
    struct ipq_handle * h;
#ifdef DEBUG
    static int num;
#endif
    ip_noise_verdict_t verdict;
    ip_noise_delayer_t * delayer;
    ip_noise_arbitrator_data_t * * data;
    ip_noise_flags_t * flags;
    ip_noise_arbitrator_packet_logic_t * packet_logic;

    context = (ip_noise_decide_what_to_do_with_packets_thread_context_t * )void_context;

    packets_to_arbitrate_queue = context->queue;
    terminate = context->terminate;
    h = context->h;
    delayer = context->delayer;
    data = context->data;
    flags = context->flags;

    free(context);


    packet_logic = ip_noise_arbitrator_packet_logic_alloc(data, flags);
    
    /* As long as another thread did not instruct us to terminate - loop!
     *
     */
    while (! (*terminate))
    {
        /* 
         * Inside the loop, we dequeue an item from the queue,
         * decide what to do with it. If it should be dropped or released,
         * it is done immidiately. Else, it is placed in the delayer's priority
         * queue for release in the future.
         * */
        msg_with_time = ip_noise_messages_queue_dequeue(packets_to_arbitrate_queue);
        if (msg_with_time == NULL)
        {
            usleep(500);
            continue;
        }

#if 0
        verdict = decide_what_to_do_with_packet(msg_with_time->m);
#endif
        verdict = ip_noise_arbitrator_packet_logic_decide_what_to_do_with_packet(packet_logic, msg_with_time->m);
        
        if (verdict.action == IP_NOISE_VERDICT_ACCEPT)
        {
#ifdef DEBUG
#if 0
            printf("Release Packet! (%i)\n", num++);
#endif
#endif
            status = ipq_set_verdict(h, msg_with_time->m->packet_id, NF_ACCEPT, 0, NULL);

            if (status < 0)
            {
                die(h);
            }

            free(msg_with_time);
        }
        else if (verdict.action == IP_NOISE_VERDICT_DROP)
        {
#ifdef DEBUG
            printf("Dropping Packet! (%i)\n", num++);
#endif
            status = ipq_set_verdict(h, msg_with_time->m->packet_id, NF_DROP, 0, NULL);

            if (status < 0)
            {
                die(h);
            }

            free(msg_with_time);
        }
        else if (verdict.action == IP_NOISE_VERDICT_DELAY)
        {
#ifdef DEBUG
            printf("Delaying Packet! (%i)\n", num++);
#endif
            ip_noise_delayer_delay_packet(
                delayer,
                msg_with_time,
                msg_with_time->tv,
                verdict.delay_len
                );
        }
        else
        {
            *terminate = 1;
            fprintf(stderr, "Unknown Action!\n");
            return NULL;
        }
    }

    return NULL;
}
#endif

struct ip_noise_release_packets_thread_context_struct
{
    int * terminate;
    ip_noise_delayer_t * delayer;
};

typedef struct ip_noise_release_packets_thread_context_struct ip_noise_release_packets_thread_context_t;

#ifndef __KERNEL__
/* 
 * This function calls ip_noise_delayer_loop() to perform a loop for the
 * relase of the delayed packets
 * */
static void * release_packets_thread_func(void * void_context)
{
    ip_noise_release_packets_thread_context_t * context;
    ip_noise_delayer_t * delayer;
    int * terminate;

    context = (ip_noise_release_packets_thread_context_t *)void_context;
    delayer = context->delayer;
    terminate = context->terminate;

    free(context);

    delayer->terminate = terminate;

    ip_noise_delayer_loop(delayer);

    return NULL;
    
}
#endif

static void ip_noise_delayer_release_function(ip_noise_message_t * m, void * context)
{
#ifndef __KERNEL__    
    struct ipq_handle * h = (struct ipq_handle *)context;
#else
#define h 5
#endif
    int status;
    status = ipq_set_verdict(h, m->m->packet_id, NF_ACCEPT, 0, NULL);

    if (status < 0)
    {
        die(h);
    }    
    free(m);
}

#ifdef __KERNEL__
#undef h
#endif

#ifndef __KERNEL__
static void * arb_iface_thread_func(void * context)
{
    ip_noise_arbitrator_iface_loop( (ip_noise_arbitrator_iface_t * )context);

    return NULL;
}


static void * arb_switcher_thread_func(void * context)
{
    ip_noise_arbitrator_switcher_loop( (ip_noise_arbitrator_switcher_t * )context);

    return NULL;
}
#endif

/*
 * What the main() function does is initialize all the objects, 
 * inter-connect them and start all of their threads.
 *
 * The objects are inter-connected very deeply, so this code is quite ugly.
 *
 * */
#ifndef __KERNEL__
int main(int argc, char * argv[])
#else
ip_noise_arbitrator_packet_logic_t * main_init_module(
        ip_noise_arbitrator_iface_t * * iface_ptr
        )
#endif
{
#ifndef __KERNEL__    
    int status;

    unsigned char message[IP_NOISE_MESSAGE_BUFSIZE];
    struct ipq_handle * h;

    ip_noise_messages_queue_t * packets_to_arbitrate_queue;
#endif
    int terminate = 0;
    
    ip_noise_decide_what_to_do_with_packets_thread_context_t * arbitrator_context;

#ifndef __KERNEL__
    pthread_t decide_what_to_with_packets_thread;
#endif    
    
    ip_noise_release_packets_thread_context_t * release_packets_context;
#ifndef __KERNEL__
    pthread_t release_packets_thread;
#endif
#ifndef __KERNEL__
    int check;
#endif
    ip_noise_delayer_t * delayer;

    ip_noise_arbitrator_data_t * data, * * data_ptr;
    ip_noise_flags_t flags;
    ip_noise_arbitrator_iface_t * arb_iface;
#ifndef __KERNEL__
    pthread_t arb_iface_thread;
#endif

    ip_noise_arbitrator_switcher_t * arb_switcher;
#ifndef __KERNEL__
    pthread_t arb_switcher_thread;
#endif
#ifdef __KERNEL__
    ip_noise_arbitrator_packet_logic_t * packet_logic;
#endif

    printf("IP-Noise Simulator\n");
    printf("Written by Shlomi Fish & Roy Glasberg and supervised by Lavy Libman\n");
    printf("The Technion - Israel Institute of Technolgy\n");
    printf("(c) 2001\n");

#ifndef __KERNEL__
    h = ipq_create_handle(0, PF_INET);
    if (h == NULL)
    {
        die(h);
    }

    status = ipq_set_mode(h, IPQ_COPY_PACKET, sizeof(message));

    if (status < 0)
    {
        die(h);
    }
#endif

#ifndef __KERNEL__
    packets_to_arbitrate_queue = ip_noise_messages_queue_alloc();
#endif

    delayer = ip_noise_delayer_alloc(
            ip_noise_delayer_release_function, 
#ifndef __KERNEL__
            (void *)h
#else
            NULL
#endif            
            );
    
    release_packets_context = malloc(sizeof(ip_noise_release_packets_thread_context_t));
    release_packets_context->delayer = delayer;
    release_packets_context->terminate = &terminate;

#ifndef __KERNEL__
    check = pthread_create(
        &release_packets_thread,
        NULL,
        release_packets_thread_func,
        (void *)release_packets_context
        );

    if (check != 0)
    {
        fprintf(stderr, "Could not create the release packets thread!\n");
        exit(-1);
    }
#endif

    data = ip_noise_arbitrator_data_alloc();
    data_ptr = malloc(sizeof(*data_ptr));
    *data_ptr = data;

    flags.reinit_switcher = 1;

    arb_switcher = ip_noise_arbitrator_switcher_alloc(data_ptr, &flags, &terminate);

#ifndef __KERNEL__
    check = pthread_create(
        &arb_switcher_thread,
        NULL,
        arb_switcher_thread_func,
        (void *)arb_switcher
        );

    if (check != 0)
    {
        fprintf(stderr, "Could not create the arbitrator switcher thread!\n");
        exit(-1);
    }
#endif

    arb_iface = ip_noise_arbitrator_iface_alloc(data_ptr, arb_switcher, &flags);

#ifdef __KERNEL__
    /*
     * We assign arb_iface to iface_ptr so it can later be de-allocated
     * inside the module ip_queue.c.
     *
     * */
    *iface_ptr = arb_iface;
#endif

#ifndef __KERNEL__
    check = pthread_create(
        &arb_iface_thread,
        NULL,
        arb_iface_thread_func,
        (void *)arb_iface
        );

    if (check != 0)
    {
        fprintf(stderr, "Could not create the arbitrator interface thread!\n");
        exit(-1);
    }
#endif


    arbitrator_context = malloc(sizeof(ip_noise_decide_what_to_do_with_packets_thread_context_t));
#ifndef __KERNEL__
    arbitrator_context->queue = packets_to_arbitrate_queue ;
    arbitrator_context->h = h;
#endif    
    arbitrator_context->terminate = &terminate;
    arbitrator_context->delayer = delayer;
    arbitrator_context->data = data_ptr;
    arbitrator_context->flags = &flags;

#ifndef __KERNEL__
    check = pthread_create(
        &decide_what_to_with_packets_thread,
        NULL,
        ip_noise_decide_what_to_do_with_packets_thread_func,
        (void *)arbitrator_context
        );

    if (check != 0)
    {
        fprintf(stderr, "Could not create the arbitrator thread!\n");
        exit(-1);
    }
#else
    packet_logic = ip_noise_arbitrator_packet_logic_alloc(data_ptr, &flags);

    return packet_logic;
    
#endif  

#ifndef __KERNEL__
    do
    {
        status = ipq_read(h, message, sizeof(message), 0);
        if (status < 0)
        {
            /* die(h); */
        }
        switch(ipq_message_type(message))
        {
            case NLMSG_ERROR:
                fprintf(
                    stderr, 
                    "Received error message %d\n",
                    ipq_get_msgerr(message)
                    );
                break;

            case IPQM_PACKET:
            {
                ip_noise_message_t * msg_with_time;
                struct timezone tz;

#if 0
                static int num = 0;
#endif

                msg_with_time = malloc(sizeof(ip_noise_message_t));

                /* We are copying the entire buffer, because otherwise we get
                 * errors, since ipq_get_packet still relies on this buffer
                 * for reference */

                memcpy(msg_with_time->message, message, sizeof(msg_with_time->message));

                msg_with_time->m = ipq_get_packet(msg_with_time->message);
                
                gettimeofday(&(msg_with_time->tv), &tz);
                
                
                ip_noise_messages_queue_enqueue(
                    packets_to_arbitrate_queue,
                    msg_with_time
                    );

#if 0
                printf("Received a message! (%i)\n", num++);
#endif

#if 0
                status = ipq_set_verdict(h, m->packet_id, NF_ACCEPT, 0, NULL);

                if (status < 0)
                {
                    die(h);
                }
#endif
                break;
            }

            default:
                fprintf(stderr, "Unknown message type!\n");
                break;                        
        }
    } while (1);
#endif

    ipq_destroy_handle(h);

    return 0;
}
