// SPDX-FileCopyrightText:  2020-2025 The DOSBox Staging Team
// SPDX-FileCopyrightText:  2002-2021 The DOSBox Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "midi.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <list>
#include <memory>
#include <string>

#include <SDL.h>

#include "../capture/capture.h"
#include "ansi_code_markup.h"
#include "control.h"
#include "cross.h"
#include "mapper.h"
#include "midi_device.h"
#include "mpu401.h"
#include "pic.h"
#include "programs.h"
#include "setup.h"
#include "string_utils.h"
#include "timer.h"

// #define DEBUG_MIDI

// clang-format off
uint8_t MIDI_message_len_by_status[256] = {
  // Data bytes (dummy zero values)
  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,  // 0x00
  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,  // 0x10
  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,  // 0x20
  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,  // 0x30
  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,  // 0x40
  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,  // 0x50
  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,  // 0x60
  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,  // 0x70

  // Status bytes
  3,3,3,3, 3,3,3,3, 3,3,3,3, 3,3,3,3,  // 0x80 -- Note Off
  3,3,3,3, 3,3,3,3, 3,3,3,3, 3,3,3,3,  // 0x90 -- Note On
  3,3,3,3, 3,3,3,3, 3,3,3,3, 3,3,3,3,  // 0xa0 -- Poly Key Pressure
  3,3,3,3, 3,3,3,3, 3,3,3,3, 3,3,3,3,  // 0xb0 -- Control Change

  2,2,2,2, 2,2,2,2, 2,2,2,2, 2,2,2,2,  // 0xc0 -- Program Change
  2,2,2,2, 2,2,2,2, 2,2,2,2, 2,2,2,2,  // 0xd0 -- Channel Pressure

  3,3,3,3, 3,3,3,3, 3,3,3,3, 3,3,3,3,  // 0xe0 -- Pitch Bend

  0,2,3,2, 0,0,1,0, 1,0,1,1, 1,0,1,0   // 0xf0 -- System Exclusive
};
// clang-format on

#include "midi_fluidsynth.h"
#include "midi_mt32.h"
#include "midi_soundcanvas.h"

#if defined(MACOSX)
#include "midi_coreaudio.h"
#include "midi_coremidi.h"

#elif defined(WIN32)
#include "midi_win32.h"
#endif

#if C_ALSA
#include "midi_alsa.h"
#endif

static std::unique_ptr<MidiDevice> create_device(
        [[maybe_unused]] const std::string& name,
        [[maybe_unused]] const std::string& config)
{
	using namespace MidiDeviceName;

	// Internal MIDI synths
	if (name == MidiDeviceName::SoundCanvas) {
		return std::make_unique<MidiDeviceSoundCanvas>();
	}
	// namespace prefix required to avoid ambiguity with FluidSynth namespace
	if (name == MidiDeviceName::FluidSynth) {
		return std::make_unique<MidiDeviceFluidSynth>();
	}
#if C_MT32EMU
	if (name == Mt32) {
		return std::make_unique<MidiDeviceMt32>();
	}
#endif

	// External MIDI devices
#if C_COREMIDI
	if (name == CoreMidi) {
		return std::make_unique<MidiDeviceCoreMidi>(config.c_str());
	}
#endif
#if C_COREAUDIO
	if (name == CoreAudio) {
		return std::make_unique<MidiDeviceCoreAudio>(config.c_str());
	}
#endif
#if defined(WIN32)
	if (name == Win32) {
		return std::make_unique<MidiDeviceWin32>(config.c_str());
	}
#endif
#if C_ALSA
	if (name == Alsa) {
		return std::make_unique<MidiDeviceAlsa>(config.c_str());
	}
#endif

	// Device not found
	return {};
}

struct Midi {
	uint8_t status = 0;

	struct {
		MidiMessage msg = {};

		size_t len = 0;
		size_t pos = 0;
	} message = {};

	MidiMessage realtime_message = {};

	struct {
		uint8_t buf[MaxMidiSysExBytes] = {};

