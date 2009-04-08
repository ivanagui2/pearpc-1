/*
 *	PearPC
 *	rtl8139.cc
 *
 *	RealTek 8139 Emulation
 *	References:
 *	[1] pearpc 3c90x driver
 *	[2] Linux Kernel 2.4.22 (drivers/net/rtl8139.c)
 *	[3] realtek 8139 technical specification/programming guide
 *
 *	Copyright (C) 2004 John Kelley (pearpc@kelley.ca)
 *	Copyright (C) 2003 Stefan Weyergraf
 *	Copyright (C) 2004 Eric Estabrooks (estabroo2battlefoundry.net)
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License version 2 as
 *	published by the Free Software Foundation.
 *
 *	This program is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU General Public License for more details.
 *
 *	You should have received a copy of the GNU General Public License
 *	along with this program; if not, write to the Free Software
 *	Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *	FIXME: Implement EthTunDriver write frame prefixing
 */

#include "stdafx.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include "system/sys.h"
#include "system/systhread.h"
#include "system/arch/sysendian.h"
#include "cpu/mem.h"
#include "cpu/debug.h"
#include "system/syseth.h"
#include "tools/crc32.h"
#include "tools/data.h"
#include "tools/debug.h"
#include "tools/except.h"
#include "tools/snprintf.h"
#include "io/pic/pic.h"
#include "io/pci/pci.h"
#include "debug/tracers.h"
#include "rtl8139.h"

#define MAX_PACKET_SIZE		6000
#define MAX_PACKETS		128


enum RxHeaderBits {
	Rx_ROK =  1<<0, // receive okay
	Rx_FAE =  1<<1, // frame alignment error
	Rx_CRC =  1<<2, // crc error
	Rx_LONG = 1<<3, // long packet > 4k
	Rx_RUNT = 1<<4, // runt packet < 64 bytes
	Rx_ISE  = 1<<5, // invalid symbol error
	/* bits 6-12 reserved */
	Rx_BAR = 1<<13, // broadcast
	Rx_PAM = 1<<14, // exact match
	Rx_MAR = 1<<15, // multicast
};

enum TxConfigurationBits {
	Tx_CRC   = 1<<16, // Append CRC
};

enum RxConfigurationBits {
	Rx_AAP   = 1<<0,  // Accept Physical Address Packets (any packet)
	Rx_APM   = 1<<1,  // Accept Physical Match Packets (my MAC address)
	Rx_AM    = 1<<2,  // Accept Multicast Packets
	Rx_AB    = 1<<3,  // Accept Broadcast Packets
	Rx_AR    = 1<<4,  // Accept Runt Packets (<64 bytes)
	Rx_AER   = 1<<5,  // Accept Error Packets (CRC error)
	Rx_RBLEN = 3<<11, // Rx Buffer Length
};

/* registers */
#ifdef TARGET_COMPILER_VC
#pragma pack(push,1)
#endif
struct Registers {
	uint8  id0; // 0x00 (mac address)
	uint8  id1;
	uint8  id2;
	uint8  id3;
	uint8  id4;
	uint8  id5;
	uint16 rsvd0; // 0x06-0x07
	uint8  mar0;
	uint8  mar1;
	uint8  mar2;
	uint8  mar3;
	uint8  mar4;
	uint8  mar5;
	uint8  mar6;
	uint8  mar7;
 	uint32 TxStatusD0; // 0x10
 	uint32 TxStatusD1; // 0x14
 	uint32 TxStatusD2; // 0x18
 	uint32 TxStatusD3; // 0x1c
	uint32 TxStartAddrD0; // 0x20
	uint32 TxStartAddrD1; // 0x24
	uint32 TxStartAddrD2; // 0x28
	uint32 TxStartAddrD3; // 0x2c
	uint32 RxBufferStartAddr; // 0x30
	uint16 EarlyRxByteCount; // 0x34
	uint8  EarlyRxStatus; // 0x36
	uint8  CommandRegister; // 0x37
	uint16 CAPR; // 0x38 initial 0xfff0
	uint16 CBA; // 0x3a initial 0x0000
	uint16 InterruptMask; // 0x3c
	uint16 InterruptStatus; // 0x3e
	uint32 TxConfiguration; // 0x40
	uint32 RxConfiguration; // 0x44
	uint32 TimerCount; // 0x48
	uint32 MissedPacketCounter; // 0x4c
	uint8  Command93C46; //0x50
	uint8  Config0; // 0x51
	uint8  Config1; // 0x52
	uint8  rsvd1 ; // 0x53
	uint32 TimerInterrupt; // 0x54
	uint8  MediaStatus; // 0x58
	uint8  Config3; // 0x59
	uint8  Config4; // 0x5a
	uint8  rsvd2; // 0x5b
	uint16 MultipleInterruptSelect; // 0x5c
	uint8  PCIRevisionID; // 0x5e should be 0x10
	uint8  rsvd3; // 0x5f
	uint16 TSAD; // 0x60 Transmit Status of All Descriptors
	uint16 BMCR; // 0x62 Basic Mode Control
	uint16 BMSR; // 0x64 Basic Mode Status
	uint16 ANAR; // 0x66 Auto-Negotiation Advertisement
	uint16 ANLPAR; // 0x68 "" Link Partner
	uint16 ANER; // 0x6a "" Expansion
	uint16 DisconnectCounter; // 0x6c
	uint16 FalseCarrierSenseCounter; // 0x6e
	uint16 NWayTest; // 0x70
	uint16 RX_ER_Counter; //0x72
	uint16 CSConfiguration; // 0x74
	uint16 rsvd4;
	uint32 PHY1; //0x78
	uint32 Twister; // 0x7c
	uint8  PHY2; // 0x80
} PACKED;
#ifdef TARGET_COMPILER_VC
#pragma pack(pop)
#endif

