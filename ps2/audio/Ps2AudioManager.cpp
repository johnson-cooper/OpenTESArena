// PS2 implementation of OpenTESArena's AudioManager (replaces Audio/AudioManager.cpp + OpenAL + WildMIDI).
//
// Sound effects: Arena's 8-bit .VOC files are encoded once to SPU ADPCM on the EE (Ps2Adpcm), uploaded to SPU2 RAM
// on first use through audsrv, and played on hardware SPU2 voices with per-voice volume/pan for 3D positioning.
// Nothing is decoded per frame and no PCM stays resident: an EE-side ADPCM cache with a byte budget avoids
// re-encoding, and SPU2 RAM is flushed and refilled on demand when full (audsrv frees SPU memory stack-wise).
//
// Music: Arena's music is MIDI. WildMIDI + GUS patches don't fit the PS2's memory, so the music path is a clean
// stub for now behind the engine's MidiDevice interface (see PS2_PORT.md "Music"). Song selection/state logic is
// identical to desktop so a PS2 sequencer can be dropped in.
//
// The shared AudioManager.h exposes OpenAL-flavored member types; on PS2 "ALuint source" is an SPU2 voice handle
// (channel + 1, 0 = none) and the forward-declared OpenALStream class is the PS2 music stream.

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <unordered_map>

#include <audsrv.h>

// audsrv.h defines MIN_VOLUME/MAX_VOLUME macros that collide with AudioManager members.
namespace { constexpr int AUDSRV_MAX_VOLUME = MAX_VOLUME; }
#undef MIN_VOLUME
#undef MAX_VOLUME

#include "OpenTESArena/src/Assets/VOCFile.h"
#include "OpenTESArena/src/Audio/AudioManager.h"
#include "OpenTESArena/src/Audio/MusicDefinition.h"

#include "components/debug/Debug.h"
#include "components/utilities/StringView.h"
#include "components/utilities/TextLinesFile.h"
#include "components/vfs/manager.hpp"

#include "Ps2Adpcm.h"
#include "ps2/platform/Ps2Platform.h"

namespace
{
	constexpr ALuint INVALID_SOURCE = 0;
	constexpr int SPU_VOICE_COUNT = 24; // audsrv exposes 24 ADPCM voices.
	constexpr size_t EE_ADPCM_CACHE_BUDGET = 768 * 1024;

	struct Ps2Sound
	{
		std::vector<uint8_t> vag; // Empty when evicted from the EE cache.
		audsrv_adpcm_t adpcm;
		int sampleCount = 0;
		int sampleRate = 0;
		bool uploaded = false;
		bool failed = false;
		uint32_t lastUseFrame = 0;

		double getSeconds() const
		{
			return (sampleRate > 0) ? (static_cast<double>(sampleCount) / static_cast<double>(sampleRate)) : 0.0;
		}
	};

	struct Ps2Voice
	{
		Ps2Sound *sound = nullptr;
		double startSeconds = 0.0;
		double endSeconds = 0.0;
		bool is3D = false;
		Double3 position;
	};

	bool g_audioReady = false;
	std::unordered_map<std::string, Ps2Sound> g_sounds; // Bounded by the number of distinct Arena sounds (~100).
	size_t g_eeCacheBytes = 0;
	uint32_t g_frame = 0;
	Ps2Voice g_voices[SPU_VOICE_COUNT];
	Double3 g_listenerPosition = Double3::Zero;
	Double3 g_listenerForward = Double3::UnitX;
	Double3 g_listenerUp = Double3::UnitY;
	bool g_warnedMusic = false;

	int VoiceIndex(ALuint source)
	{
		return static_cast<int>(source) - 1;
	}

	bool IsVoiceActive(ALuint source)
	{
		const int v = VoiceIndex(source);
		return (v >= 0) && (v < SPU_VOICE_COUNT) && (g_voices[v].sound != nullptr) && (Ps2Platform::getSeconds() < g_voices[v].endSeconds);
	}

	void StopVoice(ALuint source)
	{
		const int v = VoiceIndex(source);
		if ((v < 0) || (v >= SPU_VOICE_COUNT))
		{
			return;
		}

		if (g_audioReady && (g_voices[v].sound != nullptr) && (Ps2Platform::getSeconds() < g_voices[v].endSeconds))
		{
			audsrv_adpcm_set_volume_and_pan(v, 0, 0); // audsrv has no per-voice stop; silence it.
		}

		g_voices[v] = Ps2Voice();
	}

