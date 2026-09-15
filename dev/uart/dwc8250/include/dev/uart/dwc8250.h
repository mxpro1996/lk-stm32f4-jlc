/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

#include <stdint.h>

// Synopsys DesignWare APB UART (and the plain 16550s that share its core
// register set). The driver implements the uart_* interface from <dev/uart.h>;
// platform code only supplies the config and calls the two init routines.
//
//     static const struct dwc8250_config uart_config = {
//         .base = UART0_BASE_VIRT,
//         .irq = IRQ_UART0,
//         .flag = 0,
//         .reg_style = DWC8250_REG_32,
//     };
//
//     void platform_early_init(void) {
//         dwc8250_init_early(0, &uart_config);  // tx works from here on
//         ...
//     }
//
//     void platform_init(void) {
//         dwc8250_init(0);                      // rx interrupt and buffering
//     }

// Set this flag if the UART is a debug UART, which routes input
// directly into the console buffer if present.
#define DWC8250_FLAG_DEBUG_UART (1u << 0)

// Layout of the core register block. A DesignWare part is always 32 bit
// registers on a 4 byte stride, and only that form has the DesignWare
// extensions beyond the 16550 register set.
enum dwc8250_reg_style {
    DWC8250_REG_32, // 32 bit accesses, 4 byte stride (DesignWare)
    DWC8250_REG_8,  // 8 bit accesses, 1 byte stride (plain 16550)
};

struct dwc8250_config {
    uintptr_t base;
    uint32_t irq;
    uint32_t flag;
    enum dwc8250_reg_style reg_style;
};

// dwc8250 specific initialization routines called from platform code.
// The driver will otherwise implement the uart_* interface.
void dwc8250_init_early(int port, const struct dwc8250_config *config);
void dwc8250_init(int port);
