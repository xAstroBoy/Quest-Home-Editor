#pragma once
// Ambient audio: decode the env's looping .ogg (Ogg Vorbis, e.g. _BACKGROUND_LOOP.ogg) with
// stb_vorbis, then play the raw PCM on a miniaudio device, looping forever. Self-contained.
#include <vector>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "miniaudio.h"

// stb_vorbis: decode a whole Ogg Vorbis buffer to interleaved int16 PCM (impl in stb_vorbis_impl.c).
extern "C" int stb_vorbis_decode_memory(const unsigned char* mem, int len,
                                        int* channels, int* sample_rate, short** output);

struct AudioPlayer {
    ma_device device{};
    std::vector<short> pcm;          // interleaved s16, the whole decoded loop
    ma_uint32 channels = 0, sampleRate = 0;
    ma_uint64 totalFrames = 0, cursor = 0;
    bool ok = false;

    // Audio thread: copy frames from the decoded PCM, wrapping at the end (seamless loop).
    static void dataCb(ma_device* d, void* out, const void* in, ma_uint32 frames) {
        (void)in;
        AudioPlayer* self = (AudioPlayer*)d->pUserData;
        short* o = (short*)out;
        const ma_uint64 ch = self->channels;
        if (self->totalFrames == 0) { for (ma_uint32 i=0;i<frames*ch;++i) o[i]=0; return; }
        for (ma_uint32 f = 0; f < frames; ++f) {
            if (self->cursor >= self->totalFrames) self->cursor = 0;
            for (ma_uint64 c = 0; c < ch; ++c) o[f*ch + c] = self->pcm[self->cursor*ch + c];
            ++self->cursor;
        }
    }

    bool start(const uint8_t* data, size_t n, float /*vol*/ = 1.0f) {
        if (!data || !n) return false;
        int chs = 0, sr = 0; short* out = nullptr;
        int frames = stb_vorbis_decode_memory(data, (int)n, &chs, &sr, &out);
        if (frames <= 0 || !out || chs <= 0) { fprintf(stderr, "[AUDIO] vorbis decode failed\n"); if(out)free(out); return false; }
        channels = (ma_uint32)chs; sampleRate = (ma_uint32)sr; totalFrames = (ma_uint64)frames;
        pcm.assign(out, out + (size_t)frames * chs);
        free(out);
        ma_device_config c = ma_device_config_init(ma_device_type_playback);
        c.playback.format   = ma_format_s16;
        c.playback.channels = (ma_uint32)chs;
        c.sampleRate        = (ma_uint32)sr;
        c.dataCallback      = dataCb;
        c.pUserData         = this;
        if (ma_device_init(NULL, &c, &device) != MA_SUCCESS) { fprintf(stderr, "[AUDIO] device init failed\n"); return false; }
        if (ma_device_start(&device) != MA_SUCCESS) { ma_device_uninit(&device); return false; }
        ok = true;
        fprintf(stderr, "[AUDIO] looping ambient: %d Hz, %d ch, %d frames (%.1fs)\n",
                sr, chs, frames, frames / (float)(sr > 0 ? sr : 1));
        return true;
    }
    void stop() { if (ok) { ma_device_uninit(&device); ok = false; } }
    ~AudioPlayer() { stop(); }
};