		size_t pos       = 0;
		int64_t delay_ms = 0;
		int64_t start_ms = 0;
	} sysex = {};

	bool is_muted = false;

	std::unique_ptr<MidiDevice> device = nullptr;
};

static Midi midi                    = {};
static bool raw_midi_output_enabled = {};

constexpr auto MaxChannelVolume = 127;

// Keep track of the state of the MIDI device (e.g. channel volumes and which
// notes are currently active on each channel).
class MidiState {
public:
	MidiState()
	{
		Reset();
	}

	void Reset()
	{
		note_on_tracker.fill(false);
		channel_volume_tracker.fill(MaxChannelVolume);
	}

	void UpdateState(const MidiMessage& msg)
	{
		const auto status  = get_midi_status(msg.status());
		const auto channel = get_midi_channel(msg.status());

		if (status == MidiStatus::NoteOn) {
			const auto note = msg.data1();
			SetNoteActive(channel, note, true);

		} else if (status == MidiStatus::NoteOff) {
			const auto note = msg.data1();
			SetNoteActive(channel, note, false);

		} else if (status == MidiStatus::ControlChange) {
			if (msg.data1() == MidiController::Volume) {
				const auto volume = msg.data2();
				SetChannelVolume(channel, volume);
			}
		}
	}

	inline void SetNoteActive(const uint8_t channel, const uint8_t note,
	                          const bool is_playing)
	{
		note_on_tracker[NoteAddr(channel, note)] = is_playing;
	}

	inline bool IsNoteActive(const uint8_t channel, const uint8_t note)
	{
		return note_on_tracker[NoteAddr(channel, note)];
	}

	inline void SetChannelVolume(const uint8_t channel, const uint8_t volume)
	{
		assert(channel <= NumMidiChannels);
		assert(volume <= MaxChannelVolume);

		channel_volume_tracker[channel] = volume;
	}

	inline uint8_t GetChannelVolume(const uint8_t channel)
	{
		assert(channel <= NumMidiChannels);

		return channel_volume_tracker[channel];
	}

	~MidiState() = default;

	// prevent copying
	MidiState(const MidiState&) = delete;
	// prevent assignment
	MidiState& operator=(const MidiState&) = delete;

private:
	std::array<bool, NumMidiNotes* NumMidiChannels> note_on_tracker = {};
	std::array<uint8_t, NumMidiChannels> channel_volume_tracker     = {};

	inline size_t NoteAddr(const uint8_t channel, const uint8_t note)
	{
		assert(channel <= LastMidiChannel);
		assert(note <= LastMidiNote);
		return channel * NumMidiNotes + note;
	}
};

static MidiState midi_state = {};

void init_midi_state(Section*)
{
	midi_state.Reset();
}

/* When using a physical Roland MT-32 rev. 0 as MIDI output device,
 * some games may require a delay in order to prevent buffer overflow
 * issues.
 *
 * Explanation for this formula can be found in discussion under patch
 * that introduced it: https://sourceforge.net/p/dosbox/patches/241/
 */
static int delay_in_ms(size_t sysex_bytes_num)
{
	constexpr double MidiBaudRate = 3.125; // bytes per ms
	const auto delay_ms           = (sysex_bytes_num * 1.25) / MidiBaudRate;
	return static_cast<int>(delay_ms) + 2;
}

bool is_midi_data_byte(const uint8_t byte)
{
	return byte <= 0x7f;
}

bool is_midi_status_byte(const uint8_t byte)
{
	return !is_midi_data_byte(byte);
}

uint8_t get_midi_status(const uint8_t status_byte)
{
	return status_byte & 0xf0;
}

MessageType get_midi_message_type(const uint8_t status_byte)
{
	return get_midi_status(status_byte) == MidiStatus::SystemMessage
	             ? MessageType::SysEx
	             : MessageType::Channel;
}

uint8_t get_midi_channel(const uint8_t channel_status)
{
	return channel_status & 0x0f;
}

