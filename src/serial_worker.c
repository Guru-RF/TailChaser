/* SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS */
#include "serial_worker.h"
#include "log.h"
#include "config.h"   /* CFG_MAX_RADIOS */

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/eventfd.h>

#define SW_QUEUE_CAP 64

struct serial_worker {
    sa818_t        *dev[CFG_MAX_RADIOS];

    pthread_t       thread;
    bool            running;

    pthread_mutex_t lock;
    pthread_cond_t  cond;

    sw_cmd_t        cmdq[SW_QUEUE_CAP];
    int             cmd_head, cmd_tail;

    sw_result_t     resq[SW_QUEUE_CAP];
    int             res_head, res_tail;
    int             result_fd;       /* eventfd, written when a result lands */

    bool            stop;
};

serial_worker_t *serial_worker_new(void)
{
    serial_worker_t *sw = calloc(1, sizeof *sw);
    if (!sw)
        return NULL;
    pthread_mutex_init(&sw->lock, NULL);
    pthread_cond_init(&sw->cond, NULL);
    sw->result_fd = eventfd(0, EFD_NONBLOCK);
    if (sw->result_fd < 0) {
        log_error("serial_worker: eventfd: %s", strerror(errno));
        free(sw);
        return NULL;
    }
    return sw;
}

int serial_worker_attach(serial_worker_t *sw, uint8_t radio_idx, sa818_t *dev)
{
    if (radio_idx >= CFG_MAX_RADIOS)
        return -1;
    sw->dev[radio_idx] = dev;
    return 0;
}

int serial_worker_result_fd(const serial_worker_t *sw) { return sw->result_fd; }

static void push_result(serial_worker_t *sw, const sw_result_t *r)
{
    pthread_mutex_lock(&sw->lock);
    int next = (sw->res_head + 1) % SW_QUEUE_CAP;
    if (next != sw->res_tail) {
        sw->resq[sw->res_head] = *r;
        sw->res_head = next;
    } else {
        log_warn("serial_worker: result queue full, dropping");
    }
    pthread_mutex_unlock(&sw->lock);
    uint64_t one = 1;
    if (write(sw->result_fd, &one, sizeof one) < 0)
        log_debug("serial_worker: result_fd write: %s", strerror(errno));
}

static void process_cmd(serial_worker_t *sw, const sw_cmd_t *c)
{
    sw_result_t r = { .type = c->type, .radio_idx = c->radio_idx, .status = -1, .rssi = -1 };
    sa818_t *dev = (c->radio_idx < CFG_MAX_RADIOS) ? sw->dev[c->radio_idx] : NULL;
    if (!dev) {
        log_warn("serial_worker: command for missing radio %u", c->radio_idx);
        push_result(sw, &r);
        return;
    }

    switch (c->type) {
    case SW_SET_CHANNEL:
        r.status = sa818_set_group(dev, c->bw, c->tx_hz, c->rx_hz,
                                   c->ctcss_tx, c->ctcss_rx, c->squelch);
        break;
    case SW_SET_VOLUME:
        r.status = sa818_set_volume(dev, c->level);
        break;
    case SW_SET_FILTER:
        r.status = sa818_set_filter(dev, c->emphasis, c->highpass, c->lowpass);
        break;
    case SW_SET_TAIL:
        r.status = sa818_set_tail(dev, c->tail_open);
        break;
    case SW_RSSI: {
        int v = 0;
        r.status = sa818_rssi(dev, &v);
        if (r.status == 0) r.rssi = v;
        break;
    }
    }
    push_result(sw, &r);
}

static void *worker_main(void *arg)
{
    serial_worker_t *sw = arg;
    pthread_mutex_lock(&sw->lock);
    while (!sw->stop) {
        while (sw->cmd_head == sw->cmd_tail && !sw->stop)
            pthread_cond_wait(&sw->cond, &sw->lock);
        if (sw->stop)
            break;
        sw_cmd_t c = sw->cmdq[sw->cmd_tail];
        sw->cmd_tail = (sw->cmd_tail + 1) % SW_QUEUE_CAP;
        pthread_mutex_unlock(&sw->lock);

        process_cmd(sw, &c);

        pthread_mutex_lock(&sw->lock);
    }
    pthread_mutex_unlock(&sw->lock);
    return NULL;
}

int serial_worker_run(serial_worker_t *sw)
{
    if (pthread_create(&sw->thread, NULL, worker_main, sw) != 0) {
        log_error("serial_worker: pthread_create failed");
        return -1;
    }
    sw->running = true;
    return 0;
}

void serial_worker_stop(serial_worker_t *sw)
{
    if (!sw->running)
        return;
    pthread_mutex_lock(&sw->lock);
    sw->stop = true;
    pthread_cond_signal(&sw->cond);
    pthread_mutex_unlock(&sw->lock);
    pthread_join(sw->thread, NULL);
    sw->running = false;
}

int serial_worker_submit(serial_worker_t *sw, const sw_cmd_t *cmd)
{
    pthread_mutex_lock(&sw->lock);
    int next = (sw->cmd_head + 1) % SW_QUEUE_CAP;
    if (next == sw->cmd_tail) {
        pthread_mutex_unlock(&sw->lock);
        log_warn("serial_worker: command queue full, dropping %d", cmd->type);
        return -1;
    }
    sw->cmdq[sw->cmd_head] = *cmd;
    sw->cmd_head = next;
    pthread_cond_signal(&sw->cond);
    pthread_mutex_unlock(&sw->lock);
    return 0;
}

int serial_worker_get_result(serial_worker_t *sw, sw_result_t *out)
{
    /* Consume one eventfd token's worth of signal lazily: drain the counter
     * and rely on the queue as the source of truth. */
    uint64_t drain;
    while (read(sw->result_fd, &drain, sizeof drain) > 0) { /* clear */ }

    pthread_mutex_lock(&sw->lock);
    if (sw->res_head == sw->res_tail) {
        pthread_mutex_unlock(&sw->lock);
        return -1;
    }
    *out = sw->resq[sw->res_tail];
    sw->res_tail = (sw->res_tail + 1) % SW_QUEUE_CAP;
    pthread_mutex_unlock(&sw->lock);
    return 0;
}

void serial_worker_free(serial_worker_t *sw)
{
    if (!sw)
        return;
    serial_worker_stop(sw);
    for (int i = 0; i < CFG_MAX_RADIOS; i++)
        if (sw->dev[i]) sa818_close(sw->dev[i]);
    if (sw->result_fd >= 0)
        close(sw->result_fd);
    pthread_mutex_destroy(&sw->lock);
    pthread_cond_destroy(&sw->cond);
    free(sw);
}
