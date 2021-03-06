#ifndef __KERNEL__
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "ourrealloc.h"
#else
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/wrapper.h>

#include <asm/uaccess.h>
#include "k_stdlib.h"
#include "k_stdio.h"
#endif


#include "conn.h"
#include "iface.h"
#include "rwlock.h"

#include "read.h"
#include "fcs_dm.h"
#include "switcher.h"

#ifdef __KERNEL__
#define exit(error_value) 
#endif

enum IP_NOISE_RET_VALUE_T
{
    IP_NOISE_RET_VALUE_OK = 0,
    IP_NOISE_RET_VALUE_SOMETHING_WRONG = 1,
    IP_NOISE_RET_VALUE_UNKNOWN_OPCODE = 2,
    IP_NOISE_RET_VALUE_INDEX_OUT_OF_RANGE = 3,
    IP_NOISE_RET_VALUE_SPLIT_LINEAR_SET_POINTS_ON_OTHER_TYPE = 4,
    IP_NOISE_RET_VALUE_VALUE_OUT_OF_RANGE = 5,
};

ip_noise_arbitrator_data_t * ip_noise_arbitrator_data_alloc(void)
{
    ip_noise_arbitrator_data_t * data;

    data = malloc(sizeof(ip_noise_arbitrator_data_t));

    data->lock = ip_noise_rwlock_alloc();

    data->num_chains = 0;
    data->max_num_chains = 16;
    data->chains = malloc(sizeof(data->chains[0]) * data->max_num_chains);
    data->chain_names = ip_noise_str2int_dict_alloc();

    return data;
}

/* This is basically a linked list freeing routine */
static void ip_spec_free(ip_noise_ip_spec_t * spec)
{
    ip_noise_ip_spec_t * next_spec;

    while(spec != NULL)
    {
        if (spec->port_ranges != NULL)
        {
            free(spec->port_ranges);
        }
        next_spec = spec->next;
        free(spec);
        spec = next_spec;
    }
}

static ip_noise_ip_spec_t * ip_spec_duplicate(
        ip_noise_ip_spec_t * old
        )
{
    ip_noise_ip_spec_t * head, * tail, * prev_tail;

    /* An ugly code of duplicating a linked list */

    head = malloc(sizeof(ip_noise_ip_spec_t));
    tail = head;
    prev_tail = NULL;
    while (old != NULL)
    {
        memcpy(&(tail->ip), &(old->ip), sizeof(tail->ip));
        tail->net_mask = old->net_mask;
        tail->num_port_ranges = old->num_port_ranges;
        if (old->port_ranges != NULL)
        {
            tail->port_ranges = malloc(sizeof(tail->port_ranges[0]) * tail->num_port_ranges);
            memcpy(tail->port_ranges, old->port_ranges, sizeof(tail->port_ranges[0]) * tail->num_port_ranges);
        }
        old = old->next;
        tail->next = malloc(sizeof(ip_noise_ip_spec_t));
        prev_tail = tail;
        tail = tail->next; 
    }
    if (prev_tail == NULL)
    {
        head = NULL;
    }
    else
    {
        free(prev_tail->next);
        prev_tail->next = NULL;
    }

    return head;
}


static ip_noise_chain_filter_t * filter_duplicate(
        ip_noise_chain_filter_t * old
        )
{
    ip_noise_chain_filter_t * new;

    new = malloc(sizeof(ip_noise_chain_filter_t));

    new->source = ip_spec_duplicate(old->source);
    new->dest = ip_spec_duplicate(old->dest);
    memcpy(new->protocols, old->protocols, sizeof(new->protocols));
    memcpy(new->tos, old->tos, sizeof(new->tos));

    new->min_packet_len = old->min_packet_len;
    new->max_packet_len = old->max_packet_len;
    new->which_packet_len = old->which_packet_len;

    return new;
}

static ip_noise_state_t * state_duplicate(
        ip_noise_state_t * old
        )
{
    ip_noise_state_t * new;

    new = malloc(sizeof(ip_noise_state_t));

    memcpy(&(new->name), &(old->name), sizeof(new->name));
    new->drop_prob = old->drop_prob;
    new->delay_prob = old->delay_prob;
    new->delay_function.type = old->delay_function.type;
    switch (new->delay_function.type)
    {
        case IP_NOISE_DELAY_FUNCTION_EXP:
            new->delay_function.params.lambda = old->delay_function.params.lambda;
            break;

        case IP_NOISE_DELAY_FUNCTION_SPLIT_LINEAR:
            new->delay_function.params.split_linear.num_points = old->delay_function.params.split_linear.num_points;
            new->delay_function.params.split_linear.points = 
                malloc(
                    sizeof(new->delay_function.params.split_linear.points[0]) *
                    new->delay_function.params.split_linear.num_points
                    );
            memcpy(
                new->delay_function.params.split_linear.points,
                old->delay_function.params.split_linear.points,
                sizeof(new->delay_function.params.split_linear.points[0]) *
                new->delay_function.params.split_linear.num_points
                );
            break;

    }

    new->time_factor = old->time_factor;
    new->num_move_tos = old->num_move_tos;
    new->move_tos = malloc(sizeof(new->move_tos[0]) * new->num_move_tos);
    memcpy(new->move_tos, old->move_tos, sizeof(new->move_tos[0]) * new->num_move_tos);
    new->stable_delay_prob = old->stable_delay_prob;

    return new;
}

