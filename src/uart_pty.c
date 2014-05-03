/*
 * uart_pty.c
 * - based on uart_pty.c from simavr

	Copyright 2008, 2009 Michel Pollet <buserror@gmail.com>
    Copyright (c) 2014 Doug Goldstein <cardoe@cardoe.com>

    This file is part of drumfish.

	drumfish is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	drumfish is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with drumfish.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <sys/types.h>
#include <stdlib.h>
#include <poll.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif

#include <event2/event.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/util.h>

#include "uart_pty.h"
#include "avr_uart.h"
#include "sim_hex.h"

#include "df_log.h"

static struct event_base *dummy_base = NULL;

void uart_read_cb(struct bufferevent *bev, void *arg);
void uart_error_cb(struct bufferevent *bev, short events, void *arg);
void uart_connected_cb(evutil_socket_t sock, short events, void *arg);

#define TRACE(_w) _w
#ifndef TRACE
#define TRACE(_w)
#endif

#ifndef MAX
#define MAX(x, y) ((x) > (y) ? (x) : (y))
#endif


/*
 * called when a byte is send via the uart on the AVR
 */
static void
uart_pty_in_hook(
		struct avr_irq_t *irq,
		uint32_t value,
		void *param)
{
    (void)irq;

    uart_pty_t *p = (uart_pty_t *)param;
    df_log_msg(DF_LOG_DEBUG, "AVR UART%c -> out fifo (towards pty) %02x\n",
            p->uart, value);
    if (bufferevent_write(p->bev, (uint8_t *)&value, 1)) {
        df_log_msg(DF_LOG_ERR, "AVR UART%c -> out fifo failed to write\n",
                p->uart);
    }
}

/*
 * Called when the uart has room in it's input buffer. This is called repeateadly
 * if necessary, while the xoff is called only when the uart fifo is FULL
 */
static void
uart_pty_xon_hook(struct avr_irq_t *irq, uint32_t value, void *param)
{
    (void)irq;
    (void)value;

	uart_pty_t *p = (uart_pty_t*)param;

    if (!p->xon)
        df_log_msg(DF_LOG_INFO, "UART%c xon\n", p->uart);

    /* Re-enable reads from the TTY to result in writes to the MCU */
    p->xon = 1;
    bufferevent_enable(p->bev, EV_READ);

    if (p->peer_connected)
        uart_read_cb(p->bev, p);
}

/*
 * Called when the uart ran out of room in it's input buffer
 */
static void
uart_pty_xoff_hook(struct avr_irq_t *irq, uint32_t value, void *param)
{
    (void)irq;
    (void)value;

	uart_pty_t *p = (uart_pty_t*)param;

    if (p->xon)
        df_log_msg(DF_LOG_INFO, "UART%c xoff\n", p->uart);

    p->xon = 0;
    bufferevent_disable(p->bev, EV_READ);
}

void
uart_read_cb(struct bufferevent *bev, void *arg)
{
    int err;
    uint8_t buf[1]; /* specificially 1 byte here since a UART is byte by byte */
    struct evbuffer *input;
	uart_pty_t *p = (uart_pty_t *)arg;

    /* If we were previously unconnected, we are connected now since we got
     * a successful read. We want to stop our connection check timer from
     * firing.
     */
    if (!p->peer_connected) {
        event_del(p->timer);

        /* Mark this connection as connected */
        p->peer_connected = 1;
    }

    /* Get the right evbuffer for incoming data */
    input = bufferevent_get_input(bev);

    /* While xon is still set by the MCU, remove a byte and toss it at
     * the MCU. The reason we don't just disable the EV_READ event and use
     * this extra bit is so that if this event fired with 5 bytes but the
     * MCU only had room for 1. We'll process off the 1 byte and then
     * stop reading and leave the other bytes queued up.
     */
    while (p->xon && (err = evbuffer_remove(input, buf, sizeof(buf))) > 0) {
        df_log_msg(DF_LOG_DEBUG, "uart_pty_flush_incoming send %02x\n", buf[0]);
        avr_raise_irq(p->irq + IRQ_UART_PTY_BYTE_OUT, buf[0]);
    }
}

