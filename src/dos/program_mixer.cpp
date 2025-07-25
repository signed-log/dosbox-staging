// SPDX-FileCopyrightText:  2023-2025 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "program_mixer.h"

#include <cctype>
#include <optional>

#include "ansi_code_markup.h"
#include "audio_frame.h"
#include "channel_names.h"
#include "checks.h"
#include "math_utils.h"
#include "midi.h"
#include "notifications.h"
#include "program_more_output.h"
#include "string_utils.h"

CHECK_NARROWING();

static bool is_global_channel(const std::string& channel_name)
{
	return channel_name == GlobalVirtualChannelName;
}

static bool is_master_channel(const std::string& channel_name)
{
	return channel_name == ChannelName::Master;
}

struct {
	bool fm_message_shown   = false;
	bool spkr_message_shown = false;
} deprecation_warnings = {};

static std::string map_deprecated_channel_name(const std::string& channel_name)
{
	if (channel_name == ChannelName::PcSpeakerDeprecated) {

		if (!deprecation_warnings.spkr_message_shown) {
			NOTIFY_DisplayWarning(Notification::Source::Console,
			                      "MIXER",
			                      "SHELL_CMD_MIXER_CHANNEL_DEPRECATED",
			                      ChannelName::PcSpeakerDeprecated,
			                      ChannelName::PcSpeaker);

			deprecation_warnings.spkr_message_shown = true;
		}
		return ChannelName::PcSpeaker;

	} else if (channel_name == ChannelName::OplDeprecated) {

		if (!deprecation_warnings.fm_message_shown) {
			NOTIFY_DisplayWarning(Notification::Source::Console,
			                      "MIXER",
			                      "SHELL_CMD_MIXER_CHANNEL_DEPRECATED",
			                      ChannelName::OplDeprecated,
			                      ChannelName::Opl);
			deprecation_warnings.fm_message_shown = true;
		}
		return ChannelName::Opl;
	}

	return channel_name;
}