static ip_noise_chain_t * chain_duplicate(
        ip_noise_chain_t * old
        )
{
    ip_noise_chain_t * new;
    int a;

    new = malloc(sizeof(ip_noise_chain_t));

    new->num_states = old->num_states;
    new->max_num_states = old->max_num_states;

    new->states = malloc(new->max_num_states * sizeof(new->states[0]));

    for( a = 0 ; a < new->num_states ; a++)
    {
        new->states[a] = state_duplicate(old->states[a]);
    }

    new->current_state = old->current_state;
    new->filter = filter_duplicate(old->filter);

    new->last_packet_release_time = old->last_packet_release_time;

    new->state_names = ip_noise_str2int_dict_duplicate(old->state_names);

    return new;
}

static ip_noise_arbitrator_data_t *
    ip_noise_arbitrator_data_duplicate(
        ip_noise_arbitrator_data_t * old
        )
{
    ip_noise_arbitrator_data_t * new;
    int a;
    
    new = malloc(sizeof(ip_noise_arbitrator_data_t));
    new->max_num_chains = old->max_num_chains;
    new->num_chains = old->num_chains;
    new->chains = malloc(new->max_num_chains*sizeof(new->chains[0]));
    for(a = 0 ; a < new->num_chains ; a++)
    {
        new->chains[a] = chain_duplicate(old->chains[a]);
    }
    
    new->lock = old->lock;

    new->chain_names = ip_noise_str2int_dict_duplicate(old->chain_names);

    return new;
}

static void state_free(ip_noise_state_t * state)
{
    if (state->delay_function.type == IP_NOISE_DELAY_FUNCTION_SPLIT_LINEAR)
    {
        if(state->delay_function.params.split_linear.points != NULL)
        {
            free(state->delay_function.params.split_linear.points);
        }
    }
    free(state->move_tos);
    free(state);
}

static void chain_free(ip_noise_chain_t * chain)
{
    int a;

    for(a=0;a<chain->num_states;a++)
    {
        state_free(chain->states[a]);
    }
    
    ip_spec_free(chain->filter->source);
    ip_spec_free(chain->filter->dest);

    free(chain->filter);

    ip_noise_str2int_dict_free(chain->state_names);
    
    free(chain);
}


void ip_noise_arbitrator_data_clear_all(ip_noise_arbitrator_data_t * data)
{
    int a;

    for(a=0;a<data->num_chains;a++)
    {
        chain_free(data->chains[a]);
    }

    data->num_chains = 0;

    ip_noise_str2int_dict_reset(data->chain_names);    
}

#ifdef __KERNEL__
/*
 * This function cancels the timers that were assigned in order to switch
 * the active states of the chains.
 *
 * */
static void our_del_timers(ip_noise_arbitrator_data_t * data)
{
    int a;
    for(a=0;a<data->num_chains;a++)
    {
        del_timer(&(data->chains[a]->timer));
    }
}
#endif

void ip_noise_arbitrator_data_free(ip_noise_arbitrator_data_t * data)
{
#ifdef __KERNEL__
    our_del_timers(data);
#endif
    ip_noise_arbitrator_data_clear_all(data);
    ip_noise_str2int_dict_free(data->chain_names);
    free(data->chains);
    free(data);
}

#ifdef USE_TEXT_QUEUE_IN

static int ip_noise_read_proto(
    ip_noise_arbitrator_iface_t * self,
    char * buf,
    int len
    )
{
    int ret;
    pthread_mutex_lock(&(self->text_queue_in_mutex));
    ret = (ip_noise_text_queue_in_read_bytes(self->text_queue_in, (buf), (len)));    
    pthread_mutex_unlock(&(self->text_queue_in_mutex));
    
    return ret;
}

static void ip_noise_read_rollback_proto(ip_noise_arbitrator_iface_t * self)
{
    pthread_mutex_lock(&(self->text_queue_in_mutex));
    ip_noise_text_queue_in_rollback(self->text_queue_in);    
    pthread_mutex_unlock(&(self->text_queue_in_mutex));
  
}

