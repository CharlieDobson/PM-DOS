/*
 * IXGBE.C - IXGBE.SYS, a driver for Intel's 10 gigabit PCI Express family
 *
 * Intel's 82598EB; the 82599, the chip of the X520 cards with their SFP+
 * cages, the 10 gigabit card most often found used; and the X540 and
 * X550, both 10GBASE-T with the PHY inside the chip.  One register map
 * and one ring layout, and four generations of difference in how the
 * chip is reset, how its interrupt causes are routed and above all how
 * its link is brought up, which is most of this file.  No emulator has
 * any of these chips.  This is written from FreeBSD's ix driver and the
 * Intel shared code under it (sys/dev/ixgbe, BSD licensed), with Linux's
 * ixgbe where the two differ, and is for real hardware to prove.
 *
 * ONE QUEUE EACH WAY, queue 0, ordinary frames of up to 1514 bytes: no
 * offloads, no jumbo frames, no RSS, DCB, SR-IOV or flow director.
 *
 * THE RESET (hw_reset, from card_find): every queue stopped, the chip's
 * bus mastering stopped and waited for (CTRL.GIO_DIS until STATUS.GIO
 * clears; two resets if it will not, as the datasheet asks), then
 * CTRL.RST - or CTRL.LNK_RST on an 82599 whose link is down, the shared
 * code's choice - and on the X540 and X550 under the PHY's semaphore,
 * since their firmware shares the PHY with the driver.  The chip reloads
 * its EEPROM: EEC.ARD is waited for, and the station address is read
 * from RAL0/RAH0, where the EEPROM put it.  What the reset leaves wrong
 * is put right as the shared code does: the 82598's Atlas transmit
 * lanes powered up and four GHECCR bits cleared; on the 82598 and 82599
 * AUTOC put back as the EEPROM first had it, and the 82599's AUTOC2
 * link-disable bits cleared; the X540/X550 receive packet buffer set to
 * the 384K the chip really has.
 *
 * THE RINGS (card_setup) are Intel's "advanced" descriptors, the ones
 * FreeBSD and Linux use on every chip of the family, chosen over the
 * legacy ones for that reason: a receive descriptor is one 2K buffer
 * (SRRCTL one-buffer), a frame to send is one data descriptor with
 * PAYLEN set, as the 82599 wants, and no context descriptor.  The chip
 * strips the CRC on the way in (HLREG0, and RDRXCTL on the 82599 and
 * later, which must agree) and adds it and pads short frames on the way
 * out (TXPADEN); the driver pads to 60 as well, since the 82599 will not
 * send a frame of fewer than 17 bytes.  RXDCTL/TXDCTL.ENABLE is waited
 * for after it is set - but not on an 82598 with no link, whose ENABLE
 * reads 0 until there is one - the receive tail is written after it, and
 * RXCTRL.RXEN goes on last; on the 82599 with the security block's
 * receive path held off around it (SECRXCTRL.RX_DIS until
 * SECRXSTAT.SECRX_RDY), for an erratum: traffic arriving as receive is
 * turned on can hang the receive DMA.  The X550's receive is turned off
 * here by RXCTRL itself, not by the firmware command Intel's code tries
 * first (it falls back to the register when the command fails).
 *
 * THE INTERRUPT is the PCI line, INTx; MSI and MSI-X are switched off in
 * the configuration space in case a BIOS or an earlier system left one
 * on, since PM-DOS answers only the line.  Receive queue 0 is routed to
 * cause bit 0 through IVAR, whose layout differs: a byte an entry on the
 * 82598 (receive queues at 0-63, transmit at 64-127), two queues a
 * register on the 82599 and later.  Transmit is not routed at all, since
 * card_send finds a finished descriptor by its DD bit.  EIMS gets that
 * bit and the link change, after every pending cause is cleared.
 * card_service masks the card (EIMC) while it reads EICR - an 82598
 * erratum, and what Linux does on every chip - writes back what it read
 * (EICR is write-1-to-clear as well as cleared by the read), drains the
 * ring whatever EICR said, and unmasks.  EITR holds the card to some
 * 20,000 interrupts a second, Linux's default, so that a 10 gigabit
 * flood cannot keep the processor in the handler.
 *
 * THE LINK, by family:
 *   X540, X550   the PHY is the chip's own and runs its own firmware.
 *                The driver does what the shared code does: powers the
 *                PHY up (an earlier system may have left it in low
 *                power), advertises 10G, 1G and 100M - not the X550's
 *                2.5G and 5G, which FreeBSD and Linux both leave off for
 *                switches that cannot cope with those speeds - and restarts
 *                autonegotiation.  Only when the link is down: a reset of
 *                the MAC does not touch the PHY, and a restart would cost
 *                a link that is already up three seconds or more.
 *   82599 SFP+   the module is read over the cage's I2C, bit-banged
 *                through I2CCTL: a DA cable, an active (limiting) DA
 *                cable, SR or LR optics, or a 1G SX, LX or copper
 *                module.  The EEPROM holds a setup for each kind, written
 *                word by word to CORECTL; then AUTOC is set to 10G serial
 *                (SFI) - 1G with autonegotiation for a 1G module - and the
 *                link pipeline reset (LMS toggled with Restart_AN, until
 *                ANLP1 shows the state machine moving).  The laser is
 *                switched on (SDP3 low) unless management firmware owns
 *                it; a dual-rate optic gets its rate select (SDP5) set
 *                for 10G and the laser flapped, so the far end tries
 *                again.  Any vendor's module is taken: Intel's drivers
 *                refuse optics that are not Intel's own unless told
 *                otherwise, and this one does not ask.
 *   82598 SFP+   the same through a NetLogic PHY, whose I2C master reads
 *                the module and whose setup is a script in the EEPROM of
 *                PHY register writes and delays.
 *   82598 AT, 82599 T3   an external 10GBASE-T PHY on MDIO: as the X540.
 *   KX4, KR, CX4, XAUI, fixed optics   AUTOC as the EEPROM left it, and
 *                autonegotiation restarted.
 * Where the 82599's firmware runs the link state machine (LESM, found in
 * the EEPROM), every AUTOC write is made under its semaphore, as the
 * shared code does.  The link is brought up after every reset (on the
 * 82598 and 82599 the reset put AUTOC back, so a link still up may be
 * about to go), and again whenever card_setup finds it down.  A module
 * put in after the driver loaded is not set up until the next reset or
 * park: the shared code watches SDP1/SDP2 for that, and this does not.
 *
 * AT THE LOAD the driver says, beside NETCORE's line,
 *     <name>: <module>, 10 Gb/s     or     <name>: no link (LINKS xxxxxxxx)
 * and, where something fails, what failed: the reset, the EEPROM, the
 * station address, a semaphore the firmware would not give up, the PHY,
 * the module's setup, a queue that would not start.  Nothing is said
 * after the load (a park, the WfW bridge).  The link is waited for at
 * the load only - five seconds for 10GBASE-T, three for the rest - and
 * no link is not a failure.
 *
 * ITS OWN SWITCH, beside NETCORE's /P:, /I: and /POLL:
 *   /N:n    the nth port of the family, 1 to 8, for the second port of a
 *           dual-port card; ports are counted by device id in the order
 *           of the list below, then in bus order.  Without it, the first.
 *
 * NOT TAKEN: the X552 and X553 (X550EM, inside the Xeon D and the Atom
 * C3000).  Their link is the IOSF sideband's KR/iXFI programming of an
 * internal PHY and, on the X553, firmware commands through the host
 * interface - another driver's worth, and not one to write blind.  Nor
 * the 82599's QSFP+, LS and bypass parts.
 *
 * On the shared base (DRV\NETDRV.H, NETCORE.C).  NOTHING HERE HAS RUN ON
 * A CARD: it compiles, and that is all that is proved.
 */
#include "netdrv.h"

/* ---------------------------------------------------------------- */
/* the registers, by ixgbe_type.h's names                            */
/* ---------------------------------------------------------------- */

#define IXGBE_CTRL              0x00000
#define IXGBE_STATUS            0x00008
#define IXGBE_CTRL_EXT          0x00018
#define IXGBE_ESDP              0x00020
#define IXGBE_I2CCTL_82599      0x00028
#define IXGBE_EICR              0x00800
#define IXGBE_EIAC              0x00810
#define IXGBE_EITR0             0x00820         /* IXGBE_EITR(0) */
#define IXGBE_EIMS              0x00880
#define IXGBE_EIMC              0x00888
#define IXGBE_EIAM              0x00890
#define IXGBE_IVAR( i )         ( 0x00900 + ( i ) * 4 )
#define IXGBE_EIMC_EX( i )      ( 0x00AB0 + ( i ) * 4 )
#define IXGBE_RDBAL( i )        ( 0x01000 + ( i ) * 0x40 )
#define IXGBE_RDBAH( i )        ( 0x01004 + ( i ) * 0x40 )
#define IXGBE_RDLEN( i )        ( 0x01008 + ( i ) * 0x40 )
#define IXGBE_RDH( i )          ( 0x01010 + ( i ) * 0x40 )
#define IXGBE_RDT( i )          ( 0x01018 + ( i ) * 0x40 )
#define IXGBE_RXDCTL( i )       ( ( i ) < 64 ? 0x01028 + ( i ) * 0x40 : 0x0D028 + ( ( i ) - 64 ) * 0x40 )
#define IXGBE_SRRCTL( i )       ( 0x02100 + ( i ) * 4 )     /* queues 0-15 */
#define IXGBE_DCA_RXCTRL( i )   ( 0x02200 + ( i ) * 4 )     /* queues 0-15 */
#define IXGBE_RDRXCTL           0x02F00
#define IXGBE_RXCTRL            0x03000
#define IXGBE_RSCDBU            0x03028
#define IXGBE_RXPBSIZE( i )     ( 0x03C00 + ( i ) * 4 )
#define IXGBE_HLREG0            0x04240
#define IXGBE_MSCA              0x0425C
#define IXGBE_MSRWD             0x04260
#define IXGBE_AUTOC             0x042A0
#define IXGBE_LINKS             0x042A4
#define IXGBE_AUTOC2            0x042A8
#define IXGBE_ANLP1             0x042B0
#define IXGBE_MMNGC             0x042D0
#define IXGBE_ATLASCTL          0x04800
#define IXGBE_RTTDCS            0x04900
#define IXGBE_DMATXCTL          0x04A80
#define IXGBE_FCTRL             0x05080
#define IXGBE_MCSTCTRL          0x05090
#define IXGBE_MTA( i )          ( 0x05200 + ( i ) * 4 )
#define IXGBE_RAL( i )          ( ( i ) <= 15 ? 0x05400 + ( i ) * 8 : 0x0A200 + ( i ) * 8 )
#define IXGBE_RAH( i )          ( ( i ) <= 15 ? 0x05404 + ( i ) * 8 : 0x0A204 + ( i ) * 8 )
#define IXGBE_PSRTYPE( i )      ( 0x05480 + ( i ) * 4 )
#define IXGBE_MANC              0x05820
#define IXGBE_TDBAL( i )        ( 0x06000 + ( i ) * 0x40 )
#define IXGBE_TDBAH( i )        ( 0x06004 + ( i ) * 0x40 )
#define IXGBE_TDLEN( i )        ( 0x06008 + ( i ) * 0x40 )
#define IXGBE_DCA_TXCTRL_82599( i ) ( 0x0600C + ( i ) * 0x40 )
#define IXGBE_TDH( i )          ( 0x06010 + ( i ) * 0x40 )
#define IXGBE_TDT( i )          ( 0x06018 + ( i ) * 0x40 )
#define IXGBE_TXDCTL( i )       ( 0x06028 + ( i ) * 0x40 )
#define IXGBE_TDWBAL( i )       ( 0x06038 + ( i ) * 0x40 )
#define IXGBE_TDWBAH( i )       ( 0x0603C + ( i ) * 0x40 )
#define IXGBE_DCA_TXCTRL( i )   ( 0x07200 + ( i ) * 4 )     /* 82598 */
#define IXGBE_MTQC              0x08120
#define IXGBE_SECRXCTRL         0x08D00
#define IXGBE_SECRXSTAT         0x08D04
#define IXGBE_MPSAR_LO( i )     ( 0x0A600 + ( i ) * 8 )
#define IXGBE_MPSAR_HI( i )     ( 0x0A604 + ( i ) * 8 )
#define IXGBE_EEC               0x10010
#define IXGBE_EERD              0x10014
#define IXGBE_SWSM              0x10140
#define IXGBE_FWSM              0x10148
#define IXGBE_FACTPS            0x10150
#define IXGBE_GSSR              0x10160         /* SWFW_SYNC on the X540 and X550 */
#define IXGBE_GCR_EXT           0x11050
#define IXGBE_GHECCR            0x110B0
#define IXGBE_CORECTL           0x14F00

#define IX_MAPSIZE      0x20000L        /* every register above is below 128K,
                                           the smallest BAR0 of the family */

/* the bits */
#define IXGBE_CTRL_GIO_DIS      0x00000004L
#define IXGBE_CTRL_LNK_RST      0x00000008L
#define IXGBE_CTRL_RST          0x04000000L
#define IXGBE_CTRL_RST_MASK     ( IXGBE_CTRL_LNK_RST | IXGBE_CTRL_RST )
#define IXGBE_STATUS_LAN_ID     0x0000000CL
#define IXGBE_STATUS_LAN_ID_SHIFT 2
#define IXGBE_STATUS_GIO        0x00080000L
#define IXGBE_CTRL_EXT_NS_DIS   0x00010000L     /* no PCIe no-snoop: DMA stays coherent */
#define IXGBE_CTRL_EXT_DRV_LOAD 0x10000000L     /* the firmware: a driver has the port */

#define IXGBE_ESDP_SDP3         0x00000008L     /* 82599 SFP+: the laser off */
#define IXGBE_ESDP_SDP5         0x00000020L     /* 82599 SFP+: rate select, high = 10G */
#define IXGBE_ESDP_SDP5_DIR     0x00002000L

#define IXGBE_I2C_CLK_IN        0x00000001L     /* the 82599's I2CCTL */
#define IXGBE_I2C_CLK_OUT       0x00000002L
#define IXGBE_I2C_DATA_IN       0x00000004L
#define IXGBE_I2C_DATA_OUT      0x00000008L

