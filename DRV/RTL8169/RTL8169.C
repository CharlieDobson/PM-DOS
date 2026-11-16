/*
 * RTL8169.C - RTL8169.SYS, a driver for Realtek's descriptor-ring
 * Ethernet chips, PCI and PCI Express
 *
 * One MAC design that Realtek has carried from 2002 to now, behind many
 * part numbers:
 *
 *   RTL8169, 8169S/SB/SC, 8110S/SC    PCI gigabit (the 8169SC is 10EC:8167)
 *   RTL8111/8168 B to H, EP, FP,      PCI Express gigabit: the network on
 *     8411, 8117                      most boards of 2005-2020
 *   RTL8101E/8102E/8103E/8105E,       PCI Express 10/100 (device 8136), the
 *     8106E/8106EUS/8107E, 8401, 8402   network of the Atom netbooks
 *   RTL8125 A/B/D (Killer E3000)      PCI Express 2.5 gigabit
 *   RTL8126                           PCI Express 5 gigabit
 *
 * What they share is what this driver is built on: the version bits in
 * TxConfig, the station address at 0, a ring of 16-byte descriptors for
 * each direction (OWN, end of ring, first and last fragment, a length,
 * a 64-bit buffer address) whose bases are at 20h and E4h, the C+
 * command register, and a transmit poll register the driver writes to
 * say "look at the ring".  The 8125 moved the interrupt registers (32
 * bits wide, mask at 38h and status at 3Ch) and the transmit poll (to
 * 90h), and needs a block of MAC setup the others do not; it is its own
 * branch throughout.
 *
 * No emulator has any of these chips.  This is written blind, for real
 * hardware, from FreeBSD's re(4) (sys/dev/re/if_re.c, dev/rl/if_rlreg.h,
 * dev/mii/rgephy.c), Linux's r8169 (r8169_main.c, r8169.h) and, for the
 * 8125, OpenBSD's and FreeBSD's rge(4) as a cross-check.  FreeBSD's re
 * is the one taken as the measure of what is NEEDED: it runs every
 * 8169, 8168 and 810x without the dozens of per-version tuning tables
 * Linux writes, so a step Linux takes and FreeBSD does not was left out
 * unless it is plainly about getting the MAC to work at all (taking it
 * back from the management controller, a FIFO pointer reset, the
 * receive filter reset) rather than about power or performance.
 *
 * WHICH CHIP.  TxConfig bits 31-26 and 23-20 (Linux's "XID") are looked
 * up in Linux's table; the PCI device ID only says whether the PHY is a
 * gigabit one (8136 is not).  A version that is not in the table, the
 * RTL8127 (10 gigabit, a different interrupt scheme) and the chips that
 * need the newer extended ID register are refused with the version bits
 * on the line, so the next driver knows what to add.
 *
 * WHICH REGISTERS.  The first memory BAR of 0-2 - BAR1 on the PCI 8169,
 * BAR2 on the PCI Express parts, as both FreeBSD and Linux find - mapped
 * with net_pci_map, else the I/O BAR 0.  The 8169SC (10EC:8167) is run
 * through its ports: FreeBSD found that chip does not work memory-mapped.
 * /IO on the DEVICE= line asks for the ports on any chip but the
 * 8125/8126, whose registers run past the 256-byte I/O window.
 *
 * BRING-UP, in Linux's order (rtl_init_one, then rtl_open):
 *   card_find   the PCI function to D0, legacy INTx on, MSI off, PCIe
 *               ASPM off (all three BSD drivers turn ASPM off: it stalls
 *               the transmitter on many revisions); the registers; the
 *               version; interrupts masked and acknowledged; the chip
 *               taken from its management firmware (8168G and later,
 *               8125); a software reset; the station address, written
 *               back into the ID registers; the rings allocated, once.
 *   card_setup  DASH told a driver is here (8168DP/EP, 8117, 8125BP);
 *               the PHY woken and negotiating; the chip stopped and
 *               reset; the rings built; C+ command, maximum receive
 *               size, ring addresses, the per-version steps below; then
 *               receive and transmit on, RxConfig (accept the station,
 *               broadcast and all multicast: MAR all ones), TxConfig
 *               (shortest gap, unlimited DMA burst); at the load up to
 *               four seconds for a link; what is pending acknowledged,
 *               and only then the interrupt mask written.
 *
 * WHICH VERSIONS GOT WHICH STEPS (numbers are Linux's RTL_GIGA_MAC_VER_):
 *   all         reset, C+ command (receive checksum and VLAN stripping
 *               off), ring registers, RxConfig and TxConfig as Linux's
 *               rtl_init_rxcfg and rtl_set_tx_config_registers, MAR all
 *               ones, interrupt coalescing off.
 *   02-06       (PCI 8169) Config2's MSI bit off, PCI latency 40h and
 *               cache line 32 bytes, register 82h = 1 after each reset,
 *               PCI multiple read/write in C+, the analog PLL bit (02,
 *               03), no early transmit, register 7Ch for the 8110SC
 *               (05, 06, the value both FreeBSD and Linux use), VLAN
 *               stripping LEFT ON for 05 (Linux: interrupts stop under
 *               load on the 8110SCd otherwise), system and FIFO errors
 *               in the interrupt mask.
 *   02, 03      (8169S, 8110S) PHY register 0Bh = 0 after every reset
 *               (FreeBSD's re_reset), and FreeBSD's DSP
 *               fix for PHY revisions 0 and 1, which "need to be tuned in
 *               order to reliably negotiate a 1000Mbps link".  Linux
 *               writes a different table to every 8169S; FreeBSD's,
 *               gated by the PHY revision as FreeBSD gates it, is used.
 *   05          one Gigabyte board (subsystem 1458:E000): Linux's PHY
 *               page 1 register 10h = F01Bh.
 *   17-28, 31   PHY register 0Eh = 0, the power-up FreeBSD (PHYWAKE)
 *               and older Linux both write on these.
 *   14, 25, 26, 29, 30, 32, 33, 37, 39
 *               PMCH bit 7 set, FreeBSD's "take PHY out of power down"
 *               (and the 8401's register D1h bit 3 cleared).
 *   09 (8103E), 20, 22
 *               FreeBSD's "take controller out of deep sleep" (GPIO bit
 *               0 follows MACDBG bit 7).
 *   17-33       the jumbo bits in Config3/Config4 cleared, as both
 *               drivers do for a standard frame size.
 *   28, 31      (8168DP) the PHY-access switch at D0h around each PHY
 *               access; DASH start/stop; the transmit-poll wait on stop.
 *   32, 33      (8168E) Linux's transmit FIFO pointer reset.
 *   34-38       Linux's "now is OOB" cleared, and a stop request that
 *               waits for the transmit queue to empty.
 *   34          (8168E-VL) the address also written to the ERI copy.
 *   35-52       (but 39) the station address read from ERI E0h/E4h,
 *               where Linux reads it; the ID registers if that is not a
 *               usable address.
 *   34-38, 40-52
 *               the receive packet filter reset (ERI DCh bit 0).
 *   37, 39      EEE off (ERI 1B0h), as Linux does for both.
 *   40-52, 61+  (8168G and later) taken from the management firmware at
 *               find (receive gated, out of OOB, the E8DEh handshake),
 *               and the receive gate (MISC bit 19) opened at start.
 *   40          (8168g) Linux's PHY suspend quirk: ERI 1A8h's top bits
 *               set with each BMCR write that leaves the PHY powered.
 *   51, 52      the CMAC stopped first (Linux's rtl8168ep_stop_cmac).
 *   61-70       (8125, 8126) the whole of Linux's rtl_hw_start_8125 and
 *               rtl_hw_start_8125_common but the ASPM, LTR, EEE timer,
 *               statistics and PCIe PHY parts: the old 16-byte transmit
 *               descriptor format chosen (OCP EB58h bit 0 clear),
 *               interrupt vectors and coalescing off, RSS off, the MAC
 *               OCP block Linux writes; the address from the backup
 *               register 19E0h; 8126 only: the PCIe ZRxDC timeout bit
 *               cleared through the chip's CSI window.
 * Left out on purpose: the PCIe PHY (ephy) tables, the PHY parameter
 * tables and the PHY/MAC firmware patches, the FIFO sizes and pause
 * thresholds with TxConfig's auto-FIFO bit (8168E-VL on), the
 * per-speed ERI patches on link change, EEE, ALDPS,
 * ASPM, LTR and clock-request setup (all power or tuning).  FreeBSD's
 * re runs without those; for the 8125 Linux masks the ephy register to
 * five bits and OpenBSD to seven, so the two write different registers
 * from different tables and both drive the chip - the tables are tuning.
 *
 * RECEIVE.  32 descriptors, each a 1536-byte buffer 2048 bytes apart.
 * The length the chip writes includes the CRC on every version (both
 * references take four off), and the frame is handed over without it.
 * A frame longer than one buffer comes split over several descriptors
 * (FreeBSD and OpenBSD chain those for jumbo frames, proving the chip
 * keeps to the size each descriptor gives); any descriptor without both
 * the first and last bits is dropped.  RxMaxSize is FreeBSD's choice:
 * 16383 on the 8169 and 8168B (they split), the buffer size on the
 * 8168C and later and the 810x (which "do not support multi-fragment
 * packets"), 16384 on the 8125/8126 as all three of its drivers set.
 *
 * TRANSMIT.  8 descriptors with their own buffers.  A short frame is
 * padded to 60 bytes here on every chip: the 8169 and 8168B do not pad
 * (FreeBSD's AUTOPAD list) and Linux pads the 8168E-VL and 8125 itself.
 * OWN is written last, then the poll register.  The poll is written
 * again whenever a frame is still waiting at a completion or a tick:
 * Linux's "TxPoll requests are lost when the Tx packets are too close"
 * and FreeBSD's restart of the PCIe transmitter.  A send that finds
 * its descriptor still the chip's waits up to 100ms (not at all with
 * no link); two such timeouts in a row restart the chip, as Linux's
 * transmit timeout does.  The post is made with interrupts off, so a
 * restart from card_service cannot land between reading the ring
 * index and writing the descriptor.
 *
 * INTERRUPTS are masked from the start of card_find to the end of
 * card_setup; the status (16 bits at 3Eh, 32 at 3Ch on the 8125) is
 * acknowledged exactly as read; card_service claims the line only when
 * a cause it enabled was set: receive done or failed, transmit done or
 * failed, receive descriptors exhausted, and on the PCI 8169 system and
 * FIFO errors (Linux's mask).  Descriptors exhausted or a FIFO overrun
 * is answered by draining the ring - neither reference restarts the
 * receiver, which picks up again when descriptors come back.  A PCI
 * system error on the 8169 resets and restarts the chip, as Linux does.
 *
 * THE PHY is not bit-banged: its registers are reached through PHYAR
 * (60h) or, from the 8168G on, the OCP window (B8h).  It is touched
 * only to wake one left powered down or isolated, to put back
 * autonegotiation and the full advertisement if something (a previous
 * OS) reduced either - then negotiation is restarted - and for the 8169S
 * fix above.  The speed for the name comes from PHYstatus (6Ch).
 *
 * WHAT IS NOT PROVED: all of it - there was no chip to run.  A function
 * found in D3hot is put in D0, and a chip that loses its BARs doing so
 * (No_Soft_Reset clear) is then refused: PM-DOS assigns no resources.
 * The lines printed at the load are there to say where it stopped:
 *   RTL8169: 10EC:8168, version bits 541h, RTL8168h, memory BAR 2
 *   RTL8169: PHY BMCR 1940h ANAR 01E1h, negotiating again   (only then)
 *   Realtek RTL8168h: no link                               (only then)
 * and a one-line reason for each refusal.
 *
 * ITS OWN SWITCH, beside NETCORE's /P:, /M:, /I: and /POLL:
 *   /IO       use the I/O ports rather than the memory BAR
 *
 * On the shared base (DRV\NETDRV.H, NETCORE.C).
 */
