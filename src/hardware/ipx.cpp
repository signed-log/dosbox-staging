// SPDX-FileCopyrightText:  2020-2025 The DOSBox Staging Team
// SPDX-FileCopyrightText:  2002-2021 The DOSBox Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "dosbox.h"

#if C_IPX

#include <SDL_net.h>

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "cross.h"
#include "string_utils.h"
#include "cpu.h"
#include "regs.h"
#include "inout.h"
#include "setup.h"
#include "debug.h"
#include "callback.h"
#include "dos_system.h"
#include "mem.h"
#include "ipx.h"
#include "ipxserver.h"
#include "timer.h"
#include "programs.h"
#include "pic.h"

#define SOCKTABLESIZE	150 // DOS IPX driver was limited to 150 open sockets

struct ipxnetaddr {
	Uint8 netnum[4];   // Both are big endian
	Uint8 netnode[6];
} localIpxAddr;

uint32_t udpPort;
bool isIpxServer;
bool isIpxConnected;
IPaddress ipxServConnIp;			// IPAddress for client connection to server
UDPsocket ipxClientSocket;
int UDPChannel;						// Channel used by UDP connection
uint8_t recvBuffer[IPXBUFFERSIZE];	// Incoming packet buffer

static RealPt ipx_callback;

SDLNet_SocketSet clientSocketSet;

packetBuffer incomingPacket;

static uint16_t socketCount;
static uint16_t opensockets[SOCKTABLESIZE];

static uint16_t swapByte(uint16_t sockNum) {
	return (((sockNum>> 8)) | (sockNum << 8));
}

void UnpackIP(PackedIP ipPack, IPaddress * ipAddr) {
	ipAddr->host = ipPack.host;
	ipAddr->port = ipPack.port;
}

void PackIP(IPaddress ipAddr, PackedIP *ipPack) {
	ipPack->host = ipAddr.host;
	ipPack->port = ipAddr.port;
}

ECBClass *ECBList;  // Linked list of ECB's
ECBClass* ESRList;	// ECBs waiting to be ESR notified

#ifdef IPX_DEBUGMSG
Bitu ECBSerialNumber = 0;
Bitu ECBAmount = 0;
#endif

ECBClass::ECBClass(uint16_t segment, uint16_t offset)
        : ECBAddr(RealMake(segment, offset)),
          isInESRList(false),
          prevECB(nullptr),
          nextECB(nullptr),
          iuflag(0),
          mysocket(0),
          databuffer(nullptr),
          buflen(0)
{
#ifdef IPX_DEBUGMSG
	SerialNumber = ECBSerialNumber;
	ECBSerialNumber++;
	ECBAmount++;

	LOG_IPX("ECB: SN%7d created.   Number of ECBs: %3d, ESR %4x:%4x, ECB %4x:%4x",
		SerialNumber,ECBAmount,
		real_readw(RealSegment(ECBAddr),
		RealOffset(ECBAddr)+6),
		real_readw(RealSegment(ECBAddr),
		RealOffset(ECBAddr)+4),segment,offset);
#endif

	if (ECBList == nullptr)
		ECBList = this;
	else {
		// Transverse the list until we hit the end
		ECBClass *useECB = ECBList;

		while(useECB->nextECB != nullptr)
			useECB = useECB->nextECB;

		useECB->nextECB = this;
		this->prevECB = useECB;
	}

	iuflag = getInUseFlag();
	mysocket = getSocket();
}

void ECBClass::writeDataBuffer(uint8_t* buffer, uint16_t length) {
	delete[] databuffer;
	databuffer = new uint8_t[length];
	memcpy(databuffer,buffer,length);
	buflen=length;

}
bool ECBClass::writeData() {
	Bitu length=buflen;
	uint8_t* buffer = databuffer;
	fragmentDescriptor tmpFrag;
	setInUseFlag(USEFLAG_AVAILABLE);
	Bitu fragCount = getFragCount();
	Bitu bufoffset = 0;
	for(Bitu i = 0;i < fragCount;i++) {
		getFragDesc(i,&tmpFrag);
		for(Bitu t = 0;t < tmpFrag.size;t++) {
			real_writeb(tmpFrag.segment, tmpFrag.offset + t, buffer[bufoffset]);
			bufoffset++;
			if(bufoffset >= length) {
				setCompletionFlag(COMP_SUCCESS);
				setImmAddress(&buffer[22]);  // Write in source node
				return true;
			}
		}
	}
	if(bufoffset < length) {
		setCompletionFlag(COMP_MALFORMED);
		return false;
	}
	return false;
}

uint16_t ECBClass::getSocket(void) {
	return swapByte(real_readw(RealSegment(ECBAddr), RealOffset(ECBAddr) + 0xa));
}

uint8_t ECBClass::getInUseFlag(void) {
	return real_readb(RealSegment(ECBAddr), RealOffset(ECBAddr) + 0x8);
}

void ECBClass::setInUseFlag(uint8_t flagval) {
	iuflag = flagval;
	real_writeb(RealSegment(ECBAddr), RealOffset(ECBAddr) + 0x8, flagval);
}

void ECBClass::setCompletionFlag(uint8_t flagval) {
	real_writeb(RealSegment(ECBAddr), RealOffset(ECBAddr) + 0x9, flagval);
}

uint16_t ECBClass::getFragCount(void) {
	return real_readw(RealSegment(ECBAddr), RealOffset(ECBAddr) + 34);
}

void ECBClass::getFragDesc(uint16_t descNum, fragmentDescriptor *fragDesc) {
	uint16_t memoff = RealOffset(ECBAddr) + 30 + ((descNum+1) * 6);
	fragDesc->offset = real_readw(RealSegment(ECBAddr), memoff);
	memoff += 2;
	fragDesc->segment = real_readw(RealSegment(ECBAddr), memoff);
	memoff += 2;
	fragDesc->size = real_readw(RealSegment(ECBAddr), memoff);
}

