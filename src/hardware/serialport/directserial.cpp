// SPDX-FileCopyrightText:  2002-2025 The DOSBox Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "dosbox.h"

#if C_DIRECTSERIAL

#include "serialport.h"
#include "directserial.h"
#include "misc_util.h"
#include "pic.h"

#include "libserial.h"

/* This is a serial passthrough class.  Its amazingly simple to */
/* write now that the serial ports themselves were abstracted out */

CDirectSerial::CDirectSerial(const uint8_t port_idx, CommandLine *cmd)
        : CSerial(port_idx, cmd)
{
	InstallationSuccessful = false;
	comport = nullptr;

	rx_retry = 0;
    rx_retry_max = 0;

	std::string tmpstring;
	if(!cmd->FindStringCaseInsensitiveBegin("realport:",tmpstring,false)) return;

	LOG_MSG("SERIAL: Port %" PRIu8 " opening %s.", GetPortNumber(), tmpstring.c_str());
	if(!SERIAL_open(tmpstring.c_str(), &comport)) {
		char errorbuffer[256];
		SERIAL_getErrorString(errorbuffer, sizeof(errorbuffer));
		LOG_MSG("SERIAL: Port %" PRIu8 " could not open \"%s\" due to: %s.",
		        GetPortNumber(), tmpstring.c_str(), errorbuffer);
		return;
	}

#if SERIAL_DEBUG
	dbgmsg_poll_block=false;
	dbgmsg_rx_block=false;
#endif

	// rxdelay: How many milliseconds to wait before causing an
	// overflow when the application is unresponsive.
	if (getUintFromString("rxdelay:", rx_retry_max, cmd)) {
		if(!(rx_retry_max<=10000)) {
			rx_retry_max=0;
		}
	}

	CSerial::Init_Registers();
	InstallationSuccessful = true;
	rx_state = D_RX_IDLE;
	setEvent(SERIAL_POLLING_EVENT, 1); // millisecond receive tick
}

CDirectSerial::~CDirectSerial () {
	if(comport) SERIAL_close(comport);
	// We do not use own events so we don't have to clear them.
}

// CanReceive: true:UART part has room left
// doReceive:  true:there was really a byte to receive
// rx_retry is incremented in polling events

// in POLLING_EVENT: always add new polling event
// D_RX_IDLE + CanReceive + doReceive		-> D_RX_WAIT   , add RX_EVENT
// D_RX_IDLE + CanReceive + not doReceive	-> D_RX_IDLE
// D_RX_IDLE + not CanReceive				-> D_RX_BLOCKED, add RX_EVENT

// D_RX_BLOCKED + CanReceive + doReceive	-> D_RX_FASTWAIT, rem RX_EVENT
//											   rx_retry=0   , add RX_EVENT
// D_RX_BLOCKED + CanReceive + !doReceive	-> D_RX_IDLE,     rem RX_EVENT
//											   rx_retry=0
// D_RX_BLOCKED + !CanReceive + doReceive + retry < max	-> D_RX_BLOCKED, rx_retry++ 
// D_RX_BLOCKED + !CanReceive + doReceive + retry >=max	-> rx_retry=0 	

// to be continued...

