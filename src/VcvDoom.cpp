#include "VcvDoom.hpp"
#include <cstdio>
#include <fstream>
#include <thread>

extern "C" {
	#include "doom/i_video.h"
	#include "doom/i_system.h"
	void D_DoomMain(void);
	void I_SetTargetRGBA(uint8_t *buffer);
	extern char doom_engine_error[256];
	void W_Shutdown(void);
	void I_RequestDoomExit(void);
	void I_ClearDoomExitRequest(void);
	void I_RunAtExit(void);
	extern int myargc;
	extern char** myargv;
#include "doom/i_sound.h"
#include "doom/midifile.h"
	void G_SaveGame(int slot, char* description);
}

// The engine must never write directly into a module-owned buffer: a module
// can be deleted while the engine thread is still between frames.  Keep the
// producer buffer alive for the lifetime of the plugin instead.
static uint8_t gDoomFramebuffer[320 * 200 * 4];

// Single-instance engine owner
static VcvDoomModule* gDoomModuleOwner = nullptr;
static std::thread gDoomThread;

// Doom's engine state is global, but Rack calls process() concurrently with
// context-menu actions.  Before replacing a WAD, stop DSP from reading that
// state and wait for the in-flight sample (if any) to finish.
static std::atomic<bool> gDoomEngineTransitioning{false};
static std::atomic<unsigned int> gDoomDspReaders{0};

struct DoomDspAccess {
	bool acquired = false;

	DoomDspAccess() {
		if (gDoomEngineTransitioning.load(std::memory_order_acquire)) {
			return;
		}
		gDoomDspReaders.fetch_add(1, std::memory_order_acquire);
		if (!gDoomEngineTransitioning.load(std::memory_order_acquire)) {
			acquired = true;
			return;
		}
		gDoomDspReaders.fetch_sub(1, std::memory_order_release);
	}

	~DoomDspAccess() {
		if (acquired) {
			gDoomDspReaders.fetch_sub(1, std::memory_order_release);
		}
	}
};

struct DoomAudioAccess {
	bool acquired = false;

	explicit DoomAudioAccess(bool enabled) {
		acquired = enabled && I_BeginRackAudioRead() != 0;
	}

	~DoomAudioAccess() {
		if (acquired) {
			I_EndRackAudioRead();
		}
	}
};

struct DoomEngineTransition {
	DoomEngineTransition() {
		gDoomEngineTransitioning.store(true, std::memory_order_release);
		while (gDoomDspReaders.load(std::memory_order_acquire) != 0) {
			std::this_thread::yield();
		}
	}

	~DoomEngineTransition() {
		gDoomEngineTransitioning.store(false, std::memory_order_release);
	}
};

static void stopDoomEngineLocked() {
	I_RequestDoomExit();
	if (gDoomThread.joinable()) {
		gDoomThread.join();
	}
	W_Shutdown();
}

static void stopDoomEngine() {
	DoomEngineTransition engineTransition;
	stopDoomEngineLocked();
}

// A detached thread would continue executing code from this plugin after Rack
// unloads the DLL/SO.  Requesting exit and joining is required for safe unload.
struct DoomThreadGuard {
	~DoomThreadGuard() {
		stopDoomEngine();
	}
};
static DoomThreadGuard gThreadGuard;

VcvDoomModule::VcvDoomModule() {
	config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

	// Config inputs
	configInput(X_MOVE_INPUT, "X-Move (Strafe)");
	configInput(Y_MOVE_INPUT, "Y-Move (Forward/Backward)");
	configInput(FIRE_GATE_INPUT, "Fire Gate");
	configInput(WEAPON_CV_INPUT, "Weapon CV");

	// Config outputs
	configOutput(HEALTH_OUTPUT, "Health (0-10V)");
	configOutput(FRAG_TRIG_OUTPUT, "Frag Trigger");
	configOutput(AUDIO_L_OUTPUT, "Audio L");
	configOutput(AUDIO_R_OUTPUT, "Audio R");
	configOutput(MIDI_PITCH_OUTPUT, "MIDI Pitch");
	configOutput(MIDI_GATE_OUTPUT, "MIDI Gate");

	if (gDoomModuleOwner == nullptr) {
		gDoomModuleOwner = this;
	}

	// Preserve the convenience of restoring the most recently selected WAD.
	// The engine now targets plugin-lifetime storage, so this cannot leave a
	// worker thread writing into a module that is still being constructed.
	loadGlobalSettings();
}