RealPt ECBClass::getESRAddr()
{
	const auto segment = RealSegment(ECBAddr);
	const auto offset  = RealOffset(ECBAddr);
	return RealMake(real_readw(segment, offset + 6),
	                real_readw(segment, offset + 4));
}

void ECBClass::NotifyESR(void) {
	uint32_t ESRval = real_readd(RealSegment(ECBAddr), RealOffset(ECBAddr)+4);
	if(ESRval || databuffer) { // databuffer: write data at realmode/v86 time
		// LOG_IPX("ECB: SN%7d to be notified.", SerialNumber);
		// take the ECB out of the current list
		if(prevECB == nullptr) {	// was the first in the list
			ECBList = nextECB;
			if(ECBList != nullptr) ECBList->prevECB = nullptr;
		} else {		// not the first
			prevECB->nextECB = nextECB;
			if(nextECB != nullptr) nextECB->prevECB = prevECB;
		}

		nextECB = nullptr;
		// put it to the notification queue
		if(ESRList==nullptr) {
			ESRList = this;
			prevECB = nullptr;
		} else  {// put to end of ESR list
			ECBClass* useECB = ESRList;

			while(useECB->nextECB != nullptr)
				useECB = useECB->nextECB;

			useECB->nextECB = this;
			prevECB = useECB;
		}
		isInESRList = true;
		PIC_ActivateIRQ(11);
	}
	// this one does not want to be notified, delete it right away
	else delete this;
}

void ECBClass::setImmAddress(uint8_t *immAddr) {
	for(Bitu i=0;i<6;i++)
		real_writeb(RealSegment(ECBAddr), RealOffset(ECBAddr)+28+i, immAddr[i]);
}

void ECBClass::getImmAddress(uint8_t* immAddr) {
	for(Bitu i=0;i<6;i++)
		immAddr[i] = real_readb(RealSegment(ECBAddr), RealOffset(ECBAddr)+28+i);
}

ECBClass::~ECBClass() {
#ifdef IPX_DEBUGMSG
	ECBAmount--;
	LOG_IPX("ECB: SN%7d destroyed. Remaining ECBs: %3d", SerialNumber,ECBAmount);
#endif

	if(isInESRList) {
		// in ESR list, always the first element is deleted.
		ESRList=nextECB;
	} else {
		if(prevECB == nullptr) {	// was the first in the list
			ECBList = nextECB;
			if(ECBList != nullptr) ECBList->prevECB = nullptr;
		} else {	// not the first
			prevECB->nextECB = nextECB;
			if(nextECB != nullptr) nextECB->prevECB = prevECB;
		}
	}
	delete[] databuffer;
}



static bool sockInUse(uint16_t sockNum) {
	for(Bitu i=0;i<socketCount;i++) {
		if (opensockets[i] == sockNum) return true;
	}
	return false;
}

static void OpenSocket(void) {
	uint16_t sockNum, sockAlloc;
	sockNum = swapByte(reg_dx);

	if(socketCount >= SOCKTABLESIZE) {
		reg_al = 0xfe; // Socket table full
		return;
	}

	if(sockNum == 0x0000) {
		// Dynamic socket allocation
		sockAlloc = 0x4002;
		while(sockInUse(sockAlloc) && (sockAlloc < 0x7fff)) sockAlloc++;
		if(sockAlloc > 0x7fff) {
			// I have no idea how this could happen if the IPX driver
			// is limited to 150 open sockets at a time
			LOG_MSG("IPX: Out of dynamic sockets");
		}
		sockNum = sockAlloc;
	} else {
		if(sockInUse(sockNum)) {
			reg_al = 0xff; // Socket already open
			return;
		}
	}

	opensockets[socketCount] = sockNum;
	socketCount++;

	reg_al = 0x00; // Success
	reg_dx = swapByte(sockNum);  // Convert back to big-endian
}

static void CloseSocket(void) {
	uint16_t sockNum, i;
	ECBClass* tmpECB = ECBList;
	ECBClass* tmp2ECB = ECBList;

	sockNum = swapByte(reg_dx);
	if(!sockInUse(sockNum)) return;

	for(i=0;i<socketCount-1;i++) {
		if (opensockets[i] == sockNum) {
			// Realign list of open sockets
			memcpy(&opensockets[i], &opensockets[i+1], SOCKTABLESIZE - (i + 1));
			break;
		}
	}
	--socketCount;

	// delete all ECBs of that socket
	while(tmpECB!=nullptr) {
		tmp2ECB = tmpECB->nextECB;
		if(tmpECB->getSocket()==sockNum) {
			tmpECB->setCompletionFlag(COMP_CANCELLED);
			tmpECB->setInUseFlag(USEFLAG_AVAILABLE);
			delete tmpECB;
		}
		tmpECB = tmp2ECB;
	}
}

//static RealPt IPXVERpointer;

static bool IPX_Multiplex(void) {
	if(reg_ax != 0x7a00) return false;
	reg_al = 0xff;
	SegSet16(es,RealSegment(ipx_callback));
	reg_di = RealOffset(ipx_callback);

	//reg_bx = RealOffset(IPXVERpointer);
	//reg_cx = RealSegment(ipx_callback);
	return true;
}