	void UpdateAudioCounter()
	{
		Ps2Platform::setCounter(Ps2Platform::Counter::AudioBufferBytes, static_cast<int64_t>(g_eeCacheBytes));
	}

	void EvictEeCacheIfNeeded(const Ps2Sound *keep)
	{
		while (g_eeCacheBytes > EE_ADPCM_CACHE_BUDGET)
		{
			Ps2Sound *oldest = nullptr;
			for (auto &pair : g_sounds)
			{
				Ps2Sound &s = pair.second;
				if ((&s != keep) && !s.vag.empty() && ((oldest == nullptr) || (s.lastUseFrame < oldest->lastUseFrame)))
				{
					oldest = &s;
				}
			}

			if (oldest == nullptr)
			{
				break;
			}

			g_eeCacheBytes -= oldest->vag.size();
			std::vector<uint8_t>().swap(oldest->vag); // Actually release the memory.
		}

		UpdateAudioCounter();
	}

	void FlushSpu()
	{
		DebugLog("SPU2 sample memory full; flushing and reloading on demand.");
		audsrv_adpcm_init();
		for (auto &pair : g_sounds)
		{
			pair.second.uploaded = false;
		}

		for (Ps2Voice &voice : g_voices)
		{
			voice = Ps2Voice();
		}
	}

	bool EnsureEncoded(const std::string &filename, Ps2Sound &sound, const std::vector<VocRepairEntry> &repairs)
	{
		if (!sound.vag.empty())
		{
			return true;
		}

		if (sound.failed)
		{
			return false;
		}

		const std::string_view extension = StringView::getExtension(filename);
		if (!StringView::caseInsensitiveEquals(extension, "VOC"))
		{
			DebugLogWarningFormat("PS2 audio: unsupported sound format \"%s\".", filename.c_str());
			sound.failed = true;
			return false;
		}

		VOCFile voc;
		if (!voc.init(filename.c_str()))
		{
			DebugLogErrorFormat("PS2 audio: couldn't load .VOC \"%s\".", filename.c_str());
			sound.failed = true;
			return false;
		}

		Span<uint8_t> pcm = voc.getAudioData();

		// Same pop repairs as desktop.
		for (const VocRepairEntry &entry : repairs)
		{
			if (!StringView::equals(entry.filename, filename))
			{
				continue;
			}

			for (const VocRepairSpan &span : entry.spans)
			{
				if ((span.startIndex >= 0) && ((span.startIndex + span.count) <= pcm.getCount()))
				{
					std::fill(pcm.begin() + span.startIndex, pcm.begin() + span.startIndex + span.count, span.replacementSample);
				}
			}
		}

		Ps2Adpcm::encodeU8ToVag(pcm.begin(), pcm.getCount(), voc.getSampleRate(), &sound.vag);
		sound.sampleCount = pcm.getCount();
		sound.sampleRate = voc.getSampleRate();
		g_eeCacheBytes += sound.vag.size();
		EvictEeCacheIfNeeded(&sound);
		return true;
	}

	bool EnsureUploaded(Ps2Sound &sound)
	{
		if (sound.uploaded)
		{
			return true;
		}

		for (int attempt = 0; attempt < 2; attempt++)
		{
			const int result = audsrv_load_adpcm(&sound.adpcm, sound.vag.data(), static_cast<int>(sound.vag.size()));
			if (result == 0)
			{
				sound.uploaded = true;
				return true;
			}

			if (attempt == 0)
			{
				FlushSpu();
			}
		}

		DebugLogError("PS2 audio: audsrv_load_adpcm failed.");
		return false;
	}

	// OpenAL-like inverse distance clamped model (reference distance 1, rolloff 1) plus stereo pan.
	void Compute3DVolumePan(const Double3 &position, float gain, int *outVolume, int *outPan)
	{
		const Double3 toSource = position - g_listenerPosition;
		const double distance = toSource.length();
		const double attenuation = 1.0 / std::max(1.0, distance);
		const Double3 right = g_listenerForward.cross(g_listenerUp).normalized();
		const double pan = (distance > 1.0e-6) ? right.dot(toSource / distance) : 0.0;
		*outVolume = std::clamp(static_cast<int>(gain * attenuation * 100.0), 0, 100);
		*outPan = std::clamp(static_cast<int>(pan * 100.0), -100, 100);
	}

