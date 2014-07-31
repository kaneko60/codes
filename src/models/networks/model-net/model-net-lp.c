/*
 * Copyright (C) 2014 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#include <stddef.h>
#include <assert.h>
#include "codes/model-net.h"
#include "codes/model-net-method.h"
#include "codes/model-net-lp.h"
#include "codes/model-net-sched.h"
#include "codes/codes_mapping.h"
#include "codes/jenkins-hash.h"

#define MN_NAME "model_net_base"

/**** BEGIN SIMULATION DATA STRUCTURES ****/

int model_net_base_magic;

// message-type specific offsets - don't want to get bitten later by alignment
// issues...
static int msg_offsets[MAX_NETS];

typedef struct model_net_base_params {
    enum sched_type stype;
    uint64_t packet_size;
} model_net_base_params;

/* annotation-specific parameters (unannotated entry occurs at the 
 * last index) */
static int                       num_params = 0;
static const char              * annos[CONFIGURATION_MAX_ANNOS];
static model_net_base_params     all_params[CONFIGURATION_MAX_ANNOS];

typedef struct model_net_base_state {
    int net_id;
    // whether scheduler loop is running
    int in_sched_loop;
    // model-net scheduler
    model_net_sched *sched;
    // parameters
    const model_net_base_params * params;
    // lp type and state of underlying model net method - cache here so we
    // don't have to constantly look up
    const tw_lptype *sub_type;
    void *sub_state;
} model_net_base_state;


/**** END SIMULATION DATA STRUCTURES ****/

/**** BEGIN LP, EVENT PROCESSING FUNCTION DECLS ****/

/* ROSS LP processing functions */  
static void model_net_base_lp_init(
        model_net_base_state * ns,
        tw_lp * lp);
static void model_net_base_event(
        model_net_base_state * ns,
        tw_bf * b,
        model_net_wrap_msg * m,
        tw_lp * lp);
static void model_net_base_event_rc(
        model_net_base_state * ns,
        tw_bf * b,
        model_net_wrap_msg * m,
        tw_lp * lp);
static void model_net_base_finalize(
        model_net_base_state * ns,
        tw_lp * lp);

/* event type handlers */
static void handle_new_msg(
        model_net_base_state * ns,
        tw_bf *b,
        model_net_wrap_msg * m,
        tw_lp * lp);
static void handle_sched_next(
        model_net_base_state * ns,
        tw_bf *b,
        model_net_wrap_msg * m,
        tw_lp * lp);
static void handle_new_msg_rc(
        model_net_base_state * ns,
        tw_bf *b,
        model_net_wrap_msg * m,
        tw_lp * lp);
static void handle_sched_next_rc(
        model_net_base_state * ns,
        tw_bf *b,
        model_net_wrap_msg * m,
        tw_lp * lp);

/* ROSS function pointer table for this LP */
tw_lptype model_net_base_lp = {
     (init_f) model_net_base_lp_init,
     (event_f) model_net_base_event,
     (revent_f) model_net_base_event_rc,
     (final_f)  model_net_base_finalize, 
     (map_f) codes_mapping,
     sizeof(model_net_base_state),
};

/**** END LP, EVENT PROCESSING FUNCTION DECLS ****/

/**** BEGIN IMPLEMENTATIONS ****/

void model_net_base_register(int *do_config_nets){
    // here, we initialize ALL lp types to use the base type
    for (int i = 0; i < MAX_NETS; i++){
        if (do_config_nets[i]){
            lp_type_register(model_net_lp_config_names[i], &model_net_base_lp);
            // HACK: unfortunately, we need to special-case dragonfly 
            // registration at the moment - there are two LPs, and
            // previously the LP matched on configuration initialized
            // the "router" LP. Now that the base LP is in charge of
            // registration, we need to take care of it
            // TODO: fix the interface to have underlying networks do
            // the registration
            if (i==DRAGONFLY){
                lp_type_register("dragonfly_router",
                        &method_array[DRAGONFLY]->mn_get_lp_type()[1]);
            }
        }
    }
}

static void base_read_config(const char * anno, model_net_base_params *p){
    char sched[MAX_NAME_LENGTH];
    long int packet_size_l = 0;
    uint64_t packet_size;
    int ret;

    // TODO: make this annotation-specific - put in the config loop, make part
    // of model-net base
    ret = configuration_get_value(&config, "PARAMS", "modelnet_scheduler",
            anno, sched, MAX_NAME_LENGTH);
    configuration_get_value_longint(&config, "PARAMS", "packet_size", anno,
            &packet_size_l);
    packet_size = packet_size_l;

    if (ret > 0){
        int i;
        for (i = 0; i < MAX_SCHEDS; i++){
            if (strcmp(sched_names[i], sched) == 0){
                p->stype = i;
                break;
            }
        }
        if (i == MAX_SCHEDS){
            tw_error(TW_LOC,"Unknown value for PARAMS:modelnet-scheduler : "
                    "%s\n", sched); 
        }
    }
    else{
        // default: FCFS
        p->stype = MN_SCHED_FCFS;
    }

    if (p->stype == MN_SCHED_FCFS_FULL){
        // override packet size to something huge (leave a bit in the unlikely
        // case that an op using packet size causes overflow)
        packet_size = 1ull << 62;
    }
    else if (!packet_size && p->stype != MN_SCHED_FCFS_FULL)
    {
        packet_size = 512;
        fprintf(stderr, "Warning, no packet size specified, setting packet size to %llu\n", packet_size);
    }

    p->packet_size = packet_size;
}