namespace MixerCommand {

bool SelectChannel::operator==(const SelectChannel that) const
{
	return channel_name == that.channel_name;
}

bool SetVolume::operator==(const SetVolume that) const
{
	return volume_as_gain == that.volume_as_gain;
}

bool SetStereoMode::operator==(const SetStereoMode that) const
{
	return lineout_map == that.lineout_map;
}

bool SetCrossfeedStrength::operator==(const SetCrossfeedStrength that) const
{
	return strength == that.strength;
}

bool SetReverbLevel::operator==(const SetReverbLevel that) const
{
	return level == that.level;
}

bool SetChorusLevel::operator==(const SetChorusLevel that) const
{
	return level == that.level;
}

void Executor::operator()(const SelectChannel cmd)
{
	global_command = false;
	master_channel = false;
	channel        = nullptr;

	if (cmd.channel_name == GlobalVirtualChannelName) {
		global_command = true;
	} else if (cmd.channel_name == ChannelName::Master) {
		master_channel = true;
	} else {
		channel = MIXER_FindChannel(cmd.channel_name.c_str());
		assert(channel);
	}
}

void Executor::operator()(const SetVolume cmd)
{
	if (master_channel) {
		MIXER_SetMasterVolume(cmd.volume_as_gain);
	} else {
		assert(channel);
		channel->SetUserVolume(cmd.volume_as_gain);
	}
}

void Executor::operator()(const SetStereoMode cmd)
{
	assert(channel);
	channel->SetLineoutMap(cmd.lineout_map);
}

void Executor::operator()(const SetCrossfeedStrength cmd)
{
	// Enable crossfeed if it's disabled
	if (MIXER_GetCrossfeedPreset() == CrossfeedPreset::None) {
		MIXER_SetCrossfeedPreset(DefaultCrossfeedPreset);
	}

	if (global_command) {
		for (const auto& [_, ch] : MIXER_GetChannels()) {
			ch->SetCrossfeedStrength(cmd.strength);
		}
	} else {
		assert(channel);
		channel->SetCrossfeedStrength(cmd.strength);
	}
}

void Executor::operator()(const SetReverbLevel cmd)
{
	// Enable reverb if it's disabled
	if (MIXER_GetReverbPreset() == ReverbPreset::None) {
		MIXER_SetReverbPreset(DefaultReverbPreset);
	}

	if (global_command) {
		for (const auto& [_, ch] : MIXER_GetChannels()) {
			ch->SetReverbLevel(cmd.level);
		}
	} else {
		assert(channel);
		channel->SetReverbLevel(cmd.level);
	}
}

void Executor::operator()(const SetChorusLevel cmd)
{
	// Enable chorus if it's disabled
	if (MIXER_GetChorusPreset() == ChorusPreset::None) {
		MIXER_SetChorusPreset(DefaultChorusPreset);
	}

	if (global_command) {
		for (const auto& [_, ch] : MIXER_GetChannels()) {
			ch->SetChorusLevel(cmd.level);
		}
	} else {
		assert(channel);
		channel->SetChorusLevel(cmd.level);
	}
}

static std::optional<float> parse_percentage(const std::string& s,
                                             const float min_percent = 0.0f,
                                             const float max_percent = 100.0f)
{
	if (const auto p = parse_float(s); p) {
		if (*p >= min_percent && *p <= max_percent) {
			return percentage_to_gain(*p);
		}
	}
	return {};
}

static bool is_start_of_number(const char c)
{
	return (c == '-' || c == '+' || std::isdigit(c));
}

constexpr auto CrossfeedCommandPrefix = 'X';
constexpr auto ReverbCommandPrefix    = 'R';
constexpr auto ChorusCommandPrefix    = 'C';

constexpr auto DecibelVolumeCommandPrefix = 'D';

static bool is_volume_command(const std::string& s)
{
	if (s.size() < 1) {
		return false;
	}
	auto is_percent_volume_command = [&]() {
		return is_start_of_number(s[0]);
	};
	auto is_decibel_volume_command = [&]() {
		return (s[0] == DecibelVolumeCommandPrefix);
	};
	return is_percent_volume_command() || is_decibel_volume_command();
}

template <typename... Args>
void notify_warning(const std::string& message_name, const Args&... args) noexcept
{
	NOTIFY_DisplayWarning(Notification::Source::Console,
	                      "MIXER",
	                      message_name,
	                      args...);
}

static std::variant<ErrorType, Command> parse_volume_command(const std::string& s,
                                                             const std::string& channel_name)
{
	if (is_global_channel(channel_name)) {
		notify_warning("SHELL_CMD_MIXER_INVALID_GLOBAL_COMMAND", s.c_str());

		return ErrorType::InvalidGlobalCommand;
	}

	constexpr auto MinDb = -96.00f;
	constexpr auto MaxDb = 40.000f;

	static const auto MinPercent = decibel_to_gain(MinDb);

	// Almost 40 dB, just a *tiny* bit below to ensure that the mixer
	// columns don't get too wide.
	constexpr auto MaxPercent = 9999.0f;

	static const auto MinGain = MinPercent / 100.0f;
	static const auto MaxGain = MaxPercent / 100.0f;

	auto parse_percent_volume = [&](const std::string& s) -> std::optional<float> {
		// Allow setting the volume to absolute silence (-inf dB) when
		// specifying percentage volumes
		if (const auto p = parse_percentage(s, 0.0f, MaxPercent); p) {
			return *p;
		}
		return {};
	};

	auto parse_decibel_volume = [&](const std::string& s) -> std::optional<float> {
		if (s[0] != DecibelVolumeCommandPrefix) {
			return {};
		};
		if (const auto d = parse_float(s.substr(1));
		    d && (*d >= MinDb && *d <= MaxDb)) {
			return std::clamp(decibel_to_gain(*d), MinGain, MaxGain);
		}
		return {};
	};

	auto parse_volume = [&](const std::string& s) -> std::optional<float> {
		if (s.size() < 1) {
			return {};
		}
		auto v = parse_percent_volume(s);
		if (!v) {
			v = parse_decibel_volume(s);
		}
		if (!v) {
			return {};
		}

		// Allow setting the volume to absolute silence (-inf dB) if a
		// percentage volume of '0' was specified...
		if (*v == 0.0f) {
			return *v;
		}
		// ...but clamp to the [-96dB, 40dB] range otherwise (40dB would
		// be a 10000 percentage value, but we clamp to 9999 instead
		// because the tabular mixer output looks better that way).
		return std::clamp(*v, MinPercent, MaxPercent);
	};

	auto parts = split_with_empties(s, ':');

	if (parts.size() == 1) {
		// Single volume value for both channels (e.g. 10)
		if (const auto v = parse_volume(parts[0]); v) {
			const SetVolume cmd = {AudioFrame(*v, *v)};
			return cmd;
		} else {
			notify_warning("SHELL_CMD_MIXER_INVALID_VOLUME_COMMAND",
			               channel_name.c_str(),
			               s.c_str());

			return ErrorType::InvalidVolumeCommand;
		}

	} else if (parts.size() == 2) {
		// Colon-separated stereo volume value (e.g. 10:20)
		const auto l = parse_volume(parts[0]);
		const auto r = parse_volume(parts[1]);
		if (l && r) {
			const SetVolume cmd = {AudioFrame(*l, *r)};
			return cmd;
		} else {
			notify_warning("SHELL_CMD_MIXER_INVALID_VOLUME_COMMAND",
			               channel_name.c_str(),
			               s.c_str());

			return ErrorType::InvalidVolumeCommand;
		}
	} else { // more than 2 parts
		notify_warning("SHELL_CMD_MIXER_INVALID_VOLUME_COMMAND",
		               channel_name.c_str(),
		               s.c_str());

		return ErrorType::InvalidVolumeCommand;
	}
}

static std::optional<StereoLine> parse_stereo_mode(const std::string& s)
{
	if (s == "STEREO") {
		return StereoMap;
	}
	if (s == "REVERSE") {
		return ReverseMap;
	}
	return {};
}

static bool is_command_with_prefix(const std::string& s, const char prefix)
{
	if (s.size() < 1) {
		return false;
	}
	const auto command_prefix = s[0];
	if (s.size() == 1) {
		return (command_prefix == prefix);
	} else {
		return (command_prefix == prefix && is_start_of_number(s[1]));
	}
}

static ErrorType make_invalid_master_channel_command_error(const std::string& command)
{
	notify_warning("SHELL_CMD_MIXER_INVALID_CHANNEL_COMMAND",
	               ChannelName::Master,
	               command.c_str());

	return ErrorType::InvalidMasterChannelCommand;
}

static std::variant<ErrorType, Command> parse_crossfeed_command(
        const std::string& s, const std::string& channel_name,
        const ChannelInfos& channel_infos)
{
	assert(s.size() >= 1);

	const auto is_channel_mono = !channel_infos.HasFeature(channel_name,
	                                                       ChannelFeature::Stereo);
	if (is_channel_mono) {
		notify_warning("SHELL_CMD_MIXER_INVALID_CHANNEL_COMMAND",
		               channel_name.c_str(),
		               s.c_str());

		return ErrorType::InvalidChannelCommand;
	}

	if (is_master_channel(channel_name)) {
		return make_invalid_master_channel_command_error(s);
	}

	if (s.size() == 1) {
		if (is_global_channel(channel_name)) {
			notify_warning("SHELL_CMD_MIXER_MISSING_GLOBAL_CROSSFEED_STRENGTH");
		} else {
			notify_warning("SHELL_CMD_MIXER_MISSING_CROSSFEED_STRENGTH",
			               channel_name.c_str());
		}

		return ErrorType::MissingCrossfeedStrength;
	}

	if (const auto strength = parse_percentage(s.substr(1)); strength) {
		const SetCrossfeedStrength cmd = {*strength};
		return cmd;
	} else {
		if (is_global_channel(channel_name)) {
			notify_warning("SHELL_CMD_MIXER_INVALID_GLOBAL_CROSSFEED_STRENGTH",
			               s.c_str());

			return ErrorType::InvalidGlobalCrossfeedStrength;
		} else {
			notify_warning("SHELL_CMD_MIXER_INVALID_CROSSFEED_STRENGTH",
			               channel_name.c_str(),
			               s.c_str());

			return ErrorType::InvalidCrossfeedStrength;
		}
	}
}

static std::variant<ErrorType, Command> parse_reverb_command(
        const std::string& s, const std::string& channel_name,
        const ChannelInfos& channel_infos)
{
	assert(s.size() >= 1);

	if (is_master_channel(channel_name)) {
		return make_invalid_master_channel_command_error(s);
	}

	if (!channel_infos.HasFeature(channel_name, ChannelFeature::ReverbSend)) {
		notify_warning("SHELL_CMD_MIXER_INVALID_CHANNEL_COMMAND",
		               channel_name.c_str(),
		               s.c_str());

		return ErrorType::InvalidChannelCommand;
	}

	if (s.size() == 1) {
		if (is_global_channel(channel_name)) {
			notify_warning("SHELL_CMD_MIXER_MISSING_GLOBAL_REVERB_LEVEL");
		} else {
			notify_warning("SHELL_CMD_MIXER_MISSING_REVERB_LEVEL",
			               channel_name.c_str());
		}

		return ErrorType::MissingReverbLevel;
	}

	if (const auto level = parse_percentage(s.substr(1)); level) {
		const SetReverbLevel cmd = {*level};
		return cmd;
	} else {
		if (is_global_channel(channel_name)) {
			notify_warning("SHELL_CMD_MIXER_INVALID_GLOBAL_REVERB_LEVEL",
			               s.c_str());

			return ErrorType::InvalidGlobalReverbLevel;
		} else {
			notify_warning("SHELL_CMD_MIXER_INVALID_REVERB_LEVEL",
			               channel_name.c_str(),
			               s.c_str());

			return ErrorType::InvalidReverbLevel;
		}
	}
}

static std::variant<ErrorType, Command> parse_chorus_command(
        const std::string& s, const std::string& channel_name,
        const ChannelInfos& channel_infos)
{
	assert(s.size() >= 1);

	if (is_master_channel(channel_name)) {
		return make_invalid_master_channel_command_error(s);
	}

	if (!channel_infos.HasFeature(channel_name, ChannelFeature::ChorusSend)) {
		notify_warning("SHELL_CMD_MIXER_INVALID_CHANNEL_COMMAND",
		               channel_name.c_str(),
		               s.c_str());

		return ErrorType::InvalidChannelCommand;
	}

	if (s.size() == 1) {
		if (is_global_channel(channel_name)) {
			notify_warning("SHELL_CMD_MIXER_MISSING_GLOBAL_CHORUS_LEVEL");
		} else {
			notify_warning("SHELL_CMD_MIXER_MISSING_CHORUS_LEVEL",
			               channel_name.c_str());
		}

		return ErrorType::MissingChorusLevel;
	}

	if (const auto level = parse_percentage(s.substr(1)); level) {
		const SetChorusLevel cmd = {*level};
		return cmd;
	} else {
		if (is_global_channel(channel_name)) {
			notify_warning("SHELL_CMD_MIXER_INVALID_GLOBAL_CHORUS_LEVEL",
			               s.c_str());

			return ErrorType::InvalidGlobalChorusLevel;
		} else {
			notify_warning("SHELL_CMD_MIXER_INVALID_CHORUS_LEVEL",
			               channel_name.c_str(),
			               s.c_str());

			return ErrorType::InvalidChorusLevel;
		}
	}
}

std::variant<ErrorType, std::queue<Command>> ParseCommands(
        const std::vector<std::string>& args, const ChannelInfos& channel_infos,
        const std::vector<std::string>& all_channel_names)
{
	std::string curr_channel_name   = GlobalVirtualChannelName;
	auto curr_channel_command_count = 0;

	std::queue<Command> commands = {};

	// We always implicitly select the "global virtual channel" at the start
	const SelectChannel select_global_chan_cmd = {GlobalVirtualChannelName};
	commands.emplace(select_global_chan_cmd);

	auto parse_select_channel_command =
	        [&](const std::string& _channel_name) -> std::optional<SelectChannel> {
		const auto channel_name = map_deprecated_channel_name(_channel_name);

		if (channel_infos.HasChannel(channel_name)) {
			const SelectChannel cmd = {channel_name};
			return cmd;
		}
		return {};
	};

	auto is_valid_channel_name = [&](const std::string& _channel_name) {
		const auto channel_name = map_deprecated_channel_name(_channel_name);

		for (const auto& name : all_channel_names) {
			if (name == channel_name) {
				return true;
			}
		}
		return false;
	};

	for (const auto& argument : args) {
		auto arg = argument;
		upcase(arg);

		// The order of checking for the various error conditions *does*
		// matter. If the order is altered, some error messages will
		// become slightly less meaningful and things may break in some
		// edge cases. These cases are covered in the unit tests.

		if (!channel_infos.HasChannel(arg) && is_valid_channel_name(arg)) {
			// Argument is a valid channel name, but the channel is
			// inactive.
			notify_warning("SHELL_CMD_MIXER_INACTIVE_CHANNEL",
			               arg.c_str());

			return ErrorType::InactiveChannel;

		} else if (const auto command = parse_select_channel_command(arg);
		           command) {
			// First try to find the channel in the list of
			// channel infos which is generated from the
			// currently active channels.

			if (!is_global_channel(curr_channel_name) &&
			    curr_channel_command_count == 0) {

				notify_warning("SHELL_CMD_MIXER_MISSING_CHANNEL_COMMAND",
				               curr_channel_name.c_str());

				return ErrorType::MissingChannelCommand;
			}

			curr_channel_name = command->channel_name;
			commands.emplace(*command);
			curr_channel_command_count = 0;

		} else if (is_volume_command(arg)) {
			// Set volume command

			const auto result = parse_volume_command(arg, curr_channel_name);

			if (auto cmd = std::get_if<Command>(&result); cmd) {
				commands.emplace(*cmd);
				++curr_channel_command_count;
			} else {
				return std::get<ErrorType>(result);
			}

		} else if (const auto mode = parse_stereo_mode(arg); mode) {
			// Set stereo mode command

			if (is_global_channel(curr_channel_name)) {
				notify_warning("SHELL_CMD_MIXER_INVALID_GLOBAL_COMMAND",
				               arg.c_str());

				return ErrorType::InvalidGlobalCommand;
			}

			const auto is_channel_mono = !channel_infos.HasFeature(
			        curr_channel_name, ChannelFeature::Stereo);

			if (is_master_channel(curr_channel_name) || is_channel_mono) {
				notify_warning("SHELL_CMD_MIXER_INVALID_CHANNEL_COMMAND",
				               curr_channel_name.c_str(),
				               arg.c_str());

				return ErrorType::InvalidChannelCommand;
			}

			const SetStereoMode cmd = {*mode};
			commands.emplace(cmd);
			++curr_channel_command_count;

		} else if (is_command_with_prefix(arg, CrossfeedCommandPrefix)) {
			// Set crossfeed strength command

			const auto result = parse_crossfeed_command(arg,
			                                            curr_channel_name,
			                                            channel_infos);

			if (auto cmd = std::get_if<Command>(&result); cmd) {
				commands.emplace(*cmd);
				++curr_channel_command_count;
			} else {
				return std::get<ErrorType>(result);
			}

		} else if (is_command_with_prefix(arg, ReverbCommandPrefix)) {
			// Set reverb level command

			const auto result = parse_reverb_command(arg,
			                                         curr_channel_name,
			                                         channel_infos);

			if (auto cmd = std::get_if<Command>(&result); cmd) {
				commands.emplace(*cmd);
				++curr_channel_command_count;
			} else {
				return std::get<ErrorType>(result);
			}

		} else if (is_command_with_prefix(arg, ChorusCommandPrefix)) {
			// Set chorus level command

			const auto result = parse_chorus_command(arg,
			                                         curr_channel_name,
			                                         channel_infos);

			if (auto cmd = std::get_if<Command>(&result); cmd) {
				commands.emplace(*cmd);
				++curr_channel_command_count;
			} else {
				return std::get<ErrorType>(result);
			}

		} else {
			// Unknown command

			if (is_global_channel(curr_channel_name)) {
				notify_warning("SHELL_CMD_MIXER_INVALID_GLOBAL_COMMAND",
				               arg.c_str());

				return ErrorType::InvalidGlobalCommand;
			} else {
				const auto error_type =
				        (is_master_channel(curr_channel_name)
				                 ? ErrorType::InvalidMasterChannelCommand
				                 : ErrorType::InvalidChannelCommand);

				notify_warning("SHELL_CMD_MIXER_INVALID_CHANNEL_COMMAND",
				               curr_channel_name.c_str(),
				               arg.c_str());

				return error_type;
			}
		}
	}

	if (curr_channel_command_count == 0) {
		notify_warning("SHELL_CMD_MIXER_MISSING_CHANNEL_COMMAND",
		               curr_channel_name.c_str());

		return ErrorType::MissingChannelCommand;
	}

	return commands;
}

void ExecuteCommands(Executor& executor, std::queue<Command>& commands)
{
	while (!commands.empty()) {
		std::visit(executor, commands.front());
		commands.pop();
	}
}

} // namespace MixerCommand