#include "netdrv.h"

void copy_dwords( void *to, const void *from, u32 dwords );
#pragma aux copy_dwords = "cld" "rep movsd" parm [edi] [esi] [ecx] modify exact [edi esi ecx];

/* ---------------------------------------------------------------- */
/* the registers                                                     */
/* ---------------------------------------------------------------- */

#define R_IDR0          0x00    /* the station address, six bytes */
#define R_MAR0          0x08    /* the multicast hash, eight bytes */
#define R_TXDESC_LO     0x20    /* the transmit ring, 64 bits */
#define R_TXDESC_HI     0x24
#define R_INTCFG0       0x34    /* 8125: interrupt configuration, 8 bits */
#define R_CHIPCMD       0x37    /* command, 8 bits */
#define R_TXPOLL        0x38    /* not the 8125: transmit poll, 8 bits */
#define R_IMR_8125      0x38    /* 8125: interrupt mask, 32 bits */
#define R_IMR           0x3C    /* interrupt mask, 16 bits */
#define R_ISR_8125      0x3C    /* 8125: interrupt status, 32 bits */
#define R_ISR           0x3E    /* interrupt status, 16 bits */
#define R_TXCONFIG      0x40
#define R_RXCONFIG      0x44
#define R_CFG9346       0x50    /* the configuration-register lock */
#define R_CONFIG1       0x52
#define R_CONFIG2       0x53
#define R_CONFIG3       0x54
#define R_CONFIG4       0x55
#define R_CONFIG5       0x56
#define R_PHYAR         0x60    /* the PHY's registers, before the 8168G */
#define R_CSIDR         0x64    /* the chip's window on its own PCI space */
#define R_CSIAR         0x68
#define R_PHYSTATUS     0x6C    /* what the PHY agreed: 8 bits, 16 on the 8125 */
#define R_MACDBG        0x6D
#define R_GPIO          0x6E
#define R_PMCH          0x6F
#define R_ERIDR         0x70    /* the extended registers, 8168C and later */
#define R_ERIAR         0x74
#define R_INTCFG1       0x7A    /* 8125, 16 bits */
#define R_PARA7C        0x7C    /* the 8110SC's */
#define R_MACRESET      0x82    /* the PCI 8169's, undocumented */
#define R_TXPOLL_8125   0x90    /* 8125: transmit poll, 16 bits */
#define R_OCPDR         0xB0    /* the MAC's OCP window, 8168G and later */
#define R_OCPAR         0xB4    /* ...and the 8168DP's management window */
#define R_GPHYOCP       0xB8    /* the PHY's OCP window, 8168G and later */
#define R_DPMDIO        0xD0    /* 8168DP: bit 17 switches PHY access */
#define R_DBG           0xD1
#define R_MCU           0xD3
#define R_DESCTYPE      0xD8    /* 8126: the receive descriptor type */
#define R_RXMAXSIZE     0xDA
#define R_CPLUSCMD      0xE0
#define R_INTRMIT       0xE2
#define R_RXDESC_LO     0xE4
#define R_RXDESC_HI     0xE8
#define R_TXMAXSIZE     0xEC    /* 8169: early transmit; others: largest frame */
#define R_MISC          0xF0
#define R_IBCR0         0xF8
#define R_IBCR2         0xF9
#define R_IBISR0        0xFB

/* the 8125's own, above the 256-byte I/O window */
#define R_8125_382      0x0382
#define R_COALESCE      0x0A00
#define R_8125_1880     0x1880
#define R_MAC0BKP       0x19E0  /* the backup copy of the station address */
#define R_RSSCTRL       0x4500
#define R_QNUMCTRL      0x4800

#define BIT31           0x80000000L

#define CMD_TXEN        0x04
#define CMD_RXEN        0x08
#define CMD_RESET       0x10
#define CMD_STOPREQ     0x80

#define TXPOLL_NPQ      0x40    /* the normal-priority queue */

#define CFG9346_UNLOCK  0xC0
#define CFG2_MSI        0x20    /* the PCI 8169's MSI enable */
#define CFG2_CLKREQ     0x80
#define CFG3_RDY_L23    0x02
#define CFG3_JUMBO0     0x04
#define CFG5_ASPM       0x01

#define ISR_RXOK        0x0001
#define ISR_RXERR       0x0002
#define ISR_TXOK        0x0004
#define ISR_TXERR       0x0008
#define ISR_RXDU        0x0010  /* no receive descriptor free */
#define ISR_FIFOOVER    0x0040  /* the receive FIFO overflowed (PCI 8169) */
#define ISR_SYSERR      0x8000  /* a PCI error (PCI 8169) */

#define RX_ACCEPT       0x0000000EL     /* broadcast, multicast, the station */
#define RX_ACCEPT_ALL   0x0000003FL     /* every accept bit */
#define TX_CONFIG       0x03000700L     /* the shortest gap, unlimited DMA */

#define PHYS_LINK       0x0002
#define PHYS_10         0x0004
#define PHYS_100        0x0008
#define PHYS_1000       0x0010
#define PHYS_2500       0x0400  /* 8125 */
#define PHYS_5000       0x1000  /* 8126 */

#define MCU_LL_READY    0x02
#define MCU_RX_EMPTY    0x10
#define MCU_TX_EMPTY    0x20
#define MCU_NOW_IS_OOB  0x80

#define MISC_RXDV_GATE  0x00080000L
#define MISC_TXPLA_RST  0x20000000L
#define TXCFG_EMPTY     0x00000800L

#define CP_RXCHKSUM     0x0020
#define CP_RXVLAN       0x0040
#define CP_PCIMULRW     0x0008
#define CP_ENANAPLL     0x4000
#define CP_KEEP         0x2063  /* what Linux keeps of the value found */

/* the descriptors' first word */
#define DESC_OWN        0x80000000L
#define DESC_EOR        0x40000000L
#define DESC_FS         0x20000000L
#define DESC_LS         0x10000000L
#define RXD_RES         0x00200000L     /* receive error summary */
#define RXD_LENGTH      0x00003FFFL
#define TXD_ERRSUM      0x00800000L     /* transmit error summary */

#define ERI_EXGMAC      0x00000000L
#define ERI_OOB         0x00020000L
#define ERI_MASK_0001   0x00001000L
#define ERI_MASK_0011   0x00003000L
#define ERI_MASK_1111   0x0000F000L

/* the PHY's own registers */
#define MII_BMCR        0
#define MII_ID2         3
#define MII_ANAR        4
#define MII_GBCR        9
#define BMCR_ANRESTART  0x0200
#define BMCR_ISOLATE    0x0400
#define BMCR_PDOWN      0x0800
#define BMCR_ANENABLE   0x1000
#define OCP_PHY_BASE    0xA400  /* the standard registers, 8168G and later */

#define DASH_DP         1
#define DASH_EP         2
#define DASH_25BP       3
#define OOB_START       0x05
#define OOB_STOP        0x06

#define VER_EXT         255     /* the newer extended ID: not driven */

#define RXN             32      /* 32 * 16 = 512 bytes of ring */
#define TXN             8
#define RXBUF           1536    /* what a receive descriptor is given */
#define BUFSTEP         2048    /* and the room each buffer has */

#define LINK_WAIT       400     /* 10ms steps for a link at the load: 4s */
#define TX_WAIT         10000   /* 10us steps for a busy descriptor: 100ms */

typedef struct { u32 opts1, opts2, addr_lo, addr_hi; } DESC;

/* the chip table: TxConfig bits 31-26, 23-20, masked, as in Linux's table */
typedef struct {
    u16 mask, bits;
    u8  ver;
    const char *name;
} CHIP;

