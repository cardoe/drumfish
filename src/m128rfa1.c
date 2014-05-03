/*
 * m128rfa1.c
 *
 *  Copyright (c) 2014 Doug Goldstein <cardoe@cardoe.com>
 *
 *  This file is part of drumfish.
 *
 *  drumfish is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  drumfish is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with drumfish.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <sys/types.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sim_avr.h>
#include "uart_pty.h"

#include "drumfish.h"
#include "flash.h"
#include "df_cores.h"

#define PC_START 0x1f800

uart_pty_t uart_pty[2];

static void
m128rfa1_init(avr_t *avr, void *data)
{
    struct drumfish_cfg *config = (struct drumfish_cfg *)data;

    if (avr->flash)
        free(avr->flash);

    avr->flash = flash_open_or_create(config, avr->flashend + 1);
}

static void
m128rfa1_deinit(avr_t *avr, void *data)
{
    struct drumfish_cfg *config = (struct drumfish_cfg *)data;

    uart_pty_stop(&uart_pty[0], config->peripherals[DF_PERIPHERAL_UART0]);
    uart_pty_stop(&uart_pty[1], config->peripherals[DF_PERIPHERAL_UART1]);

    flash_close(avr->flash, avr->flashend + 1);
    avr->flash = NULL;
}

avr_t *
m128rfa1_create(struct drumfish_cfg *config, struct event_base *base)
{
    avr_t *avr;

    avr = avr_make_mcu_by_name("atmega128rfa1");
    if (!avr) {
        fprintf(stderr, "Failed to create AVR core 'atmega128rfa1'\n");
        return NULL;
    }

    /* Setup any additional init/deinit routines */
    avr->special_init = m128rfa1_init;
    avr->special_deinit = m128rfa1_deinit;
    avr->special_data = config;

    /* Initialize our AVR */
    avr_init(avr);

    /* Our chips always run at 16mhz */
    avr->frequency = 16000000;

    /* Set our fuses */
    avr->fuse[0] = 0xE6; // low
    avr->fuse[1] = 0x1C; // high
    avr->fuse[2] = 0xFE; // extended
    //avr->lockbits = 0xEF; // lock bits

    /* Check to see if we initialized our flash */
    if (!avr->flash) {
        fprintf(stderr, "Failed to initialize flash correctly.\n");
        return NULL;
    }

    /* Based on fuse values, we'll always want to boot from the bootloader
     * which will always start at 0x1f800.
     */
    avr->pc = PC_START;
    avr->codeend = avr->flashend;

    /* Setup our UARTs, if enabled */
    if (strcmp(config->peripherals[DF_PERIPHERAL_UART0], "off")) {
        memset(&uart_pty[0], 0, sizeof(uart_pty[0]));
        if (uart_pty_init(avr, &uart_pty[0], '0', base)) {
            fprintf(stderr, "Unable to start UART0.\n");
            return NULL;
        }
        uart_pty_connect(&uart_pty[0],
                config->peripherals[DF_PERIPHERAL_UART0]);
    }

    if (strcmp(config->peripherals[DF_PERIPHERAL_UART1], "off")) {
        memset(&uart_pty[1], 0, sizeof(uart_pty[1]));
        if (uart_pty_init(avr, &uart_pty[1], '1', base)) {
            fprintf(stderr, "Unable to start UART1.\n");
            return NULL;
        }
        uart_pty_connect(&uart_pty[1],
                config->peripherals[DF_PERIPHERAL_UART1]);
    }

    return avr;
}
