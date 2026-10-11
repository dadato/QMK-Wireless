/* USART1 transport for the wireless bridge client (SK32 HAL port).
 * This is a thin wrapper over the portable wb_transport.h interface: the
 * concrete USART1 implementation lives in wb_uart.c and pulls its pins / baud /
 * inter-frame gap from wb_board_config.
 */
#pragma once

#include "wb_transport.h"