#define IXGBE_EICR_LSC          0x00100000L
#define IX_EICR_RXQ0            0x00000001L     /* RTxQ bit 0: what IVAR routes queue 0 to */
#define IXGBE_EITR_CNT_WDIS     0x80000000L
#define IX_ITR                  200L            /* Linux's IXGBE_20K_ITR */
#define IXGBE_IVAR_ALLOC_VAL    0x80L

#define IXGBE_SRRCTL_BSIZEPKT_SHIFT 10
#define IXGBE_SRRCTL_BSIZEPKT_MASK  0x0000007FL
#define IXGBE_SRRCTL_BSIZEHDR_MASK  0x00003F00L
#define IXGBE_SRRCTL_DROP_EN        0x10000000L
#define IXGBE_SRRCTL_DESCTYPE_MASK  0x0E000000L
#define IXGBE_SRRCTL_DESCTYPE_ADV_ONEBUF 0x02000000L
#define IXGBE_RXDCTL_ENABLE     0x02000000L
#define IXGBE_RXDCTL_SWFLSH     0x04000000L
#define IXGBE_TXDCTL_ENABLE     0x02000000L
#define IXGBE_TXDCTL_SWFLSH     0x04000000L
#define IXGBE_TXDCTL_THRESH_MASK    0x007F7F7FL
#define IXGBE_TXDCTL_THRESH_DEFAULT ( 32L | ( 1L << 8 ) )   /* PTHRESH 32, HTHRESH 1 */
#define IX_RXDCTL_82598_THRESH  0x00080420L     /* PTHRESH 32, HTHRESH 4, WTHRESH 8 */
#define IX_RXDCTL_82598_MASK    0x003FFFFFL

#define IXGBE_RDRXCTL_CRCSTRIP  0x00000002L
#define IXGBE_RDRXCTL_RSCFRSTSIZE 0x003E0000L
#define IXGBE_RDRXCTL_RSCACKC   0x02000000L
#define IXGBE_RDRXCTL_FCOE_WRFIX 0x04000000L
#define IXGBE_RSCDBU_RSCACKDIS  0x00000080L
#define IXGBE_RXCTRL_RXEN       0x00000001L
#define IXGBE_RXCTRL_DMBYPS     0x00000002L
#define IXGBE_DCA_RXCTRL_DATA_WRO_EN 0x00002000L
#define IXGBE_DCA_RXCTRL_HEAD_WRO_EN 0x00008000L
#define IXGBE_DCA_TXCTRL_DESC_WRO_EN 0x00000800L

#define IXGBE_HLREG0_TXCRCEN    0x00000001L
#define IXGBE_HLREG0_RXCRCSTRP  0x00000002L
#define IXGBE_HLREG0_JUMBOEN    0x00000004L
#define IXGBE_HLREG0_TXPADEN    0x00000400L
#define IXGBE_HLREG0_LPBK       0x00008000L

#define IXGBE_MSCA_DEV_TYPE_SHIFT 16
#define IXGBE_MSCA_PHY_ADDR_SHIFT 21
#define IXGBE_MSCA_ADDR_CYCLE   0x00000000L
#define IXGBE_MSCA_WRITE        0x04000000L
#define IXGBE_MSCA_READ         0x0C000000L
#define IXGBE_MSCA_MDI_COMMAND  0x40000000L
#define IXGBE_MSRWD_READ_DATA_SHIFT 16

#define IXGBE_AUTOC_AN_RESTART  0x00001000L
#define IXGBE_AUTOC_LMS_SHIFT   13
#define IXGBE_AUTOC_LMS_MASK    ( 0x7L << IXGBE_AUTOC_LMS_SHIFT )
#define IXGBE_AUTOC_LMS_1G_AN   ( 0x2L << IXGBE_AUTOC_LMS_SHIFT )
#define IXGBE_AUTOC_LMS_10G_SERIAL ( 0x3L << IXGBE_AUTOC_LMS_SHIFT )
#define IXGBE_AUTOC_1G_PMA_PMD_MASK 0x00000200L
#define IXGBE_AUTOC_1G_SFI      0x00000000L
#define IXGBE_AUTOC2_10G_SERIAL_PMA_PMD_MASK 0x00030000L
#define IXGBE_AUTOC2_10G_SFI    0x00020000L
#define IXGBE_AUTOC2_UPPER_MASK 0xFFFF0000L
#define IXGBE_AUTOC2_LINK_DISABLE_MASK 0x70000000L
#define IXGBE_ANLP1_AN_STATE_MASK 0x000F0000L

#define IXGBE_LINKS_UP          0x40000000L
#define IXGBE_LINKS_SPEED       0x20000000L     /* 82598: 10G, else 1G */
#define IXGBE_LINKS_SPEED_NON_STD 0x08000000L   /* X550: 2.5G or 5G */
#define IXGBE_LINKS_SPEED_82599 0x30000000L
#define IXGBE_LINKS_SPEED_10G_82599 0x30000000L
#define IXGBE_LINKS_SPEED_1G_82599 0x20000000L
#define IXGBE_LINKS_SPEED_100_82599 0x10000000L

#define IXGBE_MMNGC_MNG_VETO    0x00000001L
#define IXGBE_ATLASCTL_WRITE_CMD 0x00010000L
#define IXGBE_ATLAS_PDN_LPBK    0x24
#define IXGBE_ATLAS_PDN_10G     0x0B
#define IXGBE_ATLAS_PDN_1G      0x0C
#define IXGBE_ATLAS_PDN_AN      0x0D
#define IXGBE_ATLAS_PDN_TX_REG_EN 0x10
#define IXGBE_ATLAS_PDN_TX_QL_ALL 0xF0
#define IXGBE_RTTDCS_ARBDIS     0x00000040L
#define IXGBE_DMATXCTL_TE       0x00000001L
#define IXGBE_MTQC_64Q_1PB      0x00000000L

#define IXGBE_FCTRL_SBP         0x00000002L
#define IXGBE_FCTRL_MPE         0x00000100L     /* every multicast */
#define IXGBE_FCTRL_UPE         0x00000200L
#define IXGBE_FCTRL_BAM         0x00000400L     /* broadcast */
#define IXGBE_FCTRL_PMCF        0x00001000L
#define IXGBE_FCTRL_DPF         0x00002000L
#define IXGBE_RAH_AV            0x80000000L
#define IXGBE_PSRTYPE_TCPHDR    0x00000010L
#define IXGBE_PSRTYPE_UDPHDR    0x00000020L
#define IXGBE_PSRTYPE_IPV4HDR   0x00000100L
#define IXGBE_PSRTYPE_IPV6HDR   0x00000200L
#define IXGBE_MANC_RCV_TCO_EN   0x00020000L
#define IXGBE_SECRXCTRL_RX_DIS  0x00000002L
#define IXGBE_SECRXSTAT_SECRX_RDY 0x00000001L

#define IXGBE_EEC_ARD           0x00000200L     /* the EEPROM's auto-read is done */
#define IXGBE_EEPROM_RW_REG_START 0x00000001L
#define IXGBE_EEPROM_RW_REG_DONE  0x00000002L
#define IXGBE_EEPROM_RW_ADDR_SHIFT 2
#define IXGBE_EEPROM_RW_REG_DATA 16
#define IXGBE_EERD_MAX_ADDR     0x3FFFL
#define IXGBE_EERD_EEWR_ATTEMPTS 100000L

#define IXGBE_SWSM_SMBI         0x00000001L
#define IXGBE_SWSM_SWESMBI      0x00000002L
#define IXGBE_SWFW_REGSMP       0x80000000L
#define IXGBE_FWSM_MODE_MASK    0x0000000EL
#define IXGBE_FWSM_FW_MODE_PT   0x00000004L
#define IXGBE_FWSM_FW_NVM_RECOVERY_MODE 0x00000020L
#define IXGBE_FACTPS_MNGCG      0x20000000L
#define IXGBE_GSSR_EEP_SM       0x0001L
#define IXGBE_GSSR_PHY0_SM      0x0002L
#define IXGBE_GSSR_PHY1_SM      0x0004L
#define IXGBE_GSSR_MAC_CSR_SM   0x0008L
#define IXGBE_GSSR_FLASH_SM     0x0010L
#define IXGBE_GSSR_SW_MNG_SM    0x0400L
#define IXGBE_GSSR_I2C_MASK     0x1800L
#define IXGBE_GCR_EXT_BUFFERS_CLEAR 0x40000000L
#define IXGBE_PCI_DEVICE_STATUS 0xAA
#define IXGBE_PCI_DEVICE_STATUS_TRANSACTION_PENDING 0x0020L
#define IXGBE_PCI_PRIMARY_DISABLE_TIMEOUT 800

/* the descriptors */
#define IXGBE_RXD_STAT_DD       0x00000001L
#define IXGBE_RXD_STAT_EOP      0x00000002L
#define IXGBE_RXDADV_ERR_FRAME_ERR_MASK 0x3B000000L /* CE LE PE OSE USE */
#define IXGBE_ADVTXD_DTYP_DATA  0x00300000L
#define IXGBE_ADVTXD_DCMD_EOP   0x01000000L
#define IXGBE_ADVTXD_DCMD_IFCS  0x02000000L
#define IXGBE_ADVTXD_DCMD_RS    0x08000000L
#define IXGBE_ADVTXD_DCMD_DEXT  0x20000000L
#define IXGBE_ADVTXD_PAYLEN_SHIFT 14
#define IXGBE_TXD_STAT_DD       0x00000001L

/* MDIO (clause 45): the devices and the registers */
#define IXGBE_MDIO_PMA_PMD_DEV_TYPE  0x1
#define IXGBE_MDIO_PHY_XS_DEV_TYPE   0x4
#define IXGBE_MDIO_AUTO_NEG_DEV_TYPE 0x7
#define IXGBE_MDIO_VENDOR_SPECIFIC_1_DEV_TYPE 0x1E
#define IXGBE_TWINAX_DEV             0x1
#define IXGBE_MDIO_COMMAND_TIMEOUT   100
#define IXGBE_MAX_PHY_ADDR           32
#define IXGBE_MDIO_PHY_ID_HIGH       0x2
#define IXGBE_MDIO_PHY_ID_LOW        0x3
#define IXGBE_MDIO_PHY_SPEED_ABILITY 0x4
#define IXGBE_MDIO_PHY_SPEED_10G     0x0001
#define IXGBE_MDIO_PHY_SPEED_1G      0x0010
#define IXGBE_MDIO_PHY_SPEED_100M    0x0020
#define IXGBE_MDIO_PHY_EXT_ABILITY   0xB
#define IXGBE_MDIO_PHY_10GBASET_ABILITY  0x0004
#define IXGBE_MDIO_PHY_1000BASET_ABILITY 0x0020
#define IXGBE_MDIO_PHY_XS_CONTROL    0x0
#define IXGBE_MDIO_PHY_XS_RESET      0x8000
#define IXGBE_MDIO_VENDOR_SPECIFIC_1_CONTROL 0x0
#define IXGBE_MDIO_PHY_SET_LOW_POWER_MODE    0x0800
#define IXGBE_MDIO_AUTO_NEG_CONTROL  0x0
#define IXGBE_MII_RESTART            0x0200
#define IXGBE_MII_10GBASE_T_AUTONEG_CTRL_REG 0x20
#define IXGBE_MII_10GBASE_T_ADVERTISE        0x1000
#define IXGBE_MII_AUTONEG_VENDOR_PROVISION_1_REG 0xC400
#define IXGBE_MII_1GBASE_T_ADVERTISE         0x8000
#define IXGBE_MII_2_5GBASE_T_ADVERTISE       0x0400
#define IXGBE_MII_5GBASE_T_ADVERTISE         0x0800
#define IXGBE_MII_AUTONEG_XNP_TX_REG         0x17
#define IXGBE_MII_1GBASE_T_ADVERTISE_XNP_TX  0x4000
#define IXGBE_MII_AUTONEG_ADVERTISE_REG      0x10
#define IXGBE_MII_100BASE_T_ADVERTISE        0x0100
#define IXGBE_MII_100BASE_T_ADVERTISE_HALF   0x0080
#define IXGBE_MDIO_PMA_PMD_SDA_SCL_ADDR 0xC30A    /* the NetLogic PHY's I2C master */
#define IXGBE_MDIO_PMA_PMD_SDA_SCL_DATA 0xC30B
#define IXGBE_MDIO_PMA_PMD_SDA_SCL_STAT 0xC30C
#define IXGBE_I2C_EEPROM_READ_MASK      0x0100
#define IXGBE_I2C_EEPROM_STATUS_MASK    0x0003
#define IXGBE_I2C_EEPROM_STATUS_PASS    0x0001
#define IXGBE_I2C_EEPROM_STATUS_IN_PROGRESS 0x0003

#define IXGBE_PHY_REVISION_MASK 0xFFFFFFF0L
#define TN1010_PHY_ID   0x00A19410L
#define X540_PHY_ID     0x01540200L
#define X550_PHY_ID     0x01540220L
#define X550_PHY_ID2    0x01540223L
#define X550_PHY_ID3    0x01540221L
#define ATH_PHY_ID      0x03429050L             /* NetLogic, the 82598's SFP+ */

/* the SFP+ module's EEPROM (SFF-8472) */
#define IXGBE_I2C_EEPROM_DEV_ADDR       0xA0
#define IXGBE_SFF_IDENTIFIER            0x00
#define IXGBE_SFF_IDENTIFIER_SFP        0x03
#define IXGBE_SFF_10GBE_COMP_CODES      0x03
#define IXGBE_SFF_1GBE_COMP_CODES       0x06
#define IXGBE_SFF_CABLE_TECHNOLOGY      0x08
#define IXGBE_SFF_CABLE_SPEC_COMP       0x3C
#define IXGBE_SFF_DA_PASSIVE_CABLE      0x04
#define IXGBE_SFF_DA_ACTIVE_CABLE       0x08
#define IXGBE_SFF_DA_SPEC_ACTIVE_LIMITING 0x04
#define IXGBE_SFF_1GBASESX_CAPABLE      0x01
#define IXGBE_SFF_1GBASELX_CAPABLE      0x02
#define IXGBE_SFF_1GBASET_CAPABLE       0x08
#define IXGBE_SFF_10GBASESR_CAPABLE     0x10
#define IXGBE_SFF_10GBASELR_CAPABLE     0x20