static void ip_noise_read_commit_proto(ip_noise_arbitrator_iface_t * self)
{
    pthread_mutex_lock(&(self->text_queue_in_mutex));
    ip_noise_text_queue_in_commit(self->text_queue_in);
    printf("Commit!\n");
    pthread_mutex_unlock(&(self->text_queue_in_mutex));
}

#endif

#ifdef USE_TEXT_QUEUE_OUT
static void ip_noise_write_proto(ip_noise_arbitrator_iface_t * self,
    char * buf,
    int len)
{
    pthread_mutex_lock(&(self->text_queue_out_mutex));
    ip_noise_text_queue_out_input_bytes(self->text_queue_out, buf, len);
    pthread_mutex_unlock(&(self->text_queue_out_mutex));
}
#endif

static int read_int(
    ip_noise_arbitrator_iface_t * self
    )
{
    unsigned char buffer[4];
    int ok;
    int ret;
    
    ok = ip_noise_read(buffer, 4);
    if (ok < 0)
    {
        return ok;
    }

    ret = (       buffer[0] | 
            (((int)buffer[1]) << 8)  | 
            (((int)buffer[2]) << 16) | 
            (((int)buffer[3]) << 24) );
    if (ret < 0)
    {
        ret = 0;
    }
    return ret;
}

static int read_uint16(
    ip_noise_arbitrator_iface_t * self
    )
{
    unsigned char buffer[2];
    int ok;
    
    ok = ip_noise_read(buffer, 2);

    if (ok < 0)
    {
        return ok;
    }
    

    return (int)(                  buffer[0]          | 
            (((unsigned short)buffer[1]) << 8)
           );
}

#define read_opcode(self) (read_int(self))

typedef int param_type_t;

enum IP_NOISE_PARAM_TYPE_T
{
    PARAM_TYPE_STRING,
    PARAM_TYPE_INT,
    PARAM_TYPE_CHAIN,
    PARAM_TYPE_STATE,
    PARAM_TYPE_IP_FILTER,
    PARAM_TYPE_BOOL,
    PARAM_TYPE_WHICH_PACKET_LENGTH,
    PARAM_TYPE_PROB,
    PARAM_TYPE_DELAY_FUNCTION_TYPE,
    PARAM_TYPE_SPLIT_LINEAR_POINTS,
    PARAM_TYPE_LAMBDA,
    PARAM_TYPE_DELAY_TYPE,
    PARAM_TYPE_NONE,
};

union param_union
{
    ip_noise_id_t string;
    int _int;
    int chain;
    int state;
    ip_noise_ip_spec_t * ip_filter;
    int bool;
    int which_packet_length;
    ip_noise_prob_t prob;
    int delay_function_type;
    ip_noise_split_linear_function_t split_linear_points;
    int lambda;
    int delay_type;
};

typedef union param_union param_t;
struct operation_struct
{
    int opcode;
    int num_params;
    param_type_t params[4];
    int num_out_params;
    param_type_t out_params[4];
    int (*handler)(ip_noise_arbitrator_iface_t * self, param_t * params, param_t * out_params);
};

typedef struct operation_struct operation_t;

static ip_noise_chain_t * chain_alloc(char * name)
{
    ip_noise_chain_t * chain;

    chain = malloc(sizeof(ip_noise_chain_t));

    strncpy(chain->name, name, IP_NOISE_ID_LEN);
    chain->name[IP_NOISE_ID_LEN-1] = '\0';

    chain->states = NULL;
    chain->num_states = chain->max_num_states = 0;
    chain->filter = malloc(sizeof(ip_noise_chain_filter_t));
    chain->filter->source = NULL;
    chain->filter->dest = NULL;
    /* Accept all the protocols */
    memset(chain->filter->protocols, '\xFF', sizeof(chain->filter->protocols));
    /* Accept all the TOS numbers */
    memset(chain->filter->tos, '\xFF', sizeof(chain->filter->tos));
    chain->filter->min_packet_len = 0;
    chain->filter->max_packet_len = 65535;
    chain->filter->which_packet_len = IP_NOISE_WHICH_PACKET_LEN_DONT_CARE;

    chain->state_names = ip_noise_str2int_dict_alloc();

#ifndef __KERNEL__
    chain->last_packet_release_time.tv_sec = 0;
#else
    chain->last_packet_release_time = 0;
#endif

    return chain;
}

static ip_noise_state_t * state_alloc(char * name)
{
    ip_noise_state_t * state;

    state = malloc(sizeof(ip_noise_state_t));

    strncpy(state->name, name, IP_NOISE_ID_LEN);
    state->name[IP_NOISE_ID_LEN-1] = '\0';

    state->drop_prob = 0;
    state->delay_prob = 0;
    state->delay_function.type = IP_NOISE_DELAY_FUNCTION_NONE;
    state->time_factor = 1000;
    state->stable_delay_prob = 0;

    return state; 
}


