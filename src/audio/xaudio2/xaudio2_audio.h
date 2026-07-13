#pragma once

#include "audio_system.h"

#define XAUDIO2_MAX_SOUND_INSTANCES 32
#define XAUDIO2_SOUND_INSTANCE_ID_BASE 100000
#define XAUDIO2_MAX_AUDIO_STREAMS 32
// This is the index space that the native runner uses
#define XAUDIO2_AUDIO_STREAM_INDEX_BASE 300000

typedef struct {
	bool active;
	char* filePath; // resolved file path (owned, freed on destroy)
	float initialGain;
	float initialPitch;
} AudioStreamEntry;

typedef struct {
	AudioSystem base;

	void* pXAudio2;		// IXAudio2*
	void* pMasterVoice; // IXAudio2MasteringVoice*
	float masterGain;
	bool initialized;

	FileSystem* fileSystem; // for loading external audio files

	// Sound instance tracking (managed in C++ implementation)
	void* instanceData; // opaque pointer to C++ instance array
	int nextInstanceCounter;

	AudioStreamEntry streams[XAUDIO2_MAX_AUDIO_STREAMS];
} XAudio2AudioSystem;

XAudio2AudioSystem* XAudio2AudioSystem_create(void);
void XAudio2AudioSystem_onRoomChanged(AudioSystem* audio, int32_t roomIndex, const char* roomName);