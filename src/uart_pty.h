/*
 * uart_pty.h
 * - based on uart_pty.h from simavr

	Copyright 2012 Michel Pollet <buserror@gmail.com>
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


#ifndef __UART_PTY_H___
#define __UART_PTY_H___

#include "sim_irq.h"

enum {
	IRQ_UART_PTY_BYTE_IN = 0,
	IRQ_UART_PTY_BYTE_OUT,
	IRQ_UART_PTY_COUNT
};

typedef struct df_uart_pty {
	avr_irq_t   *irq;
	struct avr_t *avr;

	int         xon;
    char        uart;
    char        slavename[1024];

    int         fd;
    int         peer_connected;
    struct bufferevent *bev;    /**< buffer for data to and from MCU */
    struct event_base *base;    /**< base used for connected state */
    struct event *timer;        /**< timer used to check for connection */

} uart_pty_t;

struct event_base;

int uart_pty_init( struct avr_t *avr, uart_pty_t *b, char uart,
        struct event_base *base);

void uart_pty_stop(uart_pty_t *p, const char *uart_path);

void uart_pty_connect(uart_pty_t *p, const char *uart_path);

#endif /* __UART_PTY_H___ */