struct Packet {
	uint32  pid;
	uint16	size;
	byte	packet[MAX_PACKET_SIZE];
};

// IEEE 802.3 MAC, Ethernet-II
#ifdef TARGET_COMPILER_VC
#pragma pack(push,1)
#endif
struct EthFrameII {
	byte	destMAC[6];
	byte	srcMAC[6];
	byte	type[2];
} PACKED;
#ifdef TARGET_COMPILER_VC
#pragma pack(pop)
#endif

enum EEPROMField {
        EEPROM_NodeAddress0 =           0x00,
        EEPROM_NodeAddress1 =           0x01,
        EEPROM_NodeAddress2 =           0x02,
        EEPROM_DeviceID =               0x03,
        EEPROM_ManifacturerID =         0x07,
        EEPROM_PCIParam =               0x08,
        EEPROM_RomInfo =                0x09,
        EEPROM_OEMNodeAddress0 =        0x0a,
        EEPROM_OEMNodeAddress1 =        0x0b,
        EEPROM_OEMNodeAddress2 =        0x0c,
        EEPROM_SoftwareInfo =           0x0d,
        EEPROM_CompWord =               0x0e,
        EEPROM_SoftwareInfo2 =          0x0f,
        EEPROM_Caps =                   0x10,
        EEPROM_InternalConfig0 =        0x12,
        EEPROM_InternalConfig1 =        0x13,
        EEPROM_SubsystemVendorID =      0x17,
        EEPROM_SubsystemID =            0x18,
        EEPROM_MediaOptions =           0x19,
        EEPROM_SmbAddress =             0x1b,
        EEPROM_PCIParam2 =              0x1c,
        EEPROM_PCIParam3 =              0x1d,
        EEPROM_Checksum =               0x20
};

/*
 *
 */
