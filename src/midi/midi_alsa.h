// SPDX-FileCopyrightText:  2020-2025 The DOSBox Staging Team
// SPDX-FileCopyrightText:  2002-2021 The DOSBox Team
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_MIDI_ALSA_H
#define DOSBOX_MIDI_ALSA_H

#include "midi_device.h"

#if C_ALSA

#include <alsa/asoundlib.h>

struct AlsaAddress {
	int client = -1;
	int port   = -1;
};

class MidiDeviceAlsa final : public MidiDevice {
public:
	// Throws `std::runtime_error` if the MIDI device cannot be initialiased
	MidiDeviceAlsa(const char* conf);

	~MidiDeviceAlsa() override;

	// prevent copying
	MidiDeviceAlsa(const MidiDeviceAlsa&) = delete;
	// prevent assignment
	MidiDeviceAlsa& operator=(const MidiDeviceAlsa&) = delete;

	std::string GetName() const override
	{
		return MidiDeviceName::Alsa;
	}

	Type GetType() const override
	{
		return MidiDevice::Type::External;
	}

	void SendMidiMessage(const MidiMessage& msg) override;
	void SendSysExMessage(uint8_t* sysex, size_t len) override;

	AlsaAddress GetInputPortAddress();

private:
	snd_seq_event_t ev    = {};
	snd_seq_t* seq_handle = nullptr;

	// address of input port we're connected to
	AlsaAddress seq = {-1, -1};

	int output_port = 0;

	void send_event(int do_flush);
};

void ALSA_ListDevices(MidiDeviceAlsa* device, Program* caller);

#endif // C_ALSA

#endif
