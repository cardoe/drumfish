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

#define _GNU_SOURCE

#include <sys/types.h>
#include <errno.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include <event2/event.h>

#include <sim_avr.h>
#include <sim_gdb.h>

#include "drumfish.h"
#include "flash.h"
#include "df_cores.h"
#include "df_log.h"

#define DEFAULT_PFLASH_PATH "/.drumfish/pflash.dat"
#define MAX_FLASH_FILES 1024

/* We have 1 emulated board and here's the handle to
 * the AVR core.
 */
avr_t *avr = NULL;

static const char * const df_peripheral_str[] = {
    "uart0",
    "uart1",
    NULL
};

static void
df_peripheral_parse(struct drumfish_cfg *config, const char *arg)
{
    int i;
    char *peripheral_name;
    char *peripheral_path;

    /* dup the string to mod it */
    peripheral_name = strdup(arg);
    if (!peripheral_name) {
        fprintf(stderr, "Unable to allocate memory.\n");
        exit(EXIT_FAILURE);
    }

    /* Find the = */
    peripheral_path = strchr(peripheral_name, '=');
    if (!peripheral_path) {
        fprintf(stderr, "Invalid peripheral configuration provided: '%s'.\n",
                arg);
        exit(EXIT_FAILURE);
    }

    /* Replace the = with a NULL */
    *peripheral_path = '\0';
    peripheral_path++;

    for (i = 0; i < DF_PERIPHERAL_MAX; i++) {
        if (strcmp(df_peripheral_str[i], peripheral_name) == 0) {
            free(config->peripherals[i]);
            config->peripherals[i] = strdup(peripheral_path);
            break;
        }
    }

    if (i == DF_PERIPHERAL_MAX) {
        fprintf(stderr, "Invalid peripheral name supplied '%s'\n",
                peripheral_name);
        exit(EXIT_FAILURE);
    }

    free(peripheral_name);

    return;
}


static void
handler(int sig)
{
    switch (sig) {
        case SIGINT:
        case SIGTERM:
            avr_terminate(avr);
            exit(EXIT_FAILURE);
            break;

        case SIGHUP:
            avr_reset(avr);
            break;
    }
}

static void
usage(const char *argv0)
{
    fprintf(stderr,
"Usage: %s [-v] [-s pflash] [-f firmware.hex] [-g port] [-m MAC] [-p config]\n"
"\n"
"  -s pflash    - Path to device's progammable flash storage\n"
"  -f ihex      - Load the requested 'ihex' file into the device's flash\n"
"  -e           - Erase all of progammable flash prior to loading any data\n"
"  -p config    - Configures a peripheral\n"
"  -g port      - Runs the AVR CPU under gdbserver on 'port'\n"
"  -v           - Increase verbosity of messages\n"
"  -m           - Radio MAC address\n"
"\n"
"Peripheral Config:\n"
"  Specifies a peripheral name and a value in the form of 'name=value'\n"
"\n"
"  Valid peripherals:\n"
"    uart0\n"
"    uart1\n"
"      Value can be either a valid path or 'off' or 'on'. Should 'off' be\n"
"      specified then there will be no ability to communicate with this\n"
"      peripheral but the MCU can still have it enabled. Should 'on' be\n"
"      specified then the default path of /tmp/drumfish-$PID-uartX will\n"
"      be used.\n"
"\n"
"Defaults:\n"
"  Programmable Flash Storage: $HOME/.drumfish/pflash.dat\n"
"  UART0: off\n"
"  UART1: /tmp/drumfish-$PID-uart1\n"
"\n"
"Examples:\n"
"  %s -g 1234 -m 00:11:22:00:9E:35\n"
"\n"
"  %s -f bootloader.hex\n"
"    Loads the 'bootloader.hex' blob into flash before starting the CPU\n"
"\n"
"  %s -f bootloader.hex -f payload.hex\n"
"    Would load 2 firmware blobs into flash before starting the CPU\n",
argv0, argv0, argv0, argv0);

}

