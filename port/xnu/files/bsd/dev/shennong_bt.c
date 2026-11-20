/*
 * The Bluetooth of the Xiaomi 14 Pro (shennong, Snapdragon 8 Gen 3), step by step with boot-arg bt=N.
 * It is the Bluetooth half of the WCN7850 whose Wi-Fi half is shennong_wlan.c: the same chip and the same
 * supplies, but its own path to the SoC, HCI on a four-wire UART, serial engine 6 of QUP 2 ("SE14" to the
 * device tree: qupv3_se14_4uart at 0x898000), and its own enable pin (GPIO 17, bt-reset-gpio).
 * The supplies and the reference clock are shennong_wlan.c's to switch on (step 2 of wlan=N), so wlan=2 or
 * more has to be among the boot-args; this driver waits for it.
 *
 *   1  writes  RPMh's BCM of QUP's core clocks (QUP0), GCC's votes for QUP 2's clocks and the engine's;
 *      reads   the engine's firmware (it has to be the UART's), version, FIFOs, GCC's clock levels (DFS)
 *   2  writes  the pins to the engine; BT_EN low, then high; the engine set up as Linux's
 *              qcom_geni_serial.c does, in FIFO mode, polled; 115200 8N1 with flow control. Asks the chip
 *              its version (Linux's qca_read_soc_version())
 *   3  writes  the UART to 3 Mbit/s on both sides; the rampatch and the NVM from the phone's own partition
 *              bluetooth_a (a FAT volume: image/hmtbtfw20.tlv, hmtnv20.bXX, as Linux's qca_uart_setup()
 *              sends them, but with in-band sleep and deep sleep left off); HCI reset; the address; asks
 *              for the local version and the address. bt_scan=S then scans LE for S seconds and prints
 *              what it hears: the proof that the radio works, with no iOS involved.
 *   4  writes  H4 for iOS's bluetoothd: the kernel control com.redwood.uart.bluetooth, which is what its POSIX
 *              driver opens (BT_HCI_TRANSPORT=H4:POSIX:com.redwood.uart.bluetooth,3000000 in its job), and what it
 *              looks for first: /dev/cu.bluetooth and the registry's "bluetooth", with the address. The firmware is already in, so
 *              BlueTool's job here is only to say so (userland's bluetool.c). bt_trace=N prints the first N
 *              packets either way (their headers and 32 bytes).
 *
 * bt_pause=MS (default 2200): how long a step waits after announcing writes, so that the loader, which
 * notes the end of our log every two seconds, has it when a write resets the phone (0 for none).
 * bt_baud=N: the speed of step 3 (default 3000000; 0: stay at 115200). bt_nvm=NAME: another NVM file.
 *
 * Where things come from: the phone's device tree; Linux's qcom_geni_serial.c, qcom-geni-se.c, geni-se.h,
 * gcc-sm8650.c, clk-rcg2.c, pinctrl-sm8650.c, btqca.c, hci_qca.c.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <miscfs/devfs/devfs.h>
#include <sys/conf.h>
#include <sys/filio.h>
#include <sys/ioctl.h>
#include <sys/proc.h>
#include <sys/select.h>
#include <sys/ttycom.h>
#include <sys/termios.h>
#include <sys/uio.h>
#include <sys/vnode.h>
#include <sys/kern_control.h>
#include <sys/kpi_mbuf.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_putc(char c);
extern void RedwoodBluetoothStart(const uint8_t *address);

static uint32_t last_step, pause_ms = 2200, scan_seconds, wanted_baud = 3000000;
static bool enabled_pin, leaving;
static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[300];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = "bt: "; *c != 0; c++) {
		loader_log_putc(*c);
	}
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	if (!leaving && ml_get_interrupts_enabled()) {
		printf("bt: %s", line);
	}
	return length;
}

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
	__asm__ volatile ("dsb sy");
}

static void
wait_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static void
wait_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

static uint64_t
ms_now(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_MSEC;
}

static void
announce(const char *what)
{
	say("%s\n", what);
	if (pause_ms != 0) {
		wait_ms(pause_ms);
	}
}

/* ---- the addresses ------------------------------------------------------------------------------ */

#define GCC_PHYS                0x00100000UL
#define GCC_SIZE                0x00200000UL
#define GCC(reg)                (gcc + (reg))
#define GCC_QUP_VOTE            0x52010         /* gcc-sm8650.c: enable_reg of QUP 2's clocks */
#define  VOTE_CORE              (1u << 0)
#define  VOTE_S_AHB             (1u << 1)
#define  VOTE_M_AHB             (1u << 2)
#define  VOTE_CORE_2X           (1u << 3)
#define  VOTE_S6                (1u << 10)
#define GCC_S6_BRANCH           0x1e754         /* halt_reg of gcc_qupv3_wrap2_s6_clk */
#define GCC_S6_RCG              0x1e760         /* cmd_rcgr of gcc_qupv3_wrap2_s6_clk_src */
#define  RCG_DFS                0x14            /* clk-rcg2.c SE_CMD_DFSR_OFFSET */
#define  RCG_DFS_LEVEL(l)       (0x1c + 4 * (l))
#define  RCG_DFS_M(l)           (0x5c + 4 * (l))
#define  RCG_DFS_N(l)           (0x9c + 4 * (l))
#define QUP_PHYS                0x008c0000UL    /* qupv3_2 */
#define SE_PHYS                 0x00898000UL    /* qupv3_se14_4uart */
#define SE_SIZE                 0x4000UL
#define TLMM_PHYS               0x0f100000UL
#define TLMM_SIZE               0x100000UL
#define GPIO_CFG(n)             (tlmm + 0x1000UL * (n))
#define GPIO_IO(n)              (tlmm + 0x1000UL * (n) + 4)
#define PIN_BT_EN               17
#define PIN_SW_CTRL             18
#define PIN_CTS                 24
#define PIN_RTS                 25
#define PIN_TX                  26
#define PIN_RX                  27
#define FUNC_QUP2_SE6           1               /* pinctrl-sm8650.c: the first function of those pins */
#define PULL_NONE               0
#define PULL_DOWN               1
#define PULL_UP                 3

/* the serial engine's registers (geni-se.h, qcom-geni-se.c, qcom_geni_serial.c) */
#define GENI_FORCE_DEFAULT      0x20
#define GENI_OUTPUT_CTRL        0x24
#define GENI_CGC_CTRL           0x28
#define GENI_STATUS             0x40
#define  M_ACTIVE               (1u << 0)
#define  S_ACTIVE               (1u << 12)
#define GENI_SER_M_CLK          0x48
#define GENI_SER_S_CLK          0x4c
#define GENI_FW_REVISION        0x68
#define GENI_CLK_SEL            0x7c
#define UART_LOOPBACK           0x22c
#define GENI_BYTE_GRAN          0x254
#define GENI_DMA_MODE           0x258
#define UART_TX_TRANS_CFG       0x25c
#define  UART_CTS_MASK          (1u << 1)
#define GENI_TX_PACK0           0x260
#define GENI_TX_PACK1           0x264
#define UART_TX_WORD_LEN        0x268
#define UART_TX_STOP            0x26c
#define UART_TX_LEN             0x270
#define UART_RX_TRANS_CFG       0x280
#define GENI_RX_PACK0           0x284
#define GENI_RX_PACK1           0x288
#define UART_RX_WORD_LEN        0x28c
#define UART_RX_STALE           0x294
#define UART_TX_PARITY          0x2a4
#define UART_RX_PARITY          0x2a8
#define GENI_M_CMD0             0x600
#define GENI_M_CTRL             0x604
#define GENI_M_IRQ_STATUS       0x610
#define GENI_M_IRQ_EN           0x614
#define GENI_M_IRQ_CLEAR        0x618
#define  M_CMD_DONE             (1u << 0)
#define  M_CMD_CANCEL           (1u << 4)
#define  M_CMD_ABORT            (1u << 5)
#define  M_RX_BITS              (3u << 26)
#define GENI_S_CMD0             0x630
#define GENI_S_CTRL             0x634
#define GENI_S_IRQ_STATUS       0x640
#define GENI_S_IRQ_EN           0x644
#define GENI_S_IRQ_CLEAR        0x648
#define  S_CMD_CANCEL           (1u << 4)
#define  S_CMD_ABORT            (1u << 5)
#define  S_RX_BITS              (3u << 26)
#define  CTRL_ABORT             (1u << 1)
#define  CTRL_CANCEL            (1u << 2)
#define GENI_TX_FIFO            0x700
#define GENI_RX_FIFO            0x780
#define GENI_TX_STATUS          0x800
#define GENI_RX_STATUS          0x804
#define GENI_TX_WATERMARK       0x80c
#define GENI_RX_WATERMARK       0x810
#define GENI_RX_RFR             0x814
#define SE_GSI_EVENT_EN         0xe18
#define SE_IRQ_EN               0xe1c
#define SE_HW_PARAM_0           0xe24
#define SE_HW_PARAM_1           0xe28
#define SE_DMA_GENERAL          0xe30
#define OPCODE_SHIFT            27
#define UART_START              1u
#define PROTO_UART              2