	bool ProcessVocRepairLine(const std::string_view text, std::string *outFilename, VocRepairSpan *outSpan)
	{
		constexpr int expectedTokenCount = 4;
		std::string_view tokens[expectedTokenCount];
		if (!StringView::splitExpected<expectedTokenCount>(text, ',', tokens))
		{
			return false;
		}

		*outFilename = tokens[0];
		auto parse = [](std::string_view s, auto *out)
		{
			return std::from_chars(s.data(), s.data() + s.size(), *out).ec == std::errc();
		};

		return parse(tokens[1], &outSpan->startIndex) && parse(tokens[2], &outSpan->count) && parse(tokens[3], &outSpan->replacementSample);
	}
}

// PS2 music stream (named after the forward declaration in AudioManager.h). Currently silent; see file header.
class OpenALStream
{
public:
	std::string filename;
	bool loop = false;
	bool playing = false;
};

std::unique_ptr<MidiDevice> MidiDevice::sInstance;

SoundInstance::SoundInstance()
{
	this->isOneShot = false;
	this->is3D = false;
	this->source = INVALID_SOURCE;
}

void SoundInstance::init(const std::string &filename, bool isOneShot, bool is3D)
{
	DebugAssert(this->source == INVALID_SOURCE);
	this->filename = filename;
	this->isOneShot = isOneShot;
	this->is3D = is3D;
}

AudioListenerState::AudioListenerState(const Double3 &position, const Double3 &forward, const Double3 &up)
	: position(position), forward(forward), up(up) { }

VocRepairSpan::VocRepairSpan()
{
	this->startIndex = -1;
	this->count = 0;
	this->replacementSample = 0;
}

AudioManager::AudioManager()
{
	mMusicVolume = 0.0f;
	mSfxVolume = 0.0f;
	mHasResamplerExtension = false;
	mResampler = -1;
	mIs3D = false;
}

AudioManager::~AudioManager()
{
	this->stopMusic();
	this->stopSounds();
	this->soundInstancesPool.clear();
	MidiDevice::shutdown();

	if (g_audioReady)
	{
		audsrv_quit();
		g_audioReady = false;
	}

	g_sounds.clear();
	g_eeCacheBytes = 0;
}

void AudioManager::init(double musicVolume, double soundVolume, int maxChannels, int resamplingOption,
	bool is3D, const std::string &midiConfig, const std::string &audioDataPath)
{
	DebugLog("Initializing PS2 audio (audsrv/SPU2).");
	static_cast<void>(resamplingOption);
	static_cast<void>(midiConfig);

	mIs3D = is3D;

	if (!Ps2Platform::isAudioDriverLoaded())
	{
		DebugLogWarning("PS2 audio drivers not loaded; running without sound.");
	}
	else if (audsrv_init() != 0)
	{
		DebugLogWarningFormat("audsrv_init() failed (%s); running without sound.", audsrv_get_error_string());
	}
	else if (audsrv_adpcm_init() != 0)
	{
		DebugLogWarning("audsrv_adpcm_init() failed; running without sound.");
		audsrv_quit();
	}
	else
	{
		g_audioReady = true;
		audsrv_set_volume(AUDSRV_MAX_VOLUME);
	}

	const int voiceCount = std::clamp(maxChannels, 1, SPU_VOICE_COUNT);
	for (int i = 0; i < voiceCount; i++)
	{
		mFreeSources.emplace_back(static_cast<ALuint>(i + 1));
	}

	this->setMusicVolume(musicVolume);
	this->setSoundVolume(soundVolume);

	TextLinesFile singleInstanceSoundsFile;
	const std::string singleInstanceSoundsPath = audioDataPath + "SingleInstanceSounds.txt";
	if (singleInstanceSoundsFile.init(singleInstanceSoundsPath.c_str()))
	{
		for (int i = 0; i < singleInstanceSoundsFile.getLineCount(); i++)
		{
			mSingleInstanceSounds.emplace_back(singleInstanceSoundsFile.getLine(i));
		}
	}

	TextLinesFile vocRepairFile;
	const std::string vocRepairPath = audioDataPath + "VocRepair.txt";
	if (vocRepairFile.init(vocRepairPath.c_str()))
	{
		for (int i = 0; i < vocRepairFile.getLineCount(); i++)
		{
			std::string vocFilename;
			VocRepairSpan span;
			if (ProcessVocRepairLine(vocRepairFile.getLine(i), &vocFilename, &span))
			{
				auto iter = std::find_if(mVocRepairEntries.begin(), mVocRepairEntries.end(),
					[&vocFilename](const VocRepairEntry &e) { return e.filename == vocFilename; });
				if (iter == mVocRepairEntries.end())
				{
					VocRepairEntry entry;
					entry.filename = vocFilename;
					entry.spans.emplace_back(span);
					mVocRepairEntries.emplace_back(std::move(entry));
				}
				else
				{
					iter->spans.emplace_back(span);
				}
			}
		}
	}

	// Unlike desktop, sounds are NOT all decoded up front; only their names are registered.
	const std::vector<std::string> vocFilenames = VFS::Manager::get().listFilesWithExtension("voc");
	g_sounds.reserve(vocFilenames.size());
	for (const std::string &filename : vocFilenames)
	{
		g_sounds.emplace(filename, Ps2Sound());
	}

	DebugLogFormat("PS2 audio: %d voices, %d sounds registered (lazy ADPCM, %d KiB EE cache budget).",
		voiceCount, static_cast<int>(g_sounds.size()), static_cast<int>(EE_ADPCM_CACHE_BUDGET / 1024));
}