static bool is_external_midi_device()
{
	return midi.device->GetType() == MidiDevice::Type::External;
}

static void output_note_off_for_active_notes(const uint8_t channel)
{
	assert(channel <= LastMidiChannel);

	constexpr auto NoteOffVelocity = 64;
	constexpr auto NoteOffMsgLen   = 3;

	MidiMessage msg = {};
	msg[0]          = MidiStatus::NoteOff | channel;
	msg[2]          = NoteOffVelocity;

	for (auto note = FirstMidiNote; note <= LastMidiNote; ++note) {
		if (midi_state.IsNoteActive(channel, note)) {
			msg[1] = note;

			if (CAPTURE_IsCapturingMidi()) {
				constexpr auto IsSysEx = false;
				CAPTURE_AddMidiData(IsSysEx,
				                    NoteOffMsgLen,
				                    msg.data.data());
			}
			midi.device->SendMidiMessage(msg);
		}
	}
}

// Many MIDI drivers used in games send the "All Notes Off" Channel Mode
// Message to turn off all active notes when switching between songs, instead
// of properly sending Note Off messages for each individual note as required
// by the MIDI specification (all Note On messages *must* be always paired
// with Note Offs; the "All Notes Off" message must not be used as a shortcut
// for that). E.g. all Sierra drivers exhibit this incorrect behaviour, while
// LucasArts games are doing the correct thing and pair all Note On messages
// with Note Offs.
//
// This hack can lead to "infinite notes" (hanging notes) when recording the
// MIDI output into a MIDI sequencer, or when using DOSBox's raw MIDI output
// capture functionality. What's worse, it can also result in multiple Note On
// messages for the same note on the same channel in the recorded MIDI stream,
// followed by a single Note Off only. While playing back the raw MIDI stream
// is interpreted "correctly" on MIDI modules typically used in the 1990s,
// it's up to the individual MIDI sequencer how to resolve this situation when
// dealing with recorded MIDI data. This can lead lead to missing notes, and
// it makes editing long MIDI recordings containing multiple songs very
// difficult and error-prone.
//
// See page 20, 24, 25 and A-4 of the "The Complete MIDI 1.0 Detailed
// Specification" document version 96.1, third edition (1996, MIDI
// Manufacturers Association) for further details
//
// https://archive.org/details/Complete_MIDI_1.0_Detailed_Specification_96-1-3/
//
static void sanitise_midi_stream(const MidiMessage& msg)
{
	const auto status  = get_midi_status(msg.status());
	const auto channel = get_midi_channel(msg.status());

	if (status == MidiStatus::ControlChange) {
		const auto mode = msg.data1();
		if (mode == MidiChannelMode::AllSoundOff ||
		    mode >= MidiChannelMode::AllNotesOff) {
			// Send Note Offs for the currently active notes prior
			// to sending the "All Notes Off" message, as mandated
			// by the MIDI spec
			output_note_off_for_active_notes(channel);

			for (auto note = FirstMidiNote; note <= LastMidiNote; ++note) {
				midi_state.SetNoteActive(channel, note, false);
			}
		}
	}
}