/* I2C timing, microseconds (100 kHz) */
#define IXGBE_I2C_T_HD_STA      4
#define IXGBE_I2C_T_LOW         5
#define IXGBE_I2C_T_HIGH        4
#define IXGBE_I2C_T_SU_STA      5
#define IXGBE_I2C_T_SU_DATA     1
#define IXGBE_I2C_T_RISE        1
#define IXGBE_I2C_T_FALL        1
#define IXGBE_I2C_T_SU_STO      4
#define IXGBE_I2C_T_BUF         5
#define IXGBE_I2C_CLOCK_STRETCHING_TIMEOUT 500

/* the EEPROM's words */
#define IXGBE_PHY_INIT_OFFSET_NL 0x002B         /* the list of module setups */
#define IXGBE_PHY_INIT_END_NL   0xFFFF
#define IXGBE_CONTROL_SHIFT_NL  12
#define IXGBE_DATA_MASK_NL      0x0FFF
#define IXGBE_DELAY_NL          0
#define IXGBE_DATA_NL           1
#define IXGBE_CONTROL_NL        0x000F
#define IXGBE_CONTROL_EOL_NL    0x0FFF
#define IXGBE_CONTROL_SOL_NL    0x0000
#define IXGBE_FW_PTR            0x0F
#define IXGBE_FW_LESM_PARAMETERS_PTR 0x2
#define IXGBE_FW_LESM_STATE_1   0x1
#define IXGBE_FW_LESM_STATE_ENABLED 0x8000

/* ixgbe_sfp_type: the ids of the module setups in the EEPROM.  The
   82599's come in pairs, the second for LAN function 1 */
#define SFP_DA_CU               0               /* 82598 */
#define SFP_SR                  1
#define SFP_LR                  2
#define SFP_DA_CU_CORE0         3               /* 82599 */
#define SFP_SRLR_CORE0          5
#define SFP_DA_ACT_CORE0        7
#define SFP_1G_CU_CORE0         9
#define SFP_1G_SX_CORE0         11
#define SFP_1G_LX_CORE0         13
#define SFP_NOT_PRESENT         0xFFFE
#define SFP_UNKNOWN             0xFFFF

/* ---------------------------------------------------------------- */
/* the family                                                        */
/* ---------------------------------------------------------------- */

#define MAC_82598       0               /* in the shared code's order: */
#define MAC_82599       1               /* ">= MAC_X540" means what its */
#define MAC_X540        2               /* ">= ixgbe_mac_X540" does */
#define MAC_X550        3

#define MED_BACKPLANE   0               /* KX4/KX/KR/XAUI, a backplane or mezzanine */
#define MED_FIBER       1               /* the 82598's optics fixed to the card */
#define MED_SFP         2               /* an SFP+ cage */
#define MED_CX4         3
#define MED_COPPER      4               /* 10GBASE-T: a PHY on MDIO */

#define PHY_NONE        0
#define PHY_GENERIC     1
#define PHY_CU          2               /* a copper PHY with no name */
#define PHY_TN          3               /* the 82598AT's Teranetics TN1010 */
#define PHY_AQ          4               /* the X540's and X550's own */
#define PHY_NL          5               /* NetLogic: the 82598's SFP+ */

typedef struct {
    u16 device;
    u8  mac;
    u8  media;
    const char *name;
} IX_KIND;

static const PCI_ID ix_ids[] = {
    { 0x8086, 0x10FB }, { 0x8086, 0x1557 }, { 0x8086, 0x1529 }, { 0x8086, 0x1507 },
    { 0x8086, 0x154D }, { 0x8086, 0x154A },
    { 0x8086, 0x1528 }, { 0x8086, 0x1560 }, { 0x8086, 0x1563 }, { 0x8086, 0x15D1 },
    { 0x8086, 0x10F7 }, { 0x8086, 0x1514 }, { 0x8086, 0x1517 }, { 0x8086, 0x10F8 },
    { 0x8086, 0x152A }, { 0x8086, 0x10FC }, { 0x8086, 0x10F9 }, { 0x8086, 0x151C },
    { 0x8086, 0x10F1 }, { 0x8086, 0x10DB }, { 0x8086, 0x10C6 }, { 0x8086, 0x10C7 },
    { 0x8086, 0x10E1 }, { 0x8086, 0x10F4 }, { 0x8086, 0x10DD }, { 0x8086, 0x10EC },
    { 0x8086, 0x10C8 }, { 0x8086, 0x150B }, { 0x8086, 0x10B6 }, { 0x8086, 0x1508 },
    { 0, 0 }
};

static const IX_KIND ix_kinds[] = {
    { 0x10FB, MAC_82599, MED_SFP,       "Intel X520 (82599) SFP+" },
    { 0x1557, MAC_82599, MED_SFP,       "Intel X520 (82599EN) SFP+" },
    { 0x1529, MAC_82599, MED_SFP,       "Intel 82599 SFP+ FCoE" },
    { 0x1507, MAC_82599, MED_SFP,       "Intel 82599 SFP+ (EM)" },
    { 0x154D, MAC_82599, MED_SFP,       "Intel 82599 SFP+ (SF2)" },
    { 0x154A, MAC_82599, MED_SFP,       "Intel 82599 SFP+ quad" },
    { 0x1528, MAC_X540,  MED_COPPER,    "Intel X540-T" },
    { 0x1560, MAC_X540,  MED_COPPER,    "Intel X540-T1" },
    { 0x1563, MAC_X550,  MED_COPPER,    "Intel X550-T" },
    { 0x15D1, MAC_X550,  MED_COPPER,    "Intel X550-T1" },
    { 0x10F7, MAC_82599, MED_BACKPLANE, "Intel 82599 KX4" },
    { 0x1514, MAC_82599, MED_BACKPLANE, "Intel 82599 KX4 mezzanine" },
    { 0x1517, MAC_82599, MED_BACKPLANE, "Intel 82599 KR" },
    { 0x10F8, MAC_82599, MED_BACKPLANE, "Intel 82599 backplane" },
    { 0x152A, MAC_82599, MED_BACKPLANE, "Intel 82599 backplane FCoE" },
    { 0x10FC, MAC_82599, MED_BACKPLANE, "Intel 82599 XAUI" },
    { 0x10F9, MAC_82599, MED_CX4,       "Intel 82599 CX4" },
    { 0x151C, MAC_82599, MED_COPPER,    "Intel 82599 10GBASE-T" },
    { 0x10F1, MAC_82598, MED_SFP,       "Intel 82598EB DA dual" },
    { 0x10DB, MAC_82598, MED_SFP,       "Intel 82598EB SFP+" },
    { 0x10C6, MAC_82598, MED_FIBER,     "Intel 82598EB AF dual (SR)" },
    { 0x10C7, MAC_82598, MED_FIBER,     "Intel 82598EB AF (SR)" },
    { 0x10E1, MAC_82598, MED_FIBER,     "Intel 82598EB SR dual" },
    { 0x10F4, MAC_82598, MED_FIBER,     "Intel 82598EB XF LR" },
    { 0x10DD, MAC_82598, MED_CX4,       "Intel 82598EB CX4" },
    { 0x10EC, MAC_82598, MED_CX4,       "Intel 82598EB CX4 dual" },
    { 0x10C8, MAC_82598, MED_COPPER,    "Intel 82598EB AT" },
    { 0x150B, MAC_82598, MED_COPPER,    "Intel 82598EB AT2" },
    { 0x10B6, MAC_82598, MED_BACKPLANE, "Intel 82598 KX4" },
    { 0x1508, MAC_82598, MED_BACKPLANE, "Intel 82598 BX" },
    { 0, 0, 0, 0 }
};

/* ---------------------------------------------------------------- */
/* the driver's state                                                */
/* ---------------------------------------------------------------- */

#define RXN     32              /* a multiple of 8: RDLEN counts 128 bytes */
#define TXN     16
#define BUFSZ   2048            /* SRRCTL's unit is 1K; every frame fits one */

static u32  membase;
static u8   mac_type;
static u8   media;
static u16  device_id;
static u8   lan_id;             /* the port: STATUS.LAN_ID */
static u32  phy_sem;            /* its PHY's semaphore, PHY0 or PHY1 */
static u8   found_once;         /* card_find has run to the end before */
static u8   lesm;               /* 82599: the firmware runs the link state machine */
static u8   orig_stored;
static u32  orig_autoc, orig_autoc2;    /* AUTOC, AUTOC2 as the EEPROM first had each */
static u8   link_fresh;         /* a reset came after the last link bring-up */
static int  port_want = 1;      /* /N: */

static u8   phy_kind = PHY_NONE;
static u8   phy_addr;
static u8   phy_looked;         /* the MDIO bus has been searched */

static u16  sfp_type = SFP_NOT_PRESENT;
static u8   sfp_multispeed;     /* a dual-rate optic: 1G SX and 10G SR, or LX and LR */
static u8   sfp_1g;             /* a 1G module */
static const char *sfp_text = "module not read";

static volatile u32 *rxd;       /* RXN descriptors of four dwords */
static volatile u32 *txd;       /* TXN of the same */
static u8  *rxbuf[RXN];
static u8  *txbuf[TXN];
static int  rx_cur, tx_cur;
static u8   rx_skip;            /* inside a frame that spans descriptors: dropped */
static u32  int_mask;           /* what EIMS gets back after card_service */

static u32  rd( u32 off ) { return MMIO32( membase, off ); }
static void wr( u32 off, u32 value ) { MMIO32( membase, off ) = value; }
static void flush( void ) { (void)rd( IXGBE_STATUS ); }

/* milliseconds, by the timer (net_delay_us is the only measured wait) */
static void wait_ms( u32 ms )
{
    while ( ms-- ) net_delay_us( 1000 );
}

/* ---------------------------------------------------------------- */
/* the lines this driver says - at the load only: net_line is set     */
/* while nd_init runs, and nds_print is for INIT and CALL time, not   */
/* for a park or the WfW bridge                                       */
/* ---------------------------------------------------------------- */

static char say_buf[112];
static int  say_len;

static void say_text( const char *text )
{
    while ( *text && say_len < (int)sizeof( say_buf ) - 3 ) say_buf[say_len++] = *text++;
}

static void say_begin( void )
{
    say_len = 0;
    say_text( net_name );
    say_text( ": " );
}

static void say_hex( u32 value, int digits )
{
    static const char hexdig[] = "0123456789ABCDEF";
    int shift;

    for ( shift = ( digits - 1 ) * 4; shift >= 0; shift -= 4 ) {
        if ( say_len < (int)sizeof( say_buf ) - 3 ) say_buf[say_len++] = hexdig[( value >> shift ) & 15];
    }
}

static void say_end( void )
{
    say_buf[say_len++] = '\r';
    say_buf[say_len++] = '\n';
    say_buf[say_len] = 0;
    if ( net_line ) net_print( say_buf );
}

static void say( const char *text )
{
    say_begin();
    say_text( text );
    say_end();
}

/* ---------------------------------------------------------------- */
/* the semaphores the driver shares with the firmware                */
/* ---------------------------------------------------------------- */

/* The register semaphore that guards GSSR itself: SWSM.SMBI (set by a
   read that finds it clear), then SWSM.SWESMBI on the 82598 and 82599,
   SWFW_SYNC.REGSMP (the same kind of read) on the X540 and X550 */
static void reg_sem_put( void )
{
    if ( mac_type >= MAC_X540 ) {
        wr( IXGBE_GSSR, rd( IXGBE_GSSR ) & ~IXGBE_SWFW_REGSMP );
        wr( IXGBE_SWSM, rd( IXGBE_SWSM ) & ~IXGBE_SWSM_SMBI );
    } else {
        wr( IXGBE_SWSM, rd( IXGBE_SWSM ) & ~( IXGBE_SWSM_SWESMBI | IXGBE_SWSM_SMBI ) );
    }
    flush();
}

static int reg_sem_get( void )
{
    int i;

    for ( i = 0; i < 2000; i++ ) {
        if ( !( rd( IXGBE_SWSM ) & IXGBE_SWSM_SMBI ) ) break;
        net_delay_us( 50 );
    }
    if ( i == 2000 ) {
        if ( mac_type >= MAC_X540 ) return 0;
        /* the generic code: a driver that died holding it is forgiven
           once, and one more read tried */
        reg_sem_put();
        net_delay_us( 50 );
        if ( rd( IXGBE_SWSM ) & IXGBE_SWSM_SMBI ) return 0;
    }
    for ( i = 0; i < 2000; i++ ) {
        if ( mac_type >= MAC_X540 ) {
            if ( !( rd( IXGBE_GSSR ) & IXGBE_SWFW_REGSMP ) ) return 1;
        } else {
            wr( IXGBE_SWSM, rd( IXGBE_SWSM ) | IXGBE_SWSM_SWESMBI );
            if ( rd( IXGBE_SWSM ) & IXGBE_SWSM_SWESMBI ) return 1;
        }
        net_delay_us( 50 );
    }
    reg_sem_put();
    return 0;
}

static void swfw_give( u32 mask )
{
    (void)reg_sem_get();                /* the shared code clears its bits regardless */
    wr( IXGBE_GSSR, rd( IXGBE_GSSR ) & ~mask );
    reg_sem_put();
    if ( mac_type >= MAC_X540 ) {
        /* the X540's code: flash and EEPROM want 2ms before the next
           taker, MDIO only 10us */
        net_delay_us( ( mask & ( IXGBE_GSSR_PHY0_SM | IXGBE_GSSR_PHY1_SM ) ) ? 10 : 2000 );
    }
}

/* One of the resources in GSSR (SWFW_SYNC): the software bit is ours
   when neither it nor the firmware's (five bits up) is set.  About a
   second is waited.  Then, as the X540's code does, a resource the
   firmware will not give back is taken anyway; the 82598/82599 code
   clears the stuck bits instead and reports the failure */
