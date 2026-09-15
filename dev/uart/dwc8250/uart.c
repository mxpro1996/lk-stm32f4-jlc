/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#include "dev/uart/dwc8250.h"

#include <assert.h>
#include <dev/uart.h>
#include <kernel/thread.h>
#include <lib/cbuf.h>
#include <lib/io.h>
#include <lk/reg.h>
#include <lk/trace.h>
#include <platform/interrupts.h>

// Synopsys DesignWare APB UART driver.
//
// The core register set is a 16550, so DWC8250_REG_8 drives a plain 16550 with
// byte wide registers on a 1 byte stride. Everything the DesignWare part adds
// past register 7 is 32 bits wide at a fixed byte offset and only exists in the
// DWC8250_REG_32 layout, so the two live behind separate accessors.
//
// Baud rate and line format are left as the bootloader configured them.
//
// IER and MCR are only ever read-modify-written, never assigned. Vendor
// variants put their own controls in the bits this driver does not use -- a
// PXA derived part carries UART Unit Enable in IER bit 6 -- and clearing one of
// those would shut the port down. Only the bits named here are ever disturbed.

#define LOCAL_TRACE 0

// Core 16550 registers, by register index.
enum dwc8250_reg {
    UART_RBR = 0, // rx buffer, read only
    UART_THR = 0, // tx holding, write only
    UART_IER = 1,
    UART_IIR = 2, // read only
    UART_FCR = 2, // write only
    UART_LCR = 3,
    UART_MCR = 4,
    UART_LSR = 5,
    UART_MSR = 6,
    UART_SCR = 7,
};

// DesignWare additions, by byte offset. Always 32 bits wide.
enum dwc8250_ext_reg {
    UART_USR = 0x7c, // uart status
    UART_TFL = 0x80, // tx fifo level
    UART_RFL = 0x84, // rx fifo level
    UART_CPR = 0xf4, // component parameters
    UART_UCV = 0xf8, // component version
};

#define UART_IER_ERBFI (1u << 0) // rx data available
#define UART_IER_ETBEI (1u << 1) // tx holding register empty
#define UART_IER_ELSI  (1u << 2) // rx line status
#define UART_IER_EDSSI (1u << 3) // modem status
#define UART_IER_ALL   (UART_IER_ERBFI | UART_IER_ETBEI | UART_IER_ELSI | UART_IER_EDSSI)

// Interrupt id, in the low 4 bits of IIR.
#define UART_IIR_ID_MASK         0xfu
#define UART_IIR_ID_MODEM_STATUS 0x0u
#define UART_IIR_ID_NONE         0x1u
#define UART_IIR_ID_THR_EMPTY    0x2u
#define UART_IIR_ID_RX_DATA      0x4u
#define UART_IIR_ID_LINE_STATUS  0x6u
#define UART_IIR_ID_BUSY         0x7u // DesignWare busy detect
#define UART_IIR_ID_CHAR_TIMEOUT 0xcu

#define UART_FCR_FIFOE      (1u << 0) // fifo enable
#define UART_FCR_RFIFOR     (1u << 1) // rx fifo reset
#define UART_FCR_XFIFOR     (1u << 2) // tx fifo reset
#define UART_FCR_RT_1CHAR   (0u << 6) // rx trigger level

#define UART_MCR_DTR  (1u << 0)
#define UART_MCR_RTS  (1u << 1)
#define UART_MCR_OUT2 (1u << 3) // gates the irq to the cpu on PC style parts
#define UART_MCR_LOOP (1u << 4)

#define UART_LSR_DR   (1u << 0) // rx data ready
#define UART_LSR_THRE (1u << 5) // tx holding register (or fifo) empty
#define UART_LSR_TEMT (1u << 6) // tx holding register and shift register empty

// TFL and RFL report a fifo level in the low 12 bits; the rest is reserved.
#define UART_FIFO_LEVEL_MASK 0xfffu

#define UART_USR_BUSY (1u << 0)
#define UART_USR_TFNF (1u << 1) // tx fifo not full
#define UART_USR_RFNE (1u << 3) // rx fifo not empty