static ChannelInfos create_channel_infos()
{
	ChannelInfosMap infos = {};

	for (const auto& [name, ch] : MIXER_GetChannels()) {
		infos[name] = ch->GetFeatures();
	}

	return ChannelInfos(infos);
}

ChannelInfos::ChannelInfos(const ChannelInfosMap& channel_infos)
{
	features_by_channel_name[GlobalVirtualChannelName] = {
	        ChannelFeature::Stereo,
	        ChannelFeature::ReverbSend,
	        ChannelFeature::ChorusSend};

	features_by_channel_name[ChannelName::Master] = {ChannelFeature::Stereo};

	features_by_channel_name.insert(channel_infos.begin(),
	                                channel_infos.end()); //-V837
}

bool ChannelInfos::HasChannel(const std::string& _channel_name) const
{
	const auto channel_name = map_deprecated_channel_name(_channel_name);

	return features_by_channel_name.contains(channel_name);
}

bool ChannelInfos::HasFeature(const std::string& channel_name,
                              const ChannelFeature feature) const
{
	if (auto it = features_by_channel_name.find(channel_name);
	    it != features_by_channel_name.end()) {
		const auto [_, features] = *it;
		return features.contains(feature);
	}
	return false;
}

void MIXER::Run()
{
	if (HelpRequested()) {
		MoreOutputStrings output(*this);
		output.AddString(MSG_Get("SHELL_CMD_MIXER_HELP_LONG"));
		output.Display();
		return;
	}
	if (cmd->FindExist("/LISTMIDI")) {
		MIDI_ListDevices(this);
		return;
	}

	constexpr auto remove = true;

	auto show_status = !cmd->FindExist("/NOSHOW", remove);

	if (cmd->GetCount() == 0) {
		if (show_status) {
			ShowMixerStatus();
		}
		return;
	}

	const auto args = cmd->GetArguments();

	deprecation_warnings = {};

	auto result = MixerCommand::ParseCommands(args,
	                                          create_channel_infos(),
	                                          AllChannelNames);

	if (auto commands = std::get_if<std::queue<MixerCommand::Command>>(&result);
	    commands) {
		// Success (all mixer commands executed successfully)
		MixerCommand::Executor executor = {};
		MixerCommand::ExecuteCommands(executor, *commands);

		if (show_status) {
			ShowMixerStatus();
		}
	}
}