void CDirectSerial::handleUpperEvent(uint16_t type)
{
	switch (type) {
	case SERIAL_POLLING_EVENT: {
		setEvent(SERIAL_POLLING_EVENT, 1.0f);
		// update Modem input line states
		switch (rx_state) {
		case D_RX_IDLE:
			if (CanReceiveByte()) {
				if (doReceive()) {
					// a byte was received
					rx_state = D_RX_WAIT;
					setEvent(SERIAL_RX_EVENT, bytetime * 0.9f);
				} // else still idle
			} else {
#if SERIAL_DEBUG
						if(!dbgmsg_poll_block) {
							log_ser(dbg_aux,"Directserial: block on polling.");
							dbgmsg_poll_block=true;
						}
#endif
						rx_state=D_RX_BLOCKED;
						// have both delays (1ms + bytetime)
						setEvent(SERIAL_RX_EVENT, bytetime*0.9f);
					}
					break;
				case D_RX_BLOCKED:
                    // one timeout tick
					if(!CanReceiveByte()) {
						rx_retry++;
						if(rx_retry>=rx_retry_max) {
							// it has timed out:
							rx_retry=0;
							removeEvent(SERIAL_RX_EVENT);
							if(doReceive()) {
								// read away everything
								// this will set overrun errors
								while(doReceive());
								rx_state=D_RX_WAIT;
								setEvent(SERIAL_RX_EVENT, bytetime*0.9f);
							} else {
								// much trouble about nothing
                                rx_state=D_RX_IDLE;
							}
						} // else wait further
					} else {
						// good: we can receive again
#if SERIAL_DEBUG
						dbgmsg_poll_block=false;
						dbgmsg_rx_block=false;
#endif
						removeEvent(SERIAL_RX_EVENT);
						rx_retry=0;
						if(doReceive()) {
							rx_state=D_RX_FASTWAIT;
							setEvent(SERIAL_RX_EVENT, bytetime*0.65f);
						} else {
							// much trouble about nothing
							rx_state=D_RX_IDLE;
						}
					}
					break;

				case D_RX_WAIT:
				case D_RX_FASTWAIT:
					break;
			}
			updateMSR();
			break;
		}
		case SERIAL_RX_EVENT: {
			switch(rx_state) {
				case D_RX_IDLE:
			                LOG_MSG("SERIAL: Port %" PRIu8 " internal "
			                        "error in direct mode.",
			                        GetPortNumber());
			                break;

				case D_RX_BLOCKED: // try to receive
				case D_RX_WAIT:
				case D_RX_FASTWAIT:
					if(CanReceiveByte()) {
						// just works or unblocked
						rx_retry=0; // not waiting anymore
						if(doReceive()) {
							if(rx_state==D_RX_WAIT) setEvent(SERIAL_RX_EVENT, bytetime*0.9f);
							else {
								// maybe unblocked
								rx_state=D_RX_FASTWAIT;
								setEvent(SERIAL_RX_EVENT, bytetime*0.65f);
							}
						} else {
							// didn't receive anything
							rx_state=D_RX_IDLE;
						}
					} else {
						// blocking now or still blocked
#if SERIAL_DEBUG
						if(rx_state==D_RX_BLOCKED) {
							if(!dbgmsg_rx_block) {
                                log_ser(dbg_aux,"Directserial: rx still blocked (retry=%d)",rx_retry);
								dbgmsg_rx_block=true;
							}
						}






						else log_ser(dbg_aux,"Directserial: block on continued rx (retry=%d).",rx_retry);
#endif
						setEvent(SERIAL_RX_EVENT, bytetime*0.65f);
						rx_state=D_RX_BLOCKED;
					}

					break;
			}
			updateMSR();
			break;
		}
		case SERIAL_TX_EVENT: {
			// Maybe echo cirquit works a bit better this way
			if(rx_state==D_RX_IDLE && CanReceiveByte()) {
				if(doReceive()) {
					// a byte was received
					rx_state=D_RX_WAIT;
					setEvent(SERIAL_RX_EVENT, bytetime*0.9f);
				}
			}
			ByteTransmitted();
			updateMSR();
			break;
		}
		case SERIAL_THR_EVENT: {
			ByteTransmitting();
			setEvent(SERIAL_TX_EVENT,bytetime*1.1f);
			break;				   
		}
	        }
}

bool CDirectSerial::doReceive() {
	int value = SERIAL_getextchar(comport);
	if(value) {
		receiveByteEx((uint8_t)(value & 0xff),
		              (uint8_t)((value & 0xff00) >> 8));
		return true;
	}
	return false;
}

// updatePortConfig is called when emulated app changes the serial port
// parameters baudrate, stopbits, number of databits, parity.
void CDirectSerial::updatePortConfig(uint16_t, uint8_t lcr)
{
	uint8_t parity = 0;

	switch ((lcr & 0x38)>>3) {
	case 0x1: parity='o'; break;
	case 0x3: parity='e'; break;
	case 0x5: parity='m'; break;
	case 0x7: parity='s'; break;
	default: parity='n'; break;
	}

	uint8_t bytelength = (lcr & 0x3) + 5;

	// baudrate
	const uint32_t baudrate = GetPortBaudRate();

	// stopbits
	uint8_t stopbits;
	if (lcr & 0x4) {
		if (bytelength == 5) stopbits = SERIAL_15STOP;
		else stopbits = SERIAL_2STOP;
	} else stopbits = SERIAL_1STOP;

	if(!SERIAL_setCommParameters(comport, baudrate, parity, stopbits, bytelength)) {
#if SERIAL_DEBUG
		log_ser(dbg_aux,"Serial port settings not supported by host." );
#endif
		LOG_MSG("SERIAL: Port %" PRIu8 " desired mode not supported "
		        "(%" PRIu32 ", %" PRIu8 ", %c, %" PRIu8 ".",
		        GetPortNumber(), baudrate, bytelength, parity, stopbits);
	}
	CDirectSerial::setRTSDTR(getRTS(), getDTR());
}

void CDirectSerial::updateMSR () {
	int new_status = SERIAL_getmodemstatus(comport);

	setCTS(new_status&SERIAL_CTS? true:false);
	setDSR(new_status&SERIAL_DSR? true:false);
	setRI(new_status&SERIAL_RI? true:false);
	setCD(new_status&SERIAL_CD? true:false);
}

void CDirectSerial::transmitByte(uint8_t val, bool first)
{
	if (!SERIAL_sendchar(comport, val))
		LOG_MSG("SERIAL: Port %" PRIu8 " write failed!", GetPortNumber());
	if(first) setEvent(SERIAL_THR_EVENT, bytetime/8);
	else setEvent(SERIAL_TX_EVENT, bytetime);
}

// setBreak(val) switches break on or off
void CDirectSerial::setBreak (bool value) {
	SERIAL_setBREAK(comport,value);
}

// updateModemControlLines(mcr) sets DTR and RTS.
void CDirectSerial::setRTSDTR(bool rts_state, bool dtr_state)
{
	SERIAL_setRTS(comport, rts_state);
	SERIAL_setDTR(comport, dtr_state);
}

void CDirectSerial::setRTS(bool val) {
	SERIAL_setRTS(comport,val);
}

void CDirectSerial::setDTR(bool val) {
	SERIAL_setDTR(comport,val);
}

#endif // C_DIRECTSERIAL