static void IPX_AES_EventHandler(uint32_t param)
{
	ECBClass* tmpECB = ECBList;
	ECBClass* tmp2ECB;
	while(tmpECB!=nullptr) {
		tmp2ECB = tmpECB->nextECB;
		if(tmpECB->iuflag==USEFLAG_AESCOUNT && param==(Bitu)tmpECB->ECBAddr) {
			tmpECB->setCompletionFlag(COMP_SUCCESS);
			tmpECB->setInUseFlag(USEFLAG_AVAILABLE);
			tmpECB->NotifyESR();
			// LOG_IPX("AES Notification: ECB S/N %d",tmpECB->SerialNumber);
			return;
		}
		tmpECB = tmp2ECB;
	}
	LOG_MSG("!!!! Rogue AES !!!!" );
}

static void sendPacket(ECBClass* sendecb);

static void handleIpxRequest(void) {
	ECBClass *tmpECB;

	switch (reg_bx) {
	case 0x0000: // Open socket
		OpenSocket();
		LOG_IPX("IPX: Open socket %4x", swapByte(reg_dx));
		break;
	case 0x0001: // Close socket
		LOG_IPX("IPX: Close socket %4x", swapByte(reg_dx));
		CloseSocket();
		break;
	case 0x0002: // get local target
	             // es:si
	             // Currently no support for multiple networks

		for (Bitu i = 0; i < 6; i++)
			real_writeb(SegValue(es), reg_di + i,
			            real_readb(SegValue(es), reg_si + i + 4));

		reg_cx = 1;    // time ticks expected
		reg_al = 0x00; // success
		break;

	case 0x0003: // Send packet
		tmpECB = new ECBClass(SegValue(es), reg_si);
		if (!incomingPacket.connected) {
			tmpECB->setInUseFlag(USEFLAG_AVAILABLE);
			tmpECB->setCompletionFlag(COMP_UNDELIVERABLE);
			delete tmpECB; // not notify?
			reg_al = 0xff; // Failure
		} else {
			tmpECB->setInUseFlag(USEFLAG_SENDING);
			// LOG_IPX("IPX: Sending packet on %4x",
			// tmpECB->getSocket());
			reg_al = 0x00; // Success
			sendPacket(tmpECB);
		}

		break;
	case 0x0004: // Listen for packet
		tmpECB = new ECBClass(SegValue(es), reg_si);
		// LOG_IPX("ECB: SN%7d RECEIVE.", tmpECB->SerialNumber);
		if (!sockInUse(tmpECB->getSocket())) { // Socket is not open
			reg_al = 0xff;
			tmpECB->setInUseFlag(USEFLAG_AVAILABLE);
			tmpECB->setCompletionFlag(COMP_HARDWAREERROR);
			delete tmpECB;
		} else {
			reg_al = 0x00; // Success
			tmpECB->setInUseFlag(USEFLAG_LISTENING);
			/*LOG_IPX("IPX: Listen for packet on 0x%4x - ESR address
			   %4x:%4x", tmpECB->getSocket(),
			        RealSegment(tmpECB->getESRAddr()),
			        RealOffset(tmpECB->getESRAddr()));*/
		}
		break;

	case 0x0005: // SCHEDULE IPX EVENT
	case 0x0007: // SCHEDULE SPECIAL IPX EVENT
	{
		tmpECB = new ECBClass(SegValue(es), reg_si);
		// LOG_IPX("ECB: SN%7d AES. T=%fms.", tmpECB->SerialNumber,
		//	(1000.0f/(1193182.0f/65536.0f))*(float)reg_ax);
		PIC_AddEvent(IPX_AES_EventHandler,
		             (1000.0f / (1193182.0f / 65536.0f)) * (float)reg_ax,
		             (Bitu)tmpECB->ECBAddr);
		tmpECB->setInUseFlag(USEFLAG_AESCOUNT);
	} break;
	case 0x0006: // cancel operation
	{
		RealPt ecbaddress = RealMake(SegValue(es), reg_si);
		ECBClass *tmpECB = ECBList;
		ECBClass *tmp2ECB;
		while (tmpECB) {
			tmp2ECB = tmpECB->nextECB;
			if (tmpECB->ECBAddr == ecbaddress) {
				if (tmpECB->getInUseFlag() == USEFLAG_AESCOUNT)
					PIC_RemoveSpecificEvents(IPX_AES_EventHandler,
					                         ecbaddress);
				tmpECB->setInUseFlag(USEFLAG_AVAILABLE);
				tmpECB->setCompletionFlag(COMP_CANCELLED);
				delete tmpECB;
				reg_al = 0; // Success
				LOG_IPX("IPX: ECB canceled.");
				return;
			}
			tmpECB = tmp2ECB;
		}
		reg_al = 0xff; // Fail
	} break;

	case 0x0008: // Get interval marker
		reg_ax = mem_readw(0x46c); // BIOS_TIMER
		break;
	case 0x0009: // Get internetwork address
	{
		LOG_IPX("IPX: Get internetwork address %2x:%2x:%2x:%2x:%2x:%2x",
		        localIpxAddr.netnode[5], localIpxAddr.netnode[4],
		        localIpxAddr.netnode[3], localIpxAddr.netnode[2],
		        localIpxAddr.netnode[1], localIpxAddr.netnode[0]);

		uint8_t *addrptr = (uint8_t *)&localIpxAddr;
		for (uint16_t i = 0; i < 10; i++)
			real_writeb(SegValue(es), reg_si + i, addrptr[i]);
	} break;

	case 0x000a: // Relinquish control
		break;   // Idle thingy

	case 0x000b: // Disconnect from Target
		break;   // We don't even connect

	case 0x000d: // get packet size
		reg_cx = 0;    // retry count
		reg_ax = 1024; // real implementation returns 1024
		break;

	case 0x0010:    // SPX install check
		reg_al = 0; // SPX not installed
		break;

	case 0x001a: // get driver maximum packet size
		reg_cx = 0;             // retry count
		reg_ax = IPXBUFFERSIZE; // max packet size: something near the
		                        // ethernet packet size
		break;

	default: LOG_MSG("Unhandled IPX function: %4x", reg_bx); break;
	}
}

