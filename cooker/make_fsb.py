#!/usr/bin/env python
"""V79 background-audio -> V203 FMOD-FSB5 converter (NO fsbank.exe needed).

V203 home audio = FMOD. The shell's `audio_fmod::asset::SoundDefinition` FlatBuffer embeds the audio bytes in an
FMOD-FSB container (dataFormat enum = Vorbis/FADPCM/Opus, all FSB codecs) and feeds them to FMOD createSound.
FMOD's own `fsbank` tool (needed to make Vorbis/FADPCM banks) is a gated download we don't have — so instead we
build a minimal **FSB5 / PCM16** bank by hand (FSB5 supports raw PCM16 = codec mode 2; a single subsound has data
offset 0, so the bitfields are trivial). The shipped FMOD runtime reads the codec from the FSB5 header, so PCM16
plays without any encoder. We decode the V79 `_BACKGROUND_LOOP.ogg` to PCM16 with the FMOD runtime itself
(fmod_toolkit/fmod.dll), optionally downmix to mono + downsample to keep the APK small, then wrap it in FSB5.

Usage: python make_fsb.py <in.ogg> <out.fsb> [rate=22050] [mono=1]
The result is validated by reloading it through FMOD createSound(OPENMEMORY) — the same FMOD the shell uses.
"""
import sys, os, struct, wave, io
import numpy as np

IN   = sys.argv[1] if len(sys.argv) > 1 else "_audio_tmp/_BACKGROUND_LOOP.ogg"
OUT  = sys.argv[2] if len(sys.argv) > 2 else "_audio_tmp/_BACKGROUND_LOOP.fsb"
RATE = int(sys.argv[3]) if len(sys.argv) > 3 else 22050
MONO = (len(sys.argv) <= 4) or sys.argv[4] != "0"

# FSB5 frequency enum (index into this table); FMOD remaps off-list rates, but matching avoids a resample.
FREQ_ENUM = {8000:1, 11000:2, 11025:3, 16000:4, 22050:5, 24000:6, 32000:7, 44100:8, 48000:9, 64000:10}
PCM16 = 2   # FSB5 codec mode

def main():
    import fmod_toolkit
    from pyfmodex.flags import INIT_FLAGS, MODE
    from pyfmodex.enums import TIMEUNIT
    import ctypes
    from pyfmodex.fmodex import _dll
    system,_ = fmod_toolkit.get_pyfmodex_system_instance(32, INIT_FLAGS.NORMAL)
    snd = system.create_sound(os.path.abspath(IN), mode=MODE.OPENONLY|MODE.CREATESAMPLE|MODE.ACCURATETIME)
    s_type=ctypes.c_int(); s_fmt=ctypes.c_int(); s_ch=ctypes.c_int(); s_bits=ctypes.c_int()
    _dll.FMOD_Sound_GetFormat(snd._ptr, ctypes.byref(s_type), ctypes.byref(s_fmt), ctypes.byref(s_ch), ctypes.byref(s_bits))
    channels = s_ch.value; bits = s_bits.value
    freq = ctypes.c_float(44100.0); prio = ctypes.c_int()
    _dll.FMOD_Sound_GetDefaults(snd._ptr, ctypes.byref(freq), ctypes.byref(prio))
    srcRate = int(round(freq.value)) or 44100
    nbytes = snd.get_length(TIMEUNIT.PCMBYTES)
    (ptr1, len1), (ptr2, len2) = snd.lock(0, nbytes)
    raw = ctypes.string_at(ptr1.value, len1.value)
    if ptr2.value and len2.value: raw += ctypes.string_at(ptr2.value, len2.value)
    snd.unlock((ptr1, len1), (ptr2, len2))
    pcm = np.frombuffer(raw, dtype=np.int16).astype(np.float32)
    if channels >= 2:
        pcm = pcm.reshape(-1, channels)
        if MONO: pcm = pcm.mean(axis=1, keepdims=True); channels = 1
    else:
        pcm = pcm.reshape(-1, 1)
    # resample (linear) to RATE
    outCh = pcm.shape[1]
    if RATE != srcRate:
        n_out = int(pcm.shape[0] * RATE / srcRate)
        xi = np.linspace(0, pcm.shape[0]-1, n_out)
        x0 = np.floor(xi).astype(np.int64); x1 = np.minimum(x0+1, pcm.shape[0]-1); fr = (xi-x0)[:,None]
        pcm = pcm[x0]*(1-fr) + pcm[x1]*fr
    pcm16 = np.clip(np.round(pcm), -32768, 32767).astype('<i2')
    data = pcm16.tobytes()
    numSamplesPerCh = pcm16.shape[0]
    rate = RATE
    print("decoded %d ch @%dHz -> %d ch @%dHz, %d samples (%.1fs), %d KB PCM16"
          % (channels, srcRate, outCh, rate, numSamplesPerCh, numSamplesPerCh/rate, len(data)//1024))

    # ---- build FSB5 (1 subsound, PCM16) ----
    freqEnum = FREQ_ENUM.get(rate, 8)              # fall back to 44100 enum if odd rate (FMOD still resamples)
    chanBit  = 1 if outCh == 2 else 0
    # sample header (8 bytes): nextChunk(1)|freq(4)|chan(1)|dataOffset(28,=0)|numSamples(30)
    mode64 = (0 & 1) | ((freqEnum & 0xF) << 1) | ((chanBit & 1) << 5) | ((0 & 0xFFFFFFF) << 6) | ((numSamplesPerCh & 0x3FFFFFFF) << 34)
    sampleHeader = struct.pack("<Q", mode64)
    dataPadded = data + b"\x00" * ((-len(data)) % 32)
    sampleHeadersSize = len(sampleHeader)
    nameTableSize = 0
    header = b"FSB5" + struct.pack("<IIIIII", 1, 1, sampleHeadersSize, nameTableSize, len(dataPadded), PCM16)
    header += b"\x00"*8 + b"\x00"*16 + b"\x00"*8       # zero(8) + hash(16) + dummy(8) = 60-byte v1 header
    fsb = header + sampleHeader + dataPadded
    open(OUT, "wb").write(fsb)
    print("wrote %s (%d KB, FSB5 PCM16 %dch @%dHz)" % (OUT, len(fsb)//1024, outCh, rate))

    # ---- validate: reload through FMOD from memory (same runtime the shell uses) ----
    exinfo = None
    try:
        from pyfmodex.structobject import Structobject
        from pyfmodex.structures import CREATESOUNDEXINFO
        ex = CREATESOUNDEXINFO(); ex.cbsize = ctypes.sizeof(CREATESOUNDEXINFO); ex.length = len(fsb)
        v = system.create_sound(fsb, mode=MODE.OPENMEMORY, exinfo=ex)
        n = v.num_subsounds
        sub = v.get_subsound(0) if n else v
        print("VALIDATE: FMOD loaded FSB5 OK — subsounds=%d, subsound len=%d samples, format=%s"
              % (n, sub.get_length(TIMEUNIT.PCM), sub.format))
        return 0
    except Exception as e:
        import traceback; traceback.print_exc()
        print("VALIDATE FAILED")
        return 1

if __name__ == "__main__":
    sys.exit(main())