static const CHIP chips[] = {
    { 0x7CF, 0x6C9, 80, "RTL8127A" },
    { 0x7CF, 0x64A, 70, "RTL8126A" },
    { 0x7CF, 0x649, 70, "RTL8126A" },
    { 0x7CF, 0x681, 66, "RTL8125BP" },
    { 0x7CF, 0x708, 65, "RTL8125CP" },
    { 0x7CF, 0x68B, 64, "RTL9151A" },
    { 0x7CF, 0x68A, 64, "RTL8125K" },
    { 0x7CF, 0x689, 64, "RTL8125D" },
    { 0x7CF, 0x688, 64, "RTL8125D" },
    { 0x7CF, 0x641, 63, "RTL8125B" },
    { 0x7CF, 0x609, 61, "RTL8125A" },
    { 0x7CF, 0x54B, 52, "RTL8117" },
    { 0x7CF, 0x54A, 52, "RTL8168fp" },
    { 0x7CF, 0x502, 51, "RTL8168ep" },
    { 0x7CF, 0x541, 46, "RTL8168h" },
    { 0x7CF, 0x6C0, 46, "RTL8168M" },
    { 0x7CF, 0x5C8, 44, "RTL8411b" },
    { 0x7CF, 0x509, 42, "RTL8168gu" },
    { 0x7CF, 0x4C0, 40, "RTL8168g" },
    { 0x7C8, 0x488, 38, "RTL8411" },
    { 0x7CF, 0x481, 36, "RTL8168f" },
    { 0x7CF, 0x480, 35, "RTL8168f" },
    { 0x7C8, 0x2C8, 34, "RTL8168evl" },
    { 0x7CF, 0x2C1, 32, "RTL8168e" },
    { 0x7C8, 0x2C0, 33, "RTL8168e" },
    { 0x7CF, 0x281, 25, "RTL8168d" },
    { 0x7C8, 0x280, 26, "RTL8168d" },
    { 0x7CF, 0x28A, 28, "RTL8168dp" },
    { 0x7CF, 0x28B, 31, "RTL8168dp" },
    { 0x7CF, 0x3C9, 23, "RTL8168cp" },
    { 0x7CF, 0x3C8, 18, "RTL8168cp" },
    { 0x7C8, 0x3C8, 24, "RTL8168cp" },
    { 0x7CF, 0x3C0, 19, "RTL8168c" },
    { 0x7CF, 0x3C2, 20, "RTL8168c" },
    { 0x7CF, 0x3C3, 21, "RTL8168c" },
    { 0x7C8, 0x3C0, 22, "RTL8168c" },
    { 0x7C8, 0x380, 17, "RTL8168b" },
    { 0x7C8, 0x448, 39, "RTL8106e" },
    { 0x7C8, 0x440, 37, "RTL8402" },
    { 0x7CF, 0x409, 29, "RTL8105e" },
    { 0x7C8, 0x408, 30, "RTL8105e" },
    { 0x7CF, 0x349, 8,  "RTL8102e" },
    { 0x7CF, 0x249, 8,  "RTL8102e" },
    { 0x7CF, 0x348, 7,  "RTL8102e" },
    { 0x7CF, 0x248, 7,  "RTL8102e" },
    { 0x7CF, 0x240, 14, "RTL8401" },
    { 0x7C8, 0x348, 9,  "RTL8102e" },
    { 0x7C8, 0x248, 9,  "RTL8102e" },
    { 0x7C8, 0x340, 10, "RTL8101e" },
    { 0xFC8, 0x980, 6,  "RTL8169sc" },
    { 0xFC8, 0x180, 5,  "RTL8169sc" },
    { 0xFC8, 0x100, 4,  "RTL8169sb" },
    { 0xFC8, 0x040, 3,  "RTL8110s" },
    { 0xFC8, 0x008, 2,  "RTL8169s" },
    { 0x7CF, 0x7C8, VER_EXT, "" },
    { 0x000, 0x000, 0,  "" }    /* anything else: unknown */
};

/* Linux's and FreeBSD's lists.  1186:4300 also covers the D-Link card
   whose subsystem is 1186:4B10; the Linksys EG1032 is a Realtek only as
   revision 3 (subsystem 0024: the others are Marvell), and the 0001:8168
   board only with subsystem 2410 - both are checked in card_find.  The
   RTL8127 is last so that a machine with another card finds that one
   first; it is found to be refused with a reason */
static const PCI_ID rtl_ids[] = {
    { 0x10EC, 0x8168 },         /* RTL8111/8168 B to H, 8411, 8117 */
    { 0x10EC, 0x8136 },         /* RTL810x, 8106eus, 8107e: 10/100 */
    { 0x10EC, 0x8169 },         /* RTL8169, 8110: PCI */
    { 0x10EC, 0x8167 },         /* RTL8169SC, 8110SC: PCI */
    { 0x10EC, 0x8161 },         /* RTL8168, some boards */
    { 0x10EC, 0x8162 },
    { 0x10EC, 0x8125 },         /* RTL8125: 2.5 gigabit */
    { 0x10EC, 0x3000 },         /* Killer E3000 (RTL8125B) */
    { 0x10EC, 0x8126 },         /* RTL8126: 5 gigabit */
    { 0x10EC, 0x5000 },         /* Killer E5000 (RTL8126) */
    { 0x10EC, 0x2502 },         /* Killer E2500 */
    { 0x10EC, 0x2600 },         /* Killer E2600 */
    { 0x10EC, 0x8129 },
    { 0x10EC, 0x0E10 },
    { 0x1186, 0x4300 },         /* D-Link DGE-528T */
    { 0x1186, 0x4302 },         /* D-Link DGE-530T rev C */
    { 0x1259, 0xC107 },         /* Corega CG-LAPCIGT, Allied Telesis */
    { 0x16EC, 0x0116 },         /* US Robotics 997902 */
    { 0x10FF, 0x8168 },         /* TP-Link TG-3468 v2 */
    { 0x1737, 0x1032 },         /* Linksys EG1032 rev 3 */
    { 0x0001, 0x8168 },
    { 0x10EC, 0x8127 },         /* RTL8127: 10 gigabit, refused */
    { 0, 0 }
};

/* FreeBSD rgephy's DSP fix for revisions 0 and 1 of the 8169S/8110S PHY.
   OP 0 writes VAL, 1 clears the bits in VAL, 2 sets those bits, 3 keeps the
   low twelve bits and puts VAL's top four in */
typedef struct { u8 op, reg; u16 val; } PHYOP;

static const PHYOP dsp_8169s[] = {
    { 0, 31, 0x0001 }, { 0, 21, 0x1000 }, { 0, 24, 0x65C7 },
    { 1, 4, 0x0800 },  { 3, 4, 0x0000 },
    { 0, 3, 0x00A1 },  { 0, 2, 0x0008 },  { 0, 1, 0x1020 },  { 0, 0, 0x1000 },
    { 2, 4, 0x0800 },  { 1, 4, 0x0800 },  { 3, 4, 0x7000 },
    { 0, 3, 0xFF41 },  { 0, 2, 0xDE60 },  { 0, 1, 0x0140 },  { 0, 0, 0x0077 },
    { 3, 4, 0xA000 },
    { 0, 3, 0xDF01 },  { 0, 2, 0xDF20 },  { 0, 1, 0xFF95 },  { 0, 0, 0xFA00 },
    { 3, 4, 0xB000 },
    { 0, 3, 0xFF41 },  { 0, 2, 0xDE20 },  { 0, 1, 0x0140 },  { 0, 0, 0x00BB },
    { 3, 4, 0xF000 },
    { 0, 3, 0xDF01 },  { 0, 2, 0xDF20 },  { 0, 1, 0xFF95 },  { 0, 0, 0xBF00 },
    { 2, 4, 0x0800 },  { 1, 4, 0x0800 },  { 0, 31, 0x0000 }
};

static u32  membase;            /* the memory-mapped registers, or 0 */
static u16  ioport;             /* ...else the first port */
static u8   ver;                /* Linux's RTL_GIGA_MAC_VER_ number */
static u16  xid;                /* the version bits */
static const char *chip_name;
static u16  pci_device;
static u8   gmii;               /* the PHY does gigabit (not device 8136) */
static u8   dash;               /* DASH_*, or 0 */
static u8   dash_on;            /* ...and the BIOS has it switched on */
static u8   macsleep;           /* FreeBSD's deep-sleep wake applies */
static u8   gigabyte;           /* the one 8110SC board with a PHY fix */
static u8   use_io;             /* /IO */
static u8   loaded;             /* card_find has run: the load is over */
static u8   cp_seen;            /* cp_base has been read */
static u16  cp_base;            /* the C+ command, as found and pruned */
static u32  irq_mask;           /* the causes this driver enables */
static u8   tx_stuck;           /* sends in a row that found the transmitter stopped */

static volatile DESC *rxd;
static volatile DESC *txd;
static u8  *rxbuf[RXN];
static u8  *txbuf[TXN];
static int  rx_cur, tx_cur;

/* ---------------------------------------------------------------- */
/* the lines this driver says                                        */
/* ---------------------------------------------------------------- */

static char say_buf[100];
static int  say_len;

static void say_text( const char *text )
{
    while ( *text && say_len < (int)sizeof( say_buf ) - 3 ) say_buf[say_len++] = *text++;
}

static void say_begin( const char *text )
{
    say_len = 0;
    say_text( "RTL8169: " );
    say_text( text );
}

static void say_hex( u32 value, int digits )
{
    static const char hexdig[] = "0123456789ABCDEF";
    int shift;

    for ( shift = ( digits - 1 ) * 4; shift >= 0; shift -= 4 ) {
        if ( say_len < (int)sizeof( say_buf ) - 3 ) say_buf[say_len++] = hexdig[( value >> shift ) & 15];
    }
}

static void say_dec( u32 value )
{
    char digits[12];
    int  count = 0;

    do { digits[count++] = (char)( '0' + value % 10 ); value /= 10; } while ( value && count < 11 );
    while ( count && say_len < (int)sizeof( say_buf ) - 3 ) say_buf[say_len++] = digits[--count];
}

static void say_end( void )
{
    say_buf[say_len++] = '\r';
    say_buf[say_len++] = '\n';
    say_buf[say_len] = 0;
    net_print( say_buf );
}

static void say_line( const char *text )
{
    say_begin( text );
    say_end();
}

/* ---------------------------------------------------------------- */
/* the registers, memory-mapped or through the ports                 */
/* ---------------------------------------------------------------- */

static u8 rd8( u32 reg )
{
    if ( membase ) return MMIO8( membase, reg );
    return inb( (u16)( ioport + reg ) );
}

static u16 rd16( u32 reg )
{
    if ( membase ) return MMIO16( membase, reg );
    return inw( (u16)( ioport + reg ) );
}

static u32 rd32( u32 reg )
{
    if ( membase ) return MMIO32( membase, reg );
    return inl( (u16)( ioport + reg ) );
}

static void wr8( u32 reg, u8 value )
{
    if ( membase ) MMIO8( membase, reg ) = value;
    else outb( (u16)( ioport + reg ), value );
}

