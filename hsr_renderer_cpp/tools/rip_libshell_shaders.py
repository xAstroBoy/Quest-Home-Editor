#!/usr/bin/env python3
# rip_libshell_shaders.py — extract EVERY embedded GLSL shader source bundled in libshell.so.
#
# libshell ships its shaders as GLSL SOURCE STRINGS embedded in the .so (the runtime shader factory —
# ShellEnv/SpecIbl/etc. — concatenates a #version header + #define feature flags + these body strings, then
# compiles them). So the whole shader SYSTEM is rippable as plaintext: this scans the binary for every
# printable, null/newline-delimited run that carries GLSL markers, dedups, classifies vert/frag/header, and
# writes each to a .glsl. NO guessing — this is the exact source the device compiles.
#
# Usage:
#   python tools/rip_libshell_shaders.py <libshell.so> [outdir]
#   python tools/rip_libshell_shaders.py --all                 # rip BOTH the V79 + V205 libshell copies
import sys, os, re, hashlib

# strong GLSL markers — a string containing ANY of these is treated as a shader (or a shader fragment).
MARKERS = [b'void main(', b'gl_Position', b'gl_FragColor', b'#version', b'#extension GL_',
           b'texture2D(', b'textureLod(', b'gl_FragCoord', b'layout(location', b'gl_ViewID_OVR',
           b'attribute ', b'varying ', b'uniform sampler', b'oBaseTexCoord', b'fragColor']
# a run must be at least this long AND contain >=1 marker to count
MIN_LEN = 48

def printable_runs(data):
    # yield (offset, bytes) for each maximal run of printable ASCII + \n\t (shader source), delimited by NUL / junk
    run_start = None
    n = len(data)
    for i in range(n):
        c = data[i]
        ok = (32 <= c < 127) or c in (9, 10, 13)
        if ok:
            if run_start is None: run_start = i
        else:
            if run_start is not None and i - run_start >= MIN_LEN:
                yield run_start, data[run_start:i]
            run_start = None
    if run_start is not None and n - run_start >= MIN_LEN:
        yield run_start, data[run_start:n]

def classify(txt):
    t = txt
    is_frag = (b'gl_FragColor' in t or b'fragColor' in t or b'oBaseTexCoord' in t or
               (b'texture' in t and b'gl_Position' not in t))
    is_vert = b'gl_Position' in t or b'gl_ViewID_OVR' in t
    if is_vert and not is_frag: return 'vert'
    if is_frag and not is_vert: return 'frag'
    if is_vert and is_frag:     return 'both'   # a combined/uber source
    return 'glsl'

def name_hint(txt):
    # a leading "// Name" comment, or a distinctive uniform/function, else empty
    m = re.search(rb'//\s*([A-Za-z0-9_\- ]{3,40})', txt)
    if m: return m.group(1).decode('ascii','ignore').strip().replace(' ','_')
    m = re.search(rb'\b(MeshShellEnv\w*|ShellEnv\w*|SpecIbl\w*|Holepunch\w*|Modmap\w*|Ghostly\w*|MaskDepth\w*)\b', txt)
    if m: return m.group(1).decode('ascii','ignore')
    return ''

def rip(so_path, outdir):
    data = open(so_path, 'rb').read()
    os.makedirs(outdir, exist_ok=True)
    seen = {}   # content-hash -> filename
    idx = []
    for off, run in printable_runs(data):
        if not any(mk in run for mk in MARKERS): continue
        # trim to the GLSL-ish region: from the first marker's line-ish start
        txt = run
        h = hashlib.sha1(txt).hexdigest()[:10]
        if h in seen: continue
        st = classify(txt)
        hint = name_hint(txt)
        suf = ('.%s.glsl' % st) if st != 'glsl' else '.glsl'
        fn = ('%s_%s%s' % (hint, h, suf)) if hint else ('%s%s' % (h, suf))
        path = os.path.join(outdir, fn)
        open(path, 'wb').write(txt)
        seen[h] = fn
        idx.append((off, len(txt), st, fn, txt[:60].decode('ascii','ignore').replace('\n',' ')))
    # write an index
    idx.sort()
    with open(os.path.join(outdir, '_INDEX.txt'), 'w') as f:
        f.write('# %d unique shader sources ripped from %s\n' % (len(idx), so_path))
        f.write('# offset      len  stage  file                                  preview\n')
        for off, ln, st, fn, prev in idx:
            f.write('0x%08x %6d  %-5s  %-38s  %s\n' % (off, ln, st, fn, prev))
    print('%-60s %3d shaders -> %s/' % (os.path.basename(os.path.dirname(so_path)) or so_path, len(idx), outdir))
    return len(idx)

def find_libshells():
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
    out = []
    for dp, dns, fns in os.walk(root):
        if 'libshell.so' in fns and ('VRShell' in dp or 'vrshell' in dp):
            out.append(os.path.join(dp, 'libshell.so'))
    return out

if __name__ == '__main__':
    if len(sys.argv) >= 2 and sys.argv[1] == '--all':
        for so in find_libshells():
            tag = 'v79' if 'V79' in so or 'Old' in so else ('v205' if 'V205' in so else hashlib.sha1(so.encode()).hexdigest()[:6])
            rip(so, os.path.join(os.path.dirname(__file__), '..', 'ripped_libshell_shaders', tag))
    elif len(sys.argv) >= 2:
        rip(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else '_ripped_shaders')
    else:
        print(__doc__)