void MIDI_RawOutByte(const uint8_t data)
{
	if (!MIDI_IsAvailable()) {
		return;
	}

	if (midi.sysex.start_ms) {
		const auto passed_ticks = GetTicksSince(midi.sysex.start_ms);
		if (passed_ticks < midi.sysex.delay_ms) {
			Delay(midi.sysex.delay_ms - passed_ticks);
		}
	}

	const auto is_realtime_message = (data >= MidiStatus::TimingClock);
	if (is_realtime_message) {
		midi.realtime_message[0] = data;
		midi.device->SendMidiMessage(midi.realtime_message);
		return;
	}

	if (midi.status == MidiStatus::SystemExclusive) {
		if (is_midi_data_byte(data)) {
			if (midi.sysex.pos < (MaxMidiSysExBytes - 1)) {
				midi.sysex.buf[midi.sysex.pos++] = data;
			}
			return;
		} else {
			midi.sysex.buf[midi.sysex.pos++] = MidiStatus::EndOfExclusive;

			if (midi.sysex.start_ms && (midi.sysex.pos >= 4) &&
			    (midi.sysex.pos <= 9) && (midi.sysex.buf[1] == 0x41) &&
			    (midi.sysex.buf[3] == 0x16)) {
#ifdef DEBUG_MIDI
				LOG_DEBUG(
				        "MIDI: Skipping invalid MT-32 SysEx midi message "
				        "(too short to contain a checksum)");
#endif
			} else {
#ifdef DEBUG_MIDI
				LOG_TRACE(
				        "MIDI: Playing SysEx message, "
				        "address: %02X %02X %02X, length: %4d, delay: %3d",
				        midi.sysex.buf[5],
				        midi.sysex.buf[6],
				        midi.sysex.buf[7],
				        midi.sysex.pos,
				        midi.sysex.delay_ms);
#endif
				midi.device->SendSysExMessage(midi.sysex.buf,
				                              midi.sysex.pos);

				if (midi.sysex.start_ms) {
					if (midi.sysex.buf[5] == 0x7f) {
						// Reset All Parameters fix
						midi.sysex.delay_ms = 290;

					} else if (midi.sysex.buf[5] == 0x10 &&
					           midi.sysex.buf[6] == 0x00 &&
					           midi.sysex.buf[7] == 0x04) {
						// Viking Child fix
						midi.sysex.delay_ms = 145;

					} else if (midi.sysex.buf[5] == 0x10 &&
					           midi.sysex.buf[6] == 0x00 &&
					           midi.sysex.buf[7] == 0x01) {
						// Dark Sun 1 fix
						midi.sysex.delay_ms = 30;

					} else {
						midi.sysex.delay_ms = delay_in_ms(
						        midi.sysex.pos);
					}
					midi.sysex.start_ms = GetTicks();
				}
			}

			if (CAPTURE_IsCapturingMidi()) {
				constexpr auto IsSysEx = true;
				CAPTURE_AddMidiData(IsSysEx,
				                    midi.sysex.pos - 1,
				                    &midi.sysex.buf[1]);
			}
		}
	}

	if (is_midi_status_byte(data)) {
		// Start of a new MIDI message
		midi.status      = data;
		midi.message.pos = 0;

		// Total length of the MIDI message, including the status byte
		midi.message.len = MIDI_message_len_by_status[midi.status];

		if (midi.status == MidiStatus::SystemExclusive) {
			midi.sysex.buf[0] = MidiStatus::SystemExclusive;
			midi.sysex.pos    = 1;
		}
	}

	if (midi.message.len > 0) {
		midi.message.msg[midi.message.pos++] = data;

		if (midi.message.pos >= midi.message.len) {
			// 1. Update the MIDI state based on the last non-SysEx
			// message.
			midi_state.UpdateState(midi.message.msg);

			// 2. Sanitise the MIDI stream unless raw output is
			// enabled. Currently, this can result in the emission
			// of extra MIDI Note Off events only, and updating the
			// MIDI state.
			//
			// `sanitise_midi_stream` also captures these extra
			// events if MIDI capture is enabled and sends them to
			// the MIDI device. This is a bit hacky and rather
			// limited design, but it does the job for now... A
			// better solution would be a message queue or stream
			// that we could also alter and filter, plus a
			// centralised capture and send function.
			if (!raw_midi_output_enabled) {
				sanitise_midi_stream(midi.message.msg);
			}

			// 3. Determine whether the message should be sent to
			// the device based on the mute state.
			auto play_msg = true;

			if (midi.is_muted && is_external_midi_device()) {
				const auto& msg = midi.message.msg;
				const auto status = get_midi_status(msg.status());

				// Track Channel Volume change messages in
				// MidiState, but don't send them to external
				// devices when muted.
				if (status == MidiStatus::ControlChange &&
				    msg.data1() == MidiController::Volume) {
					play_msg = false;
				}
			}

			// 4. Always capture the original message if MIDI
			// capture is enabled, regardless of the mute state.
			if (CAPTURE_IsCapturingMidi()) {
				constexpr auto IsSysEx = false;
				CAPTURE_AddMidiData(IsSysEx,
				                    midi.message.len,
				                    midi.message.msg.data.data());
			}

			// 5. Send the MIDI message to the device for playback
			if (play_msg) {
				midi.device->SendMidiMessage(midi.message.msg);
			}

			midi.message.pos = 1; // Use Running Status
		}
	}
}