class rtl8139_NIC: public PCI_Device {
protected:
	uint16		mEEPROM[0x40];
	bool		mEEPROMWritable;
	Registers	mRegisters;
	uint16		mIntStatus;
	int		mRingBufferSize;
	bool	 	mGoodBSA;
	EthDevice *	mEth;
	sys_mutex	mLock;
	int		mVerbose;
	byte		mHead;
	byte		mTail;
	byte		mActive;
	byte		mWatermark;
	byte            mLast;
	byte            mLastPackets[2];
	uint32		mPid;
	Packet		mPackets[MAX_PACKETS];
	byte		mMAC[6];

void PCIReset()
{
	IO_RTL8139_TRACE("PCIReset()\n");	// PCI config
	memset(mConfig, 0, sizeof mConfig);
	// 0-3 set by totalReset()
//	mConfig[0x04] = 0x07;	// io+memory+master

	mConfig[0x08] = 0x00;	// revision
	mConfig[0x09] = 0x00; 	//
	mConfig[0x0a] = 0x00;	// ClassCode 0x20000: Ethernet network controller
	mConfig[0x0b] = 0x02;	//

	mConfig[0x0e] = 0x00;	// header-type (single-function PCI device)

	mConfig[0x3c] = IO_PIC_IRQ_ETHERNET1;	// interrupt line
	mConfig[0x3d] = 1;	// interrupt pin (default is 1)
	mConfig[0x3e] = 5;	// MinGnt (default is 5 = 0x05 = 0101b)
	mConfig[0x3f] = 48;	// MaxLat (default is 48 = 0x30 = 110000b)

	mConfig[0x34] = 0xdc;

	mIORegSize[0] = 0x100;
	mIORegType[0] = PCI_ADDRESS_SPACE_IO;
	assignIOPort(0, 0x1800);
}

void totalReset()
{
	IO_RTL8139_TRACE("totalReset()\n");	// PCI config
	/* FIXME: resetting can be done more fine-grained (see TotalReset cmd).
	 *        this is reset ALL regs.
	 */

	mIORegSize[0] = 256; // 128;
	mIORegType[0] = PCI_ADDRESS_SPACE_IO;
	// internals
	mEEPROMWritable = false;
	memset(&mRegisters, 0, sizeof mRegisters);
	mIntStatus = 0;
	mRingBufferSize = 8192;
	mHead = 0;
	mTail = 0;
	mActive = 0;
	mWatermark = 0;
	mLastPackets[0] = 0;
	mLastPackets[1] = 0;
	mGoodBSA = false;
	// EEPROM config (FIXME: endianess)

	// set up mac address
	byte *ptr = (byte*)&mRegisters;
	memcpy(ptr, mMAC, 6);
	// negotiate link, actually set it to valid 100 half duplex
	mRegisters.BMSR = 0x2025; // 0x4025;
	mRegisters.BMCR = 0x2000; // 0x3010; // 100mbs, no ane
	mRegisters.Config1 = 0x00; 
	mRegisters.CommandRegister = 0x01;
	mRegisters.TxConfiguration = 0x63000000; // rtl8139
	mRegisters.RxConfiguration = 0x00000000; // ?????
	mRegisters.MediaStatus = 0x90;
	mRegisters.CBA = 0;
	mRegisters.CAPR = 0xfff0;

	memset(mEEPROM, 0, sizeof mEEPROM);
	mEEPROM[EEPROM_DeviceID] =		0x8139; //0x9200;
	mEEPROM[EEPROM_ManifacturerID] =	0x10ec; //0x6d50;
	mEEPROM[EEPROM_PCIParam] =		0; //0x2940;
	mEEPROM[EEPROM_RomInfo] =		0;	// no ROM
	mEEPROM[EEPROM_OEMNodeAddress0] =	mEEPROM[EEPROM_NodeAddress0];
	mEEPROM[EEPROM_OEMNodeAddress1] =	mEEPROM[EEPROM_NodeAddress1];
	mEEPROM[EEPROM_OEMNodeAddress2] =	mEEPROM[EEPROM_NodeAddress2];
	mEEPROM[EEPROM_SoftwareInfo] =		0; //0x4010;
	mEEPROM[EEPROM_CompWord] =		0;
	mEEPROM[EEPROM_SoftwareInfo2] =		0; //0x00aa;
	mEEPROM[EEPROM_Caps] =			0x72a2;
	mEEPROM[EEPROM_InternalConfig0] =	0;
	mEEPROM[EEPROM_InternalConfig1] =	0; //0x0040;	// default is 0x0180
	mEEPROM[EEPROM_SubsystemVendorID] =	0x10ec; //0x10b7;
	mEEPROM[EEPROM_SubsystemID] =		0x8139; //0x9200;
	mEEPROM[EEPROM_MediaOptions] =		0x000a;
	mEEPROM[EEPROM_SmbAddress] =		0; //0x6300;
	mEEPROM[EEPROM_PCIParam2] =		0; //0xffb7;
	mEEPROM[EEPROM_PCIParam3] =		0; //0xb7b7;
	mEEPROM[EEPROM_Checksum] =		0;

	// PCI config follow-ups
	mConfig[0x00] = mEEPROM[EEPROM_SubsystemVendorID] & 0xff;	// vendor ID
	mConfig[0x01] = mEEPROM[EEPROM_SubsystemVendorID] >> 8;
	mConfig[0x02] = mEEPROM[EEPROM_DeviceID] & 0xff;	// unit ID
	mConfig[0x03] = mEEPROM[EEPROM_DeviceID] >> 8;
}

void setCR(uint8 cr)
{
	if (cr & 0x10) {
		// FIXME: care about params
		totalReset();
	}
	if (cr & 0x08) {
		mRegisters.CommandRegister |= 0x08;
		// enable receiver
	}
	if (cr & 0x04) {
		mRegisters.CommandRegister |= 0x04;
		// enable transmitter
	}
	if (cr & ~(0x1c)) {
		IO_RTL8139_WARN("command register write invalid byte: %0x\n", cr);
	}
}

void maybeRaiseIntr()
{
	IO_RTL8139_TRACE("maybeRaiseIntr()\n");
	if (mRegisters.InterruptMask & mIntStatus) {
		//mIntStatus |= IS_interruptLatch;
		IO_RTL8139_TRACE("Generating interrupt. mIntStatus=%04x\n", mIntStatus);
		pic_raise_interrupt(mConfig[0x3c]);
	}
}

void TxPacket(uint32 address, uint32 size)
{
	byte	pbuf[MAX_PACKET_SIZE];
	byte *	p;
	uint32	crc;
	uint32	psize;

	p = pbuf;
	IO_RTL8139_TRACE("address: %08x, size: %04x\n", address, size);
	if (ppc_dma_read(pbuf, address, size)) {
/*		if (mVerbose > 1) {
			debugDumpMem(ppc_addr, size);
		}*/
		psize = size;

		if(mRegisters.TxConfiguration & Tx_CRC) {
			crc = ether_crc(size, p);
			pbuf[psize+0] = crc;
			pbuf[psize+1] = crc>>8;
			pbuf[psize+2] = crc>>16;
			pbuf[psize+3] = crc>>24;
			psize += 4;
			IO_RTL8139_TRACE("Append CRC %08x.\n", crc);
		}

		uint w = mEth->sendPacket(pbuf, psize);
		if (w) {
			if (w == psize) {
				IO_RTL8139_TRACE("Eth: %d bytes sent.\n", psize);
			} else {
				IO_RTL8139_WARN("Eth: ARGH! send error: only %d of %d bytes sent\n", w, psize);
			}
		} else {
			IO_RTL8139_WARN("Eth: ARGH! send error of %d bytes packet\n", psize);
		}
		maybeRaiseIntr();
	}
}

public:
rtl8139_NIC(EthDevice *aEthTun, const byte *mac)
: PCI_Device("rtl8139 Network interface card", 0x1, 0xd)
{
	int e;
	if ((e = sys_create_mutex(&mLock))) throw IOException(e);
	mEth = aEthTun;
	memcpy(mMAC, mac, 6);
	mPid = 0;
	PCIReset();
	totalReset();
}

void transferPacket(bool raiseIntr)
{
	uint32 addr;
	uint32 base = mRegisters.RxBufferStartAddr;
	bool good;

	if (mTail == mHead) {
		return;
	}
	addr = base + mRegisters.CBA;
	if (mRegisters.CBA > mRingBufferSize) {// sending outside, could cause problems?
		good = false;
	} else {
		good = true;
	}
#if 0
	if ((mRegisters.CBA) > mRingBufferSize) {
		IO_RTL8139_TRACE("client ring buffer wrap around [%d]\n", raiseIntr);
		addr = base;
		mRegisters.CBA = 0;
		mRegisters.CAPR = 0xfff0;
//			mRegisters.CommandRegister |= 1;
	return;
	}
#endif
	ppc_dma_write(addr, mPackets[mTail].packet, mPackets[mTail].size);
	IO_RTL8139_TRACE("wrote %04x bytes to the ring buffer\n", mPackets[mTail].size);
	mRegisters.EarlyRxByteCount = mPackets[mTail].size;
	mRegisters.EarlyRxStatus = 8;
	mRegisters.CBA += mPackets[mTail].size;
	mRegisters.CommandRegister &= 0xfe; // RxBuffer has data
	mLastPackets[1] = mLastPackets[0];
	mLastPackets[0] = mTail;
	mActive--;
	IO_RTL8139_TRACE("Outgoing - Addr: %08x, Pid: %08x, Size: %04x\n", addr, mPackets[mTail].pid, mPackets[mTail].size-4);
	if (good) {
		mTail = (mTail+1) % MAX_PACKETS;
	}
	if (raiseIntr) {
		mIntStatus |= 1;
		maybeRaiseIntr();
	}
}

virtual ~rtl8139_NIC()
{
	mEth->shutdownDevice();
	delete mEth;
	sys_destroy_mutex(mLock);
}

void readConfig(uint reg)
{
	//if (mVerbose) IO_RTL8139_TRACE("readConfig %02x\n", reg);
	if (reg >= 0xdc) {
		IO_RTL8139_WARN("readConfig(%x)\n", reg);
	}
	sys_lock_mutex(mLock);
	PCI_Device::readConfig(reg);
	sys_unlock_mutex(mLock);
}

void writeConfig(uint reg, int offset, int size)
{
	//if (mVerbose) IO_RTL8139_TRACE("writeConfig %02x, %d, %d\n", reg, offset, size);
	sys_lock_mutex(mLock);
	if (reg >= 0xdc) {
		IO_RTL8139_WARN("writeConfig(%x, %d, %d)\n", reg, offset, size);
	}
	PCI_Device::writeConfig(reg, offset, size);
	sys_unlock_mutex(mLock);
}

bool readDeviceIO(uint r, uint32 port, uint32 &data, uint size)
{
	if (r != 0) return false;
	bool retval = false;
//	IO_RTL8139_TRACE("readDevice waiting for mLock\n");
	sys_lock_mutex(mLock);
//	IO_RTL8139_TRACE("readDevice has mLock\n");

	if (port == 0x3e) {
		// IntStatus (no matter which window)
		if (size != 2) {
			IO_RTL8139_WARN("unaligned read from IntStatus\n");
		}
		IO_RTL8139_TRACE("read IntStatus = %04x\n", mIntStatus);
		data = mIntStatus;
		mIntStatus = 0; // a read resets the interrupt status register
		retval = true;
	} else if ((port >= 0) && (port+size <= sizeof(Registers))) {
		// read from (standard) register
		data = 0;
		memcpy(&data, ((byte*)&mRegisters)+port, size);

		switch (port) {
		case 0x48:
			IO_RTL8139_TRACE("read Timer = %08x\n", data);
			break;
		case 0x37: {
			IO_RTL8139_TRACE("read Command Register = %02x\n", data);
			break;
		}
		case 0x64: {
			IO_RTL8139_TRACE("read Basic Mode Status = %04x\n", data);
			if ((mTail != mHead) && (mRegisters.CommandRegister & 0x01)) {
				transferPacket(true);
			}
			break;
		}
		default:
			IO_RTL8139_TRACE("read reg %04x (size %d) = %08x\n", port, size, data);
			break;
		}
		retval = true;
	}
	sys_unlock_mutex(mLock);
//	IO_RTL8139_TRACE("readDevice freed mLock\n");
	return retval;
}

bool writeDeviceIO(uint r, uint32 port, uint32 data, uint size)
{
	uint32 original;

	if (r != 0) return false;
	bool retval = false;
//	IO_RTL8139_TRACE("writeDevice waiting for mLock\n");
	sys_lock_mutex(mLock);
//	IO_RTL8139_TRACE("writeDevice has mLock\n");
	original = data;
	if (port == 0x37) {
		// CommandReg (no matter which window)
		if (size != 1) {
			IO_RTL8139_WARN("unaligned write to CommandReg\n");
		}
		setCR(data);
		retval = true;
	} else if ((port >= 0) && (port+size <= sizeof(Registers))) {
		switch (port) {
		case 0x3c: {
			IO_RTL8139_TRACE("write Interrupt Mask Register %04x (now = %04x)\n", data, mRegisters.InterruptMask);
			mRegisters.InterruptMask = data;
			break;
		}
		case 0x10: {
			IO_RTL8139_TRACE("write to TS0, got data to send %08x\n", data);
			TxPacket(mRegisters.TxStartAddrD0, data & 0x0fff);
			mRegisters.TxStatusD0 |= ((1 << 13)|(1<<15)); //	set ownership
			mIntStatus |= 4; // Tx Ok
			break;
		}
		case 0x14: {
			IO_RTL8139_TRACE("write to TS1, got data to send %08x\n", data);
			TxPacket(mRegisters.TxStartAddrD1, data & 0x0fff);
			mRegisters.TxStatusD1 |= ((1 << 13)|(1<<15)); //	set ownership
			mIntStatus |= 4; // Tx Ok
			break;
		}
		case 0x18: {
			IO_RTL8139_TRACE("write to TS2, got data to send %08x\n", data);
			TxPacket(mRegisters.TxStartAddrD2, data & 0x0fff);
			mRegisters.TxStatusD2 |= ((1 << 13)|(1<<15)); //	set ownership
			mIntStatus |= 4; // Tx Ok
			break;
		}
		case 0x1c: {
			IO_RTL8139_TRACE("write to TS3, got data to send %08x\n", data);
			TxPacket(mRegisters.TxStartAddrD3, data & 0x0fff);
			mRegisters.TxStatusD3 |= ((1 << 13)|(1<<15)); //	set ownership
			mIntStatus |= 4; // Tx Ok
			break;
		}
		case 0x20: {
			IO_RTL8139_TRACE("write to TxSA0, address %08x\n", data);
			mRegisters.TxStartAddrD0 = data;
			break;
		}
		case 0x24: {
			IO_RTL8139_TRACE("write to TxSA1, address %08x\n", data);
			mRegisters.TxStartAddrD1 = data;
			break;
		}
		case 0x28: {
			IO_RTL8139_TRACE("write to TxSA2, address %08x\n", data);
			mRegisters.TxStartAddrD2 = data;
			break;
		}
		case 0x2c: {
			IO_RTL8139_TRACE("write to TxSA3, address %08x\n", data);
			mRegisters.TxStartAddrD3 = data;
			break;
		}
		case 0x30: {
			IO_RTL8139_TRACE("write to RxBSA, address %08x\n", data);
			mRegisters.RxBufferStartAddr = data;
			mRegisters.CBA = 0;
			mRegisters.CAPR = 0xfff0;
			mRegisters.CommandRegister |= 1;
			mGoodBSA = true;
			transferPacket(true);
			break;
		}
		case 0x38: {
			IO_RTL8139_TRACE("update to CAPR: CAPR %04x, CBA %04x\n", data, mRegisters.CBA);
			mRegisters.CAPR = data;
			if (mRegisters.CAPR >= mRegisters.CBA) {
//				IO_RTL8139_WARN("Bad packet read by client? Active %02x, CAPR %04x, CBA %04x, Tail %02x, Head %02x\n", mActive, mRegisters.CAPR, mRegisters.CBA, mTail, mHead);
				mRegisters.CBA = mRegisters.CAPR + 0x10;
			}
			if (mRegisters.CAPR > mRingBufferSize) { //client knows about wrap, so wrap
				mRegisters.CBA = 0;
				mIntStatus |= 1; // fake send
				maybeRaiseIntr();
				IO_RTL8139_TRACE("client wrap on CAPR set Active %02x, CAPR %04x, CBA %04x, Tail %02x, Head %02x\n", mActive, mRegisters.CAPR, mRegisters.CBA, mTail, mHead);
				/*
				mIntStatus |= 1; // fake send
				maybeRaiseIntr();
				mRegisters.CAPR = 0xfff0;
				mRegisters.CommandRegister |= 1;
				*/
			} else {
				if (mTail != mHead) {
					transferPacket(false);
				} else {
					mRegisters.CommandRegister |= 1;
				}
			}
			break;
		}
		case 0x44: { 
			IO_RTL8139_TRACE("write to RxConfiguration, data %08x\n", data);
			mRegisters.RxConfiguration = data;
			switch ((data & 0x1800)) {
			case 0x0000: mRingBufferSize = 8192; break;
			case 0x0800: mRingBufferSize = 16384; break;
			case 0x1000: mRingBufferSize = 32768; break;
			case 0x1800: mRingBufferSize = 65536; break;
			default: mRingBufferSize = 8192;
			};
			IO_RTL8139_TRACE("RingBuffer Size: %08x\n", mRingBufferSize);
			IO_RTL8139_TRACE("Rx Mode is: %c%c%c%c%c%c\n",
				mRegisters.RxConfiguration & Rx_AAP? 'A': '-',
				mRegisters.RxConfiguration & Rx_APM? 'P': '-',
				mRegisters.RxConfiguration & Rx_AM?  'M': '-',
				mRegisters.RxConfiguration & Rx_AB?  'B': '-',
				mRegisters.RxConfiguration & Rx_AR?  'R': '-',
				mRegisters.RxConfiguration & Rx_AER? 'E': '-');
			break;
		}
		case 0x62: {
			IO_RTL8139_TRACE("write Basic Mode Control %04x\n", data);
			//mIntStatus |= 0x2000; // cable length changed, receive enabled
			mRegisters.BMCR = data & 0xfdff;
			break;
		}
		default:
			IO_RTL8139_TRACE("write to register port=%04x, size=%d, data=0x%08x\n", port, size, data);
			// write to (standard) register
			memcpy(((byte*)&mRegisters)+port, &data, size);
		}
		retval = true;
	}
	sys_unlock_mutex(mLock);
	return retval;
}

uint16 rxFilter(byte *pbuf, uint16 &psize) {

	uint16		header = 0;
	static byte	broadcast[] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

//	if (checkFuckingEthernetCRC()) {
//	// check crc
//		header |= Rx_CRC;
//	}

	if (psize < 64) {
		for ( ; psize < 60; psize++) {
			pbuf[psize] = 0;
		}
//		header |= Rx_RUNT; // set runt status: some bug with ARP packets here!
	}

	if (psize > 4096) {
		header |= Rx_LONG; // set long status
	}

	/* pad to a 4 byte boundary */
	for (int i = 4-(psize % 4); i != 0; i--) {
		pbuf[psize++] = 0;
	}

	if (memcmp(pbuf, broadcast, 6) == 0) {
		header |= Rx_BAR;
	}

	if (memcmp(pbuf, (byte*)&(mRegisters.mar0), 6) == 0) {
		header |= Rx_MAR;
	}

	if (memcmp(pbuf, (byte*)&(mRegisters.id0), 6) == 0) {
		/*if (mVerbose > 1) IO_RTL8139_TRACE("Physical Address Match\n");*/
		header |= Rx_PAM;
	}

	header |= Rx_ROK;

	IO_RTL8139_TRACE("rxFilter: Packet state is: %c%c%c%c%c%c%c\n",
		header & Rx_RUNT? 'R': '-',
		header & Rx_LONG? 'L': '-',
		header & Rx_BAR?  'B': '-',
		header & Rx_MAR?  'M': '-',
		header & Rx_PAM?  'P': '-',
		header & Rx_CRC?  'C': '-',
		header & Rx_ROK?  'O': '-');

	if(mRegisters.RxConfiguration & Rx_AAP) {
		IO_RTL8139_TRACE("rxFilter: PASS! Promiscuous mode!\n");
		return header;
	} else {

		if(!(mRegisters.RxConfiguration & Rx_AER) && (header & Rx_CRC)) {
			IO_RTL8139_TRACE("rxFilter: SKIP! No error packets alloved!\n");
			return 0;
		}

		if(!(mRegisters.RxConfiguration & Rx_AR) && (header & Rx_RUNT)) {
			IO_RTL8139_TRACE("rxFilter: SKIP! No runt packets alloved!\n");
			return 0;
		}

		if((mRegisters.RxConfiguration & Rx_AM) && (header & Rx_MAR)) {
			IO_RTL8139_TRACE("rxFilter: PASS! Multicast alloved!\n");
			return header;
		}

		if((mRegisters.RxConfiguration & Rx_AB) && (header & Rx_BAR)) {
			IO_RTL8139_TRACE("rxFilter: PASS! Broadcast alloved!\n");
			return header;
		}

		if((mRegisters.RxConfiguration & Rx_APM) && (header & Rx_PAM)) {
			IO_RTL8139_TRACE("rxFilter: PASS! My Physical alloved!\n");
			return header;
		}
	}

	IO_RTL8139_TRACE("rxFilter: SKIP! No pass address filter!\n");
	return 0;
}

void handleRxQueue()
{
	uint16		header;
	uint16		psize;
	byte            rxPacket[MAX_PACKET_SIZE];
	uint16          rxPacketSize;
	byte		tmp;

	while (1) {
		while (mEth->waitRecvPacket() != 0) {
			// don't block the system in case of (repeated) error(s)
			sys_suspend();
		}
		rxPacketSize = mEth->recvPacket(rxPacket, sizeof rxPacket);
		if (!rxPacketSize) {
			// don't block the system in case of (repeated) error(s)
			sys_suspend();
			continue;
		}
		IO_RTL8139_TRACE("Got packet from the world of %d bytes\n", rxPacketSize);
		if (!mGoodBSA) {
			IO_RTL8139_TRACE("SKIP! Not ready yet!\n");
			continue;
		}
/*		if (mVerbose > 1) {
			debugDumpMem(rxPacket, rxPacketSize);
		}*/

//		IO_RTL8139_TRACE("handleRxQueue waiting for mLock\n");
		sys_lock_mutex(mLock);
//		IO_RTL8139_TRACE("handleRxQueue has mLock\n");

		header = rxFilter(rxPacket, rxPacketSize);
		if(header == 0) {
			IO_RTL8139_TRACE("SKIP! Packet is filtered out!\n");
			sys_unlock_mutex(mLock);
			continue;
		}

		psize = rxPacketSize;
		IO_RTL8139_TRACE("Incoming - Pid: %08x, Header: %04x, Size: %04x\n", mPid, header, psize);
		mPackets[mHead].packet[0] = header;
		mPackets[mHead].packet[1] = header>>8;
		mPackets[mHead].packet[2] = psize;
		mPackets[mHead].packet[3] = psize>>8;
		memcpy(&(mPackets[mHead].packet[4]), rxPacket, rxPacketSize);
		mPackets[mHead].size = rxPacketSize+4;
		mPackets[mHead].pid = mPid;
		tmp = mHead;
		if (mHead == mTail) { /* first recent packet buffer */
			mHead = (mHead+1) % MAX_PACKETS;
		} else {
			mHead = (mHead+1) % MAX_PACKETS;
			if (mHead == mTail) {
				mHead = tmp; // reset it back 
				IO_RTL8139_WARN("Internal Buffer wrapped around\n");
			} 
		}
		if (tmp != mHead) {
			mPid++;
			mActive++;
			if (mActive > mWatermark) {
				IO_RTL8139_TRACE("Watermark: %02x\n", mWatermark);
				mWatermark = mActive;
			}
		}
		if (mRegisters.CommandRegister & 1) { /* no packets in process, kick one out */
			transferPacket(true);
		}
		sys_unlock_mutex(mLock);
//		IO_RTL8139_TRACE("handleRxQueue freed mLock\n");
	}
}

}; // end of rtl8139 class