// Entrypoint handler
Bitu IPX_Handler(void) {
	handleIpxRequest();
	return CBRET_NONE;
}

// INT 7A handler
Bitu IPX_IntHandler(void) {
	handleIpxRequest();
	return CBRET_NONE;
}

static void pingAck(IPaddress retAddr) {
	IPXHeader regHeader;
	UDPpacket regPacket;

	SDLNet_Write16(0xffff, regHeader.checkSum);
	SDLNet_Write16(sizeof(regHeader), regHeader.length);

	SDLNet_Write32(0, regHeader.dest.network);
	PackIP(retAddr, &regHeader.dest.addr.byIP);
	SDLNet_Write16(0x2, regHeader.dest.socket);

	SDLNet_Write32(0, regHeader.src.network);
	memcpy(regHeader.src.addr.byNode.node, localIpxAddr.netnode, sizeof(regHeader.src.addr.byNode.node));
	SDLNet_Write16(0x2, regHeader.src.socket);
	regHeader.transControl = 0;
	regHeader.pType = 0x0;

	regPacket.data = (Uint8 *)&regHeader;
	regPacket.len = sizeof(regHeader);
	regPacket.maxlen = sizeof(regHeader);
	regPacket.channel = UDPChannel;

	const int result = SDLNet_UDP_Send(ipxClientSocket, regPacket.channel,
	                                   &regPacket);
	if (!result) {
		LOG_DEBUG("IPX: Failed to acknowledge send: %s", SDLNet_GetError());
	}
}

static void pingSend(void) {
	IPXHeader regHeader;
	UDPpacket regPacket;

	SDLNet_Write16(0xffff, regHeader.checkSum);
	SDLNet_Write16(sizeof(regHeader), regHeader.length);

	SDLNet_Write32(0, regHeader.dest.network);
	regHeader.dest.addr.byIP.host = 0xffffffff;
	regHeader.dest.addr.byIP.port = 0xffff;
	SDLNet_Write16(0x2, regHeader.dest.socket);

	SDLNet_Write32(0, regHeader.src.network);
	memcpy(regHeader.src.addr.byNode.node, localIpxAddr.netnode, sizeof(regHeader.src.addr.byNode.node));
	SDLNet_Write16(0x2, regHeader.src.socket);
	regHeader.transControl = 0;
	regHeader.pType = 0x0;

	regPacket.data = (Uint8 *)&regHeader;
	regPacket.len = sizeof(regHeader);
	regPacket.maxlen = sizeof(regHeader);
	regPacket.channel = UDPChannel;

	const int result = SDLNet_UDP_Send(ipxClientSocket, regPacket.channel,
	                                   &regPacket);
	if (!result)
		LOG_MSG("IPX: Failed to send a ping packet: %s", SDLNet_GetError());
}

static void receivePacket(uint8_t *buffer, int16_t bufSize) {
	ECBClass *useECB;
	ECBClass *nextECB;
	uint16_t *bufword = (uint16_t *)buffer;
	uint16_t useSocket = swapByte(bufword[8]);
	IPXHeader * tmpHeader;
	tmpHeader = (IPXHeader *)buffer;

	// Check to see if ping packet
	if(useSocket == 0x2) {
		// Is this a broadcast?
		if((tmpHeader->dest.addr.byIP.host == 0xffffffff) &&
			(tmpHeader->dest.addr.byIP.port == 0xffff)) {
			// Yes.  We should return the ping back to the sender
			IPaddress tmpAddr;
			UnpackIP(tmpHeader->src.addr.byIP, &tmpAddr);
			pingAck(tmpAddr);
			return;
		}
	}

	useECB = ECBList;
	while(useECB != nullptr)
	{
		nextECB = useECB->nextECB;
		if(useECB->iuflag == USEFLAG_LISTENING && useECB->mysocket == useSocket) {
			useECB->writeDataBuffer(buffer, bufSize);
			useECB->NotifyESR();
			return;
		}
		useECB = nextECB;
	}
	LOG_IPX("IPX: RX Packet loss!");
}

static void IPX_ClientLoop(void) {
	int numrecv;
	UDPpacket inPacket;
	inPacket.data = (Uint8 *)recvBuffer;
	inPacket.maxlen = IPXBUFFERSIZE;
	inPacket.channel = UDPChannel;

	// Its amazing how much simpler UDP is than TCP
	numrecv = SDLNet_UDP_Recv(ipxClientSocket, &inPacket);
	if(numrecv) receivePacket(inPacket.data, inPacket.len);
}


void DisconnectFromServer(bool unexpected) {
	if(unexpected) LOG_MSG("IPX: Server disconnected unexpectedly");
	if(incomingPacket.connected) {
		incomingPacket.connected = false;
		TIMER_DelTickHandler(&IPX_ClientLoop);
		SDLNet_UDP_Close(ipxClientSocket);
	}
}