MidiDevice* MIDI_GetCurrentDevice()
{
	return midi.device.get();
}

void MIDI_Reset(MidiDevice* device)
{
	MidiMessage msg = {};

	for (auto channel = FirstMidiChannel; channel <= LastMidiChannel; ++channel) {
		msg[0] = MidiStatus::ControlChange | channel;

		msg[1] = MidiChannelMode::AllNotesOff;
		device->SendMidiMessage(msg);

		msg[1] = MidiChannelMode::ResetAllControllers;
		device->SendMidiMessage(msg);
	}
}

void MIDI_Reset()
{
	if (MIDI_IsAvailable()) {
		MIDI_Reset(midi.device.get());
	}
}

void MIDI_Mute()
{
	if (!MIDI_IsAvailable() || midi.is_muted) {
		return;
	}

	if (is_external_midi_device()) {
		MidiMessage msg = {0, MidiController::Volume, 0};

		for (auto channel = FirstMidiChannel; channel <= LastMidiChannel;
		     ++channel) {

			msg[0] = MidiStatus::ControlChange | channel;
			midi.device->SendMidiMessage(msg);
		}
	}

	midi.is_muted = true;
}

void MIDI_Unmute()
{
	if (!MIDI_IsAvailable() || !midi.is_muted) {
		return;
	}

	if (is_external_midi_device()) {
		MidiMessage msg = {0, MidiController::Volume, 0};

		for (auto channel = FirstMidiChannel; channel <= LastMidiChannel;
		     ++channel) {

			msg[0] = MidiStatus::ControlChange | channel;
			msg[2] = midi_state.GetChannelVolume(channel);

			midi.device->SendMidiMessage(msg);
		}
	}

	midi.is_muted = false;
}

bool MIDI_IsAvailable()
{
	return (midi.device != nullptr);
}

static SectionProp* get_midi_section()
{
	assert(control);

	auto sec = static_cast<SectionProp*>(control->GetSection("midi"));
	assert(sec);

	return sec;
}

static std::string get_mididevice_setting()
{
	return get_midi_section()->GetString("mididevice");
}

static auto MidiDevicePortPref    = "port";
static auto DefaultMidiDevicePref = MidiDevicePortPref;

// We'll adapt the RtMidi library, eventually, so hold off any substantial
// rewrites of the MIDI stuff until then to avoid unnecessary work.
class MIDI final {
public:
	MIDI()
	{
		const auto device_pref = get_mididevice_setting();

		midi = {};

		// Has the user disable MIDI?
		if (const auto device_has_bool = parse_bool_setting(device_pref);
		    device_has_bool && *device_has_bool == false) {
			LOG_MSG("MIDI: MIDI device set to 'none'; disabling MIDI output");
			return;
		}

		const auto section      = get_midi_section();
		raw_midi_output_enabled = section->GetBool("raw_midi_output");

		std::string midiconfig_prefs = section->GetString("midiconfig");

		if (midiconfig_prefs.find("delaysysex") != std::string::npos) {
			midi.sysex.start_ms = GetTicks();
			midiconfig_prefs.erase(midiconfig_prefs.find("delaysysex"));
			LOG_MSG("MIDI: Using delayed SysEx processing");
		}

		trim(midiconfig_prefs);

		if (device_pref == MidiDevicePortPref) {
			// Use system-level MIDI interface of the host OS
#if C_COREMIDI
			// macOS
			midi.device = create_device(MidiDeviceName::CoreMidi,
			                            midiconfig_prefs);
#elif defined(WIN32)
			// Windows
			midi.device = create_device(MidiDeviceName::Win32,
			                            midiconfig_prefs);
#elif C_ALSA
			// Linux
			midi.device = create_device(MidiDeviceName::Alsa,
			                            midiconfig_prefs);
#endif
		} else {
			midi.device = create_device(device_pref, midiconfig_prefs);
		}

		if (midi.device) {
			LOG_MSG("MIDI: Opened device '%s'",
			        midi.device->GetName().c_str());
		}
	}
};

