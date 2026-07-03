#pragma once
// Audio conversion: decode ANY common audio container (ogg-vorbis / ogg-OPUS / wav / mp3 / flac) from
// memory to interleaved s16 PCM, and (for the cook) re-wrap to a WAV the device's FMOD auto-detects.
// ogg-vorbis goes through the proven stb_vorbis path; ogg-OPUS (HALF the official V79 envs: storybook,
// polarvillage, underwater, ...) through libopus with a hand-rolled Ogg page demux; wav/mp3/flac through
// miniaudio's built-in decoders (impl already linked via miniaudio_impl.c).
#include <vector>
#include <cstdint>
#include <cstring>
#include <string>
#include "miniaudio.h"
#include <opus.h>
#include <opus_multistream.h>   // official env loops are often MULTICHANNEL opus (storybook = 4ch ambisonic)

extern "C" int stb_vorbis_decode_memory(const unsigned char* mem, int len,
                                        int* channels, int* sample_rate, short** output);

namespace audioconv {

struct Pcm {
    std::vector<int16_t> samples;   // interleaved s16
    int channels = 0, sampleRate = 0;
    uint64_t frames() const { return channels ? samples.size() / (uint64_t)channels : 0; }
    bool empty() const { return samples.empty(); }
};

// Is this OggS stream OPUS (vs vorbis)? The first page's first packet starts with "OpusHead".
inline bool oggIsOpus(const uint8_t* in, size_t len) {
    if (len < 47 || memcmp(in, "OggS", 4) != 0) return false;
    uint8_t nseg = in[26];
    size_t body = 27 + nseg;                 // first packet body starts right after the segment table
    return body + 8 <= len && memcmp(in + body, "OpusHead", 8) == 0;
}

// Detect by magic bytes (FMOD/miniaudio both content-detect, but we route ogg explicitly).
inline const char* sniff(const uint8_t* in, size_t len) {
    if (len >= 4 && in[0]=='O'&&in[1]=='g'&&in[2]=='g'&&in[3]=='S') return oggIsOpus(in,len) ? "ogg-opus" : "ogg";
    if (len >= 4 && in[0]=='R'&&in[1]=='I'&&in[2]=='F'&&in[3]=='F') return "wav";
    if (len >= 4 && in[0]=='f'&&in[1]=='L'&&in[2]=='a'&&in[3]=='C') return "flac";
    if (len >= 3 && in[0]=='I'&&in[1]=='D'&&in[2]=='3') return "mp3";
    if (len >= 2 && in[0]==0xFF && (in[1]&0xE0)==0xE0) return "mp3";
    return "?";
}

// FMOD on Quest natively reads these containers -> the cook can ship them raw (small).
// ogg-OPUS is NOT on the list: FMOD's ogg reader is vorbis-only -> the cook transcodes it to WAV.
inline bool fmodNative(const char* fmt) {
    return fmt && (!strcmp(fmt,"ogg") || !strcmp(fmt,"wav") || !strcmp(fmt,"mp3"));
}

// ── Ogg-OPUS -> s16 PCM (libopus MULTISTREAM + minimal Ogg paging) ─────────────────────────────
// Ogg framing: 27-byte page header + segment table (lacing values); a packet = segments up to one
// <255. Packet 1 = "OpusHead": ver@8, channels@9, pre-skip@10..11 LE, inputRate@12..15, gain@16..17,
// mappingFamily@18 (+ streamCount@19, coupledCount@20, mapping[ch]@21 when family != 0). Packet 2 =
// "OpusTags"; every later packet is an Opus frame. Opus always decodes at 48 kHz. The MULTISTREAM
// decoder is required: official env loops are often >2 channels (storybook = 4ch), and plain
// opus_decoder_create rejects those. >2ch is downmixed to stereo for playback + the WAV cook.
inline bool decodeOpusOgg(const uint8_t* in, size_t len, Pcm& out, std::string* err) {
    int channels = 0, preSkip = 0;
    OpusMSDecoder* dec = nullptr;
    std::vector<uint8_t> packet;
    int pktIndex = 0;
    std::vector<int16_t> frame;   // per-packet decode buffer (max 120ms @ 48k * channels)
    size_t p = 0;
    bool ok = true;
    auto handlePacket = [&](const std::vector<uint8_t>& pkt) {
        if (pkt.empty()) return;
        if (pktIndex == 0) {                      // OpusHead
            if (pkt.size() >= 19 && memcmp(pkt.data(), "OpusHead", 8) == 0) {
                channels = pkt[9];
                preSkip  = pkt[10] | (pkt[11] << 8);
                uint8_t family = pkt[18];
                int streams = 1, coupled = (channels == 2) ? 1 : 0;
                unsigned char mapping[255] = {0, 1};
                if (family != 0 && pkt.size() >= (size_t)21 + channels) {
                    streams = pkt[19]; coupled = pkt[20];
                    for (int c = 0; c < channels; ++c) mapping[c] = pkt[21 + c];
                }
                int e = 0;
                dec = opus_multistream_decoder_create(48000, channels, streams, coupled, mapping, &e);
                if (e != OPUS_OK) { dec = nullptr; ok = false; }
                frame.resize((size_t)5760 * (channels > 0 ? channels : 1));
            } else ok = false;
        } else if (pktIndex >= 2 && dec) {        // skip OpusTags (packet 1); audio from packet 2 on
            int n = opus_multistream_decode(dec, pkt.data(), (opus_int32)pkt.size(), frame.data(), 5760, 0);
            if (n > 0) out.samples.insert(out.samples.end(), frame.begin(), frame.begin() + (size_t)n * channels);
        }
        ++pktIndex;
    };
    while (ok && p + 27 <= len) {
        if (memcmp(in + p, "OggS", 4) != 0) { ++p; continue; }   // resync (robust to junk)
        uint8_t nseg = in[p + 26];
        size_t seg = p + 27;
        if (seg + nseg > len) break;
        size_t body = seg + nseg;
        for (uint8_t s = 0; s < nseg && ok; ++s) {
            uint8_t lace = in[seg + s];
            if (body + lace > len) { ok = false; break; }
            packet.insert(packet.end(), in + body, in + body + lace);
            body += lace;
            if (lace < 255) { handlePacket(packet); packet.clear(); }   // <255 terminates the packet
        }
        p = body;
    }
    if (!packet.empty()) handlePacket(packet);
    if (dec) opus_multistream_decoder_destroy(dec);
    if (channels <= 0 || out.samples.empty()) { if (err) *err = "ogg/OPUS decode failed"; return false; }
    // drop the encoder pre-skip (junk priming samples at the start)
    size_t skip = (size_t)preSkip * channels;
    if (skip && skip < out.samples.size()) out.samples.erase(out.samples.begin(), out.samples.begin() + skip);
    out.channels = channels; out.sampleRate = 48000;
    // >2ch (ambisonic/quad) -> STEREO downmix: average odd/even channels (WASAPI playback + the
    // device's FMOD WAV both want plain stereo, and the spatial mix is meaningless on a flat loop).
    if (channels > 2) {
        size_t nf = out.samples.size() / channels;
        std::vector<int16_t> st(nf * 2);
        for (size_t f = 0; f < nf; ++f) {
            int l = 0, r = 0, nl = 0, nr = 0;
            for (int c = 0; c < channels; ++c) { int v = out.samples[f*channels + c]; if (c & 1) { r += v; ++nr; } else { l += v; ++nl; } }
            st[f*2]   = (int16_t)(nl ? l / nl : 0);
            st[f*2+1] = (int16_t)(nr ? r / nr : 0);
        }
        out.samples = std::move(st); out.channels = 2;
    }
    return true;
}

// Decode any supported container -> s16 PCM. ogg-vorbis via stb_vorbis; ogg-OPUS via libopus; wav/mp3/flac via miniaudio.
inline bool decode(const uint8_t* in, size_t len, Pcm& out, std::string* err = nullptr) {
    if (!in || len < 4) { if (err) *err = "empty audio buffer"; return false; }
    const char* fmt = sniff(in, len);
    if (!strcmp(fmt, "ogg-opus")) return decodeOpusOgg(in, len, out, err);
    if (!strcmp(fmt, "ogg")) {
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