VcvDoomModule::~VcvDoomModule() {
	if (gDoomModuleOwner == this) {
		stopDoomEngine();
		gDoomModuleOwner = nullptr;
	}
}

bool VcvDoomModule::isEngineOwner() const {
	return gDoomModuleOwner == this;
}

void VcvDoomModule::process(const ProcessArgs& args) {
	float outL = 0.f;
	float outR = 0.f;
	DoomDspAccess engineAccess;
	DoomAudioAccess audioAccess(gDoomModuleOwner == this && hasWad && !isBypassed());

	if (engineAccess.acquired && audioAccess.acquired && gDoomModuleOwner == this && hasWad
			&& I_GetEngineStatus() == 2 && !isBypassed()) {
		// 1. Process CV Inputs
		int weapon = -1;
		if (inputs[WEAPON_CV_INPUT].isConnected()) {
			float v = inputs[WEAPON_CV_INPUT].getVoltage();
			int w = (int)(v + 0.5f);
			if (w < 0) w = 0;
			if (w > 6) w = 6;
			weapon = w;
		}
		I_SetRackCvControls(inputs[X_MOVE_INPUT].getNormalVoltage(0.f),
			inputs[Y_MOVE_INPUT].getNormalVoltage(0.f),
			inputs[FIRE_GATE_INPUT].getNormalVoltage(0.f) >= 1.f,
			weapon, xMoveMode);

		// Song changes can happen entirely between two Rack process calls.
		// Clear all prior notes when Doom publishes a new music generation.
		if (musicGeneration != g_music_generation) {
			musicGeneration = g_music_generation;
			for (int i = 0; i < 16; ++i) {
				voices[i].gate = 0.f;
				voices[i].note = -1;
			}
		}

		// 2. Process CV Outputs
		int health = 0;
		int fragTriggered = 0;
		I_GetRackGameState(&health, &fragTriggered);
		float healthVolt = (float)health / 100.f * 10.f;
		outputs[HEALTH_OUTPUT].setVoltage(clamp(healthVolt, 0.f, 10.f));

		if (fragTriggered) {
			fragTrigTime = (int)(0.010f * args.sampleRate); // 10ms pulse
		}

		if (fragTrigTime > 0) {
			outputs[FRAG_TRIG_OUTPUT].setVoltage(10.f);
			fragTrigTime--;
		} else {
			outputs[FRAG_TRIG_OUTPUT].setVoltage(0.f);
		}

		// 3. Process Audio Resampling & Mixing
		for (int c = 0; c < MIXER_CHANNELS; ++c) {
			mixer_channel_t& chan = g_mixer_channels[c];
			if (chan.active && chan.data) {
				float pos = chan.pos;
				uint32_t idx = (uint32_t)pos;

				if (idx < chan.length) {
					// Read samples for linear interpolation
					float sample0 = (float)chan.data[idx] - 128.f;
					float sample1 = 0.f;
					if (idx + 1 < chan.length) {
						sample1 = (float)chan.data[idx + 1] - 128.f;
					}

					// Linear interpolation
					float frac = pos - (float)idx;
					float interp = sample0 + frac * (sample1 - sample0);

					// Scale to [-1.f, 1.f]
					interp /= 128.f;

					// Apply channel volume (vol is 0-15) and master scale
					float volume = (float)chan.vol / 15.f;
					float sampleVal = interp * volume * 0.5f;

					// Apply panning (sep is 0-255)
					float pan = (float)chan.sep / 255.f;
					float panL = 1.f - pan;
					float panR = pan;

					outL += sampleVal * panL * 2.f;
					outR += sampleVal * panR * 2.f;

					// Step position
					float step = ((float)chan.src_rate / args.sampleRate) * chan.pitch_factor;
					chan.pos += step;
				} else {
					chan.active = 0;
				}
			}
		}

		// Clamp mixed audio
		if (outL < -1.f) outL = -1.f;
		if (outL > 1.f) outL = 1.f;
		if (outR < -1.f) outR = -1.f;
		if (outR > 1.f) outR = 1.f;

		// 4. Process MIDI Music Sequencer
		if (g_music_playing && g_active_midi_iter) {
			double deltaSecs = (double)args.sampleTime;
			double deltaTicks = deltaSecs * g_music_ticks_per_sec;
			g_music_ticks += deltaTicks;

		// Bound work in the audio callback. Zero-delta MIDI events are valid,
		// but an arbitrarily large burst must be spread across later samples.
		int eventsProcessed = 0;
		constexpr int kMaxMidiEventsPerSample = 64;
		while (g_music_ticks >= g_next_event_tick && eventsProcessed++ < kMaxMidiEventsPerSample) {
				midi_event_t* event = nullptr;
				if (MIDI_GetNextEvent((midi_track_iter_t*)g_active_midi_iter, &event)) {
					if (event->event_type == MIDI_EVENT_NOTE_ON) {
						int channel = event->data.channel.channel;
						int note = event->data.channel.param1;
						int velocity = event->data.channel.param2;

						if (velocity == 0) {
							// Note Off
							for (int i = 0; i < 16; ++i) {
								if (voices[i].note == note && voices[i].channel == channel) {
									voices[i].gate = 0.f;
									voices[i].note = -1;
								}
							}
						} else {
							// Note On
							int targetVoice = -1;
							for (int i = 0; i < 16; ++i) {
								if (voices[i].note == note && voices[i].channel == channel) {
									targetVoice = i;
									break;
								}
							}
							if (targetVoice < 0) {
								for (int i = 0; i < 16; ++i) {
									if (voices[i].note == -1) {
										targetVoice = i;
										break;
									}
								}
							}
							if (targetVoice < 0) {
								targetVoice = voiceTriggerCounter % 16;
								voiceTriggerCounter++;
							}

							voices[targetVoice].note = note;
							voices[targetVoice].channel = channel;
							voices[targetVoice].pitch = (float)(note - 60) / 12.f;
							voices[targetVoice].gate = 10.f;
						}
					} else if (event->event_type == MIDI_EVENT_NOTE_OFF) {
						int channel = event->data.channel.channel;
						int note = event->data.channel.param1;
						for (int i = 0; i < 16; ++i) {
							if (voices[i].note == note && voices[i].channel == channel) {
								voices[i].gate = 0.f;
								voices[i].note = -1;
							}
						}
					} else if (event->event_type == MIDI_EVENT_META && event->data.meta.type == MIDI_META_SET_TEMPO) {
						unsigned int tempo = (event->data.meta.data[0] << 16) | (event->data.meta.data[1] << 8) | event->data.meta.data[2];
						if (tempo > 0) {
							g_tempo = tempo;
							g_music_ticks_per_sec = (1000000.0 / g_tempo) * g_time_division;
						}
					}

					unsigned int delta = MIDI_GetDeltaTime((midi_track_iter_t*)g_active_midi_iter);
					g_next_event_tick += delta;
				} else {
					if (g_music_looping) {
						// A MUS/MIDI loop can end with active notes. Clear them before
						// restarting so a missing note-off cannot leave a gate latched.
						for (int i = 0; i < 16; ++i) {
							voices[i].gate = 0.f;
							voices[i].note = -1;
						}
						MIDI_RestartIterator((midi_track_iter_t*)g_active_midi_iter);
						g_music_ticks = 0.0;
						g_next_event_tick = MIDI_GetDeltaTime((midi_track_iter_t*)g_active_midi_iter);
					} else {
						g_music_playing = 0;
						for (int i = 0; i < 16; ++i) {
							voices[i].gate = 0.f;
							voices[i].note = -1;
						}
						break;
					}
				}
			}
		} else {
			for (int i = 0; i < 16; ++i) {
				voices[i].gate = 0.f;
				voices[i].note = -1;
			}
		}

		// 5. Output Polyphonic MIDI CV
		// All channels are written below, so avoid the SDK's redundant
		// higher-channel clearing pass (which also trips a GCC false positive).
		outputs[MIDI_PITCH_OUTPUT].channels = 16;
		outputs[MIDI_GATE_OUTPUT].channels = 16;
		for (int i = 0; i < 16; ++i) {
			outputs[MIDI_PITCH_OUTPUT].setVoltage(voices[i].pitch, i);
			outputs[MIDI_GATE_OUTPUT].setVoltage(voices[i].gate, i);
		}

		// Signal graphical frame updates
		if (I_TakeRackFrameDirty()) {
			dirtyFrame.store(true);
		}
	} else {
		// Output 0V when bypassed or uninitialized
		outputs[HEALTH_OUTPUT].setVoltage(0.f);
		outputs[FRAG_TRIG_OUTPUT].setVoltage(0.f);
		for (int i = 0; i < 16; ++i) {
			voices[i].gate = 0.f;
			voices[i].note = -1;
		}
		outputs[MIDI_PITCH_OUTPUT].channels = 0;
		outputs[MIDI_GATE_OUTPUT].channels = 0;
	}

	outputs[AUDIO_L_OUTPUT].setVoltage(outL * 5.f);
	outputs[AUDIO_R_OUTPUT].setVoltage(outR * 5.f);
}

