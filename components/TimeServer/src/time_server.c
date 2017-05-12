/*
 * Copyright 2014, NICTA
 *
 * This software may be distributed and modified according to the terms of
 * the GNU General Public License version 2. Note that NO WARRANTY is provided.
 * See "LICENSE_GPLv2.txt" for details.
 *
 * @TAG(NICTA_GPL)
 */

#include <autoconf.h>

#include <assert.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdio.h>
#include <sel4/sel4.h>
#include <sel4/arch/constants.h>
#include <camkes.h>
#include <platsupport/plat/pit.h>
#include <platsupport/arch/tsc.h>
#include <utils/util.h>
#include <sel4utils/sel4_zf_logif.h>

/* Prototype for this function is not generated by the camkes templates yet */
seL4_Word the_timer_get_sender_id();

/* Frequency of timer interrupts that we use for processing timeouts */
#define TIMER_FREQUENCY 500

static pstimer_t *timer = NULL;

#define TIMER_TYPE_OFF 0
#define TIMER_TYPE_PERIODIC 1
#define TIMER_TYPE_ABSOLUTE 2
#define TIMER_TYPE_RELATIVE 3

typedef struct client_timer {
    int id;
    int client_id;
    int timer_type;
    uint64_t periodic_ns;
    uint64_t timeout_time;
    struct client_timer *prev, *next;
} client_timer_t;

typedef struct client_state {
    int id;
    uint32_t completed;
    client_timer_t *timers;
} client_state_t;

/* sorted list of active timers */
static client_timer_t *timer_head = NULL;

/* declare the memory needed for the clients */
static client_state_t *client_state = NULL;

static uint64_t tsc_frequency = 0;

void the_timer_emit(unsigned int);
int the_timer_largest_badge(void);

static uint64_t current_time_ns() {
    return muldivu64(rdtsc_pure(), NS_IN_S, tsc_frequency);
}

static void remove_timer(client_timer_t *timer) {
    if (timer->prev) {
        timer->prev->next = timer->next;
    } else {
        assert(timer == timer_head);
        timer_head = timer->next;
    }
    if (timer->next) {
        timer->next->prev = timer->prev;
    }
}

static void insert_timer(client_timer_t *timer) {
    client_timer_t *current, *next;
    for (current = NULL, next = timer_head; next && next->timeout_time < timer->timeout_time; current = next, next = next->next);
    timer->prev = current;
    timer->next = next;
    if (next) {
        next->prev = timer;
    }
    if (current) {
        current->next = timer;
    } else {
        timer_head = timer;
    }
}

static void signal_client(client_timer_t *timer, uint64_t current_time) {
    the_timer_emit(timer->client_id + 1);
    client_state[timer->client_id].completed |= BIT(timer->id);
    remove_timer(timer);
    switch(timer->timer_type) {
    case TIMER_TYPE_OFF:
        assert(!"not possible");
        break;
    case TIMER_TYPE_PERIODIC:
        timer->timeout_time += timer->periodic_ns;
        insert_timer(timer);
        break;
    case TIMER_TYPE_ABSOLUTE:
    case TIMER_TYPE_RELATIVE:
        timer->timer_type = TIMER_TYPE_OFF;
        break;
    }
}

static void signal_clients(uint64_t current_time) {
    while(timer_head && timer_head->timeout_time <= current_time) {
        signal_client(timer_head, current_time);
    }
}

void irq_handle() {
    int error = time_server_lock();
    ZF_LOGF_IF(error, "Failed to lock time server");

    signal_clients(current_time_ns());
    timer_handle_irq(timer, 0);
    error = irq_acknowledge();
    ZF_LOGF_IF(error, "irq acknowledge failed");
    error = time_server_unlock();
    ZF_LOGF_IF(error, "Failed to unlock time server");
}

static int _oneshot_relative(int cid, int tid, uint64_t ns) {
    if (tid >= timers_per_client || tid < 0) {
        ZF_LOGE("invalid tid, 0 >= %d >= %d\n", tid, timers_per_client);
        return -1;
    }

    int error = time_server_lock();
    ZF_LOGF_IF(error, "Failed to lock time server");

    client_timer_t *t = &client_state[cid].timers[tid];
    if (t->timer_type != TIMER_TYPE_OFF) {
        remove_timer(t);
    }
    t->timer_type = TIMER_TYPE_RELATIVE;
    t->timeout_time = current_time_ns() + ns;
    insert_timer(t);
    error = time_server_unlock();
    ZF_LOGF_IF(error, "Failed to unlock time server");
    return 0;
}

static int _oneshot_absolute(int cid, int tid, uint64_t ns) {
    if (tid >= timers_per_client || tid < 0) {
        ZF_LOGE("invalid tid, 0 >= %d >= %d\n", tid, timers_per_client);
        return -1;
    }

    int error = time_server_lock();
    ZF_LOGF_IF(error, "Failed to lock time server");

    client_timer_t *t = &client_state[cid].timers[tid];
    if (t->timer_type != TIMER_TYPE_OFF) {
        remove_timer(t);
    }
    t->timer_type = TIMER_TYPE_ABSOLUTE;
    t->timeout_time = ns;
    insert_timer(t);
    error = time_server_unlock();
    ZF_LOGF_IF(error, "Failed to unlock time server");
    return 0;
}