static vm_offset_t gcc, se, qup, tlmm;
#define SE(reg)                 (se + (reg))
static uint32_t tx_depth = 16, rx_depth = 16, sampling = 16;

/* ---- clocks and the engine ---------------------------------------------------------------------- */

static struct {
	uint64_t rate;
} clock_level[8];
static unsigned clock_levels;
static bool clock_dfs;

static void
clocks_read(void)
{
	uint32_t dfs = rd(GCC(GCC_S6_RCG + RCG_DFS));

	clock_dfs = (dfs & 1) != 0;
	say("  the engine's clock: CMD %#x CFG %#x DFS %#x (%s)\n", rd(GCC(GCC_S6_RCG)), rd(GCC(GCC_S6_RCG + 4)), dfs,
	    clock_dfs ? "by levels" : "by hand");
	clock_levels = 0;
	if (!clock_dfs) {
		return;
	}
	for (unsigned l = 0; l < 8; l++) {
		uint32_t cfg = rd(GCC(GCC_S6_RCG + RCG_DFS_LEVEL(l)));
		uint32_t src = (cfg >> 8) & 7, pre = cfg & 0x1f, mode = (cfg >> 12) & 3;
		uint64_t parent = src == 0 ? 19200000ULL : src == 1 ? 600000000ULL : src == 6 ? 300000000ULL : 0;
		uint64_t rate = parent;

		if (pre != 0) {
			rate = rate * 2 / (pre + 1);
		}
		if (mode != 0) {
			uint32_t m = rd(GCC(GCC_S6_RCG + RCG_DFS_M(l))) & 0xffff;
			uint32_t n = (~rd(GCC(GCC_S6_RCG + RCG_DFS_N(l))) & 0xffff) + m;

			if (n != 0) {
				rate = rate * m / n;
			}
		}
		clock_level[l].rate = rate;
		say("    level %u: CFG %#x = %llu Hz\n", l, cfg, rate);
		clock_levels = l + 1;
	}
}

static bool
se_clock_set(uint32_t baud)
{
	uint64_t want = (uint64_t)baud * sampling, best = 0;
	unsigned level = 0;
	uint32_t divider;

	if (clock_dfs) {
		for (unsigned l = 0; l < clock_levels; l++) {
			uint64_t r = clock_level[l].rate;

			if (r >= want && r % want == 0 && r / want <= 0xfff && (best == 0 || r < best)) {
				best = r;
				level = l;
			}
		}
		if (best == 0) {
			say("  no clock level makes %u bit/s from %llu Hz\n", baud, want);
			return false;
		}
		wr(SE(GENI_CLK_SEL), level);
		divider = (uint32_t)(best / want);
	} else {
		/* by hand, gcc-sm8650.c's table: 7.3728 MHz, 48 MHz from GPLL0's 300 MHz, half integer divider 1 */
		uint32_t m, n;

		if (want % 48000000 == 0 || 48000000 % want == 0) {
			m = 4; n = 25; best = 48000000;
		} else {
			m = 384; n = 15625; best = 7372800;
		}
		if (best % want != 0) {
			say("  no divider makes %u bit/s from %llu Hz\n", baud, best);
			return false;
		}
		wr(GCC(GCC_S6_RCG + 4), 6u << 8 | 1u | 2u << 12);
		wr(GCC(GCC_S6_RCG + 8), m);
		wr(GCC(GCC_S6_RCG + 0xc), ~(n - m));
		wr(GCC(GCC_S6_RCG + 0x10), ~n);
		wr(GCC(GCC_S6_RCG), rd(GCC(GCC_S6_RCG)) | 1);
		divider = (uint32_t)(best / want);
	}
	wr(SE(GENI_SER_M_CLK), 1u | divider << 4);
	wr(SE(GENI_SER_S_CLK), 1u | divider << 4);
	say("  %u bit/s: %llu Hz, level %u, divider %u, sampling %u\n", baud, best, level, divider, sampling);
	return true;
}

static void
rx_start(void)
{
	wr(SE(GENI_S_CMD0), UART_START << OPCODE_SHIFT);
}

static void
rx_stop(void)
{
	uint64_t until;

	if ((rd(SE(GENI_STATUS)) & S_ACTIVE) == 0) {
		return;
	}
	wr(SE(GENI_S_CTRL), CTRL_CANCEL);
	until = ms_now() + 20;
	while ((rd(SE(GENI_S_IRQ_STATUS)) & S_CMD_CANCEL) == 0 && ms_now() < until) {
		wait_us(20);
	}
	if ((rd(SE(GENI_S_IRQ_STATUS)) & S_CMD_CANCEL) == 0) {
		wr(SE(GENI_S_CTRL), CTRL_ABORT);
		wait_ms(1);
	}
	wr(SE(GENI_S_IRQ_CLEAR), 0xffffffff);
	wr(SE(GENI_M_IRQ_CLEAR), M_RX_BITS);
}

static bool
se_setup(void)
{
	uint32_t pack = 0;
	uint32_t vectors[4];

	rx_stop();
	wr(SE(UART_RX_STALE), 10 * 16);
	/* geni_se_config_packing(8 bits, 4 words a FIFO entry, least significant first) */
	for (unsigned i = 0; i < 4; i++) {
		vectors[i] = (i * 8) << 5 | 7u << 1 | (i == 3 ? 1u : 0);
	}
	pack = vectors[0] | vectors[1] << 10;
	wr(SE(GENI_TX_PACK0), pack);
	wr(SE(GENI_RX_PACK0), pack);
	pack = vectors[2] | vectors[3] << 10;
	wr(SE(GENI_TX_PACK1), pack);
	wr(SE(GENI_RX_PACK1), pack);
	wr(SE(GENI_BYTE_GRAN), 0);
	/* geni_se_init() */
	wr(SE(GENI_M_IRQ_CLEAR), 0xffffffff);
	wr(SE(GENI_S_IRQ_CLEAR), 0xffffffff);
	wr(SE(GENI_CGC_CTRL), rd(SE(GENI_CGC_CTRL)) | 0x7f);
	wr(SE(SE_DMA_GENERAL), rd(SE(SE_DMA_GENERAL)) | 0xf);
	wr(SE(GENI_OUTPUT_CTRL), 0x7f);
	wr(SE(GENI_FORCE_DEFAULT), 1);
	wr(SE(SE_IRQ_EN), rd(SE(SE_IRQ_EN)) | 0xf);
	wr(SE(GENI_DMA_MODE), rd(SE(GENI_DMA_MODE)) & ~1u);
	wr(SE(SE_GSI_EVENT_EN), 0);
	wr(SE(GENI_RX_WATERMARK), 2);
	wr(SE(GENI_RX_RFR), rx_depth - 2);
	wr(SE(GENI_TX_WATERMARK), 2);
	/* qcom_geni_serial_set_termios(): 8N1, flow control on (the CTS mask bit clear) */
	if (!se_clock_set(115200)) {
		return false;
	}
	wr(SE(UART_LOOPBACK), 0);
	wr(SE(UART_TX_TRANS_CFG), rd(SE(UART_TX_TRANS_CFG)) & ~(UART_CTS_MASK | 1u));
	wr(SE(UART_TX_PARITY), rd(SE(UART_TX_PARITY)) & ~1u);
	wr(SE(UART_RX_TRANS_CFG), rd(SE(UART_RX_TRANS_CFG)) & ~(1u << 4));
	wr(SE(UART_RX_PARITY), rd(SE(UART_RX_PARITY)) & ~1u);
	wr(SE(UART_TX_WORD_LEN), 8);
	wr(SE(UART_RX_WORD_LEN), 8);
	wr(SE(UART_TX_STOP), 0);
	rx_start();
	say("  the engine set up: status %#x, M irq %#x, S irq %#x\n", rd(SE(GENI_STATUS)), rd(SE(GENI_M_IRQ_STATUS)),
	    rd(SE(GENI_S_IRQ_STATUS)));
	return true;
}