void MIDI_ListDevices(Program* caller)
{
	[[maybe_unused]] auto write_device_name = [&](const std::string& device_name) {
		const auto color = convert_ansi_markup("[color=white]%s:[reset]\n");

		caller->WriteOut(color.c_str(), device_name.c_str());
	};

	[[maybe_unused]] auto device_ptr = midi.device.get();

	const std::string device_name = midi.device ? midi.device->GetName() : "";
#if C_MT32EMU
	write_device_name(MidiDeviceName::Mt32);

	MT32_ListDevices((device_name == MidiDeviceName::Mt32)
	                         ? dynamic_cast<MidiDeviceMt32*>(device_ptr)
	                         : nullptr,
	                 caller);
#endif

	write_device_name(MidiDeviceName::SoundCanvas);

	SOUNDCANVAS_ListDevices((device_name == MidiDeviceName::SoundCanvas)
	                                ? dynamic_cast<MidiDeviceSoundCanvas*>(device_ptr)
	                                : nullptr,
	                        caller);

	write_device_name(MidiDeviceName::FluidSynth);

	FSYNTH_ListDevices((device_name == MidiDeviceName::FluidSynth)
	                           ? dynamic_cast<MidiDeviceFluidSynth*>(device_ptr)
	                           : nullptr,

	                   caller);
#if C_COREMIDI
	write_device_name(MidiDeviceName::CoreMidi);

	COREMIDI_ListDevices((device_name == MidiDeviceName::CoreMidi)
	                             ? dynamic_cast<MidiDeviceCoreMidi*>(device_ptr)
	                             : nullptr,
	                     caller);
#endif
#if C_COREAUDIO
	write_device_name(MidiDeviceName::CoreAudio);

	COREAUDIO_ListDevices((device_name == MidiDeviceName::CoreAudio)
	                              ? dynamic_cast<MidiDeviceCoreAudio*>(device_ptr)
	                              : nullptr,
	                      caller);
#endif
#if defined(WIN32)
	write_device_name(MidiDeviceName::Win32);

	MIDI_WIN32_ListDevices((device_name == MidiDeviceName::Win32)
	                               ? dynamic_cast<MidiDeviceWin32*>(device_ptr)
	                               : nullptr,
	                       caller);
#endif
#if C_ALSA
	write_device_name(MidiDeviceName::Alsa);

	ALSA_ListDevices((device_name == MidiDeviceName::Alsa)
	                         ? dynamic_cast<MidiDeviceAlsa*>(device_ptr)
	                         : nullptr,
	                 caller);
#endif
}

static std::unique_ptr<MIDI> midi_instance = nullptr;

static void midi_destroy([[maybe_unused]] Section* sec)
{
	midi.device.reset();
}