#define UART_CPR_FIFO_STAT     (1u << 10) // USR fifo bits and TFL/RFL implemented
#define UART_CPR_ENCODED_PARMS (1u << 12) // the rest of CPR is valid
#define UART_CPR_FIFO_MODE(x)  ((((x) >> 16) & 0xff) * 16)

// Bound on any spin over a hardware status bit, so a wedged uart cannot hang
// the caller. Long enough to cover several character times at any sane baud.
#define SPIN_LIMIT 1000000

// TODO: have these be configurable
#define RXBUF_SIZE 128
#define NUM_UART   1

struct dwc8250_uart {
    bool initialized;
    struct dwc8250_config config;

    // DesignWare capabilities, read out of CPR at init.
    bool cpr_valid;     // the part described itself in CPR, so it really is a DesignWare
    bool fifo_stat;     // USR fifo bits and TFL/RFL are implemented
    uint32_t fifo_size; // 0 if the part did not tell us

    cbuf_t rx_buf;
    char rx_buf_data[RXBUF_SIZE];
};

static struct dwc8250_uart uart[NUM_UART];

// Core registers: index scaled by the configured stride.
static inline uint32_t read_reg(const struct dwc8250_uart *u, enum dwc8250_reg reg) {
    if (u->config.reg_style == DWC8250_REG_32) {
        return mmio_read32((volatile uint32_t *)(u->config.base + (uintptr_t)reg * 4));
    }
    return mmio_read8((volatile uint8_t *)(u->config.base + (uintptr_t)reg));
}

static inline void write_reg(const struct dwc8250_uart *u, enum dwc8250_reg reg, uint32_t val) {
    if (u->config.reg_style == DWC8250_REG_32) {
        mmio_write32((volatile uint32_t *)(u->config.base + (uintptr_t)reg * 4), val);
    } else {
        mmio_write8((volatile uint8_t *)(u->config.base + (uintptr_t)reg), (uint8_t)val);
    }
}

// DesignWare registers: fixed byte offset, always 32 bits. Only valid in the
// DWC8250_REG_32 layout, which every caller below checks first.
static inline uint32_t read_ext_reg(const struct dwc8250_uart *u, enum dwc8250_ext_reg reg) {
    DEBUG_ASSERT(u->config.reg_style == DWC8250_REG_32);
    return mmio_read32((volatile uint32_t *)(u->config.base + (uintptr_t)reg));
}

// True if the DesignWare register window past register 7 can be addressed at
// all. That is a property of the layout, not of the vendor: use cpr_valid to
// decide whether the part is really a DesignWare.
static inline bool has_ext_regs(const struct dwc8250_uart *u) {
    return u->config.reg_style == DWC8250_REG_32;
}

static inline void set_reg_bits(const struct dwc8250_uart *u, enum dwc8250_reg reg, uint32_t bits) {
    write_reg(u, reg, read_reg(u, reg) | bits);
}

static inline void clear_reg_bits(const struct dwc8250_uart *u, enum dwc8250_reg reg, uint32_t bits) {
    write_reg(u, reg, read_reg(u, reg) & ~bits);
}

// True if the tx holding register or fifo has room for another byte.
//
// The DesignWare programmable THRE mode (IER bit 7) would redefine LSR.THRE to
// mean "tx fifo full", but only while tx interrupts are also enabled, and this
// driver never enables them. Leaving it off keeps LSR.THRE unambiguous.
static inline bool tx_ready(const struct dwc8250_uart *u) {
    if (u->fifo_stat) {
        return read_ext_reg(u, UART_USR) & UART_USR_TFNF;
    }
    return read_reg(u, UART_LSR) & UART_LSR_THRE;
}

// Spin until the transmitter has drained all the way through the shift register.
static void wait_for_tx_idle(const struct dwc8250_uart *u) {
    for (size_t i = 0; i < SPIN_LIMIT; i++) {
        if (read_reg(u, UART_LSR) & UART_LSR_TEMT) {
            return;
        }
    }
}