static void sendPacket(ECBClass* sendecb) {
	uint8_t outbuffer[IPXBUFFERSIZE];
	fragmentDescriptor tmpFrag;
	uint16_t i, fragCount,t;
	int16_t packetsize;
	uint16_t *wordptr;
	UDPpacket outPacket;

	sendecb->setInUseFlag(USEFLAG_AVAILABLE);
	packetsize = 0;
	fragCount = sendecb->getFragCount();
	for(i=0;i<fragCount;i++) {
		sendecb->getFragDesc(i,&tmpFrag);
		if(i==0) {
			// Fragment containing IPX header
			// Must put source address into header
			uint8_t * addrptr;

			// source netnum
			addrptr = (uint8_t *)&localIpxAddr.netnum;
			for(uint16_t m=0;m<4;m++) {
				real_writeb(tmpFrag.segment,tmpFrag.offset+m+18,addrptr[m]);
			}
			// source node number
			addrptr = (uint8_t *)&localIpxAddr.netnode;
			for(uint16_t m=0;m<6;m++) {
				real_writeb(tmpFrag.segment,tmpFrag.offset+m+22,addrptr[m]);
			}
			// Source socket
			real_writew(tmpFrag.segment,tmpFrag.offset+28, swapByte(sendecb->getSocket()));

			// blank checksum
			real_writew(tmpFrag.segment,tmpFrag.offset, 0xffff);
		}

		for(t=0;t<tmpFrag.size;t++) {
			outbuffer[packetsize] = real_readb(tmpFrag.segment, tmpFrag.offset + t);
			packetsize++;
			if(packetsize>=IPXBUFFERSIZE) {
				LOG_MSG("IPX: Packet size to be sent greater than %d bytes.", IPXBUFFERSIZE);
				sendecb->setCompletionFlag(COMP_UNDELIVERABLE);
				sendecb->NotifyESR();
				return;
			}
		}
	}

	// Add length and source socket to IPX header
	wordptr = (uint16_t *)&outbuffer[0];
	// Blank CRC
	//wordptr[0] = 0xffff;
	// Length
	wordptr[1] = swapByte(packetsize);
	// Source socket
	//wordptr[14] = swapByte(sendecb->getSocket());

	sendecb->getFragDesc(0,&tmpFrag);
	real_writew(tmpFrag.segment,tmpFrag.offset+2, swapByte(packetsize));


	uint8_t immedAddr[6];
	sendecb->getImmAddress(immedAddr);
	// filter out broadcasts and local loopbacks
	// Real implementation uses the ImmedAddr to check wether this is a broadcast

	bool islocalbroadcast=true;
	bool isloopback=true;

	uint8_t * addrptr;

	addrptr = (uint8_t *)&localIpxAddr.netnum;
	for(Bitu m=0;m<4;m++) {
		if(addrptr[m]!=outbuffer[m+0x6])isloopback=false;
	}
	addrptr = (uint8_t *)&localIpxAddr.netnode;
	for(Bitu m=0;m<6;m++) {
		if(addrptr[m]!=outbuffer[m+0xa])isloopback=false;
		if(immedAddr[m]!=0xff) islocalbroadcast=false;
	}
	LOG_IPX("SEND crc:%2x",packetCRC(&outbuffer[0], packetsize));
	if(!isloopback) {
		outPacket.channel = UDPChannel;
		outPacket.data = (Uint8 *)&outbuffer[0];
		outPacket.len = packetsize;
		outPacket.maxlen = packetsize;
		// Since we're using a channel, we won't send the IP address again
		const int result = SDLNet_UDP_Send(ipxClientSocket, UDPChannel,
		                                   &outPacket);
		if (result == 0) {
			LOG_MSG("IPX: Could not send packet: %s", SDLNet_GetError());
			sendecb->setCompletionFlag(COMP_HARDWAREERROR);
			sendecb->NotifyESR();
			DisconnectFromServer(true);
			return;
		} else {
			sendecb->setCompletionFlag(COMP_SUCCESS);
			LOG_IPX("Packet sent: size: %d",packetsize);
		}
	}
	else sendecb->setCompletionFlag(COMP_SUCCESS);

	if(isloopback||islocalbroadcast) {
		// Send packet back to ourselves.
		receivePacket(&outbuffer[0],packetsize);
		LOG_IPX("Packet back: loopback:%d, broadcast:%d",isloopback,islocalbroadcast);
	}
	sendecb->NotifyESR();
}

static bool pingCheck(IPXHeader * outHeader) {
	char buffer[1024];
	UDPpacket regPacket;
	IPXHeader *regHeader;
	regPacket.data = (Uint8 *)buffer;
	regPacket.maxlen = sizeof(buffer);
	regPacket.channel = UDPChannel;
	regHeader = (IPXHeader *)buffer;

	const int result = SDLNet_UDP_Recv(ipxClientSocket, &regPacket);
	if (result != 0) {
		memcpy(outHeader, regHeader, sizeof(IPXHeader));
		return true;
	}
	return false;
}