void MIXER::AddMessages()
{
	MSG_Add("SHELL_CMD_MIXER_HELP_LONG",
	        "Display or change the sound mixer settings.\n"
	        "\n"
	        "Usage:\n"
	        "  [color=light-green]mixer[reset] [color=light-cyan][CHANNEL][reset] [color=white]COMMANDS[reset] [/noshow]\n"
	        "  [color=light-green]mixer[reset] [/listmidi]\n"
	        "\n"
	        "Parameters:\n"
	        "  [color=light-cyan]CHANNEL[reset]   mixer channel to change the settings of\n"
	        "  [color=white]COMMANDS[reset]  one or more of the following commands:\n"
	        "    Volume:      Percentage volume of [color=white]0[reset] to [color=white]9999[reset], or decibel volume prefixed\n"
	        "                 with [color=white]d[reset] (e.g. [color=white]d-7.5[reset]). Use [color=white]L:R[reset] to set the left and right\n"
	        "                 volumes of stereo channels separately (e.g. [color=white]10:20[reset], [color=white]150:d6[reset]).\n"
	        "    Stereo mode: [color=white]stereo[reset], or [color=white]reverse[reset] (stereo channels only).\n"
	        "    Crossfeed:   [color=white]x0[reset] to [color=white]x100[reset], set crossfeed strength (stereo channels only).\n"
	        "    Reverb:      [color=white]r0[reset] to [color=white]r100[reset], set reverb level.\n"
	        "    Chorus:      [color=white]c0[reset] to [color=white]c100[reset], set chorus level.\n"
	        "\n"
	        "Notes:\n"
	        "  - Run [color=light-green]mixer[reset] without arguments to view the current settings.\n"
	        "  - Run [color=light-green]mixer[reset] /listmidi to list all available MIDI devices.\n"
	        "  - You may change the settings of more than one channel in a single command.\n"
	        "  - If no channel is specified, you can set crossfeed, reverb, or chorus\n"
	        "    of all channels globally.\n"
	        "  - The /noshow option applies the changes without showing the mixer settings.\n"
	        "\n"
	        "Examples:\n"
	        "  [color=light-green]mixer[reset] [color=light-cyan]cdaudio[reset] [color=white]50[reset] [color=light-cyan]sb[reset] [color=white]reverse[reset] /noshow\n"
	        "  [color=light-green]mixer[reset] [color=white]x30[reset] [color=light-cyan]master[reset] [color=white]40[reset] [color=light-cyan]opl[reset] [color=white]150 r50 c30[reset] [color=light-cyan]sb[reset] [color=white]x10[reset]");

	MSG_Add("SHELL_CMD_MIXER_HEADER_LAYOUT",
	        "%-22s %4.0f:%-4.0f %+6.2f:%-+6.2f  %-8s %5s %7s %7s");

	MSG_Add("SHELL_CMD_MIXER_HEADER_LABELS",
	        "[color=white]Channel      Volume    Volume (dB)   Mode     Xfeed  Reverb  Chorus[reset]");

	MSG_Add("SHELL_CMD_MIXER_CHANNEL_OFF", "off");
	MSG_Add("SHELL_CMD_MIXER_CHANNEL_STEREO", "Stereo");
	MSG_Add("SHELL_CMD_MIXER_CHANNEL_REVERSE", "Reverse");
	MSG_Add("SHELL_CMD_MIXER_CHANNEL_MONO", "Mono");

	MSG_Add("SHELL_CMD_MIXER_INACTIVE_CHANNEL",
	        "Channel [color=light-cyan]%s[reset] is not active");

	MSG_Add("SHELL_CMD_MIXER_INVALID_GLOBAL_COMMAND",
	        "Invalid global command: [color=white]%s[reset]");

	MSG_Add("SHELL_CMD_MIXER_INVALID_VOLUME_COMMAND",
	        "Invalid volume for the [color=light-cyan]%s[reset] channel: "
	        "[color=white]%s[reset]");

	MSG_Add("SHELL_CMD_MIXER_INVALID_CROSSFEED_STRENGTH",
	        "Invalid crossfeed strength for the [color=light-cyan]%s[reset] channel: "
	        "[color=white]%s[reset];\n"
	        "must be a number between 0 and 100");

	MSG_Add("SHELL_CMD_MIXER_INVALID_CHORUS_LEVEL",
	        "Invalid chorus level for the [color=light-cyan]%s[reset] channel: "
	        "[color=white]%s[reset];\n"
	        "must be a number between 0 and 100");

	MSG_Add("SHELL_CMD_MIXER_INVALID_REVERB_LEVEL",
	        "Invalid reverb level for the [color=light-cyan]%s[reset] channel: "
	        "[color=white]%s[reset];\n"
	        "must be a number between 0 and 100");

	MSG_Add("SHELL_CMD_MIXER_MISSING_CROSSFEED_STRENGTH",
	        "Missing crossfeed strength after [color=white]x[reset] for the "
	        "[color=light-cyan]%s[reset] channel;\n"
	        "must provide a number between 0 and 100");

	MSG_Add("SHELL_CMD_MIXER_MISSING_CHORUS_LEVEL",
	        "Missing chorus level after [color=white]c[reset] for the "
	        "[color=light-cyan]%s[reset] channel;\n"
	        "must provide a number between 0 and 100");

	MSG_Add("SHELL_CMD_MIXER_MISSING_REVERB_LEVEL",
	        "Missing reverb level after [color=white]r[reset] for the "
	        "[color=light-cyan]%s[reset] channel;\n"
	        "must provide a number between 0 and 100");

	MSG_Add("SHELL_CMD_MIXER_INVALID_GLOBAL_CROSSFEED_STRENGTH",
	        "Invalid global crossfeed strength [color=white]%s[reset];\n"
	        "must be a number between 0 and 100");

	MSG_Add("SHELL_CMD_MIXER_INVALID_GLOBAL_CHORUS_LEVEL",
	        "Invalid global chorus level [color=white]%s[reset];\n"
	        "must be a number between 0 and 100");

	MSG_Add("SHELL_CMD_MIXER_INVALID_GLOBAL_REVERB_LEVEL",
	        "Invalid global reverb level [color=white]%s[reset];\n"
	        "must be a number between 0 and 100");

	MSG_Add("SHELL_CMD_MIXER_MISSING_GLOBAL_CROSSFEED_STRENGTH",
	        "Missing global crossfeed strength after [color=white]x[reset];\n"
	        "must provide a number between 0 and 100");

	MSG_Add("SHELL_CMD_MIXER_MISSING_GLOBAL_CHORUS_LEVEL",
	        "Missing global chorus level after [color=white]c[reset];\n"
	        "must provide a number between 0 and 100");

	MSG_Add("SHELL_CMD_MIXER_MISSING_GLOBAL_REVERB_LEVEL",
	        "Missing global reverb level after [color=white]r[reset];\n"
	        "must provide a number between 0 and 100");

	MSG_Add("SHELL_CMD_MIXER_MISSING_CHANNEL_COMMAND",
	        "Missing command for the [color=light-cyan]%s[reset] channel");

	MSG_Add("SHELL_CMD_MIXER_INVALID_CHANNEL_COMMAND",
	        "Invalid command for the [color=light-cyan]%s[reset] channel: "
	        "[color=white]%s[reset]");

	MSG_Add("SHELL_CMD_MIXER_CHANNEL_DEPRECATED",
	        "Channel name [color=light-cyan]%s[reset] is deprecated, "
	        "use [color=light-cyan]%s[reset] instead");
}

