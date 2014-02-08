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

#include <pthread.h>
#include "sim_irq.h"
#include "fifo_declare.h"

enum {
	IRQ_UART_PTY_BYTE_IN = 0,
	IRQ_UART_PTY_BYTE_OUT,
	IRQ_UART_PTY_COUNT
};

DECLARE_FIFO(uint8_t, uart_pty_fifo, 512);

typedef struct uart_pty_port_t {
	int 		s;			// socket we chat on
	char 		slavename[64];
	uart_pty_fifo_t in;
	uart_pty_fifo_t out;
	uint8_t		buffer[512];
	size_t		buffer_len;
    size_t      buffer_done;
} uart_pty_port_t;

typedef struct uart_pty_t {
	avr_irq_t *	irq;		// irq list
	struct avr_t *avr;		// keep it around so we can pause it

	pthread_t	thread;
	int			xon;
    char        uart;

    uart_pty_port_t port;
} uart_pty_t;

int uart_pty_init( struct avr_t *avr, uart_pty_t *b, char uart);

void uart_pty_stop(uart_pty_t *p);

void uart_pty_connect(uart_pty_t *p);

#endif /* __UART_PTY_H___ */