bool VcvDoomModule::loadWad(const std::string& path, int startSlot) {
	if (path.empty()) {
		return false;
	}

	if (gDoomModuleOwner != this) {
		return false;
	}

	if (hasWad && wadPath == path && startSlot < 0 && gDoomThread.joinable() && I_GetEngineStatus() == 2) {
		return true;
	}

	// Phase 1 validation: file must exist and be readable
	std::ifstream file(path, std::ios::binary);
	if (!file.good()) {
		return false;
	}

	// Basic WAD header check: First 4 bytes must be "IWAD" or "PWAD".
	uint8_t header[12];
	file.read(reinterpret_cast<char*>(header), sizeof(header));
	if (file.gcount() != (std::streamsize)sizeof(header)) {
		return false;
	}

	std::string magic(reinterpret_cast<char*>(header), 4);
	if (magic != "IWAD" && magic != "PWAD") {
		return false;
	}
	const uint32_t lumpCount = (uint32_t)header[4] | ((uint32_t)header[5] << 8)
		| ((uint32_t)header[6] << 16) | ((uint32_t)header[7] << 24);
	const uint32_t directoryOffset = (uint32_t)header[8] | ((uint32_t)header[9] << 8)
		| ((uint32_t)header[10] << 16) | ((uint32_t)header[11] << 24);
	file.seekg(0, std::ios::end);
	const std::streamoff fileSize = file.tellg();
	if (fileSize < 12 || directoryOffset > (uint64_t)fileSize
		|| lumpCount > ((uint64_t)fileSize - directoryOffset) / 16) {
		return false;
	}

	DoomEngineTransition engineTransition;
	wadPath = path;
	hasWad = true;

	// Shut down existing thread if any
	stopDoomEngineLocked();

	// Ensure the save directory exists
	std::string saveDir = system::join(asset::user(), "Ifrit/vcvdoom_saves");
	system::createDirectories(saveDir);
	std::string logDir = system::join(asset::user(), "Ifrit/Log");
	system::createDirectories(logDir);
	std::string logPath = system::join(logDir, "vcvdoom.log");

	// Start a new thread
	I_ClearDoomExitRequest();
	I_TakeRackFrameDirty();
	doom_engine_error[0] = '\0';
	I_SetEngineStatus(1);
	I_SetTargetRGBA(gDoomFramebuffer);
	const std::string engineWadPath = wadPath;
	gDoomThread = std::thread([engineWadPath, saveDir, logPath, startSlot]() {

		// Set up argc / argv using std::vector for clean formatting
		std::vector<std::string> args;
		args.push_back("vcvdoom");
		args.push_back("-iwad");
		args.push_back(engineWadPath);
		args.push_back("-savedir");
		args.push_back(saveDir);
		args.push_back("-logfile");
		args.push_back(logPath);
		if (startSlot >= 0) {
			args.push_back("-loadgame");
			args.push_back(std::to_string(startSlot));
		}

		int argc = args.size();
		char** argv = (char**)malloc((argc + 1) * sizeof(char*));
		for (int i = 0; i < argc; ++i) {
			argv[i] = strdup(args[i].c_str());
		}
		argv[argc] = NULL;
		myargc = argc;
		myargv = argv;

		D_DoomMain();
		I_RunAtExit();

		for (int i = 0; i < argc; ++i) {
			free(argv[i]);
		}
		free(argv);
	});

	saveGlobalSettings();
	return true;
}