// Number of bytes waiting in the rx fifo. Parts without the fifo status feature
// can only answer "at least one" or "none".
//
// The count drives a loop that reads RBR that many times, so it is masked to
// the field RFL actually defines and clamped to the fifo the part reported.
// Anything wider would be reserved bits driving reads of the receive buffer.
static inline uint32_t rx_pending(const struct dwc8250_uart *u) {
    if (u->fifo_stat) {
        uint32_t level = read_ext_reg(u, UART_RFL) & UART_FIFO_LEVEL_MASK;
        return (level > u->fifo_size) ? u->fifo_size : level;
    }
    return (read_reg(u, UART_LSR) & UART_LSR_DR) ? 1 : 0;
}

// Move everything in the rx fifo into the cbuf. Returns true if anything arrived.
static bool drain_rx(struct dwc8250_uart *u) {
    bool received = false;

    for (;;) {
        uint32_t pending = rx_pending(u);
        if (pending == 0) {
            return received;
        }

        while (pending-- > 0) {
#if CONSOLE_HAS_INPUT_BUFFER
            if (u->config.flag & DWC8250_FLAG_DEBUG_UART) {
                char c = read_reg(u, UART_RBR);
                cbuf_write_char(&console_input_cbuf, c, false);
                received = true;
                continue;
            }
#endif
            // If we're out of rx buffer, mask the irq instead of handling it.
            // uart_getc unmasks once it has made room.
            if (cbuf_space_avail(&u->rx_buf) == 0) {
                clear_reg_bits(u, UART_IER, UART_IER_ERBFI);
                return received;
            }

            char c = read_reg(u, UART_RBR);
            cbuf_write_char(&u->rx_buf, c, false);
            received = true;
        }
    }
}

static enum handler_return uart_irq(void *arg) {
    struct dwc8250_uart *u = (struct dwc8250_uart *)arg;
    bool resched = false;

    // IIR reports one source at a time, highest priority first, so keep going
    // until it says there is nothing left. Every case below clears its source,
    // which is what lets this loop terminate.
    for (;;) {
        uint32_t id = read_reg(u, UART_IIR) & UART_IIR_ID_MASK;

        switch (id) {
            case UART_IIR_ID_RX_DATA:
            case UART_IIR_ID_CHAR_TIMEOUT:
                if (drain_rx(u)) {
                    resched = true;
                }
                break;
            case UART_IIR_ID_LINE_STATUS:
                read_reg(u, UART_LSR); // reading LSR clears the condition
                break;
            case UART_IIR_ID_MODEM_STATUS:
                read_reg(u, UART_MSR); // reading MSR clears the condition
                break;
            case UART_IIR_ID_THR_EMPTY:
                break; // reading IIR already cleared it
            case UART_IIR_ID_BUSY:
                // A write to LCR landed while the uart was busy. Reading USR
                // clears it. A part without USR cannot raise this, so treat it
                // as unknown rather than reading a register that isn't there.
                if (has_ext_regs(u)) {
                    read_ext_reg(u, UART_USR);
                    break;
                }
                return resched ? INT_RESCHEDULE : INT_NO_RESCHEDULE;
            default:
                // UART_IIR_ID_NONE, or a source this driver cannot clear.
                return resched ? INT_RESCHEDULE : INT_NO_RESCHEDULE;
        }
    }
}

void dwc8250_init_early(int port, const struct dwc8250_config *config) {
    if (port >= NUM_UART) {
        return;
    }
    struct dwc8250_uart *u = &uart[port];

    u->config = *config;

    // Ask a DesignWare part what it was built with. Encoding the parameters in
    // CPR is a synthesis option, so a genuine DesignWare may report nothing at
    // all -- CPR and UCV both read zero on the EIC7700X. Everything keyed off
    // CPR below therefore has to stay optional, with an LSR based fallback:
    // USR bits 1 through 4 and the TFL/RFL levels only exist when the part
    // claims FIFO_STAT, and reading them from a part that does not would say
    // the tx fifo is permanently full.
    if (has_ext_regs(u)) {
        uint32_t cpr = read_ext_reg(u, UART_CPR);
        if (cpr & UART_CPR_ENCODED_PARMS) {
            u->cpr_valid = true;
            u->fifo_size = UART_CPR_FIFO_MODE(cpr);
            // A zero FIFO_MODE means the part was built without fifos, which
            // leaves nothing for the level registers to report. Requiring a
            // size here is also what makes the clamp in rx_pending safe.
            u->fifo_stat = (cpr & UART_CPR_FIFO_STAT) && u->fifo_size > 0;
        }
    }

    // Let the bootloader's last few characters get out before resetting the fifos.
    wait_for_tx_idle(u);

    // No interrupts until dwc8250_init sets up somewhere to put the data.
    clear_reg_bits(u, UART_IER, UART_IER_ALL);

    // Enable the fifos and start from empty. Trigger the rx interrupt on the
    // first byte, so no character has to wait for the fifo to fill.
    write_reg(u, UART_FCR, UART_FCR_FIFOE | UART_FCR_RFIFOR | UART_FCR_XFIFOR | UART_FCR_RT_1CHAR);

    // Assert the modem outputs since we don't manage flow control, and OUT2,
    // which some parts route to the interrupt enable. Auto flow control stays
    // off: a board with CTS unconnected would never transmit again.
    set_reg_bits(u, UART_MCR, UART_MCR_DTR | UART_MCR_RTS | UART_MCR_OUT2);
    clear_reg_bits(u, UART_MCR, UART_MCR_LOOP);

    u->initialized = true;
}