static int swfw_take( u32 mask )
{
    u32 fwmask = mask << 5, hwmask = 0, gssr = 0;
    int i;

    if ( mac_type >= MAC_X540 && ( mask & IXGBE_GSSR_EEP_SM ) ) hwmask = IXGBE_GSSR_FLASH_SM;
    for ( i = 0; i < 200; i++ ) {
        if ( !reg_sem_get() ) return 0;
        gssr = rd( IXGBE_GSSR );
        if ( !( gssr & ( mask | fwmask | hwmask ) ) ) {
            wr( IXGBE_GSSR, gssr | mask );
            reg_sem_put();
            return 1;
        }
        reg_sem_put();
        wait_ms( 5 );
    }
    if ( !reg_sem_get() ) return 0;
    gssr = rd( IXGBE_GSSR );
    if ( mac_type >= MAC_X540 && ( gssr & ( fwmask | hwmask ) ) ) {
        wr( IXGBE_GSSR, gssr | mask );
        reg_sem_put();
        wait_ms( 5 );
        return 1;
    }
    wr( IXGBE_GSSR, gssr & ~( mask | fwmask ) );
    reg_sem_put();
    return 0;
}

/* X540, X550 at the first load: whatever software bits a driver that
   died left set are cleared (ixgbe_init_swfw_sync_X540) */
static void swfw_init( void )
{
    (void)reg_sem_get();
    wr( IXGBE_GSSR, rd( IXGBE_GSSR ) & ~( IXGBE_GSSR_EEP_SM | IXGBE_GSSR_PHY0_SM | IXGBE_GSSR_PHY1_SM |
                                         IXGBE_GSSR_MAC_CSR_SM | IXGBE_GSSR_SW_MNG_SM | IXGBE_GSSR_I2C_MASK ) );
    reg_sem_put();
}

/* management firmware that owns the port (pass-through mode, receiving) */
static int mng_enabled( void )
{
    if ( ( rd( IXGBE_FWSM ) & IXGBE_FWSM_MODE_MASK ) != IXGBE_FWSM_FW_MODE_PT ) return 0;
    if ( !( rd( IXGBE_MANC ) & IXGBE_MANC_RCV_TCO_EN ) ) return 0;
    if ( mac_type <= MAC_X540 && ( rd( IXGBE_FACTPS ) & IXGBE_FACTPS_MNGCG ) ) return 0;
    return 1;
}

/* the firmware forbids a link reset just now (MMNGC.MNG_VETO) */
static int reset_blocked( void )
{
    if ( mac_type == MAC_82598 ) return 0;
    return ( rd( IXGBE_MMNGC ) & IXGBE_MMNGC_MNG_VETO ) != 0;
}

/* ---------------------------------------------------------------- */
/* the EEPROM (82598, 82599 - the X540 and X550 are not asked)        */
/* ---------------------------------------------------------------- */

static int ee_read( u32 offset, u16 *value )
{
    u32 reg;
    long i;

    if ( offset > IXGBE_EERD_MAX_ADDR ) return 0;
    wr( IXGBE_EERD, ( offset << IXGBE_EEPROM_RW_ADDR_SHIFT ) | IXGBE_EEPROM_RW_REG_START );
    for ( i = 0; i < IXGBE_EERD_EEWR_ATTEMPTS; i++ ) {
        reg = rd( IXGBE_EERD );
        if ( reg & IXGBE_EEPROM_RW_REG_DONE ) {
            *value = (u16)( reg >> IXGBE_EEPROM_RW_REG_DATA );
            return 1;
        }
        net_delay_us( 5 );
    }
    return 0;
}

/* 82599: the firmware module's LESM state word, through two pointers */
static int lesm_check( void )
{
    u16 fw, param, state;

    if ( !ee_read( IXGBE_FW_PTR, &fw ) || fw == 0 || fw == 0xFFFF ) return 0;
    if ( !ee_read( (u32)fw + IXGBE_FW_LESM_PARAMETERS_PTR, &param ) || param == 0 || param == 0xFFFF ) return 0;
    if ( !ee_read( (u32)param + IXGBE_FW_LESM_STATE_1, &state ) ) return 0;
    return ( state & IXGBE_FW_LESM_STATE_ENABLED ) != 0;
}

/* ---------------------------------------------------------------- */
/* MDIO, clause 45, through MSCA/MSRWD                               */
/* ---------------------------------------------------------------- */

static int mdi_cmd( u32 command )
{
    int i;

    wr( IXGBE_MSCA, command );
    for ( i = 0; i < IXGBE_MDIO_COMMAND_TIMEOUT; i++ ) {
        net_delay_us( 10 );
        if ( !( rd( IXGBE_MSCA ) & IXGBE_MSCA_MDI_COMMAND ) ) return 1;
    }
    return 0;
}

static u32 mdi_addr( u32 dev, u32 reg )
{
    return reg | ( dev << IXGBE_MSCA_DEV_TYPE_SHIFT ) | ( (u32)phy_addr << IXGBE_MSCA_PHY_ADDR_SHIFT );
}

/* with the PHY's semaphore already held */
static int mdi_read( u32 dev, u32 reg, u16 *value )
{
    u32 base = mdi_addr( dev, reg );

    if ( !mdi_cmd( base | IXGBE_MSCA_ADDR_CYCLE | IXGBE_MSCA_MDI_COMMAND ) ) return 0;
    if ( !mdi_cmd( base | IXGBE_MSCA_READ | IXGBE_MSCA_MDI_COMMAND ) ) return 0;
    *value = (u16)( rd( IXGBE_MSRWD ) >> IXGBE_MSRWD_READ_DATA_SHIFT );
    return 1;
}

static int mdi_write( u32 dev, u32 reg, u16 value )
{
    u32 base = mdi_addr( dev, reg );

    wr( IXGBE_MSRWD, value );
    if ( !mdi_cmd( base | IXGBE_MSCA_ADDR_CYCLE | IXGBE_MSCA_MDI_COMMAND ) ) return 0;
    return mdi_cmd( base | IXGBE_MSCA_WRITE | IXGBE_MSCA_MDI_COMMAND );
}

/* one register, the semaphore taken for it alone (as the shared code's
   read_reg/write_reg), for the NetLogic script with its long delays */
static int phy_write( u32 dev, u32 reg, u16 value )
{
    int ok;

    if ( !swfw_take( phy_sem ) ) return 0;
    ok = mdi_write( dev, reg, value );
    swfw_give( phy_sem );
    return ok;
}

static int phy_read( u32 dev, u32 reg, u16 *value )
{
    int ok;

    if ( !swfw_take( phy_sem ) ) return 0;
    ok = mdi_read( dev, reg, value );
    swfw_give( phy_sem );
    return ok;
}

/* The PHY: the first MDIO address with an id (ixgbe_identify_phy_generic),
   looked for once.  Some ports have none (fixed optics, the 82599's
   SFP+ cage): PHY_NONE, said by the caller only where one is wanted */
static void phy_look( void )
{
    u16 high, low, ext;
    u32 id;
    int addr;

    if ( phy_looked ) return;
    if ( !swfw_take( phy_sem ) ) { say( "the firmware would not give up the PHY" ); return; }
    phy_looked = 1;
    for ( addr = 0; addr < IXGBE_MAX_PHY_ADDR; addr++ ) {
        phy_addr = (u8)addr;
        if ( !mdi_read( IXGBE_MDIO_PMA_PMD_DEV_TYPE, IXGBE_MDIO_PHY_ID_HIGH, &high ) ) continue;
        if ( high == 0xFFFF || high == 0 ) continue;
        if ( !mdi_read( IXGBE_MDIO_PMA_PMD_DEV_TYPE, IXGBE_MDIO_PHY_ID_LOW, &low ) ) continue;
        id = ( (u32)high << 16 ) | ( low & IXGBE_PHY_REVISION_MASK );
        if ( id == ATH_PHY_ID ) phy_kind = PHY_NL;
        else if ( id == TN1010_PHY_ID ) phy_kind = PHY_TN;
        else if ( id == X540_PHY_ID || id == X550_PHY_ID || id == X550_PHY_ID2 || id == X550_PHY_ID3 ) phy_kind = PHY_AQ;
        else {
            ext = 0;
            (void)mdi_read( IXGBE_MDIO_PMA_PMD_DEV_TYPE, IXGBE_MDIO_PHY_EXT_ABILITY, &ext );
            phy_kind = (u8)( ( ext & ( IXGBE_MDIO_PHY_10GBASET_ABILITY | IXGBE_MDIO_PHY_1000BASET_ABILITY ) ) ? PHY_CU : PHY_GENERIC );
        }
        break;
    }
    swfw_give( phy_sem );
}

/* ---------------------------------------------------------------- */
/* the 82599's I2C to the SFP+ cage, bit-banged (ixgbe_phy.c's)       */
/* ---------------------------------------------------------------- */

static void i2c_out( u32 ctl )
{
    wr( IXGBE_I2CCTL_82599, ctl );
    flush();
}

static void i2c_clk_high( u32 *ctl )
{
    int i;

    /* until the clock reads high: a slave may hold it low (stretching) */
    for ( i = 0; i < IXGBE_I2C_CLOCK_STRETCHING_TIMEOUT; i++ ) {
        *ctl |= IXGBE_I2C_CLK_OUT;
        i2c_out( *ctl );
        net_delay_us( IXGBE_I2C_T_RISE );
        if ( rd( IXGBE_I2CCTL_82599 ) & IXGBE_I2C_CLK_IN ) break;
    }
}

static void i2c_clk_low( u32 *ctl )
{
    *ctl &= ~IXGBE_I2C_CLK_OUT;
    i2c_out( *ctl );
    net_delay_us( IXGBE_I2C_T_FALL );
}

/* the data line; a high one is read back (another driver of the line
   would hold it low) */
static int i2c_data( u32 *ctl, int high )
{
    if ( high ) *ctl |= IXGBE_I2C_DATA_OUT;
    else *ctl &= ~IXGBE_I2C_DATA_OUT;
    i2c_out( *ctl );
    net_delay_us( IXGBE_I2C_T_RISE + IXGBE_I2C_T_FALL + IXGBE_I2C_T_SU_DATA );
    if ( !high ) return 1;
    *ctl = rd( IXGBE_I2CCTL_82599 );
    return ( *ctl & IXGBE_I2C_DATA_IN ) != 0;
}

static void i2c_start( void )
{
    u32 ctl = rd( IXGBE_I2CCTL_82599 );

    (void)i2c_data( &ctl, 1 );
    i2c_clk_high( &ctl );
    net_delay_us( IXGBE_I2C_T_SU_STA );
    (void)i2c_data( &ctl, 0 );
    net_delay_us( IXGBE_I2C_T_HD_STA );
    i2c_clk_low( &ctl );
    net_delay_us( IXGBE_I2C_T_LOW );
}

static void i2c_stop( void )
{
    u32 ctl = rd( IXGBE_I2CCTL_82599 );

    (void)i2c_data( &ctl, 0 );
    i2c_clk_high( &ctl );
    net_delay_us( IXGBE_I2C_T_SU_STO );
    (void)i2c_data( &ctl, 1 );
    net_delay_us( IXGBE_I2C_T_BUF );
}

static int i2c_bit_out( int bit )
{
    u32 ctl = rd( IXGBE_I2CCTL_82599 );

    if ( !i2c_data( &ctl, bit ) ) return 0;
    i2c_clk_high( &ctl );
    net_delay_us( IXGBE_I2C_T_HIGH );
    i2c_clk_low( &ctl );
    net_delay_us( IXGBE_I2C_T_LOW );
    return 1;
}

static int i2c_bit_in( void )
{
    u32 ctl = rd( IXGBE_I2CCTL_82599 );
    int bit;

    i2c_clk_high( &ctl );
    net_delay_us( IXGBE_I2C_T_HIGH );
    ctl = rd( IXGBE_I2CCTL_82599 );
    bit = ( ctl & IXGBE_I2C_DATA_IN ) != 0;
    i2c_clk_low( &ctl );
    net_delay_us( IXGBE_I2C_T_LOW );
    return bit;
}

static int i2c_byte_out( u8 data )
{
    int i, ok = 1;

    for ( i = 7; i >= 0 && ok; i-- ) ok = i2c_bit_out( ( data >> i ) & 1 );
    i2c_out( rd( IXGBE_I2CCTL_82599 ) | IXGBE_I2C_DATA_OUT );  /* let go of the data line */
    return ok;
}

static u8 i2c_byte_in( void )
{
    int i;
    u8  data = 0;

    for ( i = 7; i >= 0; i-- ) data = (u8)( data | ( i2c_bit_in() << i ) );
    return data;
}

/* the slave pulls the data line low on the ninth clock: 1 if it did */
static int i2c_ack( void )
{
    u32 ctl = rd( IXGBE_I2CCTL_82599 );
    int i, nack = 1;

    i2c_clk_high( &ctl );
    net_delay_us( IXGBE_I2C_T_HIGH );
    for ( i = 0; i < 10; i++ ) {
        ctl = rd( IXGBE_I2CCTL_82599 );
        nack = ( ctl & IXGBE_I2C_DATA_IN ) != 0;
        net_delay_us( 1 );
        if ( !nack ) break;
    }
    i2c_clk_low( &ctl );
    net_delay_us( IXGBE_I2C_T_LOW );
    return !nack;
}

/* nine clocks, for a slave left holding the data line mid-byte */
static void i2c_bus_clear( void )
{
    u32 ctl;
    int i;

    i2c_start();
    ctl = rd( IXGBE_I2CCTL_82599 );
    (void)i2c_data( &ctl, 1 );
    for ( i = 0; i < 9; i++ ) {
        i2c_clk_high( &ctl );
        net_delay_us( IXGBE_I2C_T_HIGH );
        i2c_clk_low( &ctl );
        net_delay_us( IXGBE_I2C_T_LOW );
    }
    i2c_start();
    i2c_stop();
}

/* one byte of the module's EEPROM (A0h): three tries, the bus cleared
   between tries.  No module answers nothing, so this is also how an
   empty cage is found */