void VcvDoomModule::saveGlobalSettings() {
	json_t* rootJ = json_object();
	json_object_set_new(rootJ, "wadPath", json_string(wadPath.c_str()));

	const std::string dir = system::join(asset::user(), "Ifrit");
	system::createDirectories(dir);
	const std::string path = system::join(dir, "vcvdoom.json");
	FILE* file = std::fopen(path.c_str(), "w");
	if (file) {
		json_dumpf(rootJ, file, JSON_INDENT(2));
		std::fclose(file);
	}
	json_decref(rootJ);
}

void VcvDoomModule::loadGlobalSettings() {
	const std::string dir = system::join(asset::user(), "Ifrit");
	const std::string path = system::join(dir, "vcvdoom.json");
	FILE* file = std::fopen(path.c_str(), "r");
	if (!file) {
		return;
	}
	json_error_t error;
	json_t* rootJ = json_loadf(file, 0, &error);
	std::fclose(file);
	if (!rootJ) {
		return;
	}

	json_t* pathJ = json_object_get(rootJ, "wadPath");
	if (pathJ && json_is_string(pathJ)) {
		std::string savedPath = json_string_value(pathJ);
		if (!loadWad(savedPath)) {
			WARN("VCV Doom: Saved WAD path '%s' failed validation", savedPath.c_str());
		}
	}
	json_decref(rootJ);
}