void
uart_error_cb(struct bufferevent *bev, short events, void *arg)
{
    uart_pty_t *p = (uart_pty_t *)arg;
    struct timeval tv;

    /* When no one is connected to our TTY, libevent returns back
     * BEV_EVENT_READING | BEV_EVENT_ERROR
     */
    if (events == (BEV_EVENT_READING | BEV_EVENT_ERROR)) {

        /* If we were previously connected, we need to requeue the
         * connection check timer.
         */
        if (p->peer_connected) {
            /* Schedule another epoll() on this socket to check for connection
             * in 4 seconds
             */
            tv.tv_sec = 4;
            tv.tv_usec = 0;

            if (event_add(p->timer, &tv)) {
                fprintf(stderr, "Failed to requeue timer object for UART%c\n",
                        p->uart);
                return;
            }

            /* Note that the peer is not connected */
            p->peer_connected = 0;
        }

        /* move the socket into a dummy base so that it's not part of
         * our epoll() check (done by libevent) which would cause an
         * instant return from epoll().
         */
        bufferevent_base_set(dummy_base, p->bev);

        df_log_msg(DF_LOG_DEBUG, "UART%c: not connected.\n", p->uart);
        return;
    }

    df_log_msg(DF_LOG_DEBUG, "UART%c: error\n", p->uart);
}

/**
 * Event callback that fires when someone connects to our UART/pty
 *
 * @param sock - unused
 * @param events - unused
 * @param arg - uart_pty_t that was connected
 */
void
uart_connected_cb(evutil_socket_t sock, short events, void *arg)
{
	uart_pty_t *p = (uart_pty_t *)arg;

    df_log_msg(DF_LOG_DEBUG, "UART%c: Checking for connection.\n", p->uart);

    /* Move the PTY into the event base we actually check */
    bufferevent_base_set(p->base, p->bev);

    /* Enable the read callback */
    bufferevent_enable(p->bev, EV_READ);
}


static const char * irq_names[IRQ_UART_PTY_COUNT] = {
	[IRQ_UART_PTY_BYTE_IN] = "8<uart_pty.in",
	[IRQ_UART_PTY_BYTE_OUT] = "8>uart_pty.out",
};

int
uart_pty_init(struct avr_t *avr, uart_pty_t *p, char uart,
        struct event_base *base)
{
    struct bufferevent *bev;
    int m, s;
    struct termios tio;
    struct timeval tv;

    if (!dummy_base) {
        dummy_base = event_base_new();
    }

    /* Clear our structure */
	memset(p, 0, sizeof(*p));
    p->fd = -1;

    /* Store the 'name' of the UART we are working with */
    p->uart = uart;

	p->avr = avr;
	p->irq = avr_alloc_irq(&avr->irq_pool, 0, IRQ_UART_PTY_COUNT, irq_names);
	avr_irq_register_notify(p->irq + IRQ_UART_PTY_BYTE_IN, uart_pty_in_hook, p);

    if (openpty(&m, &s, p->slavename, NULL, NULL) < 0) {
        fprintf(stderr, "Unable to create pty for UART%c: %s\n",
                p->uart, strerror(errno));
        return -1;
    }

    if (tcgetattr(m, &tio) < 0) {
        fprintf(stderr, "Failed to retreive UART%c attributes: %s\n",
                p->uart, strerror(errno));
        goto err;
    }

    /* We want it to be raw (no terminal ctrl char processing) */
    cfmakeraw(&tio);

    if (tcsetattr(m, TCSANOW, &tio) < 0) {
        fprintf(stderr, "Failed to set UART%c attributes: %s\n",
                p->uart, strerror(errno));
        goto err;
    }

    /* The master is the socket we care about and want to use */
    p->fd = m;

    /* We close the slave side so we can watch when someone connects
     * so that we aren't buffering up the bytes before a connection and
     * then dumping that buffer on them when they connect, which is
     * obviously not how serial works.
     */
    close(s);

    /* Make the master side of the TTY non-blocking so we can use libevent */
    evutil_make_socket_nonblocking(m);

    /* Create our libevent bufferevent */
    bev = bufferevent_socket_new(base, m, BEV_OPT_CLOSE_ON_FREE);
    if (!bev) {
        fprintf(stderr, "Failed to initialize libevent bufferevent for "
                "UART%c.\n", p->uart);
        goto err;
    }
    p->bev = bev;
    p->base = base;

    /* Setup our callbacks */
    bufferevent_setcb(bev, uart_read_cb, NULL, uart_error_cb, p);