static void midi_init([[maybe_unused]] Section* sec)
{
	// This works because the registered destroy functions are always only
	// called once (they're deleted or "consumed" after they were invoked,
	// so you need to re-register them if you re-init the module).
	constexpr auto ChangeableAtRuntime = true;

	assert(sec);
	sec->AddDestroyFunction(&midi_destroy, ChangeableAtRuntime);

	// Retry loop
	for (;;) {
		MPU401_Destroy();
		MPU401_Init();

		midi_instance.reset();

		try {
			midi_instance = std::make_unique<MIDI>();
			midi_state.Reset();

			// A MIDI device has been successfully initialised
			return;

		} catch (const std::runtime_error& ex) {
			const auto mididevice_pref = get_mididevice_setting();
			if (mididevice_pref == MidiDevicePortPref) {
				LOG_WARNING(
				        "MIDI: Error opening device '%s'; "
				        "using 'mididevice = none' and disabling MIDI output",
				        mididevice_pref.c_str());

				set_section_property_value("midi", "mididevice", "none");

				// 'mididevice = auto' didn't work out; we
				// disable the MIDI output and bail out.
				return;

			} else {
				// If 'mididevice' was set to a concrete value
				// and the device could not be initialiased,
				// we'll try 'port' as a fallback.
				LOG_WARNING("MIDI: Error opening device '%s'; using '%s'",
				            mididevice_pref.c_str(),
				            DefaultMidiDevicePref);

				set_section_property_value("midi",
				                           "mididevice",
				                           MidiDevicePortPref);
			}
		}
	}
}

void MIDI_Init()
{
	midi_init(get_midi_section());
}

static void init_mididevice_settings(SectionProp& secprop)
{
	constexpr auto WhenIdle = Property::Changeable::WhenIdle;

	auto str_prop = secprop.AddString("mididevice", WhenIdle, DefaultMidiDevicePref);
	str_prop->SetHelp(
	        format_str("Set where MIDI data from the emulated MPU-401 MIDI interface is sent\n"
	                   "('%s' by default):",
	                   DefaultMidiDevicePref));

	str_prop->SetOptionHelp(DefaultMidiDevicePref,
	                        "  port:         A MIDI port of the host operating system's MIDI interface\n"
	                        "                (default). You can configure the port to use with the\n"
	                        "                'midiconfig' setting.");

	str_prop->SetOptionHelp(
	        MidiDeviceName::CoreAudio,
	        "  coreaudio:    The built-in macOS MIDI synthesiser. The SoundFont to use can\n"
	        "                be specified with the 'midiconfig' setting.");

	str_prop->SetOptionHelp(MidiDeviceName::Mt32,
	                        "  mt32:         The internal Roland MT-32 synthesizer (see the [mt32] section).");

	str_prop->SetOptionHelp(MidiDeviceName::SoundCanvas,
	                        "  soundcanvas:  The internal Roland SC-55 synthesiser (requires a CLAP audio\n"
	                        "                plugin that implements the Sound Canvas to be available;\n"
	                        "                see the [soundcanvas] section).");

	str_prop->SetOptionHelp(MidiDeviceName::FluidSynth,
	                        "  fluidsynth:   The internal FluidSynth MIDI synthesizer (SoundFont player)\n"
	                        "                (requires the FluidSynth dynamic-link library to be available;\n"
	                        "                see the [fluidsynth] section).");

	str_prop->SetOptionHelp("none", "  none:         Disable MIDI output.");

	str_prop->SetValues({
		MidiDevicePortPref,
#if C_COREAUDIO
		        MidiDeviceName::CoreAudio,
#endif
		        MidiDeviceName::Mt32, MidiDeviceName::SoundCanvas,
		        MidiDeviceName::FluidSynth,
		        "none"
	});

	str_prop->SetDeprecatedWithAlternateValue("alsa", DefaultMidiDevicePref);
	str_prop->SetDeprecatedWithAlternateValue("auto", DefaultMidiDevicePref);
	str_prop->SetDeprecatedWithAlternateValue("coremidi", DefaultMidiDevicePref);
	str_prop->SetDeprecatedWithAlternateValue("oss", DefaultMidiDevicePref);
	str_prop->SetDeprecatedWithAlternateValue("win32", DefaultMidiDevicePref);
}