static std::string encodeBase64(const std::vector<uint8_t>& data) {
	static const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string result;
	result.reserve(((data.size() + 2) / 3) * 4);
	for (size_t i = 0; i < data.size(); i += 3) {
		uint32_t value = (uint32_t)data[i] << 16;
		const bool hasSecond = i + 1 < data.size();
		const bool hasThird = i + 2 < data.size();
		if (hasSecond) value |= (uint32_t)data[i + 1] << 8;
		if (hasThird) value |= data[i + 2];
		result.push_back(chars[(value >> 18) & 0x3f]);
		result.push_back(chars[(value >> 12) & 0x3f]);
		result.push_back(hasSecond ? chars[(value >> 6) & 0x3f] : '=');
		result.push_back(hasThird ? chars[value & 0x3f] : '=');
	}
	return result;
}

static int decodeBase64Char(char c) {
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+') return 62;
	if (c == '/') return 63;
	return -1;
}

static std::vector<uint8_t> decodeBase64(const std::string& encoded) {
	std::vector<uint8_t> result;
	if (encoded.empty() || encoded.size() % 4 != 0 || encoded.size() > 1400000) return result;
	result.reserve((encoded.size() / 4) * 3);
	for (size_t i = 0; i < encoded.size(); i += 4) {
		const int a = decodeBase64Char(encoded[i]);
		const int b = decodeBase64Char(encoded[i + 1]);
		const bool pad2 = encoded[i + 2] == '=';
		const bool pad3 = encoded[i + 3] == '=';
		const int c = pad2 ? 0 : decodeBase64Char(encoded[i + 2]);
		const int d = pad3 ? 0 : decodeBase64Char(encoded[i + 3]);
		if (a < 0 || b < 0 || c < 0 || d < 0 || (pad2 && !pad3)
			|| ((pad2 || pad3) && i + 4 != encoded.size())) return {};
		const uint32_t value = ((uint32_t)a << 18) | ((uint32_t)b << 12) | ((uint32_t)c << 6) | (uint32_t)d;
		result.push_back((value >> 16) & 0xff);
		if (!pad2) result.push_back((value >> 8) & 0xff);
		if (!pad3) result.push_back(value & 0xff);
	}
	return result;
}