double AudioManager::getMusicVolume() const
{
	return static_cast<double>(mMusicVolume);
}

double AudioManager::getSoundVolume() const
{
	return static_cast<double>(mSfxVolume);
}

bool AudioManager::hasResamplerExtension() const
{
	return false; // SPU2 does its own pitch conversion.
}

bool AudioManager::soundExists(const std::string &filename) const
{
	return g_sounds.find(filename) != g_sounds.end();
}

bool AudioManager::anyPlayingSounds(const std::string &filename) const
{
	for (const SoundInstanceID soundInstID : this->soundInstancesPool.keys)
	{
		const SoundInstance &soundInst = this->soundInstancesPool.get(soundInstID);
		if ((soundInst.filename == filename) && IsVoiceActive(soundInst.source))
		{
			return true;
		}
	}

	return false;
}

int AudioManager::getTotalPlayingSoundCount() const
{
	int count = 0;
	for (const SoundInstance &soundInst : this->soundInstancesPool.values)
	{
		count += IsVoiceActive(soundInst.source) ? 1 : 0;
	}

	return count;
}

bool AudioManager::hasNextMusic() const
{
	return !mNextSong.empty();
}

void AudioManager::setListenerPosition(const Double3 &position)
{
	g_listenerPosition = position;
}

void AudioManager::setListenerOrientation(const Double3 &forward, const Double3 &up)
{
	g_listenerForward = forward;
	g_listenerUp = up;
}

void AudioManager::resetSource(ALuint source)
{
	StopVoice(source);
}

void AudioManager::playMusic(const std::string &filename, bool loop)
{
	if (mCurrentSong == filename)
	{
		return;
	}

	this->stopMusic();

	if (!g_warnedMusic)
	{
		DebugLogWarning("PS2 music playback is not implemented yet (MIDI); music is silent.");
		g_warnedMusic = true;
	}

	mSongStream = std::make_unique<OpenALStream>();
	mSongStream->filename = filename;
	mSongStream->loop = loop;
	mSongStream->playing = loop; // A silent non-looping song "finishes" immediately so staged music advances.
	mCurrentSong = filename;
}

void AudioManager::setMusic(const MusicDefinition *musicDef, const MusicDefinition *optMusicDef)
{
	if (optMusicDef != nullptr)
	{
		this->playMusic(optMusicDef->filename, false);
		DebugAssert(musicDef != nullptr);
		mNextSong = musicDef->filename;
	}
	else if (musicDef != nullptr)
	{
		this->playMusic(musicDef->filename, true);
	}
	else
	{
		this->stopMusic();
	}
}

void AudioManager::stopMusic()
{
	mCurrentMidiSong = nullptr;
	mSongStream = nullptr;
	mCurrentSong.clear();
}

void AudioManager::stopSounds()
{
	for (SoundInstance &soundInst : this->soundInstancesPool.values)
	{
		if (soundInst.source != INVALID_SOURCE)
		{
			this->resetSource(soundInst.source);
			mFreeSources.push_front(soundInst.source);
			soundInst.source = INVALID_SOURCE;
		}
	}
}

void AudioManager::setMusicVolume(double percent)
{
	mMusicVolume = static_cast<float>(percent);
}

