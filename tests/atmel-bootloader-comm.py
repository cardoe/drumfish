#!/usr/bin/env python

import os
import sys
import serial
from binascii import hexlify

# Bootloader handshake from Atmel's AVR2054 AppNote
HANDSHAKE_REQ = '\xB2\xA5\x65\x4B'

# The third byte differs from the docs but matches the source
HANDSHAKE_CONF = '\x69\xD3\xD2\x26'

if __name__ == '__main__':
    if len(sys.argv) != 2:
        print("Must supply path to the UART TTY as the only argument")
        sys.exit(1)

    uart_path = sys.argv[1]

    # Open the port
    ser = serial.Serial(uart_path)

    # Send HANDSHAKE_REQ
    ser.write(HANDSHAKE_REQ)
    # Wait for HANDSHAKE_CONF
    buf = ser.read(len(HANDSHAKE_CONF))
    if buf != HANDSHAKE_CONF:
        print('Did not get expected HANDSHAKE_CONF. Got ' +
                hexlify(buf) + '\n')
        ser.close()
        sys.exit(1)

    ser.close()
    sys.exit(0)