static std::vector<uint8_t> decodeHex(const std::string& hex) {
	std::vector<uint8_t> result;
	if (hex.size() % 2 != 0 || hex.size() > 2 * 1024 * 1024) {
		return result;
	}
	result.reserve(hex.size() / 2);
	for (size_t i = 0; i + 1 < hex.size(); i += 2) {
		char high = hex[i];
		char low = hex[i + 1];
		uint8_t byte = 0;
		if (high >= '0' && high <= '9') byte |= (high - '0') << 4;
		else if (high >= 'a' && high <= 'f') byte |= (high - 'a' + 10) << 4;
		else if (high >= 'A' && high <= 'F') byte |= (high - 'A' + 10) << 4;
		else return {};

		if (low >= '0' && low <= '9') byte |= (low - '0');
		else if (low >= 'a' && low <= 'f') byte |= (low - 'a' + 10);
		else if (low >= 'A' && low <= 'F') byte |= (low - 'A' + 10);
		else return {};
		
		result.push_back(byte);
	}
	return result;
}

json_t* VcvDoomModule::dataToJson() {
	json_t* rootJ = json_object();
	json_object_set_new(rootJ, "saveGameBase64", json_string(savedGameData.c_str()));
	json_object_set_new(rootJ, "xMoveMode", json_integer(xMoveMode));
	return rootJ;
}

void VcvDoomModule::dataFromJson(json_t* rootJ) {
	json_t* modeJ = json_object_get(rootJ, "xMoveMode");
	if (modeJ && json_is_integer(modeJ)) {
		xMoveMode = json_integer_value(modeJ);
	}

	json_t* base64J = json_object_get(rootJ, "saveGameBase64");
	json_t* hexJ = json_object_get(rootJ, "saveGameHex");
	if ((base64J && json_is_string(base64J)) || (hexJ && json_is_string(hexJ))) {
		const bool legacyHex = !base64J || !json_is_string(base64J);
		savedGameData = json_string_value(legacyHex ? hexJ : base64J);
		// Automatically reload the saved game state on patch loading
		if (!savedGameData.empty()) {
			std::vector<uint8_t> buffer = legacyHex ? decodeHex(savedGameData) : decodeBase64(savedGameData);
			if (!buffer.empty()) {
				std::string saveDir = system::join(asset::user(), "Ifrit/vcvdoom_saves");
				system::createDirectories(saveDir);
				std::string savePath = system::join(saveDir, "doomsav8.dsg");
				
				std::ofstream file(savePath, std::ios::binary);
				if (file.good()) {
					file.write((const char*)buffer.data(), buffer.size());
					file.close();
					
					if (!wadPath.empty()) {
						loadWad(wadPath, 8);
					}
				}
			}
		}
	}
}

void VcvDoomModule::triggerExplicitSave() {
	if (gDoomModuleOwner == this && hasWad && I_GetEngineStatus() == 2) {
		std::string saveDir = system::join(asset::user(), "Ifrit/vcvdoom_saves");
		std::string savePath = system::join(saveDir, "doomsav8.dsg");
		
		std::remove(savePath.c_str());
		I_RequestRackSave();
		savePending = true;
		saveDeadline = system::getTime() + 0.5;
	}
}

void VcvDoomModule::pollExplicitSave() {
	if (!savePending) {
		return;
	}

	std::string saveDir = system::join(asset::user(), "Ifrit/vcvdoom_saves");
	std::string savePath = system::join(saveDir, "doomsav8.dsg");
	std::ifstream file(savePath, std::ios::binary);
	if (file.good()) {
		file.seekg(0, std::ios::end);
		const std::streamoff size = file.tellg();
		if (size > 0) {
			file.seekg(0, std::ios::beg);
			std::vector<uint8_t> buffer((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
			savedGameData = encodeBase64(buffer);
			savePending = false;
			return;
		}
	}

	if (system::getTime() >= saveDeadline) {
		savePending = false;
	}
}

void VcvDoomModule::triggerExplicitLoad() {
	if (!savedGameData.empty()) {
		std::vector<uint8_t> buffer = decodeBase64(savedGameData);
		if (!buffer.empty()) {
			std::string saveDir = system::join(asset::user(), "Ifrit/vcvdoom_saves");
			system::createDirectories(saveDir);
			std::string savePath = system::join(saveDir, "doomsav8.dsg");
			
			std::ofstream file(savePath, std::ios::binary);
			if (file.good()) {
				file.write((const char*)buffer.data(), buffer.size());
				file.close();
				
				if (!wadPath.empty()) {
					loadWad(wadPath, 8);
				}
			}
		}
	}
}
