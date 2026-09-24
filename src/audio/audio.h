#pragma once
#include <string>

namespace audio {
bool Init();
void Shutdown();
void Pump();
void PlayEffect(const std::string& path, float volume = 1.f, unsigned delayMs = 0);
void PlayMusic(const std::string& path, float volume = 0.45f);
void StopMusic();
void ToggleMusic();
bool MusicPaused();
bool ConsumeMusicFinished();
}