void AudioManager::setSoundVolume(double percent)
{
	mSfxVolume = static_cast<float>(percent);
}

void AudioManager::setResamplingOption(int resamplingOption)
{
	static_cast<void>(resamplingOption);
}

void AudioManager::set3D(bool is3D)
{
	mIs3D = is3D;
}

void AudioManager::updateSources()
{
	g_frame++;
	const double now = Ps2Platform::getSeconds();

	// Recycle finished voices (time-based: no per-frame IOP RPC needed), destroying one-shot instances.
	SoundInstanceID toDestroy[SPU_VOICE_COUNT];
	int toDestroyCount = 0;
	for (const SoundInstanceID soundInstID : this->soundInstancesPool.keys)
	{
		SoundInstance &soundInst = this->soundInstancesPool.get(soundInstID);
		if (soundInst.source == INVALID_SOURCE)
		{
			continue;
		}

		const int v = VoiceIndex(soundInst.source);
		Ps2Voice &voice = g_voices[v];
		if (now < voice.endSeconds)
		{
			// Still playing: keep 3D volume/pan current.
			if (g_audioReady && voice.is3D && mIs3D)
			{
				int volume, pan;
				Compute3DVolumePan(voice.position, mSfxVolume, &volume, &pan);
				audsrv_adpcm_set_volume_and_pan(v, volume, pan);
			}

			continue;
		}

		voice = Ps2Voice();
		mFreeSources.push_front(soundInst.source);
		soundInst.source = INVALID_SOURCE;

		if (soundInst.isOneShot && (toDestroyCount < SPU_VOICE_COUNT))
		{
			toDestroy[toDestroyCount++] = soundInstID;
		}
	}

	for (int i = 0; i < toDestroyCount; i++)
	{
		this->soundInstancesPool.free(toDestroy[i]);
	}

	if (this->hasNextMusic())
	{
		const bool canChangeToNextMusic = (mSongStream == nullptr) || !mSongStream->playing;
		if (canChangeToNextMusic)
		{
			this->playMusic(mNextSong, true);
			mNextSong.clear();
		}
	}
}

void AudioManager::updateListener(const AudioListenerState &listenerState)
{
	this->setListenerPosition(listenerState.position);
	this->setListenerOrientation(listenerState.forward, listenerState.up);
}

SoundInstanceID AudioManager::allocateSound(const std::string &filename, bool isOneShot, bool is3D)
{
	const SoundInstanceID instID = this->soundInstancesPool.alloc();
	if (instID < 0)
	{
		DebugLogErrorFormat("Couldn't allocate sound instance ID for \"%s\".", filename.c_str());
		return -1;
	}

	SoundInstance &inst = this->soundInstancesPool.get(instID);
	inst.init(filename, isOneShot, is3D);
	return instID;
}

SoundInstanceID AudioManager::allocateSound(const std::string &filename)
{
	return this->allocateSound(filename, false, false);
}

void AudioManager::freeSound(SoundInstanceID instID)
{
	SoundInstance &inst = this->soundInstancesPool.get(instID);
	if (inst.source != INVALID_SOURCE)
	{
		this->resetSource(inst.source);
		mFreeSources.push_front(inst.source);
		inst.source = INVALID_SOURCE;
	}

	this->soundInstancesPool.free(instID);
}

double AudioManager::getSoundTotalSeconds(const std::string &filename) const
{
	auto iter = g_sounds.find(filename);
	if (iter == g_sounds.end())
	{
		DebugLogWarningFormat("Missing sound \"%s\" for duration lookup.", filename.c_str());
		return 0.0;
	}

	Ps2Sound &sound = iter->second;
	if ((sound.sampleRate == 0) && !EnsureEncoded(filename, sound, mVocRepairEntries))
	{
		return 0.0;
	}

	return sound.getSeconds();
}

double AudioManager::getSoundTotalSeconds(SoundInstanceID instID) const
{
	const SoundInstance &soundInst = this->soundInstancesPool.get(instID);
	return this->getSoundTotalSeconds(soundInst.filename);
}

double AudioManager::getSoundCurrentSeconds(SoundInstanceID instID) const
{
	const SoundInstance &soundInst = this->soundInstancesPool.get(instID);
	if (!IsVoiceActive(soundInst.source))
	{
		return 0.0;
	}

	return Ps2Platform::getSeconds() - g_voices[VoiceIndex(soundInst.source)].startSeconds;
}

