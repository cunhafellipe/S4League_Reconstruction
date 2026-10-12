#include "audio/audio.h"
#include "app/dbg.h"
#include <windows.h>
#include <xaudio2.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>
#include "../../third_party/stb_vorbis.c"

namespace audio {
struct Clip { WAVEFORMATEX format = {}; std::vector<BYTE> pcm; };
struct Playing { IXAudio2SourceVoice* voice = nullptr; std::shared_ptr<Clip> clip; bool music = false; };
struct Pending { std::string path; float volume; DWORD due; };

static IXAudio2* g_xaudio = nullptr;
static IXAudio2MasteringVoice* g_master = nullptr;
static std::unordered_map<std::string, std::shared_ptr<Clip>> g_cache;
static std::vector<Playing> g_playing;
static std::vector<Pending> g_pending;
static IXAudio2SourceVoice* g_music = nullptr;
static bool g_musicPaused = false, g_musicFinished = false, g_comInit = false;

static std::string LocalPath(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    if (path.rfind("Resources/", 0) == 0 || path.rfind("resources/", 0) == 0)
        path = "resources/" + path;
    // Some data drops the "Resources/" prefix (bginfo [BGM] "Bgm/battle1.ogg",
    // weapon Lua "Sound/..."): those live under resources/resources/ too.
    else if (path.find(':') == std::string::npos && path.rfind("/", 0) != 0)
        path = "resources/resources/" + path;
    return path;
}

static std::shared_ptr<Clip> Decode(const std::string& requested) {
    const std::string path = LocalPath(requested);
    auto found = g_cache.find(path);
    if (found != g_cache.end()) return found->second;

    int channels = 0, sampleRate = 0;
    short* pcm = nullptr;
    int samples = stb_vorbis_decode_filename(path.c_str(), &channels, &sampleRate, &pcm);
    if (samples <= 0 || !pcm || channels <= 0 || sampleRate <= 0) {
        DbgLog("[audio] OGG decode FAIL '%s'", path.c_str());
        std::free(pcm);
        return {};
    }
    auto clip = std::make_shared<Clip>();
    clip->format.wFormatTag = WAVE_FORMAT_PCM;
    clip->format.nChannels = (WORD)channels;
    clip->format.nSamplesPerSec = sampleRate;
    clip->format.wBitsPerSample = 16;
    clip->format.nBlockAlign = (WORD)(channels * sizeof(short));
    clip->format.nAvgBytesPerSec = sampleRate * clip->format.nBlockAlign;
    const size_t bytes = (size_t)samples * channels * sizeof(short);
    clip->pcm.assign(reinterpret_cast<BYTE*>(pcm), reinterpret_cast<BYTE*>(pcm) + bytes);
    std::free(pcm);
    g_cache[path] = clip;
    DbgLog("[audio] decoded '%s' bytes=%zu", path.c_str(), clip->pcm.size());
    return clip;
}

static IXAudio2SourceVoice* Start(const std::string& path, float volume, bool music) {
    if (!g_xaudio) return nullptr;
    auto clip = Decode(path);
    if (!clip) return nullptr;
    IXAudio2SourceVoice* voice = nullptr;
    if (FAILED(g_xaudio->CreateSourceVoice(&voice, &clip->format))) return nullptr;
    XAUDIO2_BUFFER buffer = {};
    buffer.AudioBytes = (UINT32)clip->pcm.size();
    buffer.pAudioData = clip->pcm.data();
    buffer.Flags = XAUDIO2_END_OF_STREAM;
    voice->SetVolume((std::max)(0.f, (std::min)(1.f, volume)));
    if (FAILED(voice->SubmitSourceBuffer(&buffer)) || FAILED(voice->Start())) { voice->DestroyVoice(); return nullptr; }
    g_playing.push_back({ voice, std::move(clip), music });
    DbgLog("[audio] playing %s '%s'", music ? "BGM" : "SFX", LocalPath(path).c_str());
    return voice;
}

bool Init() {
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    g_comInit = SUCCEEDED(co);
    if (FAILED(co) && co != RPC_E_CHANGED_MODE) { DbgLog("[audio] COM FAIL hr=0x%08X", (unsigned)co); return false; }
    HRESULT hr = XAudio2Create(&g_xaudio);
    if (FAILED(hr)) { DbgLog("[audio] XAudio2Create FAIL hr=0x%08X", (unsigned)hr); return false; }
    hr = g_xaudio->CreateMasteringVoice(&g_master);
    if (FAILED(hr)) { DbgLog("[audio] CreateMasteringVoice FAIL hr=0x%08X", (unsigned)hr); return false; }
    DbgLog("[audio] stb_vorbis + XAudio2 ready");
    return true;
}

void Shutdown() {
    for (Playing& p : g_playing) if (p.voice) p.voice->DestroyVoice();
    g_playing.clear(); g_pending.clear(); g_cache.clear(); g_music = nullptr;
    if (g_master) { g_master->DestroyVoice(); g_master = nullptr; }
    if (g_xaudio) { g_xaudio->Release(); g_xaudio = nullptr; }
    if (g_comInit) { CoUninitialize(); g_comInit = false; }
}

void Pump() {
    DWORD now = GetTickCount();
    for (size_t i = g_pending.size(); i-- > 0;)
        if ((LONG)(now - g_pending[i].due) >= 0) {
            Start(g_pending[i].path, g_pending[i].volume, false);
            g_pending.erase(g_pending.begin() + i);
        }
    for (size_t i = g_playing.size(); i-- > 0;) {
        XAUDIO2_VOICE_STATE state = {};
        g_playing[i].voice->GetState(&state);
        if (state.BuffersQueued) continue;
        if (g_playing[i].music && g_playing[i].voice == g_music) { g_music = nullptr; g_musicFinished = true; }
        g_playing[i].voice->DestroyVoice();
        g_playing.erase(g_playing.begin() + i);
    }
}

void PlayEffect(const std::string& path, float volume, unsigned delayMs) {
    if (path.empty()) return;
    if (delayMs) g_pending.push_back({ path, volume, GetTickCount() + delayMs });
    else Start(path, volume, false);
}

void PlayMusic(const std::string& path, float volume) {
    StopMusic();
    g_musicFinished = false; g_musicPaused = false;
    g_music = Start(path, volume, true);
}

void StopMusic() {
    if (!g_music) return;
    auto it = std::find_if(g_playing.begin(), g_playing.end(), [](const Playing& p) { return p.voice == g_music; });
    if (it != g_playing.end()) { it->voice->DestroyVoice(); g_playing.erase(it); }
    g_music = nullptr; g_musicPaused = false; g_musicFinished = false;
}

void ToggleMusic() {
    if (!g_music) return;
    g_musicPaused = !g_musicPaused;
    if (g_musicPaused) g_music->Stop(); else g_music->Start();
}
bool MusicPaused() { return g_musicPaused; }
bool ConsumeMusicFinished() { bool value = g_musicFinished; g_musicFinished = false; return value; }
}