void model_net_base_configure(){
    uint32_t h1=0, h2=0;

    bj_hashlittle2(MN_NAME, strlen(MN_NAME), &h1, &h2);
    model_net_base_magic = h1+h2;

    // set up offsets - doesn't matter if they are actually used or not
    msg_offsets[SIMPLENET] =
        offsetof(model_net_wrap_msg, msg.m_snet);
    msg_offsets[SIMPLEWAN] =
        offsetof(model_net_wrap_msg, msg.m_swan);
    msg_offsets[TORUS] =
        offsetof(model_net_wrap_msg, msg.m_torus);
    msg_offsets[DRAGONFLY] =
        offsetof(model_net_wrap_msg, msg.m_dfly);
    msg_offsets[LOGGP] =
        offsetof(model_net_wrap_msg, msg.m_loggp);

    // perform the configuration(s)
    // This part is tricky, as we basically have to look up all annotations that
    // have LP names of the form modelnet_*. For each of those, we need to read
    // the base parameters
    // - the init is a little easier as we can use the LP-id to look up the
    // annotation

    // first grab all of the annotations and store locally
    for (int c = 0; c < lpconf.lpannos_count; c++){
        const config_anno_map_t *amap = &lpconf.lpannos[c];
        if (strncmp("modelnet_", amap->lp_name, 9) == 0){
            for (int n = 0; n < amap->num_annos; n++){
                int a;
                for (a = 0; a < num_params; a++){
                    if (annos[a] != NULL &&
                            strcmp(amap->annotations[n], annos[a]) == 0){
                        break;
                    }
                }
                if (a == num_params){
                    // found a new annotation
                    annos[num_params++] = amap->annotations[n];
                }
            }
            if (amap->has_unanno_lp){
                int a;
                for (a = 0; a < num_params; a++){
                    if (annos[a] == NULL)
                        break;
                }
                if (a == num_params){
                    // found a new (empty) annotation
                    annos[num_params++] = NULL;
                }
            }
        }
    }

    // now that we have all of the annos for all of the networks, loop through
    // and read the configs
    for (int i = 0; i < num_params; i++){
        base_read_config(annos[i], &all_params[i]);
    }
}

void model_net_base_lp_init(
        model_net_base_state * ns,
        tw_lp * lp){
    // obtain the underlying lp type through codes-mapping
    char lp_type_name[MAX_NAME_LENGTH], anno[MAX_NAME_LENGTH];
    int dummy;

    codes_mapping_get_lp_info(lp->gid, NULL, &dummy, 
            lp_type_name, &dummy, anno, &dummy, &dummy);

    // get annotation-specific parameters
    for (int i = 0; i < num_params; i++){
        if ((anno[0]=='\0' && annos[i] == NULL) ||
                strcmp(anno, annos[i]) == 0){
            ns->params = &all_params[i];
            break;
        }
    }

    // find the corresponding method name / index
    for (int i = 0; i < MAX_NETS; i++){
        if (strcmp(model_net_lp_config_names[i], lp_type_name) == 0){
            ns->net_id = i;
            break;
        }
    }

    // TODO: parameterize scheduler type
    ns->sched = malloc(sizeof(model_net_sched));
    model_net_sched_init(ns->params->stype, method_array[ns->net_id],
            ns->sched);

    ns->sub_type = model_net_get_lp_type(ns->net_id);
    // NOTE: some models actually expect LP state to be 0 initialized...
    // *cough anything that uses mn_stats_array cough*
    ns->sub_state = calloc(1, ns->sub_type->state_sz);

    // initialize the model-net method
    ns->sub_type->init(ns->sub_state, lp);
}

void model_net_base_event(
        model_net_base_state * ns,
        tw_bf * b,
        model_net_wrap_msg * m,
        tw_lp * lp){
    assert(m->magic == model_net_base_magic);
    
    switch (m->event_type){
        case MN_BASE_NEW_MSG:
            handle_new_msg(ns, b, m, lp);
            break;
        case MN_BASE_SCHED_NEXT:
            handle_sched_next(ns, b, m, lp);
            break;
        case MN_BASE_PASS: ;
            void * sub_msg = ((char*)m)+msg_offsets[ns->net_id];
            ns->sub_type->event(ns->sub_state, b, sub_msg, lp);
            break;
        /* ... */
        default:
            assert(!"model_net_base event type not known");
            break;
    }
}