void dwc8250_init(int port) {
    if (port >= NUM_UART) {
        return;
    }
    struct dwc8250_uart *u = &uart[port];
    if (!u->initialized) {
        return;
    }

    // Only a part that described itself in CPR is known to be a DesignWare, and
    // only then is UCV worth reading: a 4 byte stride on its own says nothing
    // about what lives past register 7.
    if (u->cpr_valid) {
        uint32_t ucv = read_ext_reg(u, UART_UCV);
        if (ucv != 0) {
            dprintf(INFO, "dwc8250: version %c.%c%c, %u byte fifo%s\n", (char)((ucv >> 24) & 0xff),
                    (char)((ucv >> 16) & 0xff), (char)((ucv >> 8) & 0xff), u->fifo_size,
                    u->fifo_stat ? ", fifo status" : "");
        }
    }

    // Create a circular buffer to hold received data.
    cbuf_initialize_etc(&u->rx_buf, RXBUF_SIZE, u->rx_buf_data);

    register_int_handler(u->config.irq, &uart_irq, u);

    // Enable the rx interrupt. Line status and modem status interrupts stay
    // masked; uart_irq still clears them in case firmware left them on.
    set_reg_bits(u, UART_IER, UART_IER_ERBFI);

    unmask_interrupt(u->config.irq);
}

int uart_putc(int port, char c) {
    DEBUG_ASSERT(port < NUM_UART);
    struct dwc8250_uart *u = &uart[port];
    if (unlikely(!u->initialized)) {
        return -1;
    }

    // Spin while the tx fifo is full.
    for (size_t i = 0; !tx_ready(u); i++) {
        if (i >= SPIN_LIMIT) {
            return -1;
        }
    }
    write_reg(u, UART_THR, (uint8_t)c);

    return 1;
}

int uart_getc(int port, bool wait) {
    DEBUG_ASSERT(port < NUM_UART);
    struct dwc8250_uart *u = &uart[port];
    if (unlikely(!u->initialized)) {
        return -1;
    }

    char c;
    if (cbuf_read_char(&u->rx_buf, &c, wait) == 1) {
        // There is room again, so undo any masking drain_rx did.
        set_reg_bits(u, UART_IER, UART_IER_ERBFI);
        return (unsigned char)c;
    }

    return -1;
}

/* panic-time getc/putc */
int uart_pputc(int port, char c) {
    return uart_putc(port, c);
}

int uart_pgetc(int port) {
    DEBUG_ASSERT(port < NUM_UART);
    struct dwc8250_uart *u = &uart[port];
    if (unlikely(!u->initialized)) {
        return -1;
    }

    if (rx_pending(u) > 0) {
        return (unsigned char)read_reg(u, UART_RBR);
    }
    return -1;
}

void uart_flush_tx(int port) {
    DEBUG_ASSERT(port < NUM_UART);
    struct dwc8250_uart *u = &uart[port];
    if (unlikely(!u->initialized)) {
        return;
    }

    wait_for_tx_idle(u);
}

void uart_flush_rx(int port) {
}

// TODO collapse this into the early/regular init routines
void uart_init_port(int port, uint baud) {
}