static ip_noise_chain_t * get_chain(ip_noise_arbitrator_iface_t * self, int chain_index)
{
    ip_noise_arbitrator_data_t * data;

    data = self->data_copy;

    if (chain_index >= data->num_chains)
    {
        return NULL;
    }

    return data->chains[chain_index];
}

static ip_noise_state_t * get_state(ip_noise_arbitrator_iface_t * self, int chain_index, int state_index)
{
    ip_noise_chain_t * chain;

    chain = get_chain(self, chain_index);

    if (chain == NULL)
    {
        return NULL;
    }

    if (state_index >= chain->num_states)
    {
        return NULL;
    }

    return chain->states[state_index];
}

/* 
 * Read the specified parameter type from the line 
 * A common paradigm in this function is that the read functions return
 * a negative values. This indicates an error or an out of data notice,
 * and it causes the function to return the same thing 
 *
 * */
int read_param_type(
    ip_noise_arbitrator_iface_t * self,
    param_type_t param_type,
    param_t * ret
    )
{
    ip_noise_conn_t * conn;
    int ok;

    conn = self->conn;

    switch(param_type)
    {
        case PARAM_TYPE_STRING:
        {
            /* Read a buffer from the line */
            ok = ip_noise_read(ret->string, sizeof(ret->string));
            if (ok < 0)
            {
                return ok;
            }
            /* Make sure it ends with a null character */
            ret->string[IP_NOISE_ID_LEN-1] = '\0';
        }
        break;

        case PARAM_TYPE_INT:
        {
            ret->_int = read_int(self);
            if (ret->_int < 0)
            {
                return ret->_int;
            }
        }
        break;

        case PARAM_TYPE_CHAIN:
        {
            int which;

            which = read_int(self);
            
            /* Currently we only perform operations on the last chain 
             * that was used on the line */
            if (which == 2)
            {
                ret->chain = self->last_chain;
            }
            else if (which < 0)
            {
                return which;
            }
            else
            {
                printf("Uknown chain which %i!\n", which);
                exit(-1);
            }
        }
        break;

        case PARAM_TYPE_STATE:
        {
            int which;

            which = read_int(self);

            if (which == 0)
            {
                /* Indicates a specify state by index request.
                 * We read an extra int that would be the state ID 
                 * */
                int index = read_int(self);
                if (index < 0)
                {
                    return index;
                }

                ret->state = index;
            }
            else if (which == 2)
            {
                /* The last state chosen */
                ret->state = self->last_state;
            }
            else if (which < 0)
            {
                return which;
            }
            else
            {
                printf("Uknown state which %i!\n", which);
                exit(-1);
            }
            
        }
        break;

        case PARAM_TYPE_PROB:
        {
            double d;
            ok = ip_noise_read((char *)&d, sizeof(d));
            if (ok < 0)
            {
                return ok;
            }
            /* Sanity check - makes sure prob is in the range [0,1] */

            if ((d < 0) || (d > 1))
            {
                d = 0;
            }
            ret->prob = d;
        }
        break;

        case PARAM_TYPE_DELAY_TYPE:
        {
            int delay;

            delay = read_int(self);
            if (delay < 0)
            {
                return delay;
            }
            /* Assign the default delay */
            if (delay == 0)
            {
                delay = 1000;
            }
            ret->delay_type = delay;            
        }
        break;

        case PARAM_TYPE_DELAY_FUNCTION_TYPE:
        {
            int delay_type = read_int(self);

            if (delay_type < 0)
            {
                return delay_type;
            }

            if (delay_type == 0)
            {
                ret->delay_function_type = IP_NOISE_DELAY_FUNCTION_EXP;
            }
            else if (delay_type == 1)
            {
                ret->delay_function_type = IP_NOISE_DELAY_FUNCTION_SPLIT_LINEAR;
            }
            else 
            {
                ret->delay_function_type = IP_NOISE_DELAY_FUNCTION_NONE;
            }
        }
        break;

        case PARAM_TYPE_IP_FILTER:
        {
            /*
             *
             * This is an ugly code that constructs a linked list out
             * of the data that was sent on the line.
             * 
             * */
            ip_noise_ip_spec_t * head;
            ip_noise_ip_spec_t * tail;
            ip_noise_ip_spec_t * prev_tail;
            struct in_addr ip;
            struct in_addr terminator;
            int netmask;
            int num_port_ranges = 0;
            int max_num_port_ranges;
            ip_noise_port_range_t * port_ranges = NULL;
            int start, end;
            
            /* Allocate the initial element.
             * We keep writing data at tail, but when the function 
             * returns we use head to refer to the whole linked list.*/
            head = malloc(sizeof(ip_noise_ip_spec_t));
            head->port_ranges = NULL;
            tail = head;
            prev_tail = tail;
            tail->next = NULL;

            memset(&ip, '\x0', sizeof(ip));
            memset(&terminator, '\xFF', sizeof(terminator));

            while (memcmp(&ip, &terminator, sizeof(ip)) != 0)
            {
                ok = ip_noise_read((char*)&ip, 4);
                if (ok < 0)
                {
                    ip_spec_free(head);
                    return ok;
                }
                netmask = read_int(self);
                if (netmask < 0)
                {
                    ip_spec_free(head);
                    return netmask;
                }

                /* Read the port ranges. This is an array which is read
                 * one element by one, until a terminator is encounterd.
                 * */
                
                max_num_port_ranges = 16;
                port_ranges = malloc(sizeof(port_ranges[0])*max_num_port_ranges);
                num_port_ranges = 0;
                while (1)
                {
                    start = read_uint16(self);
                    if (start < 0)
                    {
                        free(port_ranges);
                        ip_spec_free(head);
                        return start;
                    }
                    end = read_uint16(self);
                    if (end < 0)
                    {
                        free(port_ranges);
                        ip_spec_free(head);
                        return end;
                    }
                            
                    if (start > end)
                    {
                        break;
                    }
                    /* If we reached the end of the array - expand it,
                     * so we will have more data */
                    if (num_port_ranges == max_num_port_ranges)
                    {
                        int new_max_num_port_ranges = max_num_port_ranges+16;
                        port_ranges = ourrealloc(port_ranges, sizeof(port_ranges[0])*max_num_port_ranges, sizeof(port_ranges[0])*new_max_num_port_ranges);
                        max_num_port_ranges = new_max_num_port_ranges;
                    }
                    port_ranges[num_port_ranges].start = (unsigned short)start;
                    port_ranges[num_port_ranges].end = (unsigned short)end;
                    num_port_ranges++;
                }
                /* Realloc port_ranges to have just enough memory to store all
                 * the port ranges */
                port_ranges = ourrealloc(port_ranges, sizeof(port_ranges[0])*max_num_port_ranges, sizeof(port_ranges[0])*num_port_ranges);
                if (memcmp(&ip, &terminator, sizeof(ip)) != 0)
                {
                    /* Advance to the next element. */
                    tail->ip = ip;
                    tail->net_mask = netmask;
                    tail->num_port_ranges = num_port_ranges;
                    tail->port_ranges = port_ranges;
                    tail->next = malloc(sizeof(ip_noise_ip_spec_t));
                    prev_tail = tail;
                    tail = tail->next;
                    tail->port_ranges = NULL;
                    tail->num_port_ranges = 0;
                    tail->next = NULL;
                }
            }
            free(prev_tail->next);
            prev_tail->next = NULL;
            if (num_port_ranges != 0)
            {
                free(port_ranges);
            }
            ret->ip_filter = head;
        }
        break;

        case PARAM_TYPE_SPLIT_LINEAR_POINTS:
        {
            double prob;
            int delay;
            
            int max_num_points;
            ip_noise_split_linear_function_t points;

            max_num_points = 16;
            points.points = malloc(sizeof(points.points[0])*max_num_points);
            points.num_points = 0;

            do 
            {
                param_t param;
                int ok;
                
                ok = read_param_type(self, PARAM_TYPE_PROB, &param);
                if (ok < 0)
                {
                    free(points.points);
                    return ok;
                }
                prob = param.prob;
                delay = read_int(self);
                if (delay < 0)
                {
                    free(points.points);
                    return delay;
                }

                points.points[points.num_points].prob = prob;
                points.points[points.num_points].delay = delay;

                points.num_points++;
                /* Check if we have exceeded the limit and if so - resize */
                if (points.num_points == max_num_points)
                {
                    int new_max_num_points = max_num_points + 16;
                    points.points = ourrealloc(points.points, sizeof(points.points[0])*max_num_points, sizeof(points.points[0])*new_max_num_points);
                    max_num_points = new_max_num_points;
                } 
            } while ((prob < 1));

            points.points = ourrealloc(points.points, sizeof(points.points[0])*max_num_points, sizeof(points.points[0])*points.num_points);

            ret->split_linear_points = points;
        }
        break;

        case PARAM_TYPE_BOOL:
        {
            int bool = read_int(self);
            if (bool < 0)
            {
                return bool;
            }
            ret->bool = (bool != 0);
        }
        break;

        case PARAM_TYPE_WHICH_PACKET_LENGTH:
        {
            int index = read_int(self);
            
            if (index < 0)
            {
                return index;
            }
            else if ((index > 4))
            {
                ret->which_packet_length = IP_NOISE_WHICH_PACKET_LEN_DONT_CARE;
            }
            else
            {
                ret->which_packet_length = index;
            }
        }
        break;

        case PARAM_TYPE_LAMBDA:
        {
            ret->lambda = read_int(self);
            if (ret->lambda < 0)
            {
                return ret->lambda;
            }
        }
        break;
    }

    return 0;
}