static int i2c_sfp_byte( u8 offset, u8 *value )
{
    int tries;

    for ( tries = 0; tries < 3; tries++ ) {
        if ( !swfw_take( phy_sem ) ) return 0;
        i2c_start();
        if ( i2c_byte_out( IXGBE_I2C_EEPROM_DEV_ADDR ) && i2c_ack() &&
             i2c_byte_out( offset ) && i2c_ack() ) {
            i2c_start();
            if ( i2c_byte_out( IXGBE_I2C_EEPROM_DEV_ADDR | 1 ) && i2c_ack() ) {
                *value = i2c_byte_in();
                if ( i2c_bit_out( 1 ) ) {           /* no ack: the only byte wanted */
                    i2c_stop();
                    swfw_give( phy_sem );
                    return 1;
                }
            }
        }
        i2c_bus_clear();
        swfw_give( phy_sem );
        wait_ms( 10 );
    }
    return 0;
}

/* the same byte on the 82598, through the NetLogic PHY's I2C master */
static int nl_sfp_byte( u8 offset, u8 *value )
{
    u16 stat = 0, data;
    int i, ok = 0;

    if ( !swfw_take( phy_sem ) ) return 0;
    if ( mdi_write( IXGBE_MDIO_PMA_PMD_DEV_TYPE, IXGBE_MDIO_PMA_PMD_SDA_SCL_ADDR,
                    (u16)( ( ( IXGBE_I2C_EEPROM_DEV_ADDR << 8 ) + offset ) | IXGBE_I2C_EEPROM_READ_MASK ) ) ) {
        for ( i = 0; i < 100; i++ ) {
            if ( !mdi_read( IXGBE_MDIO_PMA_PMD_DEV_TYPE, IXGBE_MDIO_PMA_PMD_SDA_SCL_STAT, &stat ) ) break;
            stat &= IXGBE_I2C_EEPROM_STATUS_MASK;
            if ( stat != IXGBE_I2C_EEPROM_STATUS_IN_PROGRESS ) break;
            wait_ms( 10 );
        }
        if ( stat == IXGBE_I2C_EEPROM_STATUS_PASS &&
             mdi_read( IXGBE_MDIO_PMA_PMD_DEV_TYPE, IXGBE_MDIO_PMA_PMD_SDA_SCL_DATA, &data ) ) {
            *value = (u8)( data >> 8 );
            ok = 1;
        }
    }
    swfw_give( phy_sem );
    return ok;
}

static int sfp_byte( u8 offset, u8 *value )
{
    if ( mac_type == MAC_82598 ) return nl_sfp_byte( offset, value );
    return i2c_sfp_byte( offset, value );
}

/* ---------------------------------------------------------------- */
/* the SFP+ module                                                   */
/* ---------------------------------------------------------------- */

/* What is in the cage, as ixgbe_identify_sfp_module_generic sorts it,
   without its rule that optics be Intel's */
static void sfp_identify( void )
{
    u8  ident = 0, comp10 = 0, comp1 = 0, tech = 0, spec = 0;
    u16 core = (u16)( lan_id ? 1 : 0 );

    sfp_type = SFP_NOT_PRESENT;
    sfp_multispeed = 0;
    sfp_1g = 0;
    sfp_text = "no module";
    if ( !sfp_byte( IXGBE_SFF_IDENTIFIER, &ident ) ) return;
    if ( ident != IXGBE_SFF_IDENTIFIER_SFP ) {
        sfp_type = SFP_UNKNOWN;
        sfp_text = "not an SFP+ module";
        return;
    }
    if ( !sfp_byte( IXGBE_SFF_10GBE_COMP_CODES, &comp10 ) || !sfp_byte( IXGBE_SFF_1GBE_COMP_CODES, &comp1 ) ||
         !sfp_byte( IXGBE_SFF_CABLE_TECHNOLOGY, &tech ) ) {
        sfp_text = "module not readable";
        return;
    }
    sfp_type = SFP_UNKNOWN;
    sfp_text = "module not supported";
    if ( mac_type == MAC_82598 ) {
        if ( tech & IXGBE_SFF_DA_PASSIVE_CABLE ) { sfp_type = SFP_DA_CU; sfp_text = "DA cable"; }
        else if ( comp10 & IXGBE_SFF_10GBASESR_CAPABLE ) { sfp_type = SFP_SR; sfp_text = "SR module"; }
        else if ( comp10 & IXGBE_SFF_10GBASELR_CAPABLE ) { sfp_type = SFP_LR; sfp_text = "LR module"; }
        return;
    }
    if ( tech & IXGBE_SFF_DA_PASSIVE_CABLE ) {
        sfp_type = (u16)( SFP_DA_CU_CORE0 + core );
        sfp_text = "DA cable";
    } else if ( tech & IXGBE_SFF_DA_ACTIVE_CABLE ) {
        if ( sfp_byte( IXGBE_SFF_CABLE_SPEC_COMP, &spec ) && ( spec & IXGBE_SFF_DA_SPEC_ACTIVE_LIMITING ) ) {
            sfp_type = (u16)( SFP_DA_ACT_CORE0 + core );
            sfp_text = "active DA cable";
        }
    } else if ( comp10 & ( IXGBE_SFF_10GBASESR_CAPABLE | IXGBE_SFF_10GBASELR_CAPABLE ) ) {
        sfp_type = (u16)( SFP_SRLR_CORE0 + core );
        sfp_text = ( comp10 & IXGBE_SFF_10GBASESR_CAPABLE ) ? "SR module" : "LR module";
    } else if ( comp1 & IXGBE_SFF_1GBASET_CAPABLE ) {
        sfp_type = (u16)( SFP_1G_CU_CORE0 + core );
        sfp_text = "1G copper module";
        sfp_1g = 1;
    } else if ( comp1 & IXGBE_SFF_1GBASESX_CAPABLE ) {
        sfp_type = (u16)( SFP_1G_SX_CORE0 + core );
        sfp_text = "1G SX module";
        sfp_1g = 1;
    } else if ( comp1 & IXGBE_SFF_1GBASELX_CAPABLE ) {
        sfp_type = (u16)( SFP_1G_LX_CORE0 + core );
        sfp_text = "1G LX module";
        sfp_1g = 1;
    }
    /* Linux's rule for a dual-rate optic (FreeBSD's newer one also counts
       DA cables, which have no rate select to set) */
    if ( ( ( comp1 & IXGBE_SFF_1GBASESX_CAPABLE ) && ( comp10 & IXGBE_SFF_10GBASESR_CAPABLE ) ) ||
         ( ( comp1 & IXGBE_SFF_1GBASELX_CAPABLE ) && ( comp10 & IXGBE_SFF_10GBASELR_CAPABLE ) ) ) sfp_multispeed = 1;
}

/* Where the EEPROM keeps the setup for this kind of module: a list of
   (id, offset) pairs at the word word 2Bh points to.  Active cables and
   1G modules are set up as SR (ixgbe_get_sfp_init_sequence_offsets) */
static int sfp_offsets( u16 *data_offset )
{
    u16 type = sfp_type, list, id;
    int i;

    if ( mac_type == MAC_82598 && device_id == 0x10E1 && type == SFP_DA_CU ) return 0;
    if ( mac_type == MAC_82599 ) {
        if ( type == SFP_DA_ACT_CORE0 || type == SFP_1G_CU_CORE0 || type == SFP_1G_SX_CORE0 || type == SFP_1G_LX_CORE0 ) {
            type = SFP_SRLR_CORE0;
        } else if ( type == SFP_DA_ACT_CORE0 + 1 || type == SFP_1G_CU_CORE0 + 1 || type == SFP_1G_SX_CORE0 + 1 ||
                    type == SFP_1G_LX_CORE0 + 1 ) {
            type = SFP_SRLR_CORE0 + 1;
        }
    }
    if ( !ee_read( IXGBE_PHY_INIT_OFFSET_NL, &list ) || list == 0 || list == 0xFFFF ) return 0;
    list++;
    for ( i = 0; i < 64; i++ ) {
        if ( !ee_read( list, &id ) || id == IXGBE_PHY_INIT_END_NL ) return 0;
        if ( id == type ) {
            if ( !ee_read( (u32)list + 1, data_offset ) ) return 0;
            return *data_offset != 0 && *data_offset != 0xFFFF;
        }
        list = (u16)( list + 2 );
    }
    return 0;
}

/* 82599: the module's setup, a run of words for CORECTL ended by FFFFh,
   written under the MAC's semaphore (ixgbe_setup_sfp_modules_82599) */
static int sfp_setup_82599( void )
{
    u16 offset, word = 0;
    int i, ok = 0;

    if ( !sfp_offsets( &offset ) ) { say( "the EEPROM has no setup for this module" ); return 0; }
    if ( !swfw_take( IXGBE_GSSR_MAC_CSR_SM ) ) { say( "the firmware would not give up the MAC" ); return 0; }
    for ( i = 0; i < 256; i++ ) {
        offset++;
        if ( !ee_read( offset, &word ) ) break;
        if ( word == 0xFFFF ) { ok = 1; break; }
        wr( IXGBE_CORECTL, word );
        flush();
    }
    swfw_give( IXGBE_GSSR_MAC_CSR_SM );
    wait_ms( 10 );                      /* the firmware's turn at the semaphore */
    if ( !ok ) say( "the module's setup in the EEPROM could not be read" );
    return ok;
}

/* 82598: the NetLogic PHY reset, then its setup - a script in the
   EEPROM of delays and runs of PHY register writes (ixgbe_reset_phy_nl).
   Bounded in steps and in the time its delays may add up to, so a
   damaged EEPROM cannot hold the load up for long */
static int nl_reset( void )
{
    u16 offset, eword, reg, ctl = 0;
    u16 edata;
    u32 slept = 0;
    int i, steps;

    if ( !sfp_offsets( &offset ) ) { say( "the EEPROM has no setup for this module" ); return 0; }
    if ( !phy_read( IXGBE_MDIO_PHY_XS_DEV_TYPE, IXGBE_MDIO_PHY_XS_CONTROL, &ctl ) ||
         !phy_write( IXGBE_MDIO_PHY_XS_DEV_TYPE, IXGBE_MDIO_PHY_XS_CONTROL, (u16)( ctl | IXGBE_MDIO_PHY_XS_RESET ) ) ) {
        say( "the PHY did not answer" );
        return 0;
    }
    for ( i = 0; i < 100; i++ ) {
        if ( phy_read( IXGBE_MDIO_PHY_XS_DEV_TYPE, IXGBE_MDIO_PHY_XS_CONTROL, &ctl ) && !( ctl & IXGBE_MDIO_PHY_XS_RESET ) ) break;
        wait_ms( 10 );
    }
    if ( i == 100 ) { say( "the PHY reset did not finish" ); return 0; }
    offset++;                           /* past the block's CRC word */
    for ( steps = 0; steps < 1024; steps++ ) {
        if ( !ee_read( offset, &eword ) ) break;
        ctl = (u16)( eword >> IXGBE_CONTROL_SHIFT_NL );
        edata = (u16)( eword & IXGBE_DATA_MASK_NL );
        offset++;
        if ( ctl == IXGBE_DELAY_NL ) {
            slept += edata;
            if ( slept > 3000 ) break;
            wait_ms( edata );
        } else if ( ctl == IXGBE_DATA_NL ) {
            if ( !ee_read( offset, &reg ) ) break;
            offset++;
            for ( i = 0; i < (int)edata; i++ ) {
                if ( !ee_read( offset, &eword ) || !phy_write( IXGBE_TWINAX_DEV, reg, eword ) ) break;
                offset++;
                reg++;
            }
            if ( i < (int)edata ) break;
        } else if ( ctl == IXGBE_CONTROL_NL && edata == IXGBE_CONTROL_EOL_NL ) {
            return 1;
        } else if ( ctl != IXGBE_CONTROL_NL || edata != IXGBE_CONTROL_SOL_NL ) {
            break;
        }
    }
    say( "the PHY setup in the EEPROM is not usable" );
    return 0;
}

/* ---------------------------------------------------------------- */
/* the MAC's link                                                    */
/* ---------------------------------------------------------------- */

/* 82599: the link pipeline restarted - LMS toggled with Restart_AN, held
   until ANLP1 shows the state machine moving, then put back
   (ixgbe_reset_pipeline_82599).  The caller holds the MAC semaphore if
   LESM wants it */
static int pipeline_reset( void )
{
    u32 autoc, autoc2, anlp1 = 0;
    int i;

    autoc2 = rd( IXGBE_AUTOC2 );
    if ( autoc2 & IXGBE_AUTOC2_LINK_DISABLE_MASK ) {
        wr( IXGBE_AUTOC2, autoc2 & ~IXGBE_AUTOC2_LINK_DISABLE_MASK );
        flush();
    }
    autoc = rd( IXGBE_AUTOC ) | IXGBE_AUTOC_AN_RESTART;
    wr( IXGBE_AUTOC, autoc ^ ( 0x4L << IXGBE_AUTOC_LMS_SHIFT ) );
    for ( i = 0; i < 10; i++ ) {
        wait_ms( 4 );
        anlp1 = rd( IXGBE_ANLP1 );
        if ( anlp1 & IXGBE_ANLP1_AN_STATE_MASK ) break;
    }
    wr( IXGBE_AUTOC, autoc );
    flush();
    return ( anlp1 & IXGBE_ANLP1_AN_STATE_MASK ) != 0;
}

/* AUTOC written as the shared code's prot_autoc_write: on the 82599
   not while the firmware vetoes it, under the MAC semaphore if LESM
   runs, and followed by a pipeline reset */
static int autoc_write( u32 value )
{
    int locked = 0, ok;

    if ( mac_type == MAC_82598 ) {
        wr( IXGBE_AUTOC, value );
        return 1;
    }
    if ( reset_blocked() ) return 0;
    if ( lesm ) {
        if ( !swfw_take( IXGBE_GSSR_MAC_CSR_SM ) ) return 0;
        locked = 1;
    }
    wr( IXGBE_AUTOC, value );
    ok = pipeline_reset();
    if ( locked ) swfw_give( IXGBE_GSSR_MAC_CSR_SM );
    return ok;
}

/* the MAC's link restarted as it stands: AUTOC's Restart_AN on the
   82598, the pipeline reset on the 82599 (ixgbe_start_mac_link_*) */
static void mac_restart( void )
{
    int locked = 0;

    if ( mac_type == MAC_82598 ) {
        wr( IXGBE_AUTOC, rd( IXGBE_AUTOC ) | IXGBE_AUTOC_AN_RESTART );
        return;
    }
    if ( mac_type != MAC_82599 ) return;
    if ( lesm ) {
        if ( !swfw_take( IXGBE_GSSR_MAC_CSR_SM ) ) { say( "the firmware would not give up the MAC" ); return; }
        locked = 1;
    }
    if ( !pipeline_reset() ) say( "the link did not restart (ANLP1)" );
    if ( locked ) swfw_give( IXGBE_GSSR_MAC_CSR_SM );
}