/* ---- bytes ----------------------------------------------------------------------------------------- */

#define RX_RING                 (64 * 1024)
static uint8_t *rx_ring;
static volatile uint32_t rx_head, rx_tail;
static uint32_t rx_overflows;

static void
uart_poll(void)
{
	uint32_t status = rd(SE(GENI_RX_STATUS));
	uint32_t words = status & 0x1ffffff;
	bool last = (status & (1u << 31)) != 0;
	uint32_t valid = (status >> 28) & 7;

	for (uint32_t i = 0; i < words; i++) {
		uint32_t word = rd(SE(GENI_RX_FIFO));
		uint32_t bytes = i == words - 1 && last && valid != 0 ? valid : 4;

		for (uint32_t b = 0; b < bytes; b++) {
			uint32_t next = (rx_head + 1) % RX_RING;

			if (next == rx_tail) {
				rx_overflows++;
				continue;
			}
			rx_ring[rx_head] = (uint8_t)(word >> (8 * b));
			rx_head = next;
		}
	}
	if (rd(SE(GENI_S_IRQ_STATUS)) & S_RX_BITS) {
		wr(SE(GENI_S_IRQ_CLEAR), S_RX_BITS);
		wr(SE(GENI_M_IRQ_CLEAR), M_RX_BITS);
	}
	if ((rd(SE(GENI_STATUS)) & S_ACTIVE) == 0) {
		rx_start();
	}
}

static bool
bt_getc(uint8_t *c, uint32_t timeout_ms)
{
	uint64_t until = ms_now() + timeout_ms;

	for (;;) {
		uart_poll();
		if (rx_tail != rx_head) {
			*c = rx_ring[rx_tail];
			rx_tail = (rx_tail + 1) % RX_RING;
			return true;
		}
		if (ms_now() >= until) {
			return false;
		}
		wait_us(50);
	}
}

static bool
uart_write(const uint8_t *data, uint32_t length)
{
	uint64_t until = ms_now() + 2000;
	uint32_t done = 0;

	wr(SE(GENI_M_IRQ_CLEAR), M_CMD_DONE);
	wr(SE(UART_TX_LEN), length);
	wr(SE(GENI_M_CMD0), UART_START << OPCODE_SHIFT);
	while (done < length) {
		uint32_t used = rd(SE(GENI_TX_STATUS)) & 0xfffffff;

		if (used >= tx_depth - 1) {
			uart_poll();
			if (ms_now() >= until) {
				say("  the UART does not take bytes (TX status %#x, M irq %#x, status %#x; is CTS held?)\n",
				    rd(SE(GENI_TX_STATUS)), rd(SE(GENI_M_IRQ_STATUS)), rd(SE(GENI_STATUS)));
				wr(SE(GENI_M_CTRL), CTRL_CANCEL);
				wait_ms(1);
				wr(SE(GENI_M_IRQ_CLEAR), 0xffffffff);
				return false;
			}
			continue;
		}
		uint32_t word = 0, n = length - done > 4 ? 4 : length - done;

		for (uint32_t b = 0; b < n; b++) {
			word |= (uint32_t)data[done + b] << (8 * b);
		}
		wr(SE(GENI_TX_FIFO), word);
		done += n;
	}
	while ((rd(SE(GENI_M_IRQ_STATUS)) & M_CMD_DONE) == 0) {
		uart_poll();
		if (ms_now() >= until) {
			say("  the UART did not finish sending (TX status %#x, M irq %#x)\n", rd(SE(GENI_TX_STATUS)),
			    rd(SE(GENI_M_IRQ_STATUS)));
			return false;
		}
	}
	wr(SE(GENI_M_IRQ_CLEAR), M_CMD_DONE);
	return true;
}

/* ---- HCI ------------------------------------------------------------------------------------------- */

#define H4_COMMAND              1
#define H4_ACL                  2
#define H4_SCO                  3
#define H4_EVENT                4
#define H4_ISO                  5
#define EV_COMMAND_COMPLETE     0x0e
#define EV_COMMAND_STATUS       0x0f
#define EV_LE_META              0x3e
#define EV_VENDOR               0xff
#define OP_RESET                0x0c03
#define OP_SET_EVENT_MASK       0x0c01
#define OP_READ_LOCAL_VERSION   0x1001
#define OP_READ_LOCAL_COMMANDS  0x1002
#define OP_READ_BD_ADDR         0x1009
#define OP_LE_SET_SCAN_PARAMS   0x200b
#define OP_LE_SET_SCAN_ENABLE   0x200c
#define OP_EDL_PATCH            0xfc00
#define OP_WRITE_BD_ADDR        0xfc14
#define OP_DISABLE_LOGGING      0xfc17
#define OP_SET_BAUDRATE         0xfc48
#define EDL_VERSION             0x19
#define EDL_TLV                 0x1e
#define EDL_BUILD_INFO          0x20
#define EDL_BOARD_ID            0x23
#define IBS_SLEEP               0xfe
#define IBS_WAKE                0xfd
#define IBS_ACK                 0xfc

static uint8_t packet[4 + 1024 + 8];    /* what the chip sent last: the type, then the packet */
static uint32_t packet_length;

/* One whole packet into packet[], false when nothing began in timeout_ms. */
static bool
read_packet(uint32_t timeout_ms)
{
	uint8_t type = 0, wake_ack = IBS_ACK;
	uint32_t header, length;
	uint64_t until = ms_now() + timeout_ms;

	for (;;) {
		uint64_t now = ms_now();

		if (now >= until || !bt_getc(&type, (uint32_t)(until - now))) {
			return false;
		}
		if (type == IBS_WAKE) {
			uart_write(&wake_ack, 1);
			continue;
		}
		if (type == IBS_SLEEP || type == IBS_ACK) {
			continue;
		}
		if (type == H4_EVENT || type == H4_ACL || type == H4_SCO || type == H4_ISO) {
			break;
		}
		say("  a byte that begins no packet: %#x\n", type);
	}
	header = type == H4_EVENT ? 2 : type == H4_SCO ? 3 : 4;
	packet[0] = type;
	for (uint32_t i = 0; i < header; i++) {
		if (!bt_getc(&packet[1 + i], 500)) {
			say("  a packet of type %u broke off in its header\n", type);
			return false;
		}
	}
	length = type == H4_EVENT ? packet[2] : type == H4_SCO ? packet[3] : (packet[3] | (uint32_t)packet[4] << 8);
	if (length > sizeof(packet) - 1 - header) {
		say("  a packet of type %u, %u bytes: too long\n", type, length);
		return false;
	}
	for (uint32_t i = 0; i < length; i++) {
		if (!bt_getc(&packet[1 + header + i], 500)) {
			say("  a packet of type %u broke off after %u of %u bytes\n", type, i, length);
			return false;
		}
	}
	packet_length = 1 + header + length;
	return true;
}

static bool
send_command(uint16_t opcode, const uint8_t *parameters, uint8_t length)
{
	uint8_t bytes[4 + 255];

	bytes[0] = H4_COMMAND;
	bytes[1] = (uint8_t)opcode;
	bytes[2] = (uint8_t)(opcode >> 8);
	bytes[3] = length;
	if (length != 0) {
		memcpy(bytes + 4, parameters, length);
	}
	return uart_write(bytes, 4u + length);
}

/* Until the command's Command Complete: where its return parameters begin and how many there are. */
static const uint8_t *
wait_complete(uint16_t opcode, uint32_t timeout_ms, uint32_t *length)
{
	uint64_t until = ms_now() + timeout_ms;

	for (;;) {
		uint64_t now = ms_now();

		if (now >= until || !read_packet((uint32_t)(until - now))) {
			return NULL;
		}
		if (packet[0] != H4_EVENT) {
			continue;
		}
		if (packet[1] == EV_COMMAND_COMPLETE && packet[2] >= 3 && (packet[4] | packet[5] << 8) == opcode) {
			*length = packet[2] - 3u;
			return packet + 6;
		}
		if (packet[1] == EV_COMMAND_STATUS && packet[2] >= 4 && (packet[5] | packet[6] << 8) == opcode) {
			say("  command %#x: status %#x\n", opcode, packet[3]);
			return NULL;
		}
		if (packet[1] != EV_VENDOR) {
			say("  while waiting for %#x: event %#x, %u bytes\n", opcode, packet[1], packet[2]);
		}
	}
}