static void wr16( u32 reg, u16 value )
{
    if ( membase ) MMIO16( membase, reg ) = value;
    else outw( (u16)( ioport + reg ), value );
}

static void wr32( u32 reg, u32 value )
{
    if ( membase ) MMIO32( membase, reg ) = value;
    else outl( (u16)( ioport + reg ), value );
}

static void set8( u32 reg, u8 bits )   { wr8( reg, (u8)( rd8( reg ) | bits ) ); }
static void clr8( u32 reg, u8 bits )   { wr8( reg, (u8)( rd8( reg ) & ~bits ) ); }
static void set32( u32 reg, u32 bits ) { wr32( reg, rd32( reg ) | bits ); }
static void clr32( u32 reg, u32 bits ) { wr32( reg, rd32( reg ) & ~bits ); }

static void unlock( void ) { wr8( R_CFG9346, CFG9346_UNLOCK ); }
static void lock( void )   { wr8( R_CFG9346, 0x00 ); }

/* Up to COUNT steps of STEP microseconds for the bits in MASK to read
   as WANT; 1 if they did.  Every wait in the driver is one of these */
static int wait8( u32 reg, u8 mask, u8 want, u32 step, u32 count )
{
    u32 i;

    for ( i = 0; i < count; i++ ) {
        if ( ( rd8( reg ) & mask ) == want ) return 1;
        net_delay_us( step );
    }
    return ( rd8( reg ) & mask ) == want;
}

static int wait16( u32 reg, u16 mask, u16 want, u32 step, u32 count )
{
    u32 i;

    for ( i = 0; i < count; i++ ) {
        if ( ( rd16( reg ) & mask ) == want ) return 1;
        net_delay_us( step );
    }
    return ( rd16( reg ) & mask ) == want;
}

static int wait32( u32 reg, u32 mask, u32 want, u32 step, u32 count )
{
    u32 i;

    for ( i = 0; i < count; i++ ) {
        if ( ( rd32( reg ) & mask ) == want ) return 1;
        net_delay_us( step );
    }
    return ( rd32( reg ) & mask ) == want;
}

static int is_8125( void ) { return ver >= 61; }

/* Linux's rtl_is_8168evl_up */
static int evl_up( void ) { return ver >= 34 && ver != 39 && ver <= 52; }

/* ---------------------------------------------------------------- */
/* the windows onto the chip's other registers                       */
/* ---------------------------------------------------------------- */

/* the extended (ERI) registers: TYPE ERI_EXGMAC or ERI_OOB.  The
   8168FP/8117's management registers are F70h << 18 further on */
static u32 eri_read( u32 addr, u32 type )
{
    u32 cmd = type | ERI_MASK_1111 | addr;

    if ( ver == 52 && type == ERI_OOB ) cmd |= 0xF70L << 18;
    wr32( R_ERIAR, cmd );
    if ( !wait32( R_ERIAR, BIT31, BIT31, 100, 100 ) ) return 0xFFFFFFFFL;
    return rd32( R_ERIDR );
}

static void eri_write( u32 addr, u32 mask, u32 value, u32 type )
{
    u32 cmd = BIT31 | type | mask | addr;

    if ( ver == 52 && type == ERI_OOB ) cmd |= 0xF70L << 18;
    wr32( R_ERIDR, value );
    wr32( R_ERIAR, cmd );
    wait32( R_ERIAR, BIT31, 0, 100, 100 );
}

/* the MAC's OCP registers, 8168G and later: no busy bit to wait on */
static void ocp_write( u32 reg, u16 value )
{
    wr32( R_OCPDR, BIT31 | ( reg << 15 ) | value );
}

static u16 ocp_read( u32 reg )
{
    wr32( R_OCPDR, reg << 15 );
    return (u16)rd32( R_OCPDR );
}

static void ocp_modify( u32 reg, u16 clear, u16 set )
{
    ocp_write( reg, (u16)( ( ocp_read( reg ) & ~clear ) | set ) );
}

/* the PHY's OCP registers, 8168G and later; -1 when it does not answer */
static void phy_ocp_write( u32 reg, u16 value )
{
    wr32( R_GPHYOCP, BIT31 | ( reg << 15 ) | value );
    wait32( R_GPHYOCP, BIT31, 0, 25, 10 );
}

static int phy_ocp_read( u32 reg )
{
    wr32( R_GPHYOCP, reg << 15 );
    if ( !wait32( R_GPHYOCP, BIT31, BIT31, 25, 10 ) ) return -1;
    return (int)( rd32( R_GPHYOCP ) & 0xFFFF );
}

/* a PHY register, the standard ones or (before the 8168G) the page in
   register 1Fh.  The chip wants 20us between two accesses */
static void phy_write( int reg, u16 value )
{
    /* Linux's rtl8168g_phy_suspend_quirk: on the 8168g a BMCR write
       that leaves the PHY powered sets ERI 1A8h's top six bits (every
       BMCR write here does) */
    if ( ver == 40 && reg == MII_BMCR ) eri_write( 0x1A8, ERI_MASK_1111, eri_read( 0x1A8, ERI_EXGMAC ) | 0xFC000000L, ERI_EXGMAC );
    if ( ver >= 40 ) {
        phy_ocp_write( OCP_PHY_BASE + (u32)reg * 2, value );
        return;
    }
    if ( ver == 28 || ver == 31 ) clr32( R_DPMDIO, 0x00020000L );
    wr32( R_PHYAR, BIT31 | ( (u32)( reg & 0x1F ) << 16 ) | value );
    wait32( R_PHYAR, BIT31, 0, 25, 20 );
    net_delay_us( 20 );
    if ( ver == 28 || ver == 31 ) set32( R_DPMDIO, 0x00020000L );
}

static int phy_read( int reg )
{
    int value = -1;

    if ( ver >= 40 ) return phy_ocp_read( OCP_PHY_BASE + (u32)reg * 2 );
    if ( ver == 28 || ver == 31 ) clr32( R_DPMDIO, 0x00020000L );
    wr32( R_PHYAR, (u32)( reg & 0x1F ) << 16 );
    if ( wait32( R_PHYAR, BIT31, BIT31, 25, 20 ) ) value = (int)( rd32( R_PHYAR ) & 0xFFFF );
    net_delay_us( 20 );
    if ( ver == 28 || ver == 31 ) set32( R_DPMDIO, 0x00020000L );
    return value;
}

/* the chip's own view of its PCI configuration space, past the 256
   bytes the kernel's mechanism 1 reaches (8126) */
static u32 csi_func( void ) { return ( ( net_pciaddr >> 8 ) & 7 ) << 16; }

static u32 csi_read( u32 addr )
{
    wr32( R_CSIAR, ( addr & 0x0FFF ) | 0xF000L | csi_func() );
    if ( !wait32( R_CSIAR, BIT31, BIT31, 10, 100 ) ) return 0xFFFFFFFFL;
    return rd32( R_CSIDR );
}

static void csi_write( u32 addr, u32 value )
{
    wr32( R_CSIDR, value );
    wr32( R_CSIAR, BIT31 | ( addr & 0x0FFF ) | 0xF000L | csi_func() );
    wait32( R_CSIAR, BIT31, 0, 10, 100 );
}

/* ---------------------------------------------------------------- */
/* DASH: the management controller of the 8168DP/EP, 8117, 8125BP     */
/* ---------------------------------------------------------------- */

static u32 dp_ocp_read( u32 reg )
{
    wr32( R_OCPAR, 0xF000L | ( reg & 0x0FFF ) );
    if ( !wait32( R_OCPAR, BIT31, BIT31, 20, 100 ) ) return 0xFFFFFFFFL;
    return rd32( R_OCPDR );
}

static void dp_ocp_write( u32 reg, u32 value )
{
    wr32( R_OCPDR, value );
    wr32( R_OCPAR, BIT31 | 0x1000L | ( reg & 0x0FFF ) );
    wait32( R_OCPAR, BIT31, 0, 20, 100 );
}

static u32 dp_flag_reg( void ) { return ver == 31 ? 0xB8 : 0x10; }

static void ep_ocp_write( u32 reg, u32 value )
{
    eri_write( reg, ERI_MASK_0001, value, ERI_OOB );
}

static int dash_enabled( void )
{
    if ( dash == DASH_DP ) return ( dp_ocp_read( dp_flag_reg() ) & 0x8000 ) != 0;
    return ( eri_read( 0x128, ERI_OOB ) & 1 ) != 0;
}

/* Linux's rtl8168ep_stop_cmac: the management MAC let go of the wire.
   Linux allows it a hundred seconds; two are allowed here */
static int stop_cmac( void )
{
    int done;

    clr8( R_IBCR2, 0x01 );
    done = wait8( R_IBISR0, 0x20, 0x20, 10000, 200 );
    set8( R_IBISR0, 0x20 );
    clr8( R_IBCR0, 0x01 );
    return done;
}

/* tell the controller a driver has the chip (OOB_START) or is letting
   go (OOB_STOP), and wait a moment for it to answer if it is on */
static void dash_notify( u8 cmd )
{
    int i;

    if ( dash == DASH_DP ) {
        eri_write( 0xE8, ERI_MASK_0001, cmd, ERI_EXGMAC );
        dp_ocp_write( 0x30, 1 );
        for ( i = 0; dash_on && i < 10; i++ ) {
            if ( ( ( dp_ocp_read( dp_flag_reg() ) & 0x0800 ) != 0 ) == ( cmd == OOB_START ) ) break;
            net_delay_us( 10000 );
        }
    } else if ( dash == DASH_EP ) {
        if ( cmd == OOB_STOP ) stop_cmac();
        ep_ocp_write( 0x180, cmd );
        ep_ocp_write( 0x30, eri_read( 0x30, ERI_OOB ) | 1 );
        for ( i = 0; dash_on && i < 30; i++ ) {
            if ( ( ( eri_read( 0x124, ERI_OOB ) & 1 ) != 0 ) == ( cmd == OOB_START ) ) break;
            net_delay_us( 10000 );
        }
    } else if ( dash == DASH_25BP ) {
        ep_ocp_write( 0x14, cmd );
        ep_ocp_write( 0x18, 0 );
        ep_ocp_write( 0x10, 1 );
    }
}

/* ---------------------------------------------------------------- */
/* the PCI function                                                  */
/* ---------------------------------------------------------------- */