/* ---------------------------------------------------------------- */
/* bringing the link up, by kind                                     */
/* ---------------------------------------------------------------- */

/* A 10GBASE-T PHY on MDIO: powered up (X540/X550), 10G, 1G and 100M
   advertised where the PHY can run each speed, autonegotiation restarted;
   then the MAC's side restarted (ixgbe_setup_phy_link_generic, _tnx) */
static void link_copper( void )
{
    u16 ability, value;

    phy_look();
    if ( phy_kind == PHY_NONE ) { say( "no PHY answered" ); return; }
    if ( !swfw_take( phy_sem ) ) { say( "the firmware would not give up the PHY" ); return; }
    if ( mac_type >= MAC_X540 &&
         mdi_read( IXGBE_MDIO_VENDOR_SPECIFIC_1_DEV_TYPE, IXGBE_MDIO_VENDOR_SPECIFIC_1_CONTROL, &value ) &&
         ( value & IXGBE_MDIO_PHY_SET_LOW_POWER_MODE ) ) {
        (void)mdi_write( IXGBE_MDIO_VENDOR_SPECIFIC_1_DEV_TYPE, IXGBE_MDIO_VENDOR_SPECIFIC_1_CONTROL,
                         (u16)( value & ~IXGBE_MDIO_PHY_SET_LOW_POWER_MODE ) );
    }
    if ( !mdi_read( IXGBE_MDIO_PMA_PMD_DEV_TYPE, IXGBE_MDIO_PHY_SPEED_ABILITY, &ability ) ) {
        ability = IXGBE_MDIO_PHY_SPEED_10G | IXGBE_MDIO_PHY_SPEED_1G | IXGBE_MDIO_PHY_SPEED_100M;
    }
    if ( mdi_read( IXGBE_MDIO_AUTO_NEG_DEV_TYPE, IXGBE_MII_10GBASE_T_AUTONEG_CTRL_REG, &value ) ) {
        value = (u16)( value & ~IXGBE_MII_10GBASE_T_ADVERTISE );
        if ( ability & IXGBE_MDIO_PHY_SPEED_10G ) value |= IXGBE_MII_10GBASE_T_ADVERTISE;
        (void)mdi_write( IXGBE_MDIO_AUTO_NEG_DEV_TYPE, IXGBE_MII_10GBASE_T_AUTONEG_CTRL_REG, value );
    }
    if ( phy_kind == PHY_TN ) {
        /* the TN1010 keeps 1G in the XNP transmit register */
        if ( ( ability & IXGBE_MDIO_PHY_SPEED_1G ) &&
             mdi_read( IXGBE_MDIO_AUTO_NEG_DEV_TYPE, IXGBE_MII_AUTONEG_XNP_TX_REG, &value ) ) {
            (void)mdi_write( IXGBE_MDIO_AUTO_NEG_DEV_TYPE, IXGBE_MII_AUTONEG_XNP_TX_REG,
                             (u16)( value | IXGBE_MII_1GBASE_T_ADVERTISE_XNP_TX ) );
        }
        if ( ( ability & IXGBE_MDIO_PHY_SPEED_100M ) &&
             mdi_read( IXGBE_MDIO_AUTO_NEG_DEV_TYPE, IXGBE_MII_AUTONEG_ADVERTISE_REG, &value ) ) {
            (void)mdi_write( IXGBE_MDIO_AUTO_NEG_DEV_TYPE, IXGBE_MII_AUTONEG_ADVERTISE_REG,
                             (u16)( value | IXGBE_MII_100BASE_T_ADVERTISE ) );
        }
    } else {
        if ( mdi_read( IXGBE_MDIO_AUTO_NEG_DEV_TYPE, IXGBE_MII_AUTONEG_VENDOR_PROVISION_1_REG, &value ) ) {
            value = (u16)( value & ~IXGBE_MII_1GBASE_T_ADVERTISE );
            if ( mac_type == MAC_X550 ) value = (u16)( value & ~( IXGBE_MII_2_5GBASE_T_ADVERTISE | IXGBE_MII_5GBASE_T_ADVERTISE ) );
            if ( ability & IXGBE_MDIO_PHY_SPEED_1G ) value |= IXGBE_MII_1GBASE_T_ADVERTISE;
            (void)mdi_write( IXGBE_MDIO_AUTO_NEG_DEV_TYPE, IXGBE_MII_AUTONEG_VENDOR_PROVISION_1_REG, value );
        }
        if ( mdi_read( IXGBE_MDIO_AUTO_NEG_DEV_TYPE, IXGBE_MII_AUTONEG_ADVERTISE_REG, &value ) ) {
            value = (u16)( value & ~( IXGBE_MII_100BASE_T_ADVERTISE | IXGBE_MII_100BASE_T_ADVERTISE_HALF ) );
            if ( ability & IXGBE_MDIO_PHY_SPEED_100M ) value |= IXGBE_MII_100BASE_T_ADVERTISE;
            (void)mdi_write( IXGBE_MDIO_AUTO_NEG_DEV_TYPE, IXGBE_MII_AUTONEG_ADVERTISE_REG, value );
        }
    }
    if ( !reset_blocked() && mdi_read( IXGBE_MDIO_AUTO_NEG_DEV_TYPE, IXGBE_MDIO_AUTO_NEG_CONTROL, &value ) ) {
        (void)mdi_write( IXGBE_MDIO_AUTO_NEG_DEV_TYPE, IXGBE_MDIO_AUTO_NEG_CONTROL, (u16)( value | IXGBE_MII_RESTART ) );
    }
    swfw_give( phy_sem );
    mac_restart();
}

/* the laser: SDP3 drives the cage's TX_DISABLE */
static void laser( int on )
{
    u32 esdp = rd( IXGBE_ESDP );

    wr( IXGBE_ESDP, on ? ( esdp & ~IXGBE_ESDP_SDP3 ) : ( esdp | IXGBE_ESDP_SDP3 ) );
    flush();
}

/* 82599 SFP+ (the shared code's setup_sfp, then setup_link) */
static void link_sfp_82599( void )
{
    u32 autoc, autoc2;
    int own_laser = !mng_enabled();

    if ( own_laser ) laser( 1 );
    sfp_identify();
    if ( sfp_type == SFP_NOT_PRESENT || sfp_type == SFP_UNKNOWN ) return;
    if ( !sfp_setup_82599() ) return;
    autoc = orig_autoc | IXGBE_AUTOC_LMS_10G_SERIAL;
    autoc2 = rd( IXGBE_AUTOC2 );
    if ( sfp_1g && ( autoc2 & IXGBE_AUTOC2_10G_SERIAL_PMA_PMD_MASK ) == IXGBE_AUTOC2_10G_SFI &&
         ( autoc & IXGBE_AUTOC_1G_PMA_PMD_MASK ) == IXGBE_AUTOC_1G_SFI ) {
        autoc = ( autoc & ~IXGBE_AUTOC_LMS_MASK ) | IXGBE_AUTOC_LMS_1G_AN;
    }
    if ( sfp_multispeed ) {
        /* a dual-rate optic: RS0 high for 10G, and the module given
           40ms to change its analog side */
        wr( IXGBE_ESDP, rd( IXGBE_ESDP ) | IXGBE_ESDP_SDP5_DIR | IXGBE_ESDP_SDP5 );
        flush();
        wait_ms( 40 );
    }
    if ( !autoc_write( autoc ) ) say( "the link setup did not finish (AUTOC)" );
    if ( sfp_multispeed && own_laser ) {
        /* the far end is made to try again: a loss of light */
        laser( 0 );
        net_delay_us( 100 );
        laser( 1 );
    }
}

/* 82598: a NetLogic PHY means an SFP+ cage; a copper PHY 10GBASE-T;
   anything else (fixed optics, CX4, KX4) is restarted as it stands */
static void link_82598( void )
{
    phy_look();
    if ( phy_kind == PHY_NL ) {
        sfp_identify();
        if ( sfp_type != SFP_NOT_PRESENT && sfp_type != SFP_UNKNOWN ) (void)nl_reset();
    } else if ( phy_kind == PHY_TN || phy_kind == PHY_CU ) {
        link_copper();
        return;
    } else if ( media == MED_SFP ) {
        say( "the SFP+ PHY did not answer" );
    }
    mac_restart();
}

static void link_start( void )
{
    if ( mac_type >= MAC_X540 || media == MED_COPPER ) link_copper();
    else if ( mac_type == MAC_82598 ) link_82598();
    else if ( media == MED_SFP ) link_sfp_82599();
    else mac_restart();
}

static int is_copper( void )
{
    return mac_type >= MAC_X540 || media == MED_COPPER || phy_kind == PHY_TN || phy_kind == PHY_CU;
}

static const char *speed_text( u32 links )
{
    if ( mac_type == MAC_82598 ) return ( links & IXGBE_LINKS_SPEED ) ? "10 Gb/s" : "1 Gb/s";
    switch ( links & IXGBE_LINKS_SPEED_82599 ) {
    case IXGBE_LINKS_SPEED_10G_82599:
        return ( mac_type >= MAC_X550 && ( links & IXGBE_LINKS_SPEED_NON_STD ) ) ? "2.5 Gb/s" : "10 Gb/s";
    case IXGBE_LINKS_SPEED_1G_82599:
        return "1 Gb/s";
    case IXGBE_LINKS_SPEED_100_82599:
        return ( mac_type == MAC_X550 && ( links & IXGBE_LINKS_SPEED_NON_STD ) ) ? "5 Gb/s" : "100 Mb/s";
    }
    return "10 Mb/s";
}

/* at the load: the link waited for, then one line - the module, if the
   port has a cage, and the speed or "no link" with LINKS as it stood */
static void link_report( void )
{
    u32 links = 0;
    int i, limit = is_copper() ? 100 : 60;  /* 50ms steps: 5s for 10GBASE-T */
    int cage = media == MED_SFP && ( mac_type != MAC_82598 || phy_kind == PHY_NL );

    if ( cage && ( sfp_type == SFP_NOT_PRESENT || sfp_type == SFP_UNKNOWN ) ) limit = 0;
    for ( i = 0; i <= limit; i++ ) {
        links = rd( IXGBE_LINKS );
        if ( ( links & IXGBE_LINKS_UP ) || i == limit ) break;
        wait_ms( 50 );
    }
    say_begin();
    if ( cage ) {
        say_text( sfp_text );
        say_text( ", " );
    }
    if ( links & IXGBE_LINKS_UP ) {
        say_text( speed_text( links ) );
    } else {
        say_text( "no link (LINKS " );
        say_hex( links, 8 );
        say_text( ")" );
    }
    say_end();
}

/* ---------------------------------------------------------------- */
/* stopping and resetting                                            */
/* ---------------------------------------------------------------- */

/* no interrupt, nothing received, queue 0 stopped (Linux's
   ixgbe_disable_rx_queue / _tx_queue: waited for, except on an 82598
   with no link, whose ENABLE does not change then); ALL also stops
   every queue an earlier owner may have used, as ixgbe_stop_adapter
   does before a reset */
static void queues_off( int all )
{
    int i, rxq = 1, txq = 1;
    u32 value;

    wr( IXGBE_EIMC, 0xFFFFFFFFL );
    if ( mac_type != MAC_82598 ) {
        wr( IXGBE_EIMC_EX( 0 ), 0xFFFFFFFFL );
        wr( IXGBE_EIMC_EX( 1 ), 0xFFFFFFFFL );
    }
    wr( IXGBE_RXCTRL, rd( IXGBE_RXCTRL ) & ~IXGBE_RXCTRL_RXEN );
    if ( all ) {
        rxq = mac_type == MAC_82598 ? 64 : 128;
        txq = mac_type == MAC_82598 ? 32 : 128;
    }
    for ( i = 0; i < txq; i++ ) wr( IXGBE_TXDCTL( i ), IXGBE_TXDCTL_SWFLSH );
    for ( i = 0; i < rxq; i++ ) {
        value = rd( IXGBE_RXDCTL( i ) ) & ~IXGBE_RXDCTL_ENABLE;
        if ( all ) value |= IXGBE_RXDCTL_SWFLSH;
        wr( IXGBE_RXDCTL( i ), value );
    }
    flush();
    if ( all ) {
        wait_ms( 2 );
    } else if ( mac_type != MAC_82598 || ( rd( IXGBE_LINKS ) & IXGBE_LINKS_UP ) ) {
        for ( i = 0; i < 10; i++ ) {
            if ( !( rd( IXGBE_RXDCTL( 0 ) ) & IXGBE_RXDCTL_ENABLE ) && !( rd( IXGBE_TXDCTL( 0 ) ) & IXGBE_TXDCTL_ENABLE ) ) break;
            wait_ms( 1 );
        }
    }
    (void)rd( IXGBE_EICR );
    wr( IXGBE_EICR, 0xFFFFFFFFL );
}

/* 82598: Atlas transmit lanes powered down by a loopback test stay down
   through a reset (ixgbe_reset_hw_82598) */
static u8 atlas_read( u32 reg )
{
    wr( IXGBE_ATLASCTL, IXGBE_ATLASCTL_WRITE_CMD | ( reg << 8 ) );
    flush();
    net_delay_us( 10 );
    return (u8)rd( IXGBE_ATLASCTL );
}

static void atlas_write( u32 reg, u8 value )
{
    wr( IXGBE_ATLASCTL, ( reg << 8 ) | value );
    flush();
    net_delay_us( 10 );
}

static void atlas_power_up( void )
{
    if ( !( atlas_read( IXGBE_ATLAS_PDN_LPBK ) & IXGBE_ATLAS_PDN_TX_REG_EN ) ) return;
    atlas_write( IXGBE_ATLAS_PDN_LPBK, (u8)( atlas_read( IXGBE_ATLAS_PDN_LPBK ) & ~IXGBE_ATLAS_PDN_TX_REG_EN ) );
    atlas_write( IXGBE_ATLAS_PDN_10G, (u8)( atlas_read( IXGBE_ATLAS_PDN_10G ) & ~IXGBE_ATLAS_PDN_TX_QL_ALL ) );
    atlas_write( IXGBE_ATLAS_PDN_1G, (u8)( atlas_read( IXGBE_ATLAS_PDN_1G ) & ~IXGBE_ATLAS_PDN_TX_QL_ALL ) );
    atlas_write( IXGBE_ATLAS_PDN_AN, (u8)( atlas_read( IXGBE_ATLAS_PDN_AN ) & ~IXGBE_ATLAS_PDN_TX_QL_ALL ) );
}

