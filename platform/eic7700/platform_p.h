/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

#include <stdbool.h>

// The console uart, as a port number for the dev/uart/dwc8250 driver.
#define DEBUG_UART 0

void platform_init_uart_early(void);
void platform_init_uart(void);