bool ConnectToServer(const char* strAddr)
{
	int numsent;
	UDPpacket regPacket;
	IPXHeader regHeader;
	if(!SDLNet_ResolveHost(&ipxServConnIp, strAddr, (uint16_t)udpPort)) {

		// Generate the MAC address.  This is made by zeroing out the first two
		// octets and then using the actual IP address for the last 4 octets.
		// This idea is from the IPX over IP implementation as specified in RFC 1234:
		// http://www.faqs.org/rfcs/rfc1234.html

		// Select an anonymous UDP port
		ipxClientSocket = SDLNet_UDP_Open(0);
		if(ipxClientSocket) {
			// Bind UDP port to address to channel
			UDPChannel = SDLNet_UDP_Bind(ipxClientSocket,-1,&ipxServConnIp);
			//ipxClientSocket = SDLNet_TCP_Open(&ipxServConnIp);
			SDLNet_Write16(0xffff, regHeader.checkSum);
			SDLNet_Write16(sizeof(regHeader), regHeader.length);

			// Echo packet with zeroed dest and src is a server registration packet
			SDLNet_Write32(0, regHeader.dest.network);
			regHeader.dest.addr.byIP.host = 0x0;
			regHeader.dest.addr.byIP.port = 0x0;
			SDLNet_Write16(0x2, regHeader.dest.socket);

			SDLNet_Write32(0, regHeader.src.network);
			regHeader.src.addr.byIP.host = 0x0;
			regHeader.src.addr.byIP.port = 0x0;
			SDLNet_Write16(0x2, regHeader.src.socket);
			regHeader.transControl = 0;

			regPacket.data = (Uint8 *)&regHeader;
			regPacket.len = sizeof(regHeader);
			regPacket.maxlen = sizeof(regHeader);
			regPacket.channel = UDPChannel;
			// Send registration string to server.  If server doesn't get
			// this, client will not be registered
			numsent = SDLNet_UDP_Send(ipxClientSocket, regPacket.channel, &regPacket);

			if(!numsent) {
				LOG_MSG("IPX: Unable to connect to server: %s", SDLNet_GetError());
				SDLNet_UDP_Close(ipxClientSocket);
				return false;
			} else {
				// Wait for return packet from server.
				// This will contain our IPX address and port num
				const auto ticks = GetTicks();

				while(true) {
					const auto elapsed = GetTicksSince(ticks);
					if(elapsed > 5000) {
						LOG_MSG("Timeout connecting to server at %s", strAddr);
						SDLNet_UDP_Close(ipxClientSocket);

						return false;
					}
					CALLBACK_Idle();
					const int res = SDLNet_UDP_Recv(ipxClientSocket,
					                                &regPacket);
					if (res != 0) {
						memcpy(localIpxAddr.netnode, regHeader.dest.addr.byNode.node, sizeof(localIpxAddr.netnode));
						memcpy(localIpxAddr.netnum, regHeader.dest.network, sizeof(localIpxAddr.netnum));
						break;
					}
				}

				LOG_MSG("IPX: Connected to server.  IPX address is %d:%d:%d:%d:%d:%d", CONVIPX(localIpxAddr.netnode));

				incomingPacket.connected = true;
				TIMER_AddTickHandler(&IPX_ClientLoop);
				return true;
			}
		} else {
			LOG_MSG("IPX: Unable to open socket");
		}
	} else {
		LOG_MSG("IPX: Unable resolve connection to server");
	}
	return false;
}

void IPX_NetworkInit() {

	localIpxAddr.netnum[0] = 0x0;
	localIpxAddr.netnum[1] = 0x0;
	localIpxAddr.netnum[2] = 0x0;
	localIpxAddr.netnum[3] = 0x1;
	localIpxAddr.netnode[0] = 0x00;
	localIpxAddr.netnode[1] = 0x00;
	localIpxAddr.netnode[2] = 0x00;
	localIpxAddr.netnode[3] = 0x00;
	localIpxAddr.netnode[4] = 0x00;
	localIpxAddr.netnode[5] = 0x00;

	socketCount = 0;
	return;
}

class IPXNET final : public Program {
public:
	void HelpCommand(const char *helpStr) {
		// Help on connect command
		if(strcasecmp("connect", helpStr) == 0) {
			WriteOut("IPXNET CONNECT opens a connection to an IPX tunneling server running on another\n");
			WriteOut("DOSBox session.  The \"address\" parameter specifies the IP address or host name\n");
			WriteOut("of the server computer.  One can also specify the UDP port to use.  By default\n");
			WriteOut("IPXNET uses port 213, the assigned IANA port for IPX tunneling, for its\nconnection.\n\n");
			WriteOut("The syntax for IPXNET CONNECT is:\n\n");
			WriteOut("IPXNET CONNECT address <port>\n\n");
			return;
		}
		// Help on the disconnect command
		if(strcasecmp("disconnect", helpStr) == 0) {
			WriteOut("IPXNET DISCONNECT closes the connection to the IPX tunneling server.\n\n");
			WriteOut("The syntax for IPXNET DISCONNECT is:\n\n");
			WriteOut("IPXNET DISCONNECT\n\n");
			return;
		}
		// Help on the startserver command
		if(strcasecmp("startserver", helpStr) == 0) {
			WriteOut("IPXNET STARTSERVER starts and IPX tunneling server on this DOSBox session.  By\n");
			WriteOut("default, the server will accept connections on UDP port 213, though this can be\n");
			WriteOut("changed.  Once the server is started, DOSBox will automatically start a client\n");
			WriteOut("connection to the IPX tunneling server.\n\n");
			WriteOut("The syntax for IPXNET STARTSERVER is:\n\n");
			WriteOut("IPXNET STARTSERVER <port>\n\n");
			return;
		}
		// Help on the stop server command
		if(strcasecmp("stopserver", helpStr) == 0) {
			WriteOut("IPXNET STOPSERVER stops the IPX tunneling server running on this DOSBox\nsession.");
			WriteOut("  Care should be taken to ensure that all other connections have\nterminated ");
			WriteOut("as well since stopping the server may cause lockups on other\nmachines still using ");
			WriteOut("the IPX tunneling server.\n\n");
			WriteOut("The syntax for IPXNET STOPSERVER is:\n\n");
			WriteOut("IPXNET STOPSERVER\n\n");
			return;
		}
		// Help on the ping command
		if(strcasecmp("ping", helpStr) == 0) {
			WriteOut("IPXNET PING broadcasts a ping request through the IPX tunneled network.  In    \n");
			WriteOut("response, all other connected computers will respond to the ping and report\n");
			WriteOut("the time it took to receive and send the ping message.\n\n");
			WriteOut("The syntax for IPXNET PING is:\n\n");
			WriteOut("IPXNET PING\n\n");
			return;
		}
		// Help on the status command
		if(strcasecmp("status", helpStr) == 0) {
			WriteOut("IPXNET STATUS reports the current state of this DOSBox's sessions IPX tunneling\n");
			WriteOut("network.  For a list of the computers connected to the network use the IPXNET \n");
			WriteOut("PING command.\n\n");
			WriteOut("The syntax for IPXNET STATUS is:\n\n");
			WriteOut("IPXNET STATUS\n\n");
			return;
		}
	}