    /* Setup the 4s tick timer to check for a connection */
    tv.tv_sec = 4;
    tv.tv_usec = 0;

    p->timer = event_new(p->base, -1, EV_PERSIST, uart_connected_cb, p);
    if (!p->timer) {
        fprintf(stderr, "Failed to create timer object for UART%c\n", p->uart);
        goto err;
    }

    if (event_add(p->timer, &tv)) {
        fprintf(stderr, "Failed to queue timer object for UART%c\n", p->uart);
        goto err;
    }

   /* Enable the events we are interested in */
    bufferevent_enable(bev, EV_READ);

    return 0;

err:
    close(m);

    return -1;
}

void
uart_pty_stop(uart_pty_t *p, const char *uart_path)
{
    char uart_link[1024];

    if (p->uart == '\0')
        return;

    df_log_msg(DF_LOG_INFO, "Shutting down UART%c\n", p->uart);

    if (strcmp(uart_path, "on") == 0) {
        /* Remove our symlink, but don't care if its already gone */
        snprintf(uart_link, sizeof(uart_link), "/tmp/drumfish-%d-uart%c",
                getpid(), p->uart);
        unlink(uart_link);
    } else {
        unlink(uart_path);
    }

    /* Clean up the is connected checking timer */
    event_del(p->timer);
    event_free(p->timer);

    bufferevent_free(p->bev);

    if (dummy_base) {
        event_base_free(dummy_base);
        dummy_base = NULL;
    }
}

void
uart_pty_connect(uart_pty_t *p, const char *uart_path)
{
	uint32_t f = 0;
    avr_irq_t *src, *dst, *xon, *xoff;
    char uart_link[1024];

    /* Disable stdio echoing of the UART since we are transmitting
     * binary data. (This feature should really be an opt-in rather
     * than an opt-out. Additionally don't do an arbitary usleep(1)
     * which messes up UART timing.
     */
    avr_ioctl(p->avr, AVR_IOCTL_UART_GET_FLAGS(p->uart), &f);
    f &= ~(AVR_UART_FLAG_STDIO|AVR_UART_FLAG_POOL_SLEEP);
    avr_ioctl(p->avr, AVR_IOCTL_UART_SET_FLAGS(p->uart), &f);

	src = avr_io_getirq(p->avr, AVR_IOCTL_UART_GETIRQ(p->uart),
            UART_IRQ_OUTPUT);
	dst = avr_io_getirq(p->avr, AVR_IOCTL_UART_GETIRQ(p->uart),
            UART_IRQ_INPUT);
	xon = avr_io_getirq(p->avr, AVR_IOCTL_UART_GETIRQ(p->uart),
            UART_IRQ_OUT_XON);
	xoff = avr_io_getirq(p->avr, AVR_IOCTL_UART_GETIRQ(p->uart),
            UART_IRQ_OUT_XOFF);

	if (src && dst) {
		avr_connect_irq(src, p->irq + IRQ_UART_PTY_BYTE_IN);
		avr_connect_irq(p->irq + IRQ_UART_PTY_BYTE_OUT, dst);
	}
	if (xon)
		avr_irq_register_notify(xon, uart_pty_xon_hook, p);
	if (xoff)
		avr_irq_register_notify(xoff, uart_pty_xoff_hook, p);

    /* Build the symlink path for the UART */
    if (strcmp(uart_path, "on") == 0) {
        snprintf(uart_link, sizeof(uart_link), "/tmp/drumfish-%d-uart%c",
                getpid(), p->uart);
        /* Unconditionally attempt to remove the old one */
        unlink(uart_link);

        if (symlink(p->slavename, uart_link) != 0) {
            fprintf(stderr, "UART%c: Can't create symlink to %s from %s: %s\n",
                    p->uart, uart_link, p->slavename, strerror(errno));
        } else {
            printf("UART%c available at %s\n", p->uart, uart_link);
        }
    } else {
        /* Unconditionally attempt to remove the old one */
        unlink(uart_path);

        if (symlink(p->slavename, uart_path) != 0) {
            fprintf(stderr, "UART%c: Can't create symlink to %s from %s: %s\n",
                    p->uart, uart_path, p->slavename, strerror(errno));
        } else {
            printf("UART%c available at %s\n", p->uart, uart_path);
        }
    }
}