void MIXER::ShowMixerStatus()
{
	std::string column_layout = MSG_Get("SHELL_CMD_MIXER_HEADER_LAYOUT");
	column_layout.append({'\n'});

	auto show_channel = [&](const std::string& name,
	                        const AudioFrame& volume_as_gain,
	                        const std::string& mode,
	                        const std::string& xfeed,
	                        const std::string& reverb,
	                        const std::string& chorus) {
		WriteOut(column_layout,
		         name.c_str(),
		         static_cast<double>(gain_to_percentage(volume_as_gain.left)),
		         static_cast<double>(gain_to_percentage(volume_as_gain.right)),
		         static_cast<double>(gain_to_decibel(volume_as_gain.left)),
		         static_cast<double>(gain_to_decibel(volume_as_gain.right)),
		         mode.c_str(),
		         xfeed.c_str(),
		         reverb.c_str(),
		         chorus.c_str());
	};

	WriteOut(MSG_Get("SHELL_CMD_MIXER_HEADER_LABELS"));
	WriteOut("\n");

	const auto off_value      = MSG_Get("SHELL_CMD_MIXER_CHANNEL_OFF");
	constexpr auto none_value = "-";

	constexpr auto master_channel_string = "[color=light-cyan]MASTER[reset]";

	show_channel(convert_ansi_markup(master_channel_string),
	             MIXER_GetMasterVolume(),
	             MSG_Get("SHELL_CMD_MIXER_CHANNEL_STEREO"),
	             none_value,
	             none_value,
	             none_value);

	for (auto& [name, chan] : MIXER_GetChannels()) {
		std::string xfeed = none_value;
		if (chan->HasFeature(ChannelFeature::Stereo)) {
			if (chan->GetCrossfeedStrength() > 0.0f) {
				xfeed = std::to_string(static_cast<uint8_t>(
				        round(gain_to_percentage(
				                chan->GetCrossfeedStrength()))));
			} else {
				xfeed = off_value;
			}
		}

		std::string reverb = none_value;
		if (chan->HasFeature(ChannelFeature::ReverbSend)) {
			if (chan->GetReverbLevel() > 0.0f) {
				reverb = std::to_string(static_cast<uint8_t>(round(
				        gain_to_percentage(chan->GetReverbLevel()))));
			} else {
				reverb = off_value;
			}
		}

		std::string chorus = none_value;
		if (chan->HasFeature(ChannelFeature::ChorusSend)) {
			if (chan->GetChorusLevel() > 0.0f) {
				chorus = std::to_string(static_cast<uint8_t>(round(
				        gain_to_percentage(chan->GetChorusLevel()))));
			} else {
				chorus = off_value;
			}
		}

		auto channel_name = std::string("[color=light-cyan]") + name +
		                    std::string("[reset]");

		auto mode = chan->DescribeLineout();

		show_channel(convert_ansi_markup(channel_name),
		             chan->GetUserVolume(),
		             mode,
		             xfeed,
		             reverb,
		             chorus);
	}

	WriteOut("\n");
}