static const uint8_t *
command(uint16_t opcode, const uint8_t *parameters, uint8_t length, uint32_t *returned)
{
	const uint8_t *answer;

	if (!send_command(opcode, parameters, length)) {
		return NULL;
	}
	answer = wait_complete(opcode, 3000, returned);
	if (answer == NULL) {
		say("  command %#x: no answer\n", opcode);
	}
	return answer;
}

/* ---- the firmware's files, from the FAT volume in partition bluetooth_a ---------------------------- */

static uint8_t *fat_volume;
static uint64_t fat_size;
static uint32_t fat_bps, fat_spc, fat_root_first, fat_root_sectors, fat_data_first, fat_table_first, fat_clusters;

static inline uint32_t
le16(const uint8_t *p)
{
	return p[0] | (uint32_t)p[1] << 8;
}

static inline uint32_t
le32(const uint8_t *p)
{
	return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t
fat_next(uint32_t cluster)
{
	const uint8_t *table = fat_volume + (uint64_t)fat_table_first * fat_bps;

	if (fat_clusters < 4085) {
		uint32_t v = le16(table + cluster + cluster / 2);

		v = (cluster & 1) ? v >> 4 : v & 0xfff;
		return v >= 0xff8 ? 0 : v;
	}
	uint32_t v = le16(table + 2 * cluster);

	return v >= 0xfff8 ? 0 : v;
}

static uint8_t *
fat_cluster(uint32_t cluster)
{
	uint64_t at = ((uint64_t)fat_data_first + (uint64_t)(cluster - 2) * fat_spc) * fat_bps;

	return at + fat_spc * fat_bps <= fat_size ? fat_volume + at : NULL;
}

static bool
name_same(const char *a, const char *b)
{
	for (; *a != 0 && *b != 0; a++, b++) {
		char x = *a >= 'a' && *a <= 'z' ? *a - 32 : *a, y = *b >= 'a' && *b <= 'z' ? *b - 32 : *b;

		if (x != y) {
			return false;
		}
	}
	return *a == *b;
}

static uint8_t *found_data;
static uint32_t found_size;

static void fat_walk(const uint8_t *entries, uint32_t count, uint32_t cluster, const char *wanted, unsigned depth);

/* The directory entries [entries, count): long names are put together; a file by the wanted name is copied out. */
static void
fat_entries(const uint8_t *entries, uint32_t count, const char *wanted, unsigned depth, char *longname)
{
	for (uint32_t i = 0; i < count; i++) {
		const uint8_t *e = entries + 32 * i;
		char name[32];
		uint32_t n = 0;

		if (e[0] == 0) {
			return;
		}
		if (e[0] == 0xe5) {
			longname[0] = 0;
			continue;
		}
		if ((e[11] & 0x3f) == 0x0f) {
			/* a piece of a long name: 13 UCS-2 letters; pieces come last one first */
			static const uint8_t at[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
			unsigned order = e[0] & 0x1f;

			if (order >= 1 && order <= 4) {
				for (unsigned k = 0; k < 13; k++) {
					char c = (char)e[at[k]];

					longname[(order - 1) * 13 + k] = c;
					if (e[0] & 0x40 && (c == 0 || k == 12)) {
						longname[(order - 1) * 13 + k + (c == 0 ? 0 : 1)] = 0;
						if (c == 0) {
							break;
						}
					}
				}
			}
			continue;
		}
		if (e[11] & 0x08) {
			longname[0] = 0;
			continue;
		}
		if (longname[0] != 0) {
			strlcpy(name, longname, sizeof(name));
		} else {
			for (unsigned k = 0; k < 8 && e[k] != ' '; k++) {
				name[n++] = (char)e[k];
			}
			if (e[8] != ' ') {
				name[n++] = '.';
				for (unsigned k = 8; k < 11 && e[k] != ' '; k++) {
					name[n++] = (char)e[k];
				}
			}
			name[n] = 0;
		}
		longname[0] = 0;

		uint32_t cluster = le16(e + 26), size = le32(e + 28);

		if (e[11] & 0x10) {
			if (name[0] != '.' && depth < 3) {
				fat_walk(NULL, 0, cluster, wanted, depth + 1);
			}
		} else if (wanted == NULL) {
			say("  %*s%s, %u bytes\n", depth * 2, "", name, size);
		} else if (found_data == NULL && name_same(name, wanted)) {
			uint8_t *copy = kalloc_data(size, Z_WAITOK);
			uint32_t done = 0;

			while (copy != NULL && done < size && cluster >= 2) {
				uint8_t *from = fat_cluster(cluster);
				uint32_t take = fat_spc * fat_bps;

				if (from == NULL) {
					break;
				}
				take = take > size - done ? size - done : take;
				memcpy(copy + done, from, take);
				done += take;
				cluster = fat_next(cluster);
			}
			if (copy != NULL && done == size) {
				found_data = copy;
				found_size = size;
			} else if (copy != NULL) {
				kfree_data(copy, size);
			}
		}
	}
}

static void
fat_walk(const uint8_t *entries, uint32_t count, uint32_t cluster, const char *wanted, unsigned depth)
{
	char longname[4 * 13 + 2];

	longname[0] = 0;
	if (entries != NULL) {
		fat_entries(entries, count, wanted, depth, longname);
		return;
	}
	for (unsigned guard = 0; cluster >= 2 && guard < 64; guard++) {
		const uint8_t *from = fat_cluster(cluster);

		if (from == NULL) {
			return;
		}
		fat_entries(from, fat_spc * fat_bps / 32, wanted, depth, longname);
		cluster = fat_next(cluster);
	}
}

static bool
volume_read(void)
{
	uint64_t size = 4096;
	uint32_t table, total, fats;
	uint8_t *head = kalloc_data(4096, Z_WAITOK);
	int error = EBUSY;

	if (head == NULL) {
		return false;
	}
	for (unsigned tries = 0; tries < 120 && error == EBUSY; tries++) {
		error = redwood_ufs_read_named("bluetooth_a", 0, head, 4096);
		if (error == EBUSY) {
			wait_ms(500);
		}
	}
	if (error != 0) {
		say("  reading partition bluetooth_a: error %d (is ufs=2 among the boot-args?)\n", error);
		kfree_data(head, 4096);
		return false;
	}
	total = le16(head + 19) != 0 ? le16(head + 19) : le32(head + 32);
	size = (uint64_t)total * le16(head + 11);
	kfree_data(head, 4096);
	say("  partition bluetooth_a: a volume of %u sectors, %llu bytes\n", total, size);
	if (size < 4096 || size > 64ULL << 20) {
		return false;
	}
	/* the volume says more than the partition has: as much of it as there is */
	do {
		fat_volume = kalloc_data(size, Z_WAITOK);
		if (fat_volume == NULL) {
			say("  no memory for the volume\n");
			return false;
		}
		error = redwood_ufs_read_named("bluetooth_a", 0, fat_volume, size);
		if (error == EINVAL) {
			kfree_data(fat_volume, size);
			fat_volume = NULL;
			size /= 2;
		}
	} while (error == EINVAL && size >= 4096);
	if (error != 0 || fat_volume == NULL) {
		say("  reading the volume: error %d\n", error);
		return false;
	}
	say("  read %llu bytes of it\n", size);
	fat_size = size;
	fat_bps = le16(fat_volume + 11);
	fat_spc = fat_volume[13];
	fats = fat_volume[16];
	fat_table_first = le16(fat_volume + 14);
	table = le16(fat_volume + 22);
	total = le16(fat_volume + 19) != 0 ? le16(fat_volume + 19) : le32(fat_volume + 32);
	fat_root_first = fat_table_first + fats * table;
	fat_root_sectors = fat_bps == 0 ? 0 : (le16(fat_volume + 17) * 32 + fat_bps - 1) / fat_bps;
	fat_data_first = fat_root_first + fat_root_sectors;
	if (fat_bps == 0 || fat_spc == 0 || table == 0) {
		say("  partition bluetooth_a is not a FAT volume we read (%u bytes a sector, %u a cluster, %u a table)\n",
		    fat_bps, fat_spc, table);
		return false;
	}
	fat_clusters = (total - fat_data_first) / fat_spc;
	say("  FAT%u: %u bytes a sector, %u a cluster, %u clusters\n", fat_clusters < 4085 ? 12 : 16, fat_bps, fat_spc,
	    fat_clusters);
	return true;
}

static bool
file_find(const char *name)
{
	const uint8_t *root = fat_volume + (uint64_t)fat_root_first * fat_bps;

	found_data = NULL;
	found_size = 0;
	fat_walk(root, fat_root_sectors * fat_bps / 32, 0, name, 0);
	return found_data != NULL;
}

static void
files_list(void)
{
	const uint8_t *root = fat_volume + (uint64_t)fat_root_first * fat_bps;

	fat_walk(root, fat_root_sectors * fat_bps / 32, 0, NULL, 0);
}

/* ---- the steps ----------------------------------------------------------------------------------- */

static uint32_t chip_rom;

static bool
step1(void)
{
	uint32_t vote, fw, params0, params1, version;

	announce("step 1: RPMh's BCM QUP0 and GCC's votes for QUP 2's clocks");
	if (!shennong_wlan_rpmh_bcm("QUP0", 1u << 30 | 1u << 29 | 1u << 14 | 1u)) {
		say("  RPMh's QUP0 was not switched on\n");
		return false;
	}
	vote = rd(GCC(GCC_QUP_VOTE));
	wr(GCC(GCC_QUP_VOTE), vote | VOTE_CORE | VOTE_S_AHB | VOTE_M_AHB | VOTE_CORE_2X | VOTE_S6);
	wait_ms(1);
	say("  GCC's votes: %#x, then %#x; the engine's clock %#x\n", vote, rd(GCC(GCC_QUP_VOTE)), rd(GCC(GCC_S6_BRANCH)));
	clocks_read();
	announce("step 1: the engine's registers");
	version = rd(qup + 4);
	fw = rd(SE(GENI_FW_REVISION));
	params0 = rd(SE(SE_HW_PARAM_0));
	params1 = rd(SE(SE_HW_PARAM_1));
	sampling = version >= 0x20050000 ? 16 : 32;
	if ((version >> 28) > 3 || ((version >> 28) == 3 && ((version >> 16) & 0xfff) >= 10)) {
		tx_depth = (params0 >> 16) & 0xff;
		rx_depth = (params1 >> 16) & 0xff;
	} else {
		tx_depth = (params0 >> 16) & 0x3f;
		rx_depth = (params1 >> 16) & 0x3f;
	}
	say("  QUP version %#x; firmware %#x (protocol %u; 2 is the UART); status %#x; FIFOs %u and %u words; DMA mode %#x\n",
	    version, fw, (fw >> 8) & 0xff, rd(SE(GENI_STATUS)), tx_depth, rx_depth, rd(SE(GENI_DMA_MODE)));
	if (((fw >> 8) & 0xff) != PROTO_UART) {
		say("  the engine has no UART firmware: stopping\n");
		return false;
	}
	return tx_depth >= 4 && rx_depth >= 4;
}

static bool
step2(void)
{
	static const struct {
		uint8_t pin, pull;
	} pins[] = { { PIN_CTS, PULL_NONE }, { PIN_RTS, PULL_DOWN }, { PIN_TX, PULL_UP }, { PIN_RX, PULL_NONE } };
	uint8_t request = EDL_VERSION;
	const uint8_t *answer;
	uint32_t length;

	announce("step 2: the UART's pins, the engine, BT_EN");
	if (!se_setup()) {
		return false;
	}
	for (unsigned i = 0; i < 4; i++) {
		wr(GPIO_CFG(pins[i].pin), pins[i].pull | FUNC_QUP2_SE6 << 2 | (2 / 2 - 1) << 6);
	}
	wr(GPIO_IO(PIN_BT_EN), 0);
	wr(GPIO_CFG(PIN_BT_EN), PULL_DOWN | (2 / 2 - 1) << 6 | 1u << 9);
	wait_ms(20);
	say("  BT_EN low: sw-ctrl %u\n", rd(GPIO_IO(PIN_SW_CTRL)) & 1);
	wr(GPIO_IO(PIN_BT_EN), 2);
	enabled_pin = true;
	wait_ms(150);
	say("  BT_EN high: sw-ctrl %u; CTS %u, RX %u (as pins); the engine's status %#x, RX FIFO %#x\n",
	    rd(GPIO_IO(PIN_SW_CTRL)) & 1, rd(GPIO_IO(PIN_CTS)) & 1, rd(GPIO_IO(PIN_RX)) & 1, rd(SE(GENI_STATUS)),
	    rd(SE(GENI_RX_STATUS)));
	/* what the line said while the chip was in reset is noise */
	rx_stop();
	while (rd(SE(GENI_RX_STATUS)) & 0x1ffffff) {
		(void)rd(SE(GENI_RX_FIFO));
	}
	wr(SE(GENI_S_IRQ_CLEAR), 0xffffffff);
	wr(SE(GENI_M_IRQ_CLEAR), 0xffffffff);
	rx_head = rx_tail = 0;
	rx_start();
	answer = command(OP_EDL_PATCH, &request, 1, &length);
	if (answer == NULL) {
		say("  RX FIFO %#x; S irq %#x; M irq %#x\n", rd(SE(GENI_RX_STATUS)), rd(SE(GENI_S_IRQ_STATUS)), rd(SE(GENI_M_IRQ_STATUS)));
		return false;
	}
	if (length != 15 || answer[0] != 0 || answer[1] != EDL_VERSION) {
		say("  the chip's version: %u bytes, %#x %#x\n", length, answer[0], answer[1]);
		return false;
	}
	chip_rom = le16(answer + 3 + 6);
	say("  the chip: product %#x, patch %#x, ROM %#x, SoC %#x\n", le32(answer + 3), le16(answer + 3 + 4), chip_rom,
	    le32(answer + 3 + 8));
	return chip_rom != 0;
}

static bool
speed_change(uint32_t baud)
{
	static const struct {
		uint32_t baud;
		uint8_t code;
	} codes[] = { { 921600, 10 }, { 1000000, 11 }, { 2000000, 13 }, { 3000000, 14 }, { 4000000, 15 }, { 3200000, 17 } };
	uint8_t code = 0xff;

	for (unsigned i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
		if (codes[i].baud == baud) {
			code = codes[i].code;
		}
	}
	if (code == 0xff) {
		say("  no code for %u bit/s\n", baud);
		return false;
	}
	/* hci_qca.c's qca_set_speed(): flow control off around it, no answer waited for */
	wr(SE(UART_TX_TRANS_CFG), rd(SE(UART_TX_TRANS_CFG)) | UART_CTS_MASK);
	if (!send_command(OP_SET_BAUDRATE, &code, 1)) {
		return false;
	}
	wait_ms(10);
	rx_stop();
	if (!se_clock_set(baud)) {
		return false;
	}
	rx_tail = rx_head;
	rx_start();
	wr(SE(UART_TX_TRANS_CFG), rd(SE(UART_TX_TRANS_CFG)) & ~UART_CTS_MASK);
	wait_ms(20);
	while (read_packet(50)) {
		say("  after the speed's change: type %u event %#x\n", packet[0], packet[1]);
	}
	return true;
}

/* qca_tlv_check_data(): the NVM's tags. In-band sleep and deep sleep off: nothing here drives them. */
static void
nvm_tags(uint8_t *file, uint32_t size, uint8_t baud_code)
{
	uint8_t *set = file;
	uint32_t type_length = le32(set);

	if ((type_length & 0xff) == 4 && size >= 8) {
		set += 4;
		type_length = le32(set);
	}
	if ((type_length & 0xff) != 2) {
		say("  the NVM's first set is of type %u: left as it is\n", type_length & 0xff);
		return;
	}

	uint32_t length = type_length >> 8, at = 0;
	uint8_t *data = set + 4;

	if (length + (uint32_t)(data - file) > size) {
		return;
	}
	while (at + 12 < length) {
		/* struct tlv_type_nvm: u16 tag, u16 length, two u32 reserved: 12 bytes of header */
		uint32_t tag = le16(data + at), tag_length = le16(data + at + 2);
		uint8_t *value = data + at + 12;

		if (at + 12 + tag_length > length) {
			break;
		}
		if (tag == 17 && tag_length >= 3) {
			say("  NVM tag 17, the transport: %#x %#x %#x\n", value[0], value[1], value[2]);
			value[0] &= (uint8_t)~0x80;
			value[1] = baud_code;
		} else if (tag == 27 && tag_length >= 1) {
			say("  NVM tag 27, sleep: %#x\n", value[0]);
			value[0] &= (uint8_t)~1;
		} else if (tag == 2 && tag_length == 6) {
			say("  NVM tag 2, the address: %02x:%02x:%02x:%02x:%02x:%02x\n", value[5], value[4], value[3], value[2], value[1], value[0]);
		}
		at += 12 + tag_length;
	}
}

static bool
download(const char *name, bool is_patch)
{
	uint32_t mode = 0, sent = 0, segments = 0;
	uint64_t began = ms_now();

	if (!file_find(name)) {
		say("  %s: not on the partition\n", name);
		return false;
	}
	if (is_patch && found_size > 14) {
		mode = found_data[4 + 4 + 4 + 2];
	}
	if (!is_patch) {
		nvm_tags(found_data, found_size, wanted_baud == 3000000 ? 14 : wanted_baud == 3200000 ? 17 : 0);
	}
	say("  %s: %u bytes, download mode %u\n", name, found_size, mode);
	while (sent < found_size) {
		uint8_t segment[2 + 243];
		uint32_t take = found_size - sent > 243 ? 243 : found_size - sent, length;
		bool last = sent + take == found_size || take < 243;

		segment[0] = EDL_TLV;
		segment[1] = (uint8_t)take;
		memcpy(segment + 2, found_data + sent, take);
		if (mode == 0 || last) {
			const uint8_t *answer = command(OP_EDL_PATCH, segment, (uint8_t)(take + 2), &length);

			if (answer == NULL) {
				say("  %s: stopped after %u bytes\n", name, sent);
				return false;
			}
			if (length < 2 || answer[0] != 0 || answer[1] != EDL_TLV) {
				say("  %s: refused after %u bytes: %#x %#x\n", name, sent, answer[0], answer[1]);
				return false;
			}
		} else if (!send_command(OP_EDL_PATCH, segment, (uint8_t)(take + 2))) {
			return false;
		}
		sent += take;
		segments++;
	}
	kfree_data(found_data, found_size);
	found_data = NULL;
	say("  %s: sent, %u segments, %llu ms\n", name, segments, ms_now() - began);
	return true;
}

static void
le_scan(uint32_t seconds)
{
	static const uint8_t mask[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x3f };
	static const uint8_t parameters[7] = { 1, 0x10, 0, 0x10, 0, 0, 0 };     /* active, 10 ms of every 10 ms */
	uint8_t enable[2] = { 1, 0 }, off[2] = { 0, 0 };
	uint32_t length, reports = 0, seen = 0;
	uint8_t heard[32][6];
	uint64_t until;

	say("step 3: LE scan for %u s\n", seconds);
	command(OP_SET_EVENT_MASK, mask, 8, &length);
	command(OP_LE_SET_SCAN_PARAMS, parameters, 7, &length);
	command(OP_LE_SET_SCAN_ENABLE, enable, 2, &length);
	until = ms_now() + seconds * 1000ULL;
	while (ms_now() < until) {
		if (!read_packet(200) || packet[0] != H4_EVENT || packet[1] != EV_LE_META || packet[3] != 2) {
			continue;
		}
		/* one report: subevent, count, type, address type, address, length, data, RSSI */
		reports++;
		unsigned known = 0;

		for (; known < seen; known++) {
			if (memcmp(heard[known], packet + 7, 6) == 0) {
				break;
			}
		}
		if (known == seen && seen < 32) {
			memcpy(heard[seen++], packet + 7, 6);
			say("  heard %02x:%02x:%02x:%02x:%02x:%02x RSSI %d\n", packet[12], packet[11], packet[10], packet[9],
			    packet[8], packet[7], (int8_t)packet[packet_length - 1]);
		}
	}
	command(OP_LE_SET_SCAN_ENABLE, off, 2, &length);
	say("step 3: %u reports from %u devices\n", reports, seen);
}

static bool
step3(void)
{
	char nvm[32] = "";
	uint8_t request = EDL_BOARD_ID, logging[2] = { 0x14, 0x00 };
	static const uint8_t address[6] = { 0x02, 0x52, 0x44, 0x57, 0x00, 0x02 };
	const uint8_t *answer;
	uint32_t length, rom = (chip_rom >> 4 & 0xf0) | (chip_rom & 0xf);
	char patch[32];

	say("step 3: the firmware's files\n");
	if (!volume_read()) {
		return false;
	}
	files_list();
	if (wanted_baud != 0) {
		announce("step 3: the speed");
		if (!speed_change(wanted_baud)) {
			return false;
		}
	}
	snprintf(patch, sizeof(patch), "hmtbtfw%02x.tlv", rom);
	if (!download(patch, true)) {
		return false;
	}
	wait_ms(10);
	answer = command(OP_EDL_PATCH, &request, 1, &length);
	PE_parse_boot_argn("bt_nvm", nvm, sizeof(nvm));
	if (nvm[0] == 0) {
		uint32_t board = answer != NULL && length >= 5 ? (uint32_t)answer[3] << 8 | answer[4] : 0;

		say("  the board's id: %#x\n", board);
		if (board == 0 || board == 0xffff) {
			snprintf(nvm, sizeof(nvm), "hmtnv%02x.bin", rom);
		} else {
			snprintf(nvm, sizeof(nvm), "hmtnv%02x.b%02x", rom, board);
		}
	}
	if (!download(nvm, false)) {
		return false;
	}
	if (command(OP_DISABLE_LOGGING, logging, 2, &length) == NULL) {
		say("  (logging was not turned off)\n");
	}
	if (command(OP_RESET, NULL, 0, &length) == NULL) {
		return false;
	}
	request = EDL_BUILD_INFO;
	answer = command(OP_EDL_PATCH, &request, 1, &length);
	if (answer != NULL && length > 3) {
		say("  the firmware: %.*s\n", (int)(length - 3), (const char *)answer + 3);
	}
	if (send_command(OP_WRITE_BD_ADDR, address, 6)) {
		wait_complete(OP_WRITE_BD_ADDR, 300, &length);
	}
	answer = command(OP_READ_LOCAL_VERSION, NULL, 0, &length);
	if (answer != NULL && length >= 9) {
		say("  HCI version %u, revision %#x, LMP %u, manufacturer %#x, subversion %#x\n", answer[1], le16(answer + 2), answer[4],
		    le16(answer + 5), le16(answer + 7));
	}
	answer = command(OP_READ_BD_ADDR, NULL, 0, &length);
	if (answer != NULL && length >= 7) {
		say("  the address: %02x:%02x:%02x:%02x:%02x:%02x\n", answer[6], answer[5], answer[4], answer[3], answer[2], answer[1]);
	}
	if (scan_seconds != 0) {
		le_scan(scan_seconds);
	}
	return true;
}

/* ---- step 4: H4 for iOS ------------------------------------------------------------------------------ */

#define DOWN_RING               (256 * 1024)
static uint8_t *down_ring;
static volatile uint32_t down_head, down_tail;
static uint32_t trace_left, le_trace_left;
static bool chip_ready __unused;

static void
trace(const char *how, const uint8_t *p, uint32_t n)
{
	char text[3 * 32 + 1];
	uint32_t shown = n > 32 ? 32 : n;

	if (trace_left == 0) {
		return;
	}
	trace_left--;
	for (uint32_t i = 0; i < shown; i++) {
		snprintf(text + 3 * i, 4, "%02x ", p[i]);
	}
	text[3 * shown] = 0;
	say("%s %u: %s\n", how, n, text);
}

/* ---- the socket bluetoothd talks through: a kernel control with what its POSIX driver asks of one ------- */

static kern_ctl_ref h4_control;
static volatile uint32_t h4_unit;

static void
up_put(const uint8_t *p, uint32_t n)
{
	uint32_t unit = h4_unit;
	uint32_t tries = 0;

	trace("<", p, n);
	if (unit == 0) {
		return;         /* no one listens: an event for nobody */
	}
	for (;;) {
		errno_t error = ctl_enqueuedata(h4_control, unit, (void *)(uintptr_t)p, n, 0);

		if (error == 0) {
			return;
		}
		if (error != ENOBUFS || ++tries > 2000 || unit != h4_unit) {
			rx_overflows++;
			say("a packet of %u bytes not passed up (%d); %u so far\n", n, error, rx_overflows);
			return;
		}
		wait_us(500);   /* bluetoothd is behind: the chip waits behind flow control meanwhile */
	}
}

static void event_changed(uint8_t *h4);
static volatile uint32_t swallow_opcode;

/* What the chip sends, cut into packets: for the trace, and for what may have to be changed on the way up. */
static uint8_t up_packet[2048];
static uint32_t up_have;

static void
up_byte(uint8_t c)
{
	static const uint8_t ack = IBS_ACK;
	uint32_t want;

	if (up_have == 0) {
		if (c == IBS_WAKE) {
			uart_write(&ack, 1);
			return;
		}
		if (c == IBS_SLEEP || c == IBS_ACK) {
			return;
		}
		if (c != H4_EVENT && c != H4_ACL && c != H4_SCO && c != H4_ISO) {
			say("a byte that begins no packet: %#x\n", c);
			return;
		}
	}
	up_packet[up_have++] = c;
	switch (up_packet[0]) {
	case H4_EVENT:
		want = up_have < 3 ? 3 : 3u + up_packet[2];
		break;
	case H4_SCO:
		want = up_have < 4 ? 4 : 4u + up_packet[3];
		break;
	default:
		want = up_have < 5 ? 5 : 5u + (up_packet[3] | (uint32_t)up_packet[4] << 8);
		break;
	}
	if (want > sizeof(up_packet)) {
		say("a packet of %u bytes from the chip: too long\n", want);
		up_have = 0;
		return;
	}
	if (up_have == want) {
		if (up_packet[0] == H4_EVENT) {
			event_changed(up_packet);
		}
		if (swallow_opcode != 0 && up_packet[0] == H4_EVENT && up_packet[1] == EV_COMMAND_COMPLETE &&
		    (up_packet[4] | up_packet[5] << 8) == swallow_opcode) {
			say("our own command %#x done: %#x\n", swallow_opcode, up_packet[6]);
			swallow_opcode = 0;
		} else {
			up_put(up_packet, up_have);
		}
		up_have = 0;
	}
}

/* What iOS writes, cut into packets. */
static uint8_t down_packet[4096];
static uint32_t down_have;

/*
 * What bluetoothd insists on and the WCN7850 has not got: answered here, as done, with as many zeros as the answer
 * has. It looks at the chip's table of supported commands first, so those are ticked in it on the way up.
 */
static const struct {
	uint16_t opcode;
	uint8_t returned;       /* bytes after the status */
	uint8_t octet, bit;     /* where in Read Local Supported Commands (octet 0xff: it is not there) */
} commands_here[] = {
	{ 0x0c5a, 1, 18, 2 },   /* Read Default Erroneous Data Reporting */
	{ 0x0c5b, 0, 18, 3 },   /* Write Default Erroneous Data Reporting */
	/*
	 * LE Read Periodic Advertiser List Size is one of the "extended" commands, and bluetoothd asks it at the start: after
	 * one of those the controller answers every legacy LE command (scanning!) Command Disallowed, as the spec says. No such list here.
	 */
	{ 0x204a, 1, 0xff, 0 },
};

static bool
command_here(const uint8_t *h4)
{
	uint16_t opcode = h4[1] | (uint16_t)h4[2] << 8;

	for (unsigned i = 0; i < sizeof(commands_here) / sizeof(commands_here[0]); i++) {
		if (commands_here[i].opcode == opcode) {
			uint8_t event[3 + 3 + 1 + 16] = { H4_EVENT, EV_COMMAND_COMPLETE, (uint8_t)(4 + commands_here[i].returned), 1, h4[1], h4[2], 0 };

			up_put(event, 7u + commands_here[i].returned);
			return true;
		}
	}
	/*
	 * Apple's and Broadcom's vendor commands mean nothing to this chip (and to some of them it does not even answer,
	 * and bluetoothd takes that for a dead firmware): refused here as the chip refuses the rest, "unknown command".
	 */
	if ((opcode & 0xfc00) == 0xfc00) {
		/*
		 * (Two bytes of answer with it: many of them, the Olympic settings 0xfce2, the factory calibration, come back
		 * through a parser that says "Invalid data size", and never calls back, under two; bluetoothd then waits
		 * 15 s and starts over.)
		 */
		uint8_t event[9] = { H4_EVENT, EV_COMMAND_COMPLETE, 6, 1, h4[1], h4[2], 1, h4[4], 0 };
		static uint32_t said;

		if (said++ < 64) {
			say("vendor command %#x, %u bytes: refused\n", opcode, h4[3]);
		}
		up_put(event, 9);
		return true;
	}
	return false;
}

static void
event_changed(uint8_t *h4)
{
	static uint32_t said;

	/* every refusal of the chip is worth a line */
	if (h4[1] == EV_COMMAND_COMPLETE && h4[2] >= 4 && h4[6] != 0 && said < 200) {
		said++;
		say("command %#x refused by the chip: %#x\n", h4[4] | h4[5] << 8, h4[6]);
	} else if (h4[1] == EV_COMMAND_STATUS && h4[2] >= 4 && h4[3] != 0 && said < 200) {
		said++;
		say("command %#x refused by the chip: %#x\n", h4[5] | h4[6] << 8, h4[3]);
	}
	if (h4[1] == EV_COMMAND_COMPLETE && h4[2] >= 4 + 64 && (h4[4] | h4[5] << 8) == OP_READ_LOCAL_COMMANDS) {
		for (unsigned i = 0; i < sizeof(commands_here) / sizeof(commands_here[0]); i++) {
			if (commands_here[i].octet != 0xff) {
				h4[7 + commands_here[i].octet] |= (uint8_t)(1u << commands_here[i].bit);
			}
		}
	}
}

/*
 * An iPhone's chip is given its LE random address by a vendor command. This one needs the standard one before a scan with
 * own address type "random", or it refuses to scan (0x12): once after each reset, before the first such command.
 */
static bool random_address_set;

static void up_drain(void);

static void
random_address_first(const uint8_t *h4)
{
	static const uint8_t address[6] = { 0x02, 0x00, 0x57, 0x44, 0x52, 0xc2 };      /* C2:52:44:57:00:02, static random */
	uint16_t opcode = h4[1] | (uint16_t)h4[2] << 8;
	uint64_t until;

	if (opcode == OP_RESET) {
		random_address_set = false;
		return;
	}
	if (random_address_set || (opcode != OP_LE_SET_SCAN_PARAMS && opcode != 0x2006 && opcode != 0x200d)) {
		return;
	}
	random_address_set = true;
	swallow_opcode = 0x2005;
	send_command(0x2005, address, 6);
	until = ms_now() + 200;
	while (swallow_opcode != 0 && ms_now() < until) {
		up_drain();
		wait_us(100);
	}
	swallow_opcode = 0;
}

static void
down_byte(uint8_t c)
{
	uint32_t want;

	if (down_have == 0 && c != H4_COMMAND && c != H4_ACL && c != H4_SCO && c != H4_ISO) {
		say("a byte from iOS that begins no packet: %#x\n", c);
		return;
	}
	down_packet[down_have++] = c;
	switch (down_packet[0]) {
	case H4_COMMAND:
		want = down_have < 4 ? 4 : 4u + down_packet[3];
		break;
	case H4_SCO:
		want = down_have < 4 ? 4 : 4u + down_packet[3];
		break;
	default:
		want = down_have < 5 ? 5 : 5u + (down_packet[3] | (uint32_t)down_packet[4] << 8);
		break;
	}
	if (want > sizeof(down_packet)) {
		say("a packet of %u bytes from iOS: too long\n", want);
		down_have = 0;
		return;
	}
	if (down_have == want) {
		trace(">", down_packet, down_have);
		if (le_trace_left != 0 && down_packet[0] == H4_COMMAND && (down_packet[2] & 0xfc) == 0x20) {
			uint32_t was = trace_left;

			le_trace_left--;
			trace_left = 1;
			trace("LE", down_packet, down_have);
			trace_left = was;
		}
		if (down_packet[0] == H4_COMMAND) {
			random_address_first(down_packet);
		}
		if (down_packet[0] != H4_COMMAND || !command_here(down_packet)) {
			uart_write(down_packet, down_have);
		}
		down_have = 0;
	}
}

static void
up_drain(void)
{
	uart_poll();
	while (rx_tail != rx_head) {
		uint8_t c = rx_ring[rx_tail];

		rx_tail = (rx_tail + 1) % RX_RING;
		up_byte(c);
	}
}

static void
pump(void)
{
	for (;;) {
		bool busy = false;

		uart_poll();
		if (rx_tail != rx_head) {
			up_drain();
			busy = true;
		}
		while (down_tail != down_head) {
			uint8_t c = down_ring[down_tail];

			down_tail = (down_tail + 1) % DOWN_RING;
			down_byte(c);
			busy = true;
		}
		if (leaving) {
			return;
		}
		wait_us(busy ? 100 : 500);
	}
}

static errno_t
h4_connect(kern_ctl_ref control __unused, struct sockaddr_ctl *address, void **unitinfo)
{
	say("bluetoothd connects: unit %u\n", address->sc_unit);
	*unitinfo = NULL;
	down_tail = down_head;
	down_have = 0;
	h4_unit = address->sc_unit;
	return 0;
}

static errno_t
h4_disconnect(kern_ctl_ref control __unused, u_int32_t unit, void *unitinfo __unused)
{
	say("bluetoothd is gone: unit %u\n", unit);
	if (h4_unit == unit) {
		h4_unit = 0;
	}
	return 0;
}

static errno_t
h4_send(kern_ctl_ref control __unused, u_int32_t unit __unused, void *unitinfo __unused, mbuf_t m, int flags __unused)
{
	size_t length = mbuf_pkthdr_len(m), done = 0;

	while (done < length) {
		uint32_t tail = down_tail, head = down_head;
		uint32_t room = head >= tail ? DOWN_RING - head - (tail == 0 ? 1 : 0) : tail - head - 1;

		if (room == 0) {
			wait_us(200);
			continue;
		}
		if (room > length - done) {
			room = (uint32_t)(length - done);
		}
		if (mbuf_copydata(m, done, room, down_ring + head) != 0) {
			break;
		}
		down_head = (head + room) % DOWN_RING;
		done += room;
	}
	mbuf_freem(m);
	return 0;
}

/* It is a tty to bluetoothd, through socket options: what it asks of a UART is all true, or does not matter, here. */
static struct user_termios h4_termios;

static errno_t
h4_getopt(kern_ctl_ref control __unused, u_int32_t unit __unused, void *unitinfo __unused, int option, void *data, size_t *length)
{
	static uint32_t said;

	if (said++ < 16) {
		say("getsockopt %#x, %zu bytes\n", option, *length);
	}
	if (data != NULL && *length >= sizeof(h4_termios) && (uint32_t)option == (uint32_t)TIOCGETA_64) {
		memcpy(data, &h4_termios, sizeof(h4_termios));
		*length = sizeof(h4_termios);
	} else if (data != NULL) {
		bzero(data, *length);
	}
	return 0;
}

static errno_t
h4_setopt(kern_ctl_ref control __unused, u_int32_t unit __unused, void *unitinfo __unused, int option, void *data, size_t length)
{
	static uint32_t said;

	if (said++ < 16) {
		say("setsockopt %#x, %zu bytes\n", option, length);
	}
	if (data != NULL && length >= sizeof(h4_termios) && (option & 0xff00) == 0x7400 && (option & 0xff) >= 20 && (option & 0xff) <= 22) {
		memcpy(&h4_termios, data, sizeof(h4_termios));
	}
	return 0;
}

static int
h4_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	return 0;
}

static int
h4_close(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	return 0;
}

/* (bluetoothd only looks whether this exists: its data go through the socket above) */
static const struct cdevsw h4_cdevsw = {
	.d_open = h4_open, .d_close = h4_close, .d_read = eno_rdwrt, .d_write = eno_rdwrt,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

static void
h4_device(void)
{
	static struct kern_ctl_reg registration;
	int major = cdevsw_add(-1, &h4_cdevsw);
	errno_t error;

	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "cu.bluetooth") == NULL) {
		say("/dev/cu.bluetooth cannot be made\n");
	}
	h4_termios.c_cflag = CS8 | CREAD | CLOCAL | CRTSCTS;
	h4_termios.c_ispeed = h4_termios.c_ospeed = 3000000;
	strlcpy(registration.ctl_name, "com.redwood.uart.bluetooth", sizeof(registration.ctl_name));
	registration.ctl_flags = CTL_FLAG_REG_SOCK_STREAM;
	registration.ctl_sendsize = 256 * 1024;
	registration.ctl_recvsize = 256 * 1024;
	registration.ctl_connect = h4_connect;
	registration.ctl_disconnect = h4_disconnect;
	registration.ctl_send = h4_send;
	registration.ctl_getopt = h4_getopt;
	registration.ctl_setopt = h4_setopt;
	error = ctl_register(&registration, &h4_control);
	if (error != 0) {
		say("the kernel control cannot be made: %d\n", error);
	}
}