static u32 cfg_read( int reg, int bytes ) { return net_pci_cfg_read( net_pciaddr, reg, bytes ); }
static void cfg_write( int reg, int bytes, u32 value ) { net_pci_cfg_write( net_pciaddr, reg, bytes, value ); }

/* the offset of capability ID in the list, or 0 */
static int pci_cap( int id )
{
    int at, guard;

    if ( !( cfg_read( 0x06, 2 ) & 0x0010 ) ) return 0;
    at = (int)( cfg_read( 0x34, 1 ) & 0xFC );
    for ( guard = 0; at >= 0x40 && guard < 48; guard++ ) {
        if ( (int)( cfg_read( at, 1 ) & 0xFF ) == id ) return at;
        at = (int)( cfg_read( at + 1, 1 ) & 0xFC );
    }
    return 0;
}

/* The function awake (D0), answering on its INTx line (MSI, MSI-X and
   the INTx-disable bit off: the kernel hooks the line), and PCIe ASPM
   off on this end of the link, as FreeBSD's re and both rge drivers
   do.  Then I/O, memory and bus mastering on. */
static void pci_prepare( int first )
{
    int  at;
    u32  value;

    at = pci_cap( 0x01 );
    if ( at ) {
        value = cfg_read( at + 4, 2 );
        if ( value != 0xFFFFFFFFL && ( value & 3 ) ) {
            cfg_write( at + 4, 2, value & ~0x8003L );
            net_delay_us( 10000 );              /* D3hot to D0 */
            if ( first ) {
                say_begin( "woken from power state D" );
                say_dec( value & 3 );
                say_end();
            }
        }
    }
    value = cfg_read( 0x04, 2 );
    if ( value != 0xFFFFFFFFL && ( value & 0x0400 ) ) cfg_write( 0x04, 2, value & ~0x0400L );
    net_pci_enable( net_pciaddr );
    at = pci_cap( 0x05 );
    if ( at ) {
        value = cfg_read( at + 2, 2 );
        if ( value != 0xFFFFFFFFL && ( value & 0x0001 ) ) cfg_write( at + 2, 2, value & ~0x0001L );
    }
    at = pci_cap( 0x11 );
    if ( at ) {
        value = cfg_read( at + 2, 2 );
        if ( value != 0xFFFFFFFFL && ( value & 0x8000 ) ) cfg_write( at + 2, 2, value & ~0x8000L );
    }
    at = pci_cap( 0x10 );
    if ( at ) {
        value = cfg_read( at + 0x10, 2 );       /* link control */
        if ( value != 0xFFFFFFFFL && ( value & 0x0103 ) ) cfg_write( at + 0x10, 2, value & ~0x0103L );
    }
}

/* Is /NAME on the DEVICE= line?  A letter or digit straight after the
   name makes it another switch */