	void Run(void) override
	{
		WriteOut("IPX Tunneling utility for DOSBox\n\n");
		if(!cmd->GetCount()) {
			WriteOut("The syntax of this command is:\n\n");
			WriteOut("IPXNET [ CONNECT | DISCONNECT | STARTSERVER | STOPSERVER | PING | HELP |\n         STATUS ]\n\n");
			return;
		}

		if(cmd->FindCommand(1, temp_line)) {
			if(strcasecmp("help", temp_line.c_str()) == 0) {
				if(!cmd->FindCommand(2, temp_line)) {
					WriteOut("The following are valid IPXNET commands:\n\n");
					WriteOut("IPXNET CONNECT        IPXNET DISCONNECT       IPXNET STARTSERVER\n");
					WriteOut("IPXNET STOPSERVER     IPXNET PING             IPXNET STATUS\n\n");
					WriteOut("To get help on a specific command, type:\n\n");
					WriteOut("IPXNET HELP command\n\n");

				} else {
					HelpCommand(temp_line.c_str());
					return;
				}
				return;
			}
			if(strcasecmp("startserver", temp_line.c_str()) == 0) {
				if(!isIpxServer) {
					if(incomingPacket.connected) {
						WriteOut("IPX Tunneling Client already connected to another server.  Disconnect first.\n");
						return;
					}
					bool startsuccess;
					if(!cmd->FindCommand(2, temp_line)) {
						udpPort = 213;
					} else {
						udpPort = strtol(temp_line.c_str(), nullptr, 10);
					}
					startsuccess = IPX_StartServer((uint16_t)udpPort);
					if(startsuccess) {
						WriteOut("IPX Tunneling Server started\n");
						isIpxServer = true;
						ConnectToServer("localhost");
					} else {
						WriteOut("IPX Tunneling Server failed to start.\n");
						if(udpPort < 1024) WriteOut("Try a port number above 1024. See IPXNET HELP CONNECT on how to specify a port.\n");
					}
				} else {
					WriteOut("IPX Tunneling Server already started\n");
				}
				return;
			}
			if(strcasecmp("stopserver", temp_line.c_str()) == 0) {
				if(!isIpxServer) {
					WriteOut("IPX Tunneling Server not running in this DOSBox session.\n");
				} else {
					isIpxServer = false;
					DisconnectFromServer(false);
					IPX_StopServer();
					WriteOut("IPX Tunneling Server stopped.");
				}
				return;
			}
			if(strcasecmp("connect", temp_line.c_str()) == 0) {
				char strHost[1024];
				if(incomingPacket.connected) {
					WriteOut("IPX Tunneling Client already connected.\n");
					return;
				}
				if(!cmd->FindCommand(2, temp_line)) {
					WriteOut("IPX Server address not specified.\n");
					return;
				}
				safe_strcpy(strHost, temp_line.c_str());

				if(!cmd->FindCommand(3, temp_line)) {
					udpPort = 213;
				} else {
					udpPort = strtol(temp_line.c_str(), nullptr, 10);
				}

				if(ConnectToServer(strHost)) {
					WriteOut("IPX Tunneling Client connected to server at %s.\n",
					         strHost);
				} else {
					WriteOut("IPX Tunneling Client failed to connect to server at %s.\n", strHost);
				}
				return;
			}

			if(strcasecmp("disconnect", temp_line.c_str()) == 0) {
				if(!incomingPacket.connected) {
					WriteOut("IPX Tunneling Client not connected.\n");
					return;
				}
				// TODO: Send a packet to the server notifying of disconnect
				WriteOut("IPX Tunneling Client disconnected from server.\n");
				DisconnectFromServer(false);
				return;
			}

			if(strcasecmp("status", temp_line.c_str()) == 0) {
				WriteOut("IPX Tunneling Status:\n\n");
				WriteOut("Server status: %s\n",
				         (isIpxServer ? "ACTIVE" : "INACTIVE"));
				WriteOut("Client status: ");
				if(incomingPacket.connected) {
					WriteOut("CONNECTED -- Server at %d.%d.%d.%d port %d\n", CONVIP(ipxServConnIp.host), udpPort);
				} else {
					WriteOut("DISCONNECTED\n");
				}
				if(isIpxServer) {
					WriteOut("List of active connections:\n\n");
					int i;
					IPaddress *ptrAddr;
					for(i=0;i<SOCKETTABLESIZE;i++) {
						if(IPX_isConnectedToServer(i,&ptrAddr)) {
							WriteOut("     %d.%d.%d.%d from port %d\n", CONVIP(ptrAddr->host), SDLNet_Read16(&ptrAddr->port));
						}
					}
					WriteOut("\n");
				}
				return;
			}

			if(strcasecmp("ping", temp_line.c_str()) == 0) {
				IPXHeader pingHead;

				if(!incomingPacket.connected) {
					WriteOut("IPX Tunneling Client not connected.\n");
					return;
				}
				TIMER_DelTickHandler(&IPX_ClientLoop);
				WriteOut("Sending broadcast ping:\n\n");
				pingSend();
				const auto ticks = GetTicks();
				while ((GetTicksSince(ticks)) < 1500) {
					CALLBACK_Idle();
					if(pingCheck(&pingHead)) {
						WriteOut(
						        "Response from %d.%d.%d.%d, port %d time=%lldms\n",
						        CONVIP(pingHead.src.addr.byIP.host),
						        SDLNet_Read16(&pingHead.src.addr.byIP.port),
						        GetTicksSince(ticks));
					}
				}
				TIMER_AddTickHandler(&IPX_ClientLoop);
				return;
			}
		}
	}
};

