#pragma once
// Audio conversion: decode ANY common audio container (ogg / wav / mp3 / flac) from memory to interleaved
// s16 PCM, and (for the cook) re-wrap to a WAV the device's FMOD auto-detects. ogg goes through the proven
// stb_vorbis path; wav/mp3/flac go through miniaudio's built-in decoders (impl already linked via miniaudio_impl.c).
// This is what lets a user drop in a .wav/.mp3/.flac theme instead of only a pre-made .ogg.
#include <vector>
#include <cstdint>
#include <cstring>
#include <string>
#include "miniaudio.h"

extern "C" int stb_vorbis_decode_memory(const unsigned char* mem, int len,
                                        int* channels, int* sample_rate, short** output);

namespace audioconv {

struct Pcm {
    std::vector<int16_t> samples;   // interleaved s16
    int channels = 0, sampleRate = 0;
    uint64_t frames() const { return channels ? samples.size() / (uint64_t)channels : 0; }
    bool empty() const { return samples.empty(); }
};

// Detect by magic bytes (FMOD/miniaudio both content-detect, but we route ogg to stb_vorbis explicitly).
inline const char* sniff(const uint8_t* in, size_t len) {
    if (len >= 4 && in[0]=='O'&&in[1]=='g'&&in[2]=='g'&&in[3]=='S') return "ogg";
    if (len >= 4 && in[0]=='R'&&in[1]=='I'&&in[2]=='F'&&in[3]=='F') return "wav";
    if (len >= 4 && in[0]=='f'&&in[1]=='L'&&in[2]=='a'&&in[3]=='C') return "flac";
    if (len >= 3 && in[0]=='I'&&in[1]=='D'&&in[2]=='3') return "mp3";
    if (len >= 2 && in[0]==0xFF && (in[1]&0xE0)==0xE0) return "mp3";
    return "?";
}

// FMOD on Quest natively reads these containers -> the cook can ship them raw (small). Anything else we transcode to WAV.
inline bool fmodNative(const char* fmt) {
    return fmt && (!strcmp(fmt,"ogg") || !strcmp(fmt,"wav") || !strcmp(fmt,"mp3"));
}

// Decode any supported container -> s16 PCM. ogg via stb_vorbis; wav/mp3/flac via miniaudio.
inline bool decode(const uint8_t* in, size_t len, Pcm& out, std::string* err = nullptr) {
    if (!in || len < 4) { if (err) *err = "empty audio buffer"; return false; }
    if (!strcmp(sniff(in, len), "ogg")) {
        int ch = 0, sr = 0; short* o = nullptr;
        int fr = stb_vorbis_decode_memory(in, (int)len, &ch, &sr, &o);
        if (fr > 0 && o && ch > 0) { out.channels = ch; out.sampleRate = sr; out.samples.assign(o, o + (size_t)fr * ch); free(o); return true; }
        if (o) free(o);
        if (err) *err = "ogg/vorbis decode failed"; return false;
    }
    ma_decoder dec;
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_s16, 0, 0);   // keep source channels + sample rate
    if (ma_decoder_init_memory(in, len, &cfg, &dec) != MA_SUCCESS) { if (err) *err = "unsupported audio format"; return false; }
    out.channels = (int)dec.outputChannels; out.sampleRate = (int)dec.outputSampleRate;
    const ma_uint64 CHUNK = 8192; std::vector<int16_t> buf((size_t)CHUNK * out.channels); ma_uint64 read = 0;
    do {
        if (ma_decoder_read_pcm_frames(&dec, buf.data(), CHUNK, &read) != MA_SUCCESS) break;
        out.samples.insert(out.samples.end(), buf.begin(), buf.begin() + (size_t)read * out.channels);
    } while (read == CHUNK);
    ma_decoder_uninit(&dec);
    if (out.samples.empty()) { if (err) *err = "no PCM frames decoded"; return false; }
    return true;
}

// PCM16 -> 16-bit PCM WAV (RIFF) bytes. FMOD createSound auto-detects WAV; used to ship transcoded (e.g. flac) audio.
inline std::vector<uint8_t> toWav(const Pcm& p) {
    std::vector<uint8_t> out;
    uint32_t dataBytes = (uint32_t)(p.samples.size() * 2), sr = (uint32_t)p.sampleRate;
    uint16_t ch = (uint16_t)p.channels; uint32_t byteRate = sr * ch * 2; uint16_t blockAlign = (uint16_t)(ch * 2);
    out.reserve(44 + dataBytes);
    auto w32 = [&](uint32_t v){ for (int i = 0; i < 4; i++) out.push_back((uint8_t)(v >> (8*i))); };
    auto w16 = [&](uint16_t v){ out.push_back((uint8_t)v); out.push_back((uint8_t)(v >> 8)); };
    auto ws  = [&](const char* s){ out.insert(out.end(), s, s + 4); };
    ws("RIFF"); w32(36 + dataBytes); ws("WAVE");
    ws("fmt "); w32(16); w16(1); w16(ch); w32(sr); w32(byteRate); w16(blockAlign); w16(16);
    ws("data"); w32(dataBytes);
    const uint8_t* pb = (const uint8_t*)p.samples.data(); out.insert(out.end(), pb, pb + dataBytes);
    return out;
}

} // namespace audioconv