/*
 * This proecdure frees the param type in case it was not used.
 * */
static void free_param_type(
    ip_noise_arbitrator_iface_t * self,
    param_type_t param_type,
    param_t * param
    )
{
    switch (param_type)
    {
        case PARAM_TYPE_IP_FILTER:
        {
            ip_spec_free(param->ip_filter);
        }
        break;

        case PARAM_TYPE_SPLIT_LINEAR_POINTS:
        {
            free(param->split_linear_points.points);
        }
        break;
    }
}


#include "iface_handlers.c"

static int opcode_compare_w_context(const void * void_a, const void * void_b, void * context)
{
    operation_t * op_a = (operation_t *)void_a;
    operation_t * op_b = (operation_t *)void_b;

    int a = op_a->opcode;
    int b = op_b->opcode;

    if (a < b)
    {
        return -1;
    }
    else if (a > b)
    {
        return 1;
    }
    else
    {
        return 0;
    }    
}

static void write_int(
    ip_noise_arbitrator_iface_t * self,
    int retvalue    
    )
{
    unsigned char buffer[4];
    int a;
    for(a=0;a<4;a++)
    {
        buffer[a] = ((retvalue>>(a*8))&0xFF);
    }
    ip_noise_write(buffer, 4);
}


static void write_retvalue(
    ip_noise_arbitrator_iface_t * self,
    int retvalue    
    )
{
    write_int(self, retvalue);
}