Bitu IPX_ESRHandler(void) {
	LOG_IPX("ESR: >>>>>>>>>>>>>>>" );
	while(ESRList!=nullptr) {
		// LOG_IPX("ECB: SN%7d notified.", ESRList->SerialNumber);
		if(ESRList->databuffer) ESRList->writeData();
		if(ESRList->getESRAddr()) {
			// setup registers
			SegSet16(es, RealSegment(ESRList->ECBAddr));
			reg_si = RealOffset(ESRList->ECBAddr);
			reg_al = 0xff;
			CALLBACK_RunRealFar(RealSegment(ESRList->getESRAddr()),
								RealOffset(ESRList->getESRAddr()));
		}
		delete ESRList; //Destructor updates this pointer to the next value or NULL
	}	// while

	IO_WriteB(0xa0,0x63);	//EOI11
	IO_WriteB(0x20,0x62);	//EOI2
	LOG_IPX("ESR: <<<<<<<<<<<<<<<");
	return CBRET_NONE;
}

bool NetWrapper_InitializeSDLNet(); // from misc_util.cpp

class IPX final : public ModuleBase {
private:
	CALLBACK_HandlerObject callback_ipx = {};
	CALLBACK_HandlerObject callback_esr = {};
	CALLBACK_HandlerObject callback_ipxint = {};
	RealPt old_73_vector = 0;
	static uint16_t dospage;

public:
	IPX(Section *configuration) : ModuleBase(configuration)
	{
		ECBList = nullptr;
		ESRList = nullptr;
		isIpxServer = false;
		isIpxConnected = false;

		SectionProp *section = static_cast<SectionProp *>(configuration);
		if (section && !section->GetBool("ipx"))
			return;

		if (!NetWrapper_InitializeSDLNet())
			return;

		IPX_NetworkInit();

		DOS_AddMultiplexHandler(IPX_Multiplex);

		callback_ipx.Install(&IPX_Handler,CB_RETF,"IPX Handler");
		ipx_callback = callback_ipx.Get_RealPointer();

		callback_ipxint.Install(&IPX_IntHandler,CB_IRET,"IPX (int 7a)");
		callback_ipxint.Set_RealVec(0x7a);

		callback_esr.Allocate(&IPX_ESRHandler,"IPX_ESR");
		uint16_t call_ipxesr1 = callback_esr.Get_callback();

		if(!dospage) dospage = DOS_GetMemory(2); // can not be freed yet

		PhysPt phyDospage = PhysicalMake(dospage,0);

		LOG_IPX("ESR callback address: %x, HandlerID %d", phyDospage,call_ipxesr1);

		//save registers
		phys_writeb(phyDospage+0,(uint8_t)0xFA);    // CLI
		phys_writeb(phyDospage+1,(uint8_t)0x60);    // PUSHA
		phys_writeb(phyDospage+2,(uint8_t)0x1E);    // PUSH DS
		phys_writeb(phyDospage+3,(uint8_t)0x06);    // PUSH ES
		phys_writew(phyDospage+4,(uint16_t)0xA00F); // PUSH FS
		phys_writew(phyDospage+6,(uint16_t)0xA80F); // PUSH GS

		// callback
		phys_writeb(phyDospage+8,(uint8_t)0xFE);  // GRP 4
		phys_writeb(phyDospage+9,(uint8_t)0x38);  // Extra Callback instruction
		phys_writew(phyDospage+10,call_ipxesr1);        // Callback identifier

		// register recreation
		phys_writew(phyDospage+12,(uint16_t)0xA90F); // POP GS
		phys_writew(phyDospage+14,(uint16_t)0xA10F); // POP FS
		phys_writeb(phyDospage+16,(uint8_t)0x07);    // POP ES
		phys_writeb(phyDospage+17,(uint8_t)0x1F);    // POP DS
		phys_writeb(phyDospage+18,(uint8_t)0x61);    // POPA
		phys_writeb(phyDospage+19,(uint8_t)0xCF);    // IRET: restores flags, CS, IP

		// IPX version 2.12
		//phys_writeb(phyDospage+27,(uint8_t)0x2);
		//phys_writeb(phyDospage+28,(uint8_t)0x12);
		//IPXVERpointer = RealMake(dospage,27);

		RealPt ESRRoutineBase = RealMake(dospage, 0);

		// Interrupt enabling
		RealSetVec(0x73,ESRRoutineBase,old_73_vector);	// IRQ11
		IO_WriteB(0xa1,IO_ReadB(0xa1)&(~8));			// enable IRQ11

		PROGRAMS_MakeFile("IPXNET.COM",ProgramCreate<IPXNET>);
	}

	~IPX() {
		SectionProp * section = static_cast<SectionProp *>(m_configuration);
		PIC_RemoveEvents(IPX_AES_EventHandler);
		if(!section->GetBool("ipx")) return;

		if(isIpxServer) {
			isIpxServer = false;
			IPX_StopServer();
		}
		DisconnectFromServer(false);

		DOS_DeleteMultiplexHandler(IPX_Multiplex);
		RealSetVec(0x73,old_73_vector);
		IO_WriteB(0xa1,IO_ReadB(0xa1)|8);	// disable IRQ11

		PhysPt phyDospage = PhysicalMake(dospage,0);
		for(Bitu i = 0;i < 32;i++)
			phys_writeb(phyDospage+i,(uint8_t)0x00);

		VFILE_Remove("IPXNET.COM");
	}
};

static IPX* test;

void IPX_ShutDown([[maybe_unused]] Section* sec) {
	delete test;
}

void IPX_Init(Section* sec)
{
	assert(sec);

	test = new IPX(sec);

	constexpr auto changeable_at_runtime = true;
	sec->AddDestroyFunction(&IPX_ShutDown, changeable_at_runtime);
}

//Initialize static members;
uint16_t IPX::dospage = 0;

#endif