/* when bus mastering would not stop: the PCIe block's buffers cleared
   with the MAC in loopback, so nothing goes out meanwhile
   (ixgbe_clear_tx_pending) */
static void tx_pending_clear( void )
{
    u32 hlreg0 = rd( IXGBE_HLREG0 ), gcr_ext;
    int i;

    wr( IXGBE_HLREG0, hlreg0 | IXGBE_HLREG0_LPBK );
    flush();
    wait_ms( 3 );
    for ( i = 0; i < 800; i++ ) {
        if ( !( net_pci_cfg_read( net_pciaddr, IXGBE_PCI_DEVICE_STATUS, 2 ) & IXGBE_PCI_DEVICE_STATUS_TRANSACTION_PENDING ) ) break;
        net_delay_us( 100 );
    }
    gcr_ext = rd( IXGBE_GCR_EXT );
    wr( IXGBE_GCR_EXT, gcr_ext | IXGBE_GCR_EXT_BUFFERS_CLEAR );
    flush();
    net_delay_us( 20 );
    wr( IXGBE_GCR_EXT, gcr_ext );
    wr( IXGBE_HLREG0, hlreg0 );
}

static int hw_reset( void )
{
    u32 ctrl, autoc, autoc2 = 0;
    int i, pass, passes = 1, locked;

    queues_off( 1 );

    /* bus mastering off and its requests finished (ixgbe_disable_pcie_primary) */
    wr( IXGBE_CTRL, IXGBE_CTRL_GIO_DIS );
    for ( i = 0; i < IXGBE_PCI_PRIMARY_DISABLE_TIMEOUT && ( rd( IXGBE_STATUS ) & IXGBE_STATUS_GIO ); i++ ) net_delay_us( 100 );
    if ( rd( IXGBE_STATUS ) & IXGBE_STATUS_GIO ) {
        passes = 2;
        if ( mac_type != MAC_82598 ) tx_pending_clear();
    }
    if ( mac_type == MAC_82598 ) atlas_power_up();

    for ( pass = 0; pass < passes; pass++ ) {
        ctrl = IXGBE_CTRL_RST;
        if ( mac_type == MAC_82599 && !( rd( IXGBE_LINKS ) & IXGBE_LINKS_UP ) ) ctrl = IXGBE_CTRL_LNK_RST;
        locked = mac_type >= MAC_X540 && swfw_take( phy_sem );
        wr( IXGBE_CTRL, rd( IXGBE_CTRL ) | ctrl );
        flush();
        if ( locked ) swfw_give( phy_sem );
        for ( i = 0; i < 100 && ( rd( IXGBE_CTRL ) & IXGBE_CTRL_RST_MASK ); i++ ) net_delay_us( 10 );
        if ( rd( IXGBE_CTRL ) & IXGBE_CTRL_RST_MASK ) { say( "the reset did not finish" ); return 0; }
        wait_ms( mac_type >= MAC_X540 ? 100 : 50 );
    }

    if ( mac_type == MAC_82598 ) {
        wr( IXGBE_GHECCR, rd( IXGBE_GHECCR ) & ~( ( 1L << 21 ) | ( 1L << 18 ) | ( 1L << 9 ) | ( 1L << 6 ) ) );
    }
    if ( mac_type <= MAC_82599 ) {
        autoc = rd( IXGBE_AUTOC );
        if ( mac_type == MAC_82599 ) {
            autoc2 = rd( IXGBE_AUTOC2 );
            if ( autoc2 & IXGBE_AUTOC2_LINK_DISABLE_MASK ) {
                autoc2 &= ~IXGBE_AUTOC2_LINK_DISABLE_MASK;
                wr( IXGBE_AUTOC2, autoc2 );
                flush();
            }
        }
        if ( !orig_stored ) {
            orig_autoc = autoc;
            orig_autoc2 = autoc2;
            orig_stored = 1;
        } else {
            if ( autoc != orig_autoc ) (void)autoc_write( orig_autoc );
            if ( mac_type == MAC_82599 && ( autoc2 & IXGBE_AUTOC2_UPPER_MASK ) != ( orig_autoc2 & IXGBE_AUTOC2_UPPER_MASK ) ) {
                wr( IXGBE_AUTOC2, ( autoc2 & ~IXGBE_AUTOC2_UPPER_MASK ) | ( orig_autoc2 & IXGBE_AUTOC2_UPPER_MASK ) );
            }
        }
    } else {
        wr( IXGBE_RXPBSIZE( 0 ), 384L << 10 );     /* the X540's buffer is 384K */
    }

    for ( i = 0; i < 100 && !( rd( IXGBE_EEC ) & IXGBE_EEC_ARD ); i++ ) wait_ms( 1 );
    if ( !( rd( IXGBE_EEC ) & IXGBE_EEC_ARD ) ) say( "the EEPROM was not read in time (EEC.ARD)" );

    wr( IXGBE_EIMC, 0xFFFFFFFFL );
    (void)rd( IXGBE_EICR );
    wr( IXGBE_EICR, 0xFFFFFFFFL );
    link_fresh = 1;
    return 1;
}

/* ---------------------------------------------------------------- */
/* the PCI side                                                      */
/* ---------------------------------------------------------------- */

/* INTx on: the command register's interrupt-disable bit off, and MSI
   and MSI-X off in the capability list (bounded: a broken list ends) */
static void intx_on( void )
{
    u32 cmd, cap, id, ctl;
    int n;

    cmd = net_pci_cfg_read( net_pciaddr, 0x04, 2 );
    if ( cmd == 0xFFFFFFFFL ) return;
    if ( cmd & 0x0400 ) net_pci_cfg_write( net_pciaddr, 0x04, 2, cmd & ~0x0400L );
    if ( !( net_pci_cfg_read( net_pciaddr, 0x06, 2 ) & 0x0010 ) ) return;
    cap = net_pci_cfg_read( net_pciaddr, 0x34, 1 ) & 0xFC;
    for ( n = 0; n < 48 && cap >= 0x40; n++ ) {
        id = net_pci_cfg_read( net_pciaddr, (int)cap, 1 );
        if ( id == 0x05 || id == 0x11 ) {
            ctl = net_pci_cfg_read( net_pciaddr, (int)cap + 2, 2 );
            if ( id == 0x05 && ( ctl & 0x0001 ) ) net_pci_cfg_write( net_pciaddr, (int)cap + 2, 2, ctl & ~0x0001L );
            if ( id == 0x11 && ( ctl & 0x8000 ) ) net_pci_cfg_write( net_pciaddr, (int)cap + 2, 2, ctl & ~0x8000L );
        }
        cap = net_pci_cfg_read( net_pciaddr, (int)cap + 1, 1 ) & 0xFC;
    }
}

/* /N:n on the DEVICE= line, 1..8; only at the load, while NETCORE has
   the line, and kept for an un-park */
static int port_switch( void )
{
    const char *text = net_line;

    if ( text == 0 ) return port_want;
    while ( *text && *text != ' ' && *text != '\t' && *text != '\r' && *text != '\n' ) text++;
    for ( ; *text && *text != '\r' && *text != '\n'; text++ ) {
        if ( text[0] == '/' && ( text[1] | 0x20 ) == 'n' && text[2] == ':' && text[3] >= '1' && text[3] <= '8' ) {
            return text[3] - '0';
        }
    }
    return 1;
}

/* the nth function of the family: by id in ix_ids' order, then in bus
   order (BUS_PCIID's SI) */
static int port_find( int want, const PCI_ID **which )
{
    REGS regs;
    int i, index, seen = 0;

    for ( i = 0; ix_ids[i].vendor; i++ ) {
        for ( index = 0; index < 8; index++ ) {
            net_memzero( &regs, sizeof( regs ) );
            regs.eax = (u32)BUS_PCIID << 8;
            regs.ecx = ix_ids[i].vendor;
            regs.edx = ix_ids[i].device;
            regs.esi = (u32)index;
            if ( !nds_bus( &regs ) ) break;
            if ( ++seen == want ) {
                net_pciaddr = regs.ebx;
                *which = &ix_ids[i];
                return 1;
            }
        }
    }
    return 0;
}

static const IX_KIND *kind_of( u16 device )
{
    int i;

    for ( i = 0; ix_kinds[i].device; i++ ) {
        if ( ix_kinds[i].device == device ) return &ix_kinds[i];
    }
    return 0;
}

static u32 align128( u32 p ) { return ( p + 127 ) & ~127L; }

/* ---------------------------------------------------------------- */
/* the five a card provides                                          */
/* ---------------------------------------------------------------- */

int card_find( void )
{
    const PCI_ID *which;
    const IX_KIND *kind;
    int isio, i;
    u32 bar, at, ral, rah, status;
    u8 *pool;

    if ( net_line ) port_want = port_switch();
    net_strcpy( net_name, "Intel 10 gigabit" );
    if ( port_want <= 1 ) {
        if ( !net_pci_find( ix_ids, &which ) ) return 1;
    } else if ( !port_find( port_want, &which ) ) {
        return 1;
    }
    kind = kind_of( which->device );
    if ( kind == 0 ) return 1;
    mac_type = kind->mac;
    media = kind->media;
    device_id = kind->device;
    net_strcpy( net_name, kind->name );
    net_pci_enable( net_pciaddr );
    intx_on();
    bar = net_pci_bar( net_pciaddr, 0, &isio );     /* BAR0: 64-bit memory */
    if ( bar == 0 || isio ) return 1;
    membase = net_pci_map( bar & 0xFFFFFFF0L, IX_MAPSIZE );
    if ( membase == 0 ) { say( "the registers could not be mapped" ); return 1; }
    net_mem = membase;
    if ( !found_once ) {
        /* once: an un-park comes here again, and the line, /POLL and the
           WfW bridge's choices are NETCORE's from then on */
        net_irq = (u8)net_pci_irqline( net_pciaddr );
        net_bus = NICB_PCI;
        net_flags = NICF_WORD;
    }

    status = rd( IXGBE_STATUS );
    if ( status == 0xFFFFFFFFL ) { say( "the card does not answer" ); return 1; }
    lan_id = (u8)( ( status & IXGBE_STATUS_LAN_ID ) >> IXGBE_STATUS_LAN_ID_SHIFT );
    phy_sem = lan_id ? IXGBE_GSSR_PHY1_SM : IXGBE_GSSR_PHY0_SM;
    if ( mac_type == MAC_X550 && ( rd( IXGBE_FWSM ) & IXGBE_FWSM_FW_NVM_RECOVERY_MODE ) ) {
        say( "the firmware is in recovery mode: the card's NVM needs an update" );
        return 1;
    }
    if ( !found_once ) {
        if ( mac_type >= MAC_X540 ) swfw_init();
        if ( mac_type == MAC_82599 ) lesm = (u8)lesm_check();
    }
    if ( !hw_reset() ) return 1;

    ral = rd( IXGBE_RAL( 0 ) );
    rah = rd( IXGBE_RAH( 0 ) );
    if ( !( rah & IXGBE_RAH_AV ) ) { say( "no station address was loaded (RAH0)" ); return 1; }
    net_mac[0] = (u8)ral;           net_mac[1] = (u8)( ral >> 8 );
    net_mac[2] = (u8)( ral >> 16 ); net_mac[3] = (u8)( ral >> 24 );
    net_mac[4] = (u8)rah;           net_mac[5] = (u8)( rah >> 8 );
    if ( !net_mac_ok( net_mac ) ) { say( "the station address is not usable" ); return 1; }

    if ( rxd == 0 ) {
        /* once, never given back: descriptor rings on 128 bytes (RDBAL's
           and TDBAL's low seven bits are not there), every buffer after
           the rings on 128 too */
        pool = net_alloc( RXN * 16 + TXN * 16 + ( RXN + TXN ) * BUFSZ + 128 );
        if ( pool == 0 ) { say( "no memory for the rings" ); return 1; }
        at = align128( (u32)pool );
        rxd = (volatile u32 *)at;   at += RXN * 16;
        txd = (volatile u32 *)at;   at += TXN * 16;
        for ( i = 0; i < RXN; i++ ) { rxbuf[i] = (u8 *)at; at += BUFSZ; }
        for ( i = 0; i < TXN; i++ ) { txbuf[i] = (u8 *)at; at += BUFSZ; }
    }
    found_once = 1;
    return 0;
}

/* a receive descriptor handed (back) to the chip: the buffer, and a zero
   header address, whose low bit is where DD will be written */
static void rx_arm( int entry )
{
    volatile u32 *desc = rxd + entry * 4;

    desc[0] = (u32)rxbuf[entry];
    desc[1] = 0;
    desc[2] = 0;
    desc[3] = 0;
}

/* RXCTRL.RXEN on; on the 82599 with the security block's receive path
   held off around it (ixgbe_enable_rx_dma_82599) */
static void rx_enable( void )
{
    u32 value = rd( IXGBE_RXCTRL ) | IXGBE_RXCTRL_RXEN;
    int i;

    if ( mac_type == MAC_82598 ) value |= IXGBE_RXCTRL_DMBYPS;
    if ( mac_type != MAC_82599 ) {
        wr( IXGBE_RXCTRL, value );
        return;
    }
    wr( IXGBE_SECRXCTRL, rd( IXGBE_SECRXCTRL ) | IXGBE_SECRXCTRL_RX_DIS );
    for ( i = 0; i < 4000 && !( rd( IXGBE_SECRXSTAT ) & IXGBE_SECRXSTAT_SECRX_RDY ); i++ ) net_delay_us( 10 );
    wr( IXGBE_RXCTRL, value );
    wr( IXGBE_SECRXCTRL, rd( IXGBE_SECRXCTRL ) & ~IXGBE_SECRXCTRL_RX_DIS );
    flush();
}

