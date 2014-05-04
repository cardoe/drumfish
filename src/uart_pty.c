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
#include <pthread.h>
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

#include "uart_pty.h"
#include "avr_uart.h"
#include "sim_hex.h"

#include "df_log.h"

DEFINE_FIFO(uint8_t, uart_pty_fifo);

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

    uart_pty_t *p = (uart_pty_t*)param;
    df_log_msg(DF_LOG_DEBUG, "AVR UART%c -> out fifo (towards pty) %02x\n",
            p->uart, value);
    uart_pty_fifo_write(&p->port.in, value);
}

// try to empty our fifo, the uart_pty_xoff_hook() will be called when
// other side is full
static void
uart_pty_flush_incoming(uart_pty_t *p)
{
    uint8_t byte;

    while (p->xon && !uart_pty_fifo_isempty(&p->port.out)) {
        byte = uart_pty_fifo_read(&p->port.out);
        df_log_msg(DF_LOG_DEBUG, "uart_pty_flush_incoming send r %03d:%02x\n",
                p->port.out.read, byte);
        avr_raise_irq(p->irq + IRQ_UART_PTY_BYTE_OUT, byte);
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

    p->xon = 1;
    uart_pty_flush_incoming(p);
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
}

static void *
uart_pty_thread(void *param)
{
	uart_pty_t *p = (uart_pty_t*)param;
    int ret;
    sigset_t set;

    /* Setup our poll info. We'll always be checking the tty */
    struct pollfd pfd = {
        .fd = p->port.s,
    };

    sigfillset(&set);
    sigprocmask(SIG_SETMASK, &set, NULL);

	while (1) {
        /* Reset the events we care about to just HUP */
        pfd.events = POLLHUP;

        // read more only if buffer was empty
        if (p->port.buffer_len == p->port.buffer_done) {
            /* listen for if there's data to read */
            pfd.events |= POLLIN;
        }

        /* If we have data in our outbound fifo, check that we can write */
        if (!uart_pty_fifo_isempty(&p->port.in)) {
            pfd.events |= POLLOUT;
		}

        /* Something short but not too short */
        ret = poll(&pfd, 1, 1);

		if (!ret)
			continue;
		if (ret < 0)
			break;

        /* If no one is connected to the UART, we don't want to
         * cache data.
         */
        if (pfd.revents & POLLHUP) {
            uart_pty_fifo_read(&p->port.in);
        }

        if (pfd.revents & POLLIN) {
            ssize_t r = read(p->port.s, p->port.buffer,
                    sizeof(p->port.buffer) - 1);
            p->port.buffer_len = r;
            p->port.buffer_done = 0;
            TRACE(hdump("pty recv", p->port.buffer, r);)
        }

        // write them in fifo
        while (p->port.buffer_done < p->port.buffer_len &&
                !uart_pty_fifo_isfull(&p->port.out)) {
            int idx = p->port.buffer_done++;
            uart_pty_fifo_write(&p->port.out, p->port.buffer[idx]);

            df_log_msg(DF_LOG_DEBUG, "w %3d:%02x\n", p->port.out.write,
                    p->port.buffer[idx]);
        }

        /* Can we write data to the TTY */
        if (pfd.revents & POLLOUT) {
            uint8_t buffer[512];
            // write them in fifo
            uint8_t *dst = buffer;
            while (!uart_pty_fifo_isempty(&p->port.in) &&
                    dst < (buffer + sizeof(buffer)))
                *dst++ = uart_pty_fifo_read(&p->port.in);
            size_t len = dst - buffer;
            TRACE(size_t r =) write(p->port.s, buffer, len);
            TRACE(hdump("pty send", buffer, r);)
		}
		/* DO NOT call this, this create a concurency issue with the
		 * FIFO that can't be solved cleanly with a memory barrier
			uart_pty_flush_incoming(p);
		  */
	}
	return NULL;
}

static const char * irq_names[IRQ_UART_PTY_COUNT] = {
	[IRQ_UART_PTY_BYTE_IN] = "8<uart_pty.in",
	[IRQ_UART_PTY_BYTE_OUT] = "8>uart_pty.out",
};

int
uart_pty_init(struct avr_t *avr, uart_pty_t *p, char uart)
{
    int m, s;
    struct termios tio;
    int ret;

    /* Clear our structure */
	memset(p, 0, sizeof(*p));
    p->port.s = -1;

    /* Store the 'name' of the UART we are working with */
    p->uart = uart;

	p->avr = avr;
	p->irq = avr_alloc_irq(&avr->irq_pool, 0, IRQ_UART_PTY_COUNT, irq_names);
	avr_irq_register_notify(p->irq + IRQ_UART_PTY_BYTE_IN, uart_pty_in_hook, p);

    if (openpty(&m, &s, p->port.slavename, NULL, NULL) < 0) {
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
    p->port.s = m;

    /* We close the slave side so we can watch when someone connects
     * so that we aren't buffering up the bytes before a connection and
     * then dumping that buffer on them when they connect, which is
     * obviously not how serial works.
     */
    close(s);

	ret = pthread_create(&p->thread, NULL, uart_pty_thread, p);
    if (ret) {
        fprintf(stderr, "Failed to create thread for UART%c IRQ handling: %s\n",
                p->uart, strerror(ret));
        goto err;
    }

    return 0;

err:
    close(m);

    return -1;
}

void
uart_pty_stop(uart_pty_t *p, const char *uart_path)
{
	void *ret;
    char uart_link[1024];
    int join_status;

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

	pthread_cancel(p->thread);

    if (p->port.s != -1) {
        close(p->port.s);
        p->port.s = -1;
    }

	if ((join_status = pthread_join(p->thread, &ret))) {
        df_log_msg(DF_LOG_ERR, "Shutting down UART%c failed: %s\n",
                p->uart, strerror(join_status));
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

        if (symlink(p->port.slavename, uart_link) != 0) {
            fprintf(stderr, "UART%c: Can't create symlink to %s from %s: %s\n",
                    p->uart, uart_link, p->port.slavename, strerror(errno));
        } else {
            printf("UART%c available at %s\n", p->uart, uart_link);
        }
    } else {
        /* Unconditionally attempt to remove the old one */
        unlink(uart_path);

        if (symlink(p->port.slavename, uart_path) != 0) {
            fprintf(stderr, "UART%c: Can't create symlink to %s from %s: %s\n",
                    p->uart, uart_path, p->port.slavename, strerror(errno));
        } else {
            printf("UART%c available at %s\n", p->uart, uart_path);
        }
    }
}