static int has_switch( const char *name )
{
    const char *at = net_line;
    char ch;
    int  i;

    if ( at == 0 ) return 0;
    while ( *at && *at != ' ' && *at != '\t' && *at != '\r' && *at != '\n' ) at++;
    for ( ; *at && *at != '\r' && *at != '\n'; at++ ) {
        if ( *at != '/' ) continue;
        for ( i = 0; name[i] && ( at[1 + i] | 0x20 ) == ( name[i] | 0x20 ); i++ ) ;
        if ( name[i] ) continue;
        ch = (char)( at[1 + i] | 0x20 );
        if ( ( ch >= 'a' && ch <= 'z' ) || ( ch >= '0' && ch <= '9' ) ) continue;
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* the chip                                                          */
/* ---------------------------------------------------------------- */

static void irq_off( void )
{
    if ( is_8125() ) {
        wr32( R_IMR_8125, 0 );
        wr32( R_ISR_8125, 0xFFFFFFFFL );
    } else {
        wr16( R_IMR, 0 );
        wr16( R_ISR, 0xFFFF );
    }
    (void)rd8( R_CHIPCMD );
}

/* what is pending acknowledged first, then the mask: a level line left
   asserted with nobody answering is shut by the kernel */
static void irq_on( void )
{
    u32 mask = ( net_flags & NICF_POLLED ) ? 0 : irq_mask;

    if ( is_8125() ) {
        wr32( R_ISR_8125, 0xFFFFFFFFL );
        wr32( R_IMR_8125, mask );
    } else {
        wr16( R_ISR, 0xFFFF );
        wr16( R_IMR, (u16)mask );
    }
}

static void tx_kick( void )
{
    if ( is_8125() ) wr16( R_TXPOLL_8125, 0x0001 );
    else wr8( R_TXPOLL, TXPOLL_NPQ );
}

/* Linux's rtl_wait_txrx_fifo_empty */
static void fifo_empty_wait( void )
{
    if ( ver >= 40 && ver <= 52 ) {
        wait32( R_TXCONFIG, TXCFG_EMPTY, TXCFG_EMPTY, 100, 42 );
        wait8( R_MCU, MCU_RX_EMPTY | MCU_TX_EMPTY, MCU_RX_EMPTY | MCU_TX_EMPTY, 100, 42 );
    } else if ( ver == 61 ) {
        wait8( R_MCU, MCU_RX_EMPTY | MCU_TX_EMPTY, MCU_RX_EMPTY | MCU_TX_EMPTY, 100, 42 );
    } else if ( ver > 61 ) {
        set8( R_CHIPCMD, CMD_STOPREQ );
        wait8( R_MCU, MCU_RX_EMPTY | MCU_TX_EMPTY, MCU_RX_EMPTY | MCU_TX_EMPTY, 100, 42 );
        wait16( R_INTRMIT, 0x0103, 0x0103, 100, 42 );
    }
}

/* the receiver gated off from the wire (8168G and later) */
static void rxdv_gate_on( void )
{
    set32( R_MISC, MISC_RXDV_GATE );
    net_delay_us( 2000 );
    fifo_empty_wait();
}

static void link_list_wait( void )
{
    wait8( R_MCU, MCU_LL_READY, MCU_LL_READY, 100, 42 );
}

/* Linux's rtl_hw_initialize: the chip taken back from the management
   firmware (out of OOB, the E8DEh handshake) on the 8168G and later */
static void hw_initialize( int first )
{
    if ( ver == 51 || ver == 52 ) {
        if ( !stop_cmac() && first ) say_line( "the management MAC did not stop" );
    }
    if ( ver < 40 ) return;
    rxdv_gate_on();
    clr8( R_CHIPCMD, CMD_TXEN | CMD_RXEN );
    net_delay_us( 1000 );
    clr8( R_MCU, MCU_NOW_IS_OOB );
    ocp_modify( 0xE8DE, 0x4000, 0 );
    link_list_wait();
    if ( is_8125() ) {
        ocp_write( 0xC0AA, 0x07D0 );
        ocp_write( 0xC0A6, 0x0150 );
        ocp_write( 0xC01E, 0x5555 );
    } else {
        ocp_modify( 0xE8DE, 0, 0x8000 );
    }
    link_list_wait();
}

/* the software reset: 10ms, as Linux allows */
static int hw_reset( void )
{
    int done;

    wr8( R_CHIPCMD, CMD_RESET );
    done = wait8( R_CHIPCMD, CMD_RESET, 0, 100, 100 );
    if ( ver <= 6 ) wr8( R_MACRESET, 0x01 );    /* FreeBSD's re_reset */
    if ( ver == 2 || ver == 3 ) {
        /* FreeBSD's re_reset again, and the end of Linux's 8169S table:
           PHY register 0Bh cleared, on page 0, after every reset */
        phy_write( 0x1F, 0x0000 );
        phy_write( 0x0B, 0x0000 );
    }
    return done;
}

/* Linux's rtl8169_cleanup, before a reset: no more frames accepted, and
   what is moving given time to stop, in the way each version wants */
static void chip_quiesce( void )
{
    clr32( R_RXCONFIG, RX_ACCEPT_ALL );
    if ( ver == 28 || ver == 31 ) {
        wait8( R_TXPOLL, TXPOLL_NPQ, 0, 20, 2000 );
    } else if ( ver >= 34 && ver <= 38 ) {
        set8( R_CHIPCMD, CMD_STOPREQ );
        wait32( R_TXCONFIG, TXCFG_EMPTY, TXCFG_EMPTY, 100, 666 );
    } else if ( ver >= 40 ) {
        rxdv_gate_on();
        net_delay_us( 2000 );
    } else {
        set8( R_CHIPCMD, CMD_STOPREQ );
        net_delay_us( 100 );
    }
}

/* the station address: where Linux reads it, else the ID registers */
static int read_mac( void )
{
    u32 low, high;
    int i;

    net_memzero( net_mac, 6 );
    if ( evl_up() && ver != 34 ) {
        low = eri_read( 0xE0, ERI_EXGMAC );
        high = eri_read( 0xE4, ERI_EXGMAC );
        for ( i = 0; i < 4; i++ ) net_mac[i] = (u8)( low >> ( i * 8 ) );
        net_mac[4] = (u8)high;
        net_mac[5] = (u8)( high >> 8 );
    } else if ( is_8125() ) {
        for ( i = 0; i < 6; i++ ) net_mac[i] = rd8( R_MAC0BKP + i );
    }
    if ( net_mac_ok( net_mac ) ) return 1;
    for ( i = 0; i < 6; i++ ) net_mac[i] = rd8( R_IDR0 + i );
    return net_mac_ok( net_mac );
}

/* and written back into the ID registers the receive filter uses, in
   doublewords with the configuration unlocked (Linux's rtl_rar_set) */
static void write_mac( void )
{
    u32 low = (u32)net_mac[0] | ( (u32)net_mac[1] << 8 ) | ( (u32)net_mac[2] << 16 ) | ( (u32)net_mac[3] << 24 );
    u32 high = (u32)net_mac[4] | ( (u32)net_mac[5] << 8 );

    unlock();
    wr32( R_IDR0 + 4, high );
    (void)rd8( R_CHIPCMD );
    wr32( R_IDR0, low );
    (void)rd8( R_CHIPCMD );
    if ( ver == 34 ) {
        eri_write( 0xE0, ERI_MASK_1111, low, ERI_EXGMAC );
        eri_write( 0xE4, ERI_MASK_1111, high, ERI_EXGMAC );
        eri_write( 0xF0, ERI_MASK_1111, ( low & 0xFFFF ) << 16, ERI_EXGMAC );
        eri_write( 0xF4, ERI_MASK_1111, ( low >> 16 ) | ( high << 16 ), ERI_EXGMAC );
    }
    lock();
}

/* ---------------------------------------------------------------- */
/* the PHY                                                           */
/* ---------------------------------------------------------------- */

static void phy_dsp_8169s( void )
{
    int i, value;

    for ( i = 0; i < (int)( sizeof( dsp_8169s ) / sizeof( dsp_8169s[0] ) ); i++ ) {
        if ( dsp_8169s[i].op == 0 ) {
            phy_write( dsp_8169s[i].reg, dsp_8169s[i].val );
            continue;
        }
        value = phy_read( dsp_8169s[i].reg );
        if ( value < 0 ) value = 0;
        if ( dsp_8169s[i].op == 1 ) value &= ~dsp_8169s[i].val;
        else if ( dsp_8169s[i].op == 2 ) value |= dsp_8169s[i].val;
        else value = ( value & 0x0FFF ) | dsp_8169s[i].val;
        phy_write( dsp_8169s[i].reg, (u16)value );
    }
    net_delay_us( 40 );
}

/* The PHY awake and negotiating everything it can.  After a reset it
   is, and then nothing is written; a previous OS may have left it
   powered down, isolated, forced or advertising less, and that is put
   back and negotiation restarted. */
static void phy_start( int first )
{
    int bmcr, anar, gbcr, adv, id2, want, i, restart = 0;

    if ( ver == 14 ) clr8( R_DBG, 0x08 );
    if ( ver == 14 || ver == 25 || ver == 26 || ver == 29 || ver == 30 ||
         ver == 32 || ver == 33 || ver == 37 || ver == 39 ) set8( R_PMCH, 0x80 );
    if ( is_8125() ) set8( R_PMCH, 0xC0 );
    if ( macsleep ) {
        if ( rd8( R_MACDBG ) & 0x80 ) set8( R_GPIO, 0x01 );
        else clr8( R_GPIO, 0x01 );
    }
    if ( ver < 40 ) phy_write( 0x1F, 0x0000 );          /* page 0 */
    if ( ( ver >= 17 && ver <= 28 ) || ver == 31 ) phy_write( 0x0E, 0x0000 );
    if ( ver == 2 || ver == 3 ) {
        id2 = phy_read( MII_ID2 );
        if ( id2 >= 0 && ( ( id2 >> 4 ) & 0x3F ) == 0x11 && ( id2 & 0x0F ) < 2 ) {
            phy_dsp_8169s();
            restart = 1;
        }
    }
    if ( ver == 5 && gigabyte ) {
        phy_write( 0x1F, 0x0001 );
        phy_write( 0x10, 0xF01B );
        phy_write( 0x1F, 0x0000 );
    }

    bmcr = phy_read( MII_BMCR );
    if ( bmcr < 0 || bmcr == 0xFFFF ) {
        if ( first ) say_line( "the PHY does not answer" );
        return;
    }
    if ( bmcr & ( BMCR_PDOWN | BMCR_ISOLATE ) ) {
        phy_write( MII_BMCR, BMCR_ANENABLE );
        if ( is_8125() ) {
            for ( i = 0; i < 100; i++ ) {
                if ( ( phy_ocp_read( 0xA420 ) & 7 ) == 3 ) break;
                net_delay_us( 1000 );
            }
        } else {
            net_delay_us( 20000 );
        }
        restart = 1;
    } else if ( !( bmcr & BMCR_ANENABLE ) ) {
        restart = 1;
    }
    anar = phy_read( MII_ANAR );
    if ( anar >= 0 && ( ( anar & 0x01E0 ) != 0x01E0 || ( anar & 0x001F ) != 0x0001 ) ) {
        /* 10 and 100, half and full, the 802.3 selector; the two pause
           bits kept as they were and nothing else */
        phy_write( MII_ANAR, (u16)( ( anar & 0x0C00 ) | 0x01E1 ) );
        restart = 1;
    }
    if ( gmii ) {
        gbcr = phy_read( MII_GBCR );
        if ( gbcr >= 0 && ( gbcr & 0x0300 ) != 0x0300 ) {
            phy_write( MII_GBCR, (u16)( gbcr | 0x0300 ) );
            restart = 1;
        }
    }
    if ( is_8125() ) {
        want = ( ver == 70 ) ? 0x0180 : 0x0080;         /* 2.5G, and 5G on the 8126 */
        adv = phy_ocp_read( 0xA5D4 );
        if ( adv >= 0 && ( adv & want ) != want ) {
            phy_ocp_write( 0xA5D4, (u16)( adv | want ) );
            restart = 1;
        }
    }
    if ( !restart ) return;
    phy_write( MII_BMCR, BMCR_ANENABLE | BMCR_ANRESTART );
    if ( first ) {
        say_begin( "PHY BMCR " );
        say_hex( (u32)bmcr, 4 );
        say_text( "h ANAR " );
        say_hex( anar < 0 ? 0 : (u32)anar, 4 );
        say_text( "h, negotiating again" );
        say_end();
    }
}

static u32 link_bits( void )
{
    if ( is_8125() ) return rd16( R_PHYSTATUS );
    return rd8( R_PHYSTATUS );
}

/* "Realtek RTL8168h, 1000 Mbps": 31 characters at the most */
static void set_name( u32 phys )
{
    char *end;

    net_strcpy( net_name, "Realtek " );
    net_strcpy( net_name + 8, chip_name );
    if ( !( phys & PHYS_LINK ) ) return;
    end = net_name;
    while ( *end ) end++;
    if ( is_8125() && ( phys & PHYS_5000 ) ) net_strcpy( end, ", 5000 Mbps" );
    else if ( is_8125() && ( phys & PHYS_2500 ) ) net_strcpy( end, ", 2500 Mbps" );
    else if ( phys & PHYS_1000 ) net_strcpy( end, ", 1000 Mbps" );
    else if ( phys & PHYS_100 ) net_strcpy( end, ", 100 Mbps" );
    else if ( phys & PHYS_10 ) net_strcpy( end, ", 10 Mbps" );
}

/* ---------------------------------------------------------------- */
/* starting it                                                       */
/* ---------------------------------------------------------------- */

static void rx_give( int entry )
{
    volatile DESC *desc = &rxd[entry];

    desc->addr_lo = (u32)rxbuf[entry];
    desc->addr_hi = 0;
    desc->opts2 = 0;
    desc->opts1 = DESC_OWN | ( entry == RXN - 1 ? DESC_EOR : 0 ) | RXBUF;
}

static void rings_init( void )
{
    int i;

    for ( i = 0; i < RXN; i++ ) rx_give( i );
    for ( i = 0; i < TXN; i++ ) {
        txd[i].addr_lo = (u32)txbuf[i];
        txd[i].addr_hi = 0;
        txd[i].opts2 = 0;
        txd[i].opts1 = ( i == TXN - 1 ) ? DESC_EOR : 0;
    }
    rx_cur = 0;
    tx_cur = 0;
}

/* RxConfig's FIFO, DMA and early-interrupt bits, per Linux's rtl_init_rxcfg */
static u32 rx_config( void )
{
    if ( ver <= 6 || ( ver >= 10 && ver <= 17 ) ) return 0x0000E700L;
    if ( ( ver >= 18 && ver <= 24 ) || ( ver >= 34 && ver <= 36 ) || ver == 38 ) return 0x0000C700L;
    if ( ver >= 40 && ver <= 52 ) return 0x0000CF00L;
    if ( ver == 61 ) return 0x40000700L;
    if ( ver > 61 ) return 0x40000F00L;
    return 0x00008700L;
}

static u16 rx_max_size( void )
{
    if ( ver <= 6 || ver == 17 ) return 16383;
    if ( is_8125() ) return 16384;
    return RXBUF;
}

/* ASPM and clock requests off inside the chip (Linux's
   rtl_hw_aspm_clkreq_enable( false ), 8168E and later) */
static void chip_aspm_off( void )
{
    if ( ver < 32 ) return;
    if ( ver == 46 || ver == 48 || ver == 52 || is_8125() ) ocp_modify( 0xE092, 0x00FF, 0 );
    if ( ver == 70 ) clr8( R_INTCFG0, 0x08 );
    else clr8( R_CONFIG2, CFG2_CLKREQ );
    clr8( R_CONFIG5, CFG5_ASPM );
}

/* Linux's rtl_jumbo_config for a standard frame */
static void jumbo_off( void )
{
    if ( ver == 17 ) {
        clr8( R_CONFIG4, 0x01 );
    } else if ( ver >= 18 && ver <= 26 ) {
        clr8( R_CONFIG3, CFG3_JUMBO0 );
        clr8( R_CONFIG4, 0x02 );
    } else if ( ver == 28 ) {
        clr8( R_CONFIG3, CFG3_JUMBO0 );
    } else if ( ver >= 31 && ver <= 33 ) {
        wr8( R_TXMAXSIZE, 0x3F );
        clr8( R_CONFIG3, CFG3_JUMBO0 );
        clr8( R_CONFIG4, 0x01 );
    }
}

/* the receive packet filter restarted (Linux's rtl_reset_packet_filter) */
static void packet_filter_reset( void )
{
    eri_write( 0xDC, ERI_MASK_1111, eri_read( 0xDC, ERI_EXGMAC ) & ~1L, ERI_EXGMAC );
    eri_write( 0xDC, ERI_MASK_1111, eri_read( 0xDC, ERI_EXGMAC ) | 1L, ERI_EXGMAC );
}

/* the 8168 and 8101 steps that are kept (see the top of the file) */
static void start_8168( void )
{
    if ( ver == 32 || ver == 33 ) {
        set32( R_MISC, MISC_TXPLA_RST );            /* the transmit FIFO pointer */
        clr32( R_MISC, MISC_TXPLA_RST );
    }
    if ( ver >= 34 && ver <= 38 ) clr8( R_MCU, MCU_NOW_IS_OOB );
    if ( ( ver >= 34 && ver <= 38 ) || ( ver >= 40 && ver <= 52 ) ) packet_filter_reset();
    if ( ver == 37 || ver == 39 ) eri_write( 0x1B0, ERI_MASK_0011, 0, ERI_EXGMAC );
    if ( ver >= 40 ) clr32( R_MISC, MISC_RXDV_GATE );
}

/* Linux's rtl_hw_start_8125 and rtl_hw_start_8125_common, but for the
   ASPM, LTR, EEE timer, statistics and PCIe PHY parts */
static void start_8125( void )
{
    u32 reg, end;
    u32 value;

    wr8( R_INTCFG0, 0x00 );
    end = ( ver == 63 || ver == 70 ) ? 0x80 : 0x100;
    for ( reg = 0; reg < end; reg += 4 ) wr32( R_COALESCE + reg, 0 );
    if ( ver == 63 || ver == 70 ) wr16( R_INTCFG1, 0x0000 );
    if ( ver == 70 ) {
        value = csi_read( 0x890 );                  /* the ZRxDC timeout */
        if ( value != 0xFFFFFFFFL ) csi_write( 0x890, value & ~1L );
    }

    clr8( R_CONFIG3, CFG3_RDY_L23 );
    wr16( R_8125_382, 0x221B );
    wr32( R_RSSCTRL, 0 );
    wr16( R_QNUMCTRL, 0 );
    ocp_modify( 0xD40A, 0x0010, 0x0000 );          /* UPS off */
    clr8( R_CONFIG1, 0x10 );
    ocp_write( 0xC140, 0xFFFF );
    ocp_write( 0xC142, 0xFFFF );
    ocp_modify( 0xD3E2, 0x0FFF, 0x03A9 );
    ocp_modify( 0xD3E4, 0x00FF, 0x0000 );
    ocp_modify( 0xE860, 0x0000, 0x0080 );
    ocp_modify( 0xEB58, 0x0001, 0x0000 );          /* the old transmit descriptors */
    if ( ver == 70 ) clr8( R_DESCTYPE, 0x02 );     /* and the old receive ones */
    ocp_modify( 0xE614, 0x0700, ver == 70 ? 0x0400 : ver == 63 ? 0x0200 : 0x0300 );
    ocp_modify( 0xE63E, 0x0C30, ver == 63 ? 0x0000 : 0x0020 );
    ocp_modify( 0xC0B4, 0x0000, 0x000C );
    ocp_modify( 0xEB6A, 0x00FF, 0x0033 );
    ocp_modify( 0xEB50, 0x03E0, 0x0040 );
    ocp_modify( 0xE056, 0x00F0, 0x0000 );
    ocp_modify( 0xE040, 0x1000, 0x0000 );
    ocp_modify( 0xEA1C, 0x0003, 0x0001 );
    if ( ver == 70 ) ocp_modify( 0xEA1C, 0x0300, 0x0000 );
    else ocp_modify( 0xEA1C, 0x0004, 0x0000 );
    ocp_modify( 0xE0C0, 0x4F0F, 0x4403 );
    ocp_modify( 0xE052, 0x0080, 0x0068 );
    ocp_modify( 0xD430, 0x0FFF, 0x047F );
    ocp_modify( 0xEA1C, 0x0004, 0x0000 );
    ocp_modify( 0xEB54, 0x0000, 0x0001 );
    net_delay_us( 1 );
    ocp_modify( 0xEB54, 0x0001, 0x0000 );
    wr16( R_8125_1880, (u16)( rd16( R_8125_1880 ) & ~0x0030 ) );
    ocp_write( 0xE098, 0xC302 );
    for ( reg = 0; reg < 10; reg++ ) {
        if ( !( ocp_read( 0xE00E ) & 0x2000 ) ) break;
        net_delay_us( 1000 );
    }
    clr32( R_MISC, MISC_RXDV_GATE );
}

/* Linux's rtl_hw_start, with the rings already built */
static void hw_start( void )
{
    u16 cp = cp_base;
    u32 value;

    unlock();
    chip_aspm_off();
    if ( ver <= 6 ) {
        cp |= CP_PCIMULRW;
        if ( ver <= 3 ) cp |= CP_ENANAPLL;
        wr16( R_CPLUSCMD, cp );
        wr8( R_TXMAXSIZE, 0x3F );                   /* no early transmit */
        if ( ver == 5 || ver == 6 ) {
            value = ( ver == 5 ) ? 0x000FFF00L : 0x00FFFF00L;
            if ( rd8( R_CONFIG2 ) & 0x01 ) value |= 0xFF;     /* a 66MHz bus */
            wr32( R_PARA7C, value );
        }
        wr16( R_INTRMIT, 0x0000 );
    } else if ( !is_8125() ) {
        wr16( R_CPLUSCMD, cp );
        wr8( R_TXMAXSIZE, (u8)( evl_up() ? 0x27 : 0x3F ) );
        start_8168();
        wr16( R_INTRMIT, 0x0000 );
    } else {
        wr16( R_CPLUSCMD, cp );
        start_8125();
    }
    wr16( R_RXMAXSIZE, rx_max_size() );
    wr32( R_TXDESC_HI, 0 );                         /* high half first, as Linux */
    wr32( R_TXDESC_LO, (u32)txd );
    wr32( R_RXDESC_HI, 0 );
    wr32( R_RXDESC_LO, (u32)rxd );
    if ( is_8125() ) wr32( R_RSSCTRL, 0 );
    jumbo_off();
    lock();
    (void)rd8( R_CHIPCMD );

    /* transmit and receive on, then their configuration: the 8169's
       TxConfig is taken only with the transmitter on */
    wr8( R_CHIPCMD, CMD_TXEN | CMD_RXEN );
    wr32( R_RXCONFIG, rx_config() );
    wr32( R_TXCONFIG, TX_CONFIG );
    /* every multicast (NetBEUI's 03-00-00-00-00-01 included) */
    wr32( R_MAR0 + 4, 0xFFFFFFFFL );
    wr32( R_MAR0, 0xFFFFFFFFL );
    wr32( R_RXCONFIG, ( rd32( R_RXCONFIG ) & ~RX_ACCEPT_ALL ) | RX_ACCEPT );
}

/* stopped, reset and started again, the rings rebuilt; interrupts are
   left masked */
static int chip_bringup( void )
{
    irq_off();
    chip_quiesce();
    if ( !hw_reset() ) return 0;
    irq_off();
    rings_init();
    hw_start();
    return 1;
}

/* ---------------------------------------------------------------- */
/* card_find                                                         */
/* ---------------------------------------------------------------- */

int card_find( void )
{
    const PCI_ID *which;
    const CHIP *chip;
    int  first = !loaded;
    u32  txcfg, at, mem_phys = 0;
    int  bar, isio, mem_bar = -1;
    u8  *pool;
    int  i;

    loaded = 1;
    net_strcpy( net_name, "Realtek RTL8169" );
    if ( !net_pci_find( rtl_ids, &which ) ) return 1;
    pci_device = which->device;
    gmii = (u8)( pci_device != 0x8136 );
    if ( which->vendor == 0x1737 && cfg_read( 0x2E, 2 ) != 0x0024 ) return 1;
    if ( which->vendor == 0x0001 && cfg_read( 0x2E, 2 ) != 0x2410 ) return 1;
    gigabyte = (u8)( cfg_read( 0x2C, 2 ) == 0x1458 && cfg_read( 0x2E, 2 ) == 0xE000 );
    if ( first ) use_io = (u8)has_switch( "IO" );
    pci_prepare( first );

    /* the registers: the first memory BAR of 0-2, else the ports */
    membase = 0;
    ioport = 0;
    for ( bar = 0; bar < 3; bar++ ) {
        at = net_pci_bar( net_pciaddr, bar, &isio );
        if ( at == 0 ) continue;
        if ( isio ) {
            if ( ioport == 0 && at <= 0xFF00 ) ioport = (u16)at;
        } else if ( mem_phys == 0 ) {
            mem_phys = at;
            mem_bar = bar;
        }
    }
    if ( mem_phys && ( !( use_io || pci_device == 0x8167 ) || ioport == 0 ) ) {
        membase = net_pci_map( mem_phys, 0x1000 );
        if ( membase ) ioport = 0;
        else if ( first ) say_line( "the memory registers could not be mapped" );
    }
    if ( membase == 0 && ioport == 0 ) {
        if ( first ) say_line( "no registers the driver can reach" );
        return 1;
    }

    /* which chip */
    txcfg = rd32( R_TXCONFIG );
    if ( txcfg == 0xFFFFFFFFL ) {
        if ( first ) say_line( "the chip does not answer (TxConfig reads FFFFFFFFh)" );
        return 1;
    }
    xid = (u16)( ( txcfg >> 20 ) & 0xFCF );
    for ( chip = chips; ( xid & chip->mask ) != chip->bits; chip++ ) ;
    ver = chip->ver;
    chip_name = chip->name;
    if ( ver == 42 && !gmii ) { ver = 43; chip_name = "RTL8106eus"; }
    if ( ver == 46 && !gmii ) { ver = 48; chip_name = "RTL8107e"; }
    if ( ver == 9 && ( xid & 0xF04 ) == 0x304 ) chip_name = "RTL8103e";
    macsleep = (u8)( ( xid & 0x7CF ) == 0x3C2 || ( xid & 0x7CC ) == 0x3C4 || ( xid & 0x7CC ) == 0x34C );
    if ( first ) {
        say_begin( "" );
        say_hex( which->vendor, 4 );
        say_text( ":" );
        say_hex( which->device, 4 );
        say_text( ", version bits " );
        say_hex( xid, 3 );
        say_text( "h" );
        if ( ver != 0 && ver != VER_EXT ) {
            say_text( ", " );
            say_text( chip_name );
        }
        if ( membase ) {
            say_text( ", memory BAR " );
            say_dec( (u32)mem_bar );
        } else {
            say_text( ", ports " );
            say_hex( ioport, 4 );
            say_text( "h" );
        }
        say_end();
    }
    if ( ver == 0 || ver == VER_EXT ) {
        if ( first ) {
            say_begin( "unknown chip, version bits " );
            say_hex( xid, 3 );
            say_text( "h" );
            say_end();
        }
        return 1;
    }
    if ( ver == 80 ) {
        if ( first ) say_line( "the RTL8127A (10 gigabit) is not supported" );
        return 1;
    }
    set_name( 0 );                  /* NETCORE's "not found" names the chip */
    if ( is_8125() ) {
        /* registers up to 4800h: the I/O window is too small */
        if ( mem_phys == 0 ) {
            if ( first ) say_line( "this chip needs memory registers PM-DOS can reach" );
            return 1;
        }
        if ( membase == 0 && first ) say_line( "this chip needs the memory BAR: /IO is not used" );
        membase = net_pci_map( mem_phys, 0x10000L );
        if ( membase == 0 ) {
            if ( first ) say_line( "the memory registers could not be mapped" );
            return 1;
        }
        ioport = 0;
    }

    irq_off();
    if ( !cp_seen ) {
        /* what Linux keeps of the C+ command it finds; receive checksums
           and VLAN stripping off - but the 8110SCd's stripping stays on */
        cp_base = (u16)( rd16( R_CPLUSCMD ) & CP_KEEP & ~CP_RXCHKSUM );
        if ( ver != 5 ) cp_base &= ~CP_RXVLAN;
        cp_seen = 1;
    }
    if ( ver <= 6 ) {
        unlock();
        clr8( R_CONFIG2, CFG2_MSI );
        lock();
        cfg_write( 0x0D, 1, 0x40 );                 /* latency timer */
        cfg_write( 0x0C, 1, 0x08 );                 /* cache line: 32 bytes */
    }
    if ( ver == 28 || ver == 31 ) dash = DASH_DP;
    else if ( ver == 51 || ver == 52 ) dash = DASH_EP;
    else if ( ver == 66 ) dash = DASH_25BP;
    else dash = 0;
    if ( dash ) {
        dash_on = (u8)dash_enabled();
        if ( first && dash_on ) say_line( "DASH remote management is on" );
    }
    irq_mask = ISR_RXOK | ISR_RXERR | ISR_TXOK | ISR_TXERR | ISR_RXDU;
    if ( ver <= 6 ) irq_mask |= ISR_SYSERR | ISR_FIFOOVER;

    hw_initialize( first );
    if ( !hw_reset() ) {
        if ( first ) say_line( "the reset did not finish" );
        return 1;
    }
    if ( !read_mac() ) {
        if ( first ) say_line( "no usable station address" );
        return 1;
    }
    write_mac();

    if ( rxd == 0 ) {
        pool = net_alloc( 256 + RXN * 16 + 256 + ( RXN + TXN ) * BUFSTEP );
        if ( pool == 0 ) {
            if ( first ) say_line( "no memory for the rings" );
            return 1;
        }
        at = ( (u32)pool + 255 ) & ~255L;
        rxd = (volatile DESC *)at;  at += RXN * 16;
        txd = (volatile DESC *)at;  at += 256;      /* 128 used, 256-aligned */
        for ( i = 0; i < RXN; i++ ) { rxbuf[i] = (u8 *)at; at += BUFSTEP; }
        for ( i = 0; i < TXN; i++ ) { txbuf[i] = (u8 *)at; at += BUFSTEP; }
    }

    /* the line the card has, at the DEVICE= load; after a park the line
       is the one nd_init settled on and hooked, and stays */
    if ( first ) net_irq = (u8)net_pci_irqline( net_pciaddr );
    net_bus = NICB_PCI;
    net_flags = (u8)( NICF_WORD | ( net_flags & NICF_POLLED ) );
    net_port = membase ? 0 : ioport;
    net_mem = membase;
    set_name( 0 );
    return 0;
}

/* ---------------------------------------------------------------- */
/* card_setup, card_stop                                             */
/* ---------------------------------------------------------------- */

int card_setup( void )
{
    int first = ( net_line != 0 );
    u32 phys;
    int i;

    irq_off();
    if ( dash ) dash_notify( OOB_START );
    phy_start( first );
    if ( !chip_bringup() ) {
        if ( first ) say_line( "the reset did not finish" );
        return 1;
    }
    /* at the load, up to four seconds for the other end to agree; the
       stack works when the cable is put in later, so no link is not a
       failure */
    if ( first ) {
        for ( i = 0; i < LINK_WAIT; i++ ) {
            if ( link_bits() & PHYS_LINK ) break;
            net_delay_us( 10000 );
        }
    }
    phys = link_bits();
    set_name( phys );
    if ( first && !( phys & PHYS_LINK ) ) {
        say_len = 0;
        say_text( net_name );
        say_text( ": no link" );
        say_end();
    }
    irq_on();
    return 0;
}

void card_stop( void )
{
    irq_off();
    chip_quiesce();
    hw_reset();
    irq_off();
    if ( dash ) dash_notify( OOB_STOP );
}

/* ---------------------------------------------------------------- */
/* sending and receiving                                             */
/* ---------------------------------------------------------------- */

static void rx_drain( void );

int card_send( const u8 *frame, u32 len )
{
    volatile DESC *desc;
    int  entry;
    u8  *buf;
    u32  n, use, eor, flags, whole;

    if ( len > NIC_MAXFRAME ) { net_stats.txerrors++; return 0; }

    /* the descriptor wanted still holds a frame being sent.  With no
       link there is nothing to wait for; with one, the poll is written
       again now and then in case the chip missed one */
    if ( txd[tx_cur].opts1 & DESC_OWN ) {
        if ( !( link_bits() & PHYS_LINK ) ) { net_stats.txerrors++; return 0; }
        for ( n = 0; n < TX_WAIT; n++ ) {
            if ( !( txd[tx_cur].opts1 & DESC_OWN ) ) break;
            if ( ( n & 63 ) == 0 ) tx_kick();
            net_delay_us( 10 );
        }
    }

    /* From here the ring is not to be restarted under us (card_service
       does that on a PCI error): interrupts off for the post.  The copy
       is by doublewords and takes microseconds */
    flags = int_off();
    entry = tx_cur;
    desc = &txd[entry];
    buf = txbuf[entry];
    if ( desc->opts1 & DESC_OWN ) {
        /* a link and still not gone after 100ms: a transmitter that has
           stopped.  Linux resets the chip on a transmit timeout; here
           the second time running starts it again */
        net_stats.txerrors++;
        if ( ++tx_stuck >= 2 ) {
            tx_stuck = 0;
            rx_drain();
            if ( chip_bringup() ) irq_on();
        }
        int_back( flags );
        return 0;
    }
    tx_stuck = 0;
    if ( desc->opts1 & TXD_ERRSUM ) net_stats.txerrors++;      /* the last frame from here */

    /* the whole doublewords straight from the frame, the rest by bytes:
       nothing past the caller's frame is read; short frames padded */
    whole = len / 4;
    if ( whole ) copy_dwords( buf, frame, whole );
    net_memcpy( buf + whole * 4, frame + whole * 4, len - whole * 4 );
    use = ( len < 60 ) ? 60 : len;
    if ( use > len ) net_memzero( buf + len, use - len );

    eor = ( entry == TXN - 1 ) ? DESC_EOR : 0;
    desc->addr_lo = (u32)buf;
    desc->addr_hi = 0;
    desc->opts2 = 0;
    desc->opts1 = eor | DESC_FS | DESC_LS | use;
    desc->opts1 = eor | DESC_OWN | DESC_FS | DESC_LS | use;     /* OWN last */
    tx_cur = ( entry + 1 ) & ( TXN - 1 );
    tx_kick();
    int_back( flags );
    return 1;
}

/* the ring drained into the queue, once round at the most.  Interrupts
   are off */
static void rx_drain( void )
{
    u32 status, len;
    int n;
    u8 *slot;

    for ( n = 0; n < RXN; n++ ) {
        status = rxd[rx_cur].opts1;
        if ( status & DESC_OWN ) break;
        len = status & RXD_LENGTH;                  /* the frame and its CRC */
        if ( ( status & ( DESC_FS | DESC_LS ) ) != ( DESC_FS | DESC_LS ) ) {
            net_stats.rxerrors++;                   /* part of an overlong frame */
        } else if ( status & RXD_RES ) {
            net_stats.rxerrors++;
        } else if ( len < NIC_HDRLEN + 4 || len > RXBUF || len - 4 > NIC_MAXFRAME ) {
            net_stats.rxerrors++;
        } else {
            slot = net_rx_begin();
            net_memcpy( slot, rxbuf[rx_cur], len - 4 );
            net_rx_commit( len - 4 );
        }
        rx_give( rx_cur );
        rx_cur = ( rx_cur + 1 ) & ( RXN - 1 );
    }
}

static int tx_waiting( void )
{
    int i;

    for ( i = 0; i < TXN; i++ ) {
        if ( txd[i].opts1 & DESC_OWN ) return 1;
    }
    return 0;
}

int card_service( void )
{
    u32 status, gone;
    int mine = 0, pass;

    gone = is_8125() ? 0xFFFFFFFFL : 0xFFFFL;
    for ( pass = 0; pass < 4; pass++ ) {
        status = is_8125() ? rd32( R_ISR_8125 ) : rd16( R_ISR );
        if ( status == 0 || status == gone ) break;     /* nothing, or no card */
        if ( is_8125() ) wr32( R_ISR_8125, status );    /* acknowledged as read */
        else wr16( R_ISR, (u16)status );
        if ( status & irq_mask ) mine = 1;
        if ( status & ( ISR_RXDU | ISR_FIFOOVER ) ) net_stats.overflows++;
        if ( status & ( ISR_RXOK | ISR_RXERR | ISR_RXDU | ISR_FIFOOVER ) ) rx_drain();
        if ( ver <= 6 && ( status & ISR_SYSERR ) ) {
            /* a PCI error: the error bits cleared and the chip started
               again, as Linux does */
            cfg_write( 0x06, 2, cfg_read( 0x06, 2 ) & 0xF900L );
            net_stats.rxerrors++;
            if ( chip_bringup() ) irq_on();
            break;
        }
    }
    /* frames left on the ring when the drain stopped, or with no new
       cause to say so (a polled card) */
    if ( !( rxd[rx_cur].opts1 & DESC_OWN ) ) rx_drain();
    if ( tx_waiting() ) tx_kick();
    return mine;
}

const ND_TABLE nd_table = {
    NDT_SIGVAL, NDT_VERSION, NDTF_NIC,
    { 'R', 'T', 'L', '8', '1', '6', '9', ' ' },
    ndv_init, ndv_park, ndv_halt, ndv_call
};
