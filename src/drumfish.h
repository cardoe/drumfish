/*
 * drumfish.c
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


#ifndef __DRUMFISH_H__
#define __DRUMFISH_H__

enum df_peripherals {
    DF_PERIPHERAL_UART0,
    DF_PERIPHERAL_UART1,

    DF_PERIPHERAL_MAX /**< must always be the last value */
};

struct drumfish_cfg {
    char *mac;
    char *pflash;
    int foreground;
    int verbose;
    short gdb;
    int erase_pflash;
    char *peripherals[DF_PERIPHERAL_MAX];
};

#endif /* __DRUMFISH_H__ */