static void init_midiconfig_settings(SectionProp& secprop)
{
	constexpr auto WhenIdle = Property::Changeable::WhenIdle;

	auto str_prop = secprop.AddString("midiconfig", WhenIdle, "");
	str_prop->SetHelp(
	        "Configuration options for the selected MIDI device (unset by default).\n"
	        "Notes:");

	str_prop->SetOptionHelp(
	        "windows_or_macos",
	        "  - When using 'mididevice = port', find the ID or name of the MIDI port you\n"
	        "    want to use with the DOS command 'MIXER /LISTMIDI', then set either the ID\n"
	        "    or a substring of the name (e.g., to use the port called \"loopMIDI Port A\"\n"
	        "    with ID 2, set 'midiconfig = 2' or 'midiconfig = port a').");

	str_prop->SetOptionHelp(
	        "coreaudio",
	        "  - When using 'mididevice = coreaudio', this setting specifies the SoundFont\n"
	        "    to use. You must use the absolute path of the SoundFont file.");

	str_prop->SetOptionHelp("linux",
	                        "  - When using 'mididevice = port', use the Linux command 'aconnect -l' to list\n"
	                        "    all open MIDI ports and select one (e.g., 'midiconfig = 14:0' for sequencer\n"
	                        "    client 14, port 0).");

	str_prop->SetOptionHelp(
	        "internal_synth",
	        "  - The setting has no effect when using the internal synthesizers\n"
	        "    ('mididevice = fluidsynth', 'mt32', or 'soundcanvas').");

	str_prop->SetOptionHelp(
	        "physical_mt32",
	        "  - If you're using a physical rev.0 Roland MT-32, the hardware may require a\n"
	        "    delay to prevent buffer overflows. You can enable this with 'delaysysex'\n"
	        "    after the port ID or name (e.g., 'midiconfig = 2 delaysysex').");

	str_prop->SetEnabledOptions({
#if (C_COREMIDI == 1 || defined(WIN32))
		"windows_or_macos",
#endif
#if C_COREAUDIO
		        "coreaudio",
#endif
#if C_ALSA
		        "linux",
#endif
		        "internal_synth", "physical_mt32"
	});
}

void init_midi_dosbox_settings(SectionProp& secprop)
{
	init_mididevice_settings(secprop);
	init_midiconfig_settings(secprop);

	constexpr auto WhenIdle = Property::Changeable::WhenIdle;

	auto str_prop = secprop.AddString("mpu401", WhenIdle, "intelligent");
	str_prop->SetValues({"intelligent", "uart", "none"});
	str_prop->SetHelp("MPU-401 mode to emulate ('intelligent' by default).");

	auto bool_prop = secprop.AddBool("raw_midi_output", WhenIdle, false);
	bool_prop->SetHelp(
	        "Enable raw, unaltered MIDI output ('off' by default).\n"
	        "The MIDI drivers of many games don't fully conform to the MIDI standard,\n"
	        "which makes editing the MIDI recordings of these games very error-prone and\n"
	        "cumbersome in MIDI sequencers, often resulting in hanging or missing notes.\n"
	        "DOSBox corrects the MIDI output of such games by default. This results in no\n"
	        "audible difference whatsoever; it only affects the representation of the MIDI\n"
	        "data. You should only enable 'raw_midi_output' if you really need to capture\n"
	        "the raw, unaltered MIDI output of a program, e.g. when working with music\n"
	        "applications, or when debugging MIDI issues.");
}

static void register_midi_text_messages()
{
	MSG_Add("MIDI_DEVICE_LIST_NOT_SUPPORTED", "Listing not supported");
	MSG_Add("MIDI_DEVICE_NOT_CONFIGURED", "Device not configured");
	MSG_Add("MIDI_DEVICE_NO_PORTS", "No available ports");
	MSG_Add("MIDI_DEVICE_NO_MODEL_ACTIVE", "No model is currently active");
	MSG_Add("MIDI_DEVICE_NO_MODELS", "No available models");
}

void MIDI_AddConfigSection(const ConfigPtr& conf)
{
	assert(conf);

	constexpr auto ChangeableAtRuntime = true;

	SectionProp* sec = conf->AddSectionProp("midi", &midi_init, ChangeableAtRuntime);
	assert(sec);

	init_midi_dosbox_settings(*sec);

	register_midi_text_messages();
}