static void *rtl8139HandleRxQueue(void *nic)
{
	rtl8139_NIC *NIC = (rtl8139_NIC *)nic;
	NIC->handleRxQueue();
	return NULL;
}

bool rtl8139_installed = false;
bool rtl8139_type_pcap = false;

#include "configparser.h"
#include "tools/strtools.h"

#define RTL8139_KEY_INSTALLED   "pci_rtl8139_installed"
#define RTL8139_KEY_MAC         "pci_rtl8139_mac"
#define RTL8139_KEY_TYPE        "pci_rtl8139_type"
#define RTL8139_KEY_INTERFACE   "pci_rtl8139_interface"

#define RTL8139_KEY_TYPE_TAP    "tap"
#define RTL8139_KEY_TYPE_PCAP   "pcap"

void rtl8139_init()
{
	if (gConfig->getConfigInt(RTL8139_KEY_INSTALLED)) {
		rtl8139_installed = true;
		byte mac[6];
		mac[0] = 0xde;
		mac[1] = 0xad;
		mac[2] = 0xca;
		mac[3] = 0xfe;
		mac[4] = 0x12;
		mac[5] = 0x34;
		if (gConfig->haveKey(RTL8139_KEY_MAC)) {
			String macstr_;
			gConfig->getConfigString(RTL8139_KEY_MAC, macstr_);
			// do something useful with mac
			const char *macstr = macstr_.contentChar();
			byte cfgmac[6];
			for (uint i=0; i<6; i++) {
				uint64 v;
				if (!parseIntStr(macstr, v, 16) || (v>255) || ((*macstr != ':') && (i!=5))) {
					IO_RTL8139_ERR("error in config key %s:"
					"expected format: XX:XX:XX:XX:XX:XX, "
					"where X stands for any digit or the "
					"letters a-f, A-F (error at: %s)\n",
					RTL8139_KEY_MAC, macstr);
				}
				macstr++;
				cfgmac[i] = v;
			}
			memcpy(mac, cfgmac, sizeof mac);
		}
		rtl8139_type_pcap = false;
		if (gConfig->haveKey(RTL8139_KEY_TYPE)) {
			String type;
			gConfig->getConfigString(RTL8139_KEY_TYPE, type);
			if (type == RTL8139_KEY_TYPE_TAP) {
				rtl8139_type_pcap = false;
			} else if (type == RTL8139_KEY_TYPE_PCAP) {
				rtl8139_type_pcap = true;
			} else {
				IO_RTL8139_ERR("error in config key %s:"
					"value must be '%s' or '%s' but not '%s'\n",
					RTL8139_KEY_TYPE,
					RTL8139_KEY_TYPE_TAP,
					RTL8139_KEY_TYPE_PCAP,
					type.contentChar());
			}
		}
		EthDevice *eth = NULL;
		if (rtl8139_type_pcap) {
			String rtl8139_interface;
			if (!gConfig->haveKey(RTL8139_KEY_INTERFACE)) {
				IO_RTL8139_ERR("error: no '%s' key specified "
					"for PCAP device\n",
					RTL8139_KEY_INTERFACE);
				exit(1);
			}
			gConfig->getConfigString(RTL8139_KEY_INTERFACE, rtl8139_interface);
			eth = createEthernetPcap(rtl8139_interface.contentChar());
			if (!eth) {
				IO_RTL8139_ERR("Couldn't create PCAP ethernet bridge\n");
				exit(1);
			}
		} else {
			eth = createEthernetTunnel();
			if (!eth) {
				IO_RTL8139_ERR("Couldn't create ethernet tunnel\n");
				exit(1);
			}
		}
		if (eth->initDevice()) {
			IO_RTL8139_ERR("Couldn't initialize ethernet device\n");
			exit(1);
		}
#if 0
		printf("Creating RealTek rtl8139 NIC emulation with eth_addr = ");
		for (uint i=0; i<6; i++) {
			if (i<5) {
				printf("%02x:", mac[i]);
			} else {
				printf("%02x", mac[i]);
			}
		}
		printf("\n");
#endif
		rtl8139_NIC *MyNIC = new rtl8139_NIC(eth, mac);
		gPCI_Devices->insert(MyNIC);
		sys_thread rxthread;
		sys_create_thread(&rxthread, 0, rtl8139HandleRxQueue, MyNIC);
	}
}

void rtl8139_done()
{
}

void rtl8139_init_config()
{
	gConfig->acceptConfigEntryIntDef(RTL8139_KEY_INSTALLED, 0);
	gConfig->acceptConfigEntryString(RTL8139_KEY_MAC, false);
	gConfig->acceptConfigEntryString(RTL8139_KEY_TYPE, RTL8139_KEY_TYPE_TAP);
	gConfig->acceptConfigEntryString(RTL8139_KEY_INTERFACE, "");
}
