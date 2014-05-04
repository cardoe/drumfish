#!/bin/bash

flash=/tmp/flash.$$
uart=/tmp/uart.$$

../src/drumfish \
    -s ${flash} \
    -e \
    -f Bootloader_Atmega128rfa1.hex \
    -p uart1=${uart} &
DF_PID=$!

./atmel-bootloader-comm.py ${uart}
result=$?

kill ${DF_PID}
rm -f ${flash}

exit ${result}