static void write_param_type(
    ip_noise_arbitrator_iface_t * self,
    param_type_t param_type,
    param_t value
    )
{
    ip_noise_conn_t * conn;

    conn = self->conn;

    switch(param_type)
    {
        case PARAM_TYPE_INT:
        {
            write_int(self, value._int); 
        }
        break;

#if 0
        default:
        {
            printf("Unknown out param type: %i!\n", param_type);
            exit(-1);
        }
#endif
        break;
    }
}

#ifdef __KERNEL__
static ip_noise_arbitrator_iface_t * ip_noise_arb_iface;

int Major;

static ssize_t ip_noise_device_read(struct file * file,
        char * buffer,
        size_t length,
        loff_t * offset
        )
{
    ip_noise_arbitrator_iface_t * self;
    char * local_buffer;
    int how_many;

    self = ip_noise_arb_iface;

    local_buffer = malloc(length);

    memset(local_buffer, '\0', length);
    
    how_many = ip_noise_text_queue_out_write_bytes(self->text_queue_out, local_buffer, length);
    copy_to_user(buffer, local_buffer, how_many);

    free(local_buffer);

    return how_many;
}

static void ip_noise_arbitrator_iface_transact(
    ip_noise_arbitrator_iface_t * self
    );

static ssize_t ip_noise_device_write(struct file *file,
    const char *buffer,    /* The buffer */
    size_t length,   /* The length of the buffer */
    loff_t *offset)  /* Our offset in the file */
{
    ip_noise_arbitrator_iface_t * self;

    self = ip_noise_arb_iface;

    ip_noise_text_queue_in_put_bytes(self->text_queue_in, buffer, length);
    
    /* We received new input - so let's try to perform a transaction - the
     * worst case scenario is that we will rollback */
    ip_noise_arbitrator_iface_transact(self);

    return length;
}

static void ip_noise_arbitrator_iface_init_connection(
    ip_noise_arbitrator_iface_t * self
    );

static int ip_noise_device_open(struct inode *inode, 
                       struct file *file)
{
    ip_noise_arbitrator_iface_t * self;
    
    self = ip_noise_arb_iface;

    if (self->_continue == 1)
    {
        /* We are already open. */
        return -EBUSY;
    }

    ip_noise_arbitrator_iface_init_connection(self);  

    return 0;
}

static void close_connection(
    ip_noise_arbitrator_iface_t * self
    );

static int ip_noise_device_release(struct inode *inode, 
                          struct file *file)
{
    ip_noise_arbitrator_iface_t * self;
    
    self = ip_noise_arb_iface;

    self->_continue = 0;

    close_connection(self);

    return 0;
}
 