void model_net_base_event_rc(
        model_net_base_state * ns,
        tw_bf * b,
        model_net_wrap_msg * m,
        tw_lp * lp){
    assert(m->magic == model_net_base_magic);
    
    switch (m->event_type){
        case MN_BASE_NEW_MSG:
            handle_new_msg_rc(ns, b, m, lp);
            break;
        case MN_BASE_SCHED_NEXT:
            handle_sched_next_rc(ns, b, m, lp);
            break;
        case MN_BASE_PASS: ;
            void * sub_msg = ((char*)m)+msg_offsets[ns->net_id];
            ns->sub_type->revent(ns->sub_state, b, sub_msg, lp);
            break;
        /* ... */
        default:
            assert(!"model_net_base event type not known");
            break;
    }

    *(int*)b = 0;
}

void model_net_base_finalize(
        model_net_base_state * ns,
        tw_lp * lp){
    ns->sub_type->final(ns->sub_state, lp);
    free(ns->sub_state);
}

/// bitfields used:
/// c0 - we initiated a sched_next event
void handle_new_msg(
        model_net_base_state * ns,
        tw_bf *b,
        model_net_wrap_msg * m,
        tw_lp * lp){
    // simply pass down to the scheduler
    model_net_request *r = &m->msg.m_base.u.req;
    // don't forget to set packet size, now that we're responsible for it!
    r->packet_size = ns->params->packet_size;
    void * m_data = m+1;
    void *remote = NULL, *local = NULL;
    if (r->remote_event_size > 0){
        remote = m_data;
        m_data = (char*)m_data + r->remote_event_size;
    }
    if (r->self_event_size > 0){
        local = m_data;
    }
    
    model_net_sched_add(r, r->remote_event_size, remote, r->self_event_size,
            local, ns->sched, &m->msg.m_base.rc, lp);
    
    if (ns->in_sched_loop == 0){
        b->c0 = 1;
        tw_event *e = codes_event_new(lp->gid, codes_local_latency(lp), lp);
        model_net_wrap_msg *m = tw_event_data(e);
        m->event_type = MN_BASE_SCHED_NEXT;
        m->magic = model_net_base_magic;
        // m_base not used in sched event
        tw_event_send(e);
        ns->in_sched_loop = 1;
    }
}

void handle_new_msg_rc(
        model_net_base_state *ns,
        tw_bf *b,
        model_net_wrap_msg *m,
        tw_lp *lp){
    model_net_sched_add_rc(ns->sched, &m->msg.m_base.rc, lp);
    if (b->c0){
        codes_local_latency_reverse(lp);
        ns->in_sched_loop = 0;
    }
}

/// bitfields used
/// c0 - scheduler loop is finished
void handle_sched_next(
        model_net_base_state * ns,
        tw_bf *b,
        model_net_wrap_msg * m,
        tw_lp * lp){
    tw_stime poffset;
    int ret = model_net_sched_next(&poffset, ns->sched, m+1, 
            &m->msg.m_base.rc, lp);
    // we only need to know whether scheduling is finished or not - if not,
    // go to the 'next iteration' of the loop
    if (ret == -1){
        b->c0 = 1;
        ns->in_sched_loop = 0;
    }
    else {
        tw_event *e = codes_event_new(lp->gid, 
                poffset+codes_local_latency(lp), lp);
        model_net_wrap_msg *m = tw_event_data(e);
        m->event_type = MN_BASE_SCHED_NEXT;
        m->magic = model_net_base_magic;
        // no need to set m_base here
        tw_event_send(e);
    }
}

void handle_sched_next_rc(
        model_net_base_state * ns,
        tw_bf *b,
        model_net_wrap_msg * m,
        tw_lp * lp){
    model_net_sched_next_rc(ns->sched, m+1, &m->msg.m_base.rc, lp);

    if (b->c0){
        ns->in_sched_loop = 1;
    }
    else{
        codes_local_latency_reverse(lp);
    }
}

/**** END IMPLEMENTATIONS ****/

tw_event * model_net_method_event_new(
        tw_lpid dest_gid,
        tw_stime offset_ts,
        tw_lp *sender,
        int net_id,
        void **msg_data,
        void **extra_data){
    tw_event *e = tw_event_new(dest_gid, offset_ts, sender);
    model_net_wrap_msg *m_wrap = tw_event_data(e);
    m_wrap->event_type = MN_BASE_PASS;
    m_wrap->magic = model_net_base_magic;
    *msg_data = ((char*)m_wrap)+msg_offsets[net_id];
    // extra_data is optional
    if (extra_data != NULL){
        *extra_data = m_wrap + 1;
    }
    return e;
}

void * model_net_method_get_edata(int net_id, void *msg){
    return (char*)msg + sizeof(model_net_wrap_msg) - msg_offsets[net_id];
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