static void
bt_thread(void *arg __unused, wait_result_t result __unused)
{
	uint64_t began = ms_now(), until = began + 60000;
	uint32_t dummy = 0;

	while (!shennong_wlan_powered() && ms_now() < until) {
		wait_ms(50);
	}
	/* the Wi-Fi side's bring-up first (the chip's power unit, its firmware), when there is one coming */
	if (PE_parse_boot_argn("wlan_driver", &dummy, sizeof(dummy)) && dummy != 0) {
		until = ms_now() + 30000;
		while (!shennong_wlan_driver_up() && ms_now() < until) {
			wait_ms(50);
		}
		say("Wi-Fi's link %s, %llu ms after the start\n", shennong_wlan_driver_up() ? "is up" : "did not come up",
		    ms_now() - began);
	} else {
		wait_ms(500);
	}
	if (!shennong_wlan_powered()) {
		say("the radio's supplies are not on (wlan=2 or more among the boot-args?): stopping\n");
		return;
	}
	gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
	se = ml_io_map(SE_PHYS, SE_SIZE);
	qup = ml_io_map(QUP_PHYS & ~0xfffUL, 0x1000) + (QUP_PHYS & 0xfff);
	tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	if (rx_ring == NULL || !step1() || last_step < 2 || !step2() || last_step < 3 || !step3()) {
		say("steps over, stopped early or as asked (bt=%u)\n", last_step);
		return;
	}
	say("steps over: the controller is up\n");
	if (last_step >= 4) {
		rx_head = rx_tail = 0;
		chip_ready = true;
		say("step 4: H4 for iOS\n");
		pump();
	}
}