int card_setup( void )
{
    u32 value;
    int i, linked;

    /* masked and quiet: card_setup comes after card_find, after
       card_stop, and twice running for the WfW bridge */
    queues_off( 0 );

    wr( IXGBE_CTRL_EXT, rd( IXGBE_CTRL_EXT ) | IXGBE_CTRL_EXT_NS_DIS | IXGBE_CTRL_EXT_DRV_LOAD );
    /* relaxed ordering off for queue 0, as start_hw does for every queue */
    if ( mac_type == MAC_82598 ) {
        wr( IXGBE_DCA_TXCTRL( 0 ), rd( IXGBE_DCA_TXCTRL( 0 ) ) & ~IXGBE_DCA_TXCTRL_DESC_WRO_EN );
    } else {
        wr( IXGBE_DCA_TXCTRL_82599( 0 ), rd( IXGBE_DCA_TXCTRL_82599( 0 ) ) & ~IXGBE_DCA_TXCTRL_DESC_WRO_EN );
    }
    wr( IXGBE_DCA_RXCTRL( 0 ), rd( IXGBE_DCA_RXCTRL( 0 ) ) & ~( IXGBE_DCA_RXCTRL_DATA_WRO_EN | IXGBE_DCA_RXCTRL_HEAD_WRO_EN ) );

    /* who is listened to: the station address (RAR0, left as the EEPROM
       loaded it, in pool 0), no other, broadcast and every multicast -
       NetBEUI's 03-00-00-00-00-01 is one - and nothing promiscuous */
    for ( i = 1; i < ( mac_type == MAC_82598 ? 16 : 128 ); i++ ) {
        wr( IXGBE_RAL( i ), 0 );
        wr( IXGBE_RAH( i ), 0 );
    }
    if ( mac_type != MAC_82598 ) {
        wr( IXGBE_MPSAR_LO( 0 ), 1 );
        wr( IXGBE_MPSAR_HI( 0 ), 0 );
    }
    wr( IXGBE_MCSTCTRL, 0 );
    for ( i = 0; i < 128; i++ ) wr( IXGBE_MTA( i ), 0 );
    value = ( rd( IXGBE_FCTRL ) & ~( IXGBE_FCTRL_UPE | IXGBE_FCTRL_SBP ) ) | IXGBE_FCTRL_BAM | IXGBE_FCTRL_MPE;
    if ( mac_type == MAC_82598 ) value |= IXGBE_FCTRL_DPF | IXGBE_FCTRL_PMCF;
    wr( IXGBE_FCTRL, value );
    value = rd( IXGBE_HLREG0 ) | IXGBE_HLREG0_TXCRCEN | IXGBE_HLREG0_RXCRCSTRP | IXGBE_HLREG0_TXPADEN;
    wr( IXGBE_HLREG0, value & ~( IXGBE_HLREG0_JUMBOEN | IXGBE_HLREG0_LPBK ) );
    if ( mac_type != MAC_82598 ) {
        /* Linux's ixgbe_setup_rdrxctl: the CRC stripped here as in
           HLREG0, and the bits the hardware wants set */
        wr( IXGBE_RSCDBU, rd( IXGBE_RSCDBU ) | IXGBE_RSCDBU_RSCACKDIS );
        value = rd( IXGBE_RDRXCTL ) & ~IXGBE_RDRXCTL_RSCFRSTSIZE;
        wr( IXGBE_RDRXCTL, value | IXGBE_RDRXCTL_RSCACKC | IXGBE_RDRXCTL_FCOE_WRFIX | IXGBE_RDRXCTL_CRCSTRIP );
        wr( IXGBE_PSRTYPE( 0 ), IXGBE_PSRTYPE_TCPHDR | IXGBE_PSRTYPE_UDPHDR | IXGBE_PSRTYPE_IPV4HDR | IXGBE_PSRTYPE_IPV6HDR );
    }

    /* the receive ring, queue 0 */
    for ( i = 0; i < RXN; i++ ) rx_arm( i );
    rx_cur = 0;
    rx_skip = 0;
    wr( IXGBE_RDBAL( 0 ), (u32)rxd );
    wr( IXGBE_RDBAH( 0 ), 0 );
    wr( IXGBE_RDLEN( 0 ), RXN * 16 );
    flush();
    wr( IXGBE_RDH( 0 ), 0 );
    wr( IXGBE_RDT( 0 ), 0 );
    value = rd( IXGBE_SRRCTL( 0 ) ) & ~( IXGBE_SRRCTL_BSIZEHDR_MASK | IXGBE_SRRCTL_BSIZEPKT_MASK |
                                        IXGBE_SRRCTL_DESCTYPE_MASK | IXGBE_SRRCTL_DROP_EN );
    wr( IXGBE_SRRCTL( 0 ), value | ( BUFSZ >> IXGBE_SRRCTL_BSIZEPKT_SHIFT ) | IXGBE_SRRCTL_DESCTYPE_ADV_ONEBUF );

    /* the transmit ring, every descriptor free (DD) */
    for ( i = 0; i < TXN; i++ ) {
        txd[i * 4 + 0] = (u32)txbuf[i];
        txd[i * 4 + 1] = 0;
        txd[i * 4 + 2] = 0;
        txd[i * 4 + 3] = IXGBE_TXD_STAT_DD;
    }
    tx_cur = 0;
    wr( IXGBE_TDBAL( 0 ), (u32)txd );
    wr( IXGBE_TDBAH( 0 ), 0 );
    wr( IXGBE_TDLEN( 0 ), TXN * 16 );
    wr( IXGBE_TDH( 0 ), 0 );
    wr( IXGBE_TDT( 0 ), 0 );
    wr( IXGBE_TDWBAL( 0 ), 0 );             /* no head write-back: DD it is */
    wr( IXGBE_TDWBAH( 0 ), 0 );
    if ( mac_type != MAC_82598 ) {
        /* transmit DMA on before any queue, and MTQC set with the DCB
           arbiter held off: one packet buffer, no DCB */
        wr( IXGBE_DMATXCTL, rd( IXGBE_DMATXCTL ) | IXGBE_DMATXCTL_TE );
        value = rd( IXGBE_RTTDCS );
        wr( IXGBE_RTTDCS, value | IXGBE_RTTDCS_ARBDIS );
        wr( IXGBE_MTQC, IXGBE_MTQC_64Q_1PB );
        wr( IXGBE_RTTDCS, value & ~IXGBE_RTTDCS_ARBDIS );
    }

    /* the queues on, and waited for */
    linked = ( rd( IXGBE_LINKS ) & IXGBE_LINKS_UP ) != 0;
    value = rd( IXGBE_TXDCTL( 0 ) ) & ~( IXGBE_TXDCTL_THRESH_MASK | IXGBE_TXDCTL_SWFLSH );
    wr( IXGBE_TXDCTL( 0 ), value | IXGBE_TXDCTL_ENABLE | IXGBE_TXDCTL_THRESH_DEFAULT );
    if ( mac_type != MAC_82598 || linked ) {
        for ( i = 0; i < 10 && !( rd( IXGBE_TXDCTL( 0 ) ) & IXGBE_TXDCTL_ENABLE ); i++ ) wait_ms( 1 );
        if ( !( rd( IXGBE_TXDCTL( 0 ) ) & IXGBE_TXDCTL_ENABLE ) ) say( "the transmit queue did not start (TXDCTL)" );
    }
    value = rd( IXGBE_RXDCTL( 0 ) ) & ~IXGBE_RXDCTL_SWFLSH;
    if ( mac_type == MAC_82598 ) value = ( value & ~IX_RXDCTL_82598_MASK ) | IX_RXDCTL_82598_THRESH;
    wr( IXGBE_RXDCTL( 0 ), value | IXGBE_RXDCTL_ENABLE );
    if ( mac_type != MAC_82598 || linked ) {
        for ( i = 0; i < 10 && !( rd( IXGBE_RXDCTL( 0 ) ) & IXGBE_RXDCTL_ENABLE ); i++ ) wait_ms( 1 );
        if ( !( rd( IXGBE_RXDCTL( 0 ) ) & IXGBE_RXDCTL_ENABLE ) ) say( "the receive queue did not start (RXDCTL)" );
    }
    wr( IXGBE_RDT( 0 ), RXN - 1 );         /* all but one, the gap between tail and head */
    rx_enable();

    /* the causes: receive queue 0 to EICR bit 0, transmit to none, no
       auto-mask or auto-clear, at most some 20,000 a second */
    if ( mac_type == MAC_82598 ) {
        wr( IXGBE_IVAR( 0 ), ( rd( IXGBE_IVAR( 0 ) ) & ~0xFFL ) | IXGBE_IVAR_ALLOC_VAL );
        wr( IXGBE_IVAR( 16 ), rd( IXGBE_IVAR( 16 ) ) & ~0xFFL );
        wr( IXGBE_EITR0, IX_ITR | ( IX_ITR << 16 ) );
    } else {
        wr( IXGBE_IVAR( 0 ), ( rd( IXGBE_IVAR( 0 ) ) & ~0xFFFFL ) | IXGBE_IVAR_ALLOC_VAL );
        wr( IXGBE_EITR0, IX_ITR | IXGBE_EITR_CNT_WDIS );
    }
    wr( IXGBE_EIAM, 0 );
    wr( IXGBE_EIAC, 0 );

    /* the link: after every reset - except on a 10GBASE-T port whose
       link stayed up, since a MAC reset leaves the PHY alone - and
       whenever the link is down */
    linked = ( rd( IXGBE_LINKS ) & IXGBE_LINKS_UP ) != 0;
    if ( !linked || ( link_fresh && !is_copper() ) ) {
        link_start();
    } else if ( net_line && media == MED_SFP && ( mac_type != MAC_82598 || phy_kind == PHY_NL ) ) {
        sfp_identify();                     /* for the line only */
    }
    link_fresh = 0;
    if ( net_line ) link_report();

    /* every cause cleared, and only then unmasked */
    (void)rd( IXGBE_EICR );
    wr( IXGBE_EICR, 0xFFFFFFFFL );
    int_mask = IX_EICR_RXQ0 | IXGBE_EICR_LSC;
    if ( !( net_flags & NICF_POLLED ) ) wr( IXGBE_EIMS, int_mask );
    return 0;
}

void card_stop( void )
{
    queues_off( 0 );
}

int card_send( const u8 *frame, u32 len )
{
    int entry = tx_cur, i;
    volatile u32 *desc = txd + entry * 4;
    u32 size = len < 60 ? 60 : len;

    if ( len > NIC_MAXFRAME ) return 0;
    for ( i = 0; i < 200 && !( desc[3] & IXGBE_TXD_STAT_DD ); i++ ) net_delay_us( 50 );
    if ( !( desc[3] & IXGBE_TXD_STAT_DD ) ) { net_stats.txerrors++; return 0; }
    net_memcpy( txbuf[entry], frame, len );
    if ( len < 60 ) net_memzero( txbuf[entry] + len, 60 - len );
    desc[0] = (u32)txbuf[entry];
    desc[1] = 0;
    desc[2] = size | IXGBE_ADVTXD_DTYP_DATA | IXGBE_ADVTXD_DCMD_DEXT | IXGBE_ADVTXD_DCMD_IFCS |
              IXGBE_ADVTXD_DCMD_EOP | IXGBE_ADVTXD_DCMD_RS;
    desc[3] = size << IXGBE_ADVTXD_PAYLEN_SHIFT;    /* DD cleared with it */
    tx_cur = ( entry + 1 ) % TXN;
    wr( IXGBE_TDT( 0 ), (u32)tx_cur );
    return 1;
}

/* the frames the chip has written back, handed over; how many
   descriptors.  Written back, dword 2 is the status and errors and
   dword 3 the length */
static int rx_drain( void )
{
    volatile u32 *desc;
    u32 staterr, len;
    int n, last = -1, got = 0;
    u8 *slot;

    for ( n = 0; n < RXN; n++ ) {
        desc = rxd + rx_cur * 4;
        staterr = desc[2];
        if ( !( staterr & IXGBE_RXD_STAT_DD ) ) break;
        len = desc[3] & 0xFFFF;
        if ( !( staterr & IXGBE_RXD_STAT_EOP ) ) {
            /* a frame longer than a buffer: never with JUMBOEN off, but
               its pieces are dropped to the end if one comes */
            if ( !rx_skip ) net_stats.rxerrors++;
            rx_skip = 1;
        } else if ( rx_skip ) {
            rx_skip = 0;
        } else if ( ( staterr & IXGBE_RXDADV_ERR_FRAME_ERR_MASK ) || len < NIC_HDRLEN || len > NIC_MAXFRAME ) {
            net_stats.rxerrors++;
        } else {
            slot = net_rx_begin();
            net_memcpy( slot, rxbuf[rx_cur], len );     /* the CRC is already off */
            net_rx_commit( len );
        }
        rx_arm( rx_cur );
        last = rx_cur;
        rx_cur = ( rx_cur + 1 ) % RXN;
        got++;
    }
    if ( last >= 0 ) wr( IXGBE_RDT( 0 ), (u32)last );
    return got;
}

/* masked while EICR is read (the 82598's erratum 26; Linux does so on
   every chip), what was read written back, the ring drained whatever EICR
   said - a polled card still latches its causes, but the ring is the
   truth - and unmasked unless the card is polled.  The interrupt is
   the card's only if a cause it may raise the line for was set (EICR
   latches others, which EIMS keeps off the line) or frames came */
int card_service( void )
{
    u32 eicr;
    int got, masked = !( net_flags & NICF_POLLED );

    if ( masked ) wr( IXGBE_EIMC, 0xFFFFFFFFL );
    eicr = rd( IXGBE_EICR );
    if ( eicr == 0xFFFFFFFFL ) return 0;    /* the card is not answering: not ours */
    if ( eicr ) wr( IXGBE_EICR, eicr );
    got = rx_drain();
    if ( masked ) wr( IXGBE_EIMS, int_mask );
    return ( eicr & int_mask ) != 0 || got != 0;
}

const ND_TABLE nd_table = {
    NDT_SIGVAL, NDT_VERSION, NDTF_NIC,
    { 'i', 'x', 'g', 'b', 'e', ' ', ' ', ' ' },
    ndv_init, ndv_park, ndv_halt, ndv_call
};