struct file_operations ip_noise_fops = {
    NULL,
    NULL, /* seek */
    ip_noise_device_read,
    ip_noise_device_write,
    NULL,
    NULL,
    NULL,
    NULL,
    ip_noise_device_open,
    NULL,
    ip_noise_device_release        
};
#endif
ip_noise_arbitrator_iface_t * ip_noise_arbitrator_iface_alloc(
    ip_noise_arbitrator_data_t * * data,
    void * switcher,
    ip_noise_flags_t * flags
    )
{
    ip_noise_arbitrator_iface_t * self;

    self = malloc(sizeof(ip_noise_arbitrator_iface_t));

    self->data = data;
    self->flags = flags;
    
    self->last_chain = -1;

    self->switcher = switcher;

#ifdef __KERNEL__
    Major = register_chrdev(0, "ip_noise_arb_iface", &ip_noise_fops);

    if (Major < 0)
    {
        printf("%s device failed with %d'n", "Sorrt, registering the character", Major);
        free(self);

        return NULL;
    }

    printf ("%s The major device number is %d.\n",
          "Registeration is a success.",
          Major);
    printf ("If you want to talk to the device driver,\n");
    printf ("you'll have to create a device file. \n");
    printf ("We suggest you use:\n");
    printf ("mknod <name> c %d <minor>\n", Major);
    printf ("You can try different minor numbers %s",
          "and see what happens.\n");

    ip_noise_arb_iface = self;
    
#endif

    self->_continue = 0;
    
    return self;
}

void ip_noise_arbitrator_iface_destroy(ip_noise_arbitrator_iface_t * self)
{
#ifdef __KERNEL__
    unregister_chrdev(Major, "ip_noise_arb_iface");
#endif

    free(self);
}

#ifndef __KERNEL__
#if defined(USE_TEXT_QUEUE_IN)||defined(USE_TEXT_QUEUE_OUT)
extern const pthread_mutex_t ip_noise_global_initial_mutex_constant;
#endif
#endif

#ifdef USE_TEXT_QUEUE_IN


#ifndef __KERNEL__
static void * poll_conn(void * void_iface)
{
    ip_noise_arbitrator_iface_t * self = (ip_noise_arbitrator_iface_t * )void_iface;
    char buffer[1];
    int ret;
    
    while (1)
    {
        ret = ip_noise_conn_read(self->conn, buffer, 1);
        if (ret == -1)
        {
            pthread_mutex_lock(&(self->text_queue_in_mutex));
            ip_noise_text_queue_in_set_conn_closed(self->text_queue_in);
            pthread_mutex_unlock(&(self->text_queue_in_mutex));
            break;
        }
        pthread_mutex_lock(&(self->text_queue_in_mutex));
        ip_noise_text_queue_in_put_bytes(self->text_queue_in, buffer, 1);
        pthread_mutex_unlock(&(self->text_queue_in_mutex));
    }

    return NULL;
}
#endif
    
#endif

#ifdef USE_TEXT_QUEUE_OUT

#ifndef __KERNEL__
static void * write_poll_conn(void * void_iface)
{
    ip_noise_arbitrator_iface_t * self = (ip_noise_arbitrator_iface_t * )void_iface;
    char buffer[1];
    int ret;
    
    while (1)
    {
        pthread_mutex_lock(&(self->text_queue_in_mutex));
        ret = ip_noise_text_queue_out_write_bytes(self->text_queue_out, buffer, 1);
        pthread_mutex_unlock(&(self->text_queue_in_mutex));
        
        if (ret == 1)
        {
            ip_noise_conn_write(self->conn, buffer, 1);
        }
    }

    return NULL;
}
#endif
    
#endif

static void ip_noise_arbitrator_iface_init_connection(
    ip_noise_arbitrator_iface_t * self
    )
{
    ip_noise_rwlock_t * data_lock;

    
    data_lock = (*self->data)->lock;

    self->_continue = 1;
    
    printf("%s", "Trying to open a connection!\n");
#ifndef __KERNEL__
    self->conn = ip_noise_conn_open();
#endif

#ifdef USE_TEXT_QUEUE_IN
#ifndef __KERNEL__
    self->text_queue_in_mutex = ip_noise_global_initial_mutex_constant;
#endif
    pthread_mutex_init(&(self->text_queue_in_mutex), NULL);
    self->text_queue_in = ip_noise_text_queue_in_alloc();

#ifndef __KERNEL__
    {
        pthread_t thread;
        pthread_create(&thread, NULL, poll_conn, self);
    }
#endif
#endif
#ifdef USE_TEXT_QUEUE_OUT
#ifndef __KERNEL__
    self->text_queue_out_mutex = ip_noise_global_initial_mutex_constant;
#endif
    pthread_mutex_init(&(self->text_queue_out_mutex), NULL);
    self->text_queue_out = ip_noise_text_queue_out_alloc();

#ifndef __KERNEL__
    pthread_create(&(self->write_poll_conn_thread), NULL, write_poll_conn, self);
#endif
#endif
    

    /* Make a copy of the data */

    ip_noise_rwlock_down_read(data_lock);
    self->data_copy = ip_noise_arbitrator_data_duplicate(*(self->data));
    ip_noise_rwlock_up_read(data_lock);    
}