static int _periodic(int cid, int tid, uint64_t ns) {
    if (tid >= timers_per_client || tid < 0) {
        ZF_LOGE("invalid tid, 0 >= %d >= %d\n", tid, timers_per_client);
        return -1;
    }

    int error = time_server_lock();
    ZF_LOGF_IF(error, "Failed to lock time server");

    client_timer_t *t = &client_state[cid].timers[tid];
    if (t->timer_type != TIMER_TYPE_OFF) {
        remove_timer(t);
    }
    t->timer_type = TIMER_TYPE_PERIODIC;
    t->periodic_ns = ns;
    t->timeout_time = current_time_ns() + ns;
    insert_timer(t);
    error = time_server_unlock();
    ZF_LOGF_IF(error, "Failed to unlock time server");
    return 0;
}

static int _stop(int cid, int tid) {
    if (tid >= timers_per_client || tid < 0) {
        ZF_LOGE("invalid tid, 0 >= %d >= %d\n", tid, timers_per_client);
        return -1;
    }
    int error = time_server_lock();
    ZF_LOGF_IF(error, "Failed to lock time server");

    client_timer_t *t = &client_state[cid].timers[tid];
    if (t->timer_type != TIMER_TYPE_OFF) {
        remove_timer(t);
        t->timer_type = TIMER_TYPE_OFF;
    }
    error = time_server_unlock();
    ZF_LOGF_IF(error, "Failed to unlock time server");
    return 0;
}

static unsigned int _completed(int cid) {
    int error = time_server_lock();
    ZF_LOGF_IF(error, "Failed to lock time server");

    unsigned int ret = client_state[cid].completed;
    client_state[cid].completed = 0;
    error = time_server_unlock();
    ZF_LOGF_IF(error, "Failed to unlock time server");

    return ret;
}

static uint64_t _time(int cid) {
    return current_time_ns();
}

/* substract 1 from the badge as we started counting badges at 1
 * to avoid using the 0 badge */
int the_timer_oneshot_relative(int id, uint64_t ns) {
    return _oneshot_relative(the_timer_get_sender_id() - 1, id, ns);
}

int the_timer_oneshot_absolute(int id, uint64_t ns) {
    return _oneshot_absolute(the_timer_get_sender_id() - 1, id, ns);
}

int the_timer_periodic(int id, uint64_t ns) {
    return _periodic(the_timer_get_sender_id() - 1, id, ns);
}

int the_timer_stop(int id) {
    return _stop(the_timer_get_sender_id() - 1, id);
}

unsigned int the_timer_completed() {
    return _completed(the_timer_get_sender_id() - 1);
}

uint64_t the_timer_time() {
    return _time(the_timer_get_sender_id() - 1);
}

uint64_t the_timer_tsc_frequency() {
    return tsc_frequency;
}

static int pit_port_in(void *cookie, uint32_t port, int io_size, uint32_t *result) {
    if (io_size != 1) {
        return -1;
    }
    switch(port) {
    case 0x43:
        *result = pit_command_in8(port);
        return 0;
    case 0x40:
        *result = pit_channel0_in8(port);
        return 0;
    default:
        return -1;
    }
}

static int pit_port_out(void *cookie, uint32_t port, int io_size, uint32_t val) {
    if (io_size != 1) {
        return -1;
    }
    switch(port) {
    case 0x43:
        pit_command_out8(port, val);
        return 0;
    case 0x40:
        pit_channel0_out8(port, val);
        return 0;
    default:
        return -1;
    }
}

void post_init() {
    int error = time_server_lock();
    ZF_LOGF_IF(error, "Failed to lock timer server");

    client_state = calloc(1, sizeof(client_state_t) * the_timer_largest_badge());
    ZF_LOGF_IF(client_state == NULL, "calloc failed");

    for (int i = 0; i < the_timer_largest_badge(); i++) {
        client_state[i].id = i;
        client_state[i].timers = calloc(timers_per_client, sizeof(client_timer_t));
        ZF_LOGF_IF(client_state[i].timers == NULL, "calloc failed");

        for (int j = 0; j < timers_per_client; j++) {
            client_state[i].timers[j].id = j;
            client_state[i].timers[j].client_id = i;
            client_state[i].timers[j].timer_type = TIMER_TYPE_OFF;
        }
    }
    ps_io_port_ops_t ops = (ps_io_port_ops_t){.io_port_in_fn = pit_port_in, .io_port_out_fn = pit_port_out};
    timer = pit_get_timer(&ops);
    ZF_LOGF_IF(timer == NULL, "Failed to get timer");

    tsc_frequency = tsc_calculate_frequency(timer);
    ZF_LOGF_IF(tsc_frequency == 0, "Failed to calculate tsc freq");

    error = irq_acknowledge();
    ZF_LOGF_IF(error, "Failed to ack irq");

    /* start timer */
    timer_start(timer);
    timer_periodic(timer, NS_IN_S / TIMER_FREQUENCY);
    error = time_server_unlock();
    ZF_LOGF_IF(error, "Failed to unlock timer server");

    set_putchar(putchar_putchar);
}