void
shennong_bt_init(void)
{
	thread_t thread;

	if (strcmp(redwood_phone(), "shennong") != 0 || !PE_parse_boot_argn("bt", &last_step, sizeof(last_step)) || last_step == 0) {
		return;
	}
	PE_parse_boot_argn("bt_pause", &pause_ms, sizeof(pause_ms));
	PE_parse_boot_argn("bt_scan", &scan_seconds, sizeof(scan_seconds));
	PE_parse_boot_argn("bt_baud", &wanted_baud, sizeof(wanted_baud));
	PE_parse_boot_argn("bt_trace", &trace_left, sizeof(trace_left));
	PE_parse_boot_argn("bt_trace_le", &le_trace_left, sizeof(le_trace_left));
	/* now: bluetoothd may well connect, and write, before the chip is up; what it writes waits */
	rx_ring = kalloc_data(RX_RING, Z_WAITOK);
	down_ring = kalloc_data(DOWN_RING, Z_WAITOK);
	if (last_step >= 4) {
		static const uint8_t address[6] = { 0x02, 0x52, 0x44, 0x57, 0x00, 0x02 };

		h4_device();
		RedwoodBluetoothStart(address);
	}
	if (kernel_thread_start(bt_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/* At the run's end and on a panic's way out: Bluetooth's half of the chip off. */
void
shennong_bt_quiesce(void)
{
	leaving = true;
	if (enabled_pin) {
		wr(GPIO_IO(PIN_BT_EN), 0);
		enabled_pin = false;
	}
}