bool AudioManager::isSoundPlaying(SoundInstanceID instID) const
{
	const SoundInstance &soundInst = this->soundInstancesPool.get(instID);
	return IsVoiceActive(soundInst.source);
}

void AudioManager::setSoundPosition(SoundInstanceID instID, const Double3 &position)
{
	const SoundInstance &soundInst = this->soundInstancesPool.get(instID);
	if (soundInst.source == INVALID_SOURCE)
	{
		return;
	}

	Ps2Voice &voice = g_voices[VoiceIndex(soundInst.source)];
	voice.position = position;
	if (g_audioReady && voice.is3D && mIs3D)
	{
		int volume, pan;
		Compute3DVolumePan(position, mSfxVolume, &volume, &pan);
		audsrv_adpcm_set_volume_and_pan(VoiceIndex(soundInst.source), volume, pan);
	}
}

void AudioManager::playSound(SoundInstanceID instID)
{
	SoundInstance &soundInst = this->soundInstancesPool.get(instID);
	const std::string &soundFilename = soundInst.filename;

	if (soundInst.source == INVALID_SOURCE)
	{
		if (mFreeSources.empty())
		{
			DebugLogWarningFormat("No free SPU2 voices to play sound \"%s\".", soundFilename.c_str());
			return;
		}

		const bool isSingleInstance = std::find(mSingleInstanceSounds.begin(), mSingleInstanceSounds.end(), soundFilename) != mSingleInstanceSounds.end();
		if (isSingleInstance && this->anyPlayingSounds(soundFilename))
		{
			return;
		}

		soundInst.source = mFreeSources.back();
		mFreeSources.pop_back();
	}

	auto iter = g_sounds.find(soundFilename);
	if (iter == g_sounds.end())
	{
		DebugLogErrorFormat("Unknown sound \"%s\".", soundFilename.c_str());
		return;
	}

	Ps2Sound &sound = iter->second;
	sound.lastUseFrame = g_frame;
	if (!g_audioReady || !EnsureEncoded(soundFilename, sound, mVocRepairEntries) || !EnsureUploaded(sound))
	{
		return;
	}

	const int v = VoiceIndex(soundInst.source);
	Ps2Voice &voice = g_voices[v];
	voice.sound = &sound;
	voice.is3D = soundInst.is3D;
	voice.startSeconds = Ps2Platform::getSeconds();
	voice.endSeconds = voice.startSeconds + sound.getSeconds();

	int volume = std::clamp(static_cast<int>(mSfxVolume * 100.0f), 0, 100);
	int pan = 0;
	if (voice.is3D && mIs3D)
	{
		Compute3DVolumePan(voice.position, mSfxVolume, &volume, &pan);
	}

	audsrv_adpcm_set_volume_and_pan(v, volume, pan);
	const int channel = audsrv_ch_play_adpcm(v, &sound.adpcm);
	if (channel < 0)
	{
		DebugLogWarningFormat("audsrv_ch_play_adpcm(%d) failed for \"%s\" (%d).", v, soundFilename.c_str(), channel);
		voice.endSeconds = 0.0;
	}
}

void AudioManager::pauseSound(SoundInstanceID instID)
{
	// audsrv voices can't pause; stopping is the closest behavior.
	this->stopSound(instID);
}

void AudioManager::stopSound(SoundInstanceID instID)
{
	SoundInstance &soundInst = this->soundInstancesPool.get(instID);
	StopVoice(soundInst.source);
}

void AudioManager::playSoundOneShot(const std::string &filename, const Double3 &position)
{
	const SoundInstanceID instID = this->allocateSound(filename, true, true);
	if (instID < 0)
	{
		DebugLogWarningFormat("Couldn't play one-shot sound \"%s\".", filename.c_str());
		return;
	}

	// Position first so the initial volume/pan is right.
	SoundInstance &soundInst = this->soundInstancesPool.get(instID);
	this->playSound(instID);
	if (soundInst.source != INVALID_SOURCE)
	{
		this->setSoundPosition(instID, position);
	}
}

void AudioManager::playSoundOneShot(const std::string &filename)
{
	const SoundInstanceID instID = this->allocateSound(filename, true, false);
	if (instID < 0)
	{
		DebugLogWarningFormat("Couldn't play one-shot sound \"%s\".", filename.c_str());
		return;
	}

	this->playSound(instID);
}