static void close_connection(
    ip_noise_arbitrator_iface_t * self
    )
{
    ip_noise_flags_t * flags;
    ip_noise_rwlock_t * data_lock;
    
    flags = self->flags;
    data_lock = (*self->data)->lock;
    
#ifdef USE_TEXT_QUEUE_OUT
#ifndef __KERNEL__
    pthread_cancel(self->write_poll_conn_thread);
#endif
#endif
    
    flags->reinit_switcher = 1;

    ip_noise_rwlock_down_write(data_lock);
    ip_noise_arbitrator_data_free(*(self->data));
    *(self->data) = self->data_copy;
#ifdef __KERNEL__
    ip_noise_arbitrator_switcher_reinit(self->switcher);
#endif
    ip_noise_rwlock_up_write(data_lock);
#if 0
    /* Release the data for others to use */
    ip_noise_rwlock_up_write(data_lock);
#endif
    
#ifndef __KERNEL__
    printf("%s", "IFace: Closing a connection!\n");
    
    ip_noise_conn_destroy(self->conn);    
#endif

}
        

static void ip_noise_arbitrator_iface_transact(
    ip_noise_arbitrator_iface_t * self
    )
{
    int opcode;
    operation_t * record, opcode_record;
    int found;
    int a;
    int ret_code;
    int ok;
    param_t params[4];
    param_t out_params[4];

    

    
    /* Read the opcode of this transaction. This opcode uniquely 
     * identifies the transaction. Based on it we will know what to do
     * next */
    opcode = read_opcode(self);

    if (opcode == IP_NOISE_READ_CONN_TERM)
    {                
        self->_continue = 0;
        return;
    }
    else if (opcode == IP_NOISE_READ_NOT_FULLY)
    {
        /* We only received part of the opcode. Let's rollback the data
         * so we will read the whole thing next time we receive more data 
         * */
        ip_noise_read_rollback();
        return;
    }

    /* Search for the transaction's record that contains this opcode.
     * We need to process the rest of the transaction. */
    opcode_record.opcode = opcode;

    record = SFO_bsearch(
        &opcode_record, 
        operations, 
        NUM_OPERATIONS,
        sizeof(operation_t),
        opcode_compare_w_context,
        NULL,
        &found
        );

    if (found == 0)
    {
        printf("Unknown opcode 0x%x!\n", opcode);
        write_retvalue(self, 0x2);
        return;
    }
    
    /* Read the parameters from the line - one at a time*/
    for(a=0;a<record->num_params;a++)
    {
        ok = read_param_type(self, record->params[a], &params[a]);
        if (ok < 0)
        {
            int i;
            /* Free the previous parameters that were read from the line*/
            for(i=0;i<a;i++)
            {
                free_param_type(self, record->params[i], &params[i]);
            }
            
            if (ok == IP_NOISE_READ_CONN_TERM)
            {
                self->_continue = 0;
            }
            else if (ok == IP_NOISE_READ_NOT_FULLY)
            {
                /* Restore the input to the place it was before this
                 * transaction. This way, when new data arrives, we will
                 * still process the old data */
                ip_noise_read_rollback();
            }
            return;
        }                    
    }

    /* 
     * Call the handler of this transaction type to process the transaction
     * and do the actual work 
     * */
    ret_code = record->handler(self, params, out_params);

    if (ret_code < 0)
    {
        if (ret_code == IP_NOISE_READ_CONN_TERM)
        {
            self->_continue = 0;
        }
        else if (ret_code == IP_NOISE_READ_NOT_FULLY)
        {
            /* Restore the input to the place it was before this
             * transaction. This way, when new data arrives, we will
             * still process the old data */
            ip_noise_read_rollback();
        }
        return;
    }
    
    /* Flush the inputted data out of the buffer, so we will not process
     * it again. */
    ip_noise_read_commit();

    /* Write all the data that this transaction returns to the line. */
    write_retvalue(self, ret_code);

    for(a=0;a<record->num_out_params;a++)
    {
        write_param_type(
            self,
            record->out_params[a],
            out_params[a]
            );
    }
}

void ip_noise_arbitrator_iface_loop(
    ip_noise_arbitrator_iface_t * self 
    )
{
    ip_noise_flags_t * flags;
    ip_noise_rwlock_t * data_lock;

    flags = self->flags;
    data_lock = (*self->data)->lock;

    while (1)
    {
        ip_noise_arbitrator_iface_init_connection(self);

        while (self->_continue)
        {
            ip_noise_arbitrator_iface_transact(self);
        }

        close_connection(self);
    }
}