int
main(int argc, char *argv[])
{
    const char *argv0 = argv[0];
    char *env;
    struct drumfish_cfg config;
    struct sigaction act;
    int state = cpu_Limbo;
    int opt;
    char **flash_file = NULL;
    size_t flash_file_len = 0;
    long port;
    struct event_config *ev_config;
    struct event_base *ev_base;

    config.mac = NULL;
    config.pflash = NULL;
    config.foreground = 1;
    config.verbose = 0;
    config.gdb = 0;
    config.erase_pflash = 0;
    config.peripherals[DF_PERIPHERAL_UART0] = strdup("off");
    config.peripherals[DF_PERIPHERAL_UART1] = strdup("on");

    while ((opt = getopt(argc, argv, "ef:p:m:vg:s:h")) != -1) {
        switch (opt) {
            case 'e':
                config.erase_pflash = 1;
                break;
            case 'f':
                /* Increment how many file names we need to keep track of */
                flash_file_len++;

                /* Set a reasonable max which is less than 2^30 to avoid
                 * overflowing realloc().
                 */
                if (flash_file_len > MAX_FLASH_FILES) {
                    fprintf(stderr, "Unable to load more than %d "
                            "firmware images at once.\n", MAX_FLASH_FILES);
                    exit(EXIT_FAILURE);
                }

                /* Make room for our next pointer */
                flash_file = realloc(flash_file,
                        sizeof(void *) * flash_file_len);
                if (!flash_file) {
                    fprintf(stderr, "Failed to allocate memory for "
                            "flash file list.\n");
                    exit(EXIT_FAILURE);
                }

                /* Now let's store our file name */
                flash_file[flash_file_len - 1] = strdup(optarg);
                if (!flash_file[flash_file_len - 1]) {
                    fprintf(stderr, "Failed to allocate memory for "
                            "flash file.\n");
                    exit(EXIT_FAILURE);
                }
                break;
            case 's':
                /* path to storage for flash */
                config.pflash = strdup(optarg);
                if (!config.pflash) {
                    fprintf(stderr, "Failed to allocate memory for "
                            "programmable flashh.\n");
                    exit(EXIT_FAILURE);
                }
                break;
            case 'm':
                config.mac = strdup(optarg);
                if (!config.mac) {
                    fprintf(stderr, "Failed to allocate memory for MAC.\n");
                    exit(EXIT_FAILURE);
                }
                break;
            case 'v':
               config.verbose++;
               break;
            case 'g':
               errno = 0;
               port = strtol(optarg, NULL, 10);
               if (errno != 0) {
                   fprintf(stderr, "Invalid supplied GDB port '%s': %s\n",
                           optarg, strerror(errno));
                   exit(EXIT_FAILURE);
               }

               if (port <= 1024 && port > UINT16_MAX) {
                   fprintf(stderr, "Invalid supplied GDB port %ld. "
                           "Must be 1024 < port <= %d\n", port, UINT16_MAX);
                   exit(EXIT_FAILURE);
               }

               config.gdb = port;
               break;
            case 'p':
               /* store requested port (UART) path */
               df_peripheral_parse(&config, optarg);
               break;
            case 'V':
               /* print version */
               break;
            case 'h':
               usage(argv0);
               exit(EXIT_SUCCESS);
               break;
            default: /* '?' */
                usage(argv0);
                exit(EXIT_FAILURE);
        }
    }

    /* Initialize our logging support */
    df_log_init(&config);

    /* If the user did not override the default location of the
     * programmable flash storage, then set the default
     */
    env = getenv("HOME");
    if (!env || !env[0]) {
        fprintf(stderr, "Unable to determine your HOME.\n");
        exit(EXIT_FAILURE);
    }

    if (asprintf(&config.pflash, "%s%s", env, DEFAULT_PFLASH_PATH) < 0) {
        fprintf(stderr, "Failed to allocate memory for pflash filename.\n");
        exit(EXIT_FAILURE);
    }

    printf("Programmable Flash Storage: %s\n", config.pflash);

    /* Handle the bare minimum signals */
    /* Yes I should use sigset_t here and use sigemptyset() */
    memset(&act, 0, sizeof(act));

    act.sa_handler = handler;
    if (sigaction(SIGHUP, &act, NULL) < 0) {
        fprintf(stderr, "Failed to install SIGHUP handler\n");
        exit(EXIT_FAILURE);
    }
    if (sigaction(SIGINT, &act, NULL) < 0) {
        fprintf(stderr, "Failed to install SIGINT handler\n");
        exit(EXIT_FAILURE);
    }
    if (sigaction(SIGTERM, &act, NULL) < 0) {
        fprintf(stderr, "Failed to install SIGTERM handler\n");
        exit(EXIT_FAILURE);
    }

    /* Setup libevent */
    ev_config = event_config_new();
    if (!ev_config) {
        fprintf(stderr, "Failed to configure libevent %s.\n",
                event_get_version());
        exit(EXIT_FAILURE);
    }
    /* We need edge-triggered interrupts for socket shutdown to
     * detect when UARTs are disconnected. Otherwise the UART will
     * always be firing a HUP event when its disconnected.
     */
    event_config_require_features(ev_config, EV_FEATURE_ET);

    /* We don't dup() any of our sockets so this is safe until we do */
    event_config_set_flag(ev_config, EVENT_BASE_FLAG_EPOLL_USE_CHANGELIST);

    /* Get our event base */
    ev_base = event_base_new_with_config(ev_config);
    event_config_free(ev_config);
    if (!ev_base) {
        const char **methods = event_get_supported_methods();

        fprintf(stderr, "Failed to get a workable libevent backend.\n");
        fprintf(stderr, "libevent %s. Supported backends are:\n",
                event_get_version());
        for (int i = 0; methods[i] != NULL; i++) {
            fprintf(stderr, "  %s\n", methods[i]);
        }

        exit(EXIT_FAILURE);
    }

    avr = m128rfa1_create(&config, ev_base);
    if (!avr) {
        fprintf(stderr, "Unable to initialize requested board.\n");
        exit(EXIT_FAILURE);
    }

    /* Flash in any requested firmware, while cleaning up our memory */
    for (size_t i = 0; i < flash_file_len; i++) {
        if (flash_load(flash_file[i], avr->flash, avr->flashend + 1)) {
            fprintf(stderr, "Failed to load '%s' into flash.\n", flash_file[i]);
            exit(EXIT_FAILURE);
        }
        /* Don't need this memory anymore */
        free(flash_file[i]);
    }
    free(flash_file);

    /* Ensure the instruction we're about to execute is legit */
    if (avr->flash[avr->pc] == 0xff) {
        fprintf(stderr, "No firmware loaded in programmable flash, unable "
                "to boot.\n");
        fprintf(stderr, "Try using '-f firmware.hex' to supply one.\n");
        exit(EXIT_FAILURE);
    }

    /* If the user wants to run the core with GDB server enabled,
     * set that up.
     */
    if (config.gdb) {
        avr->gdb_port = config.gdb;
        /* Normally starting the CPU should be in limbo, but the
         * GDB code of simavr wants it to be stopped.
         */
        state = cpu_Stopped;

        avr_gdb_init(avr);
    }

    df_log_msg(DF_LOG_DEBUG, "libevent %s. Backend: %s\n", event_get_version(),
            event_base_get_method(ev_base));

    /* Capture the current time to be used as when our CPU started */
    df_log_start_time();

    df_log_msg(DF_LOG_INFO, "Booting CPU from 0x%x.\n", avr->pc);

    /* Our main event loop */
    for (;;) {
        state = avr_run(avr);
        if (state == cpu_Done || state == cpu_Crashed)
            break;

        event_base_loop(ev_base, EVLOOP_ONCE|EVLOOP_NONBLOCK);
    }

    avr_terminate(avr);

    event_base_free(ev_base);

    free(config.pflash);
}
