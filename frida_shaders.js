/* Rip the V203 runtime-generated GLSL shaders. Hook glCreateShader (type) + glShaderSource (source),
 * correlate by shader id, write each to /data/local/tmp/v203_shaders/. Race-attach so the env's
 * compile-on-load is caught. */
'use strict';
const TAG = '[SHADER]';
const OUT = '/data/local/tmp/v203_shaders/';
function findExp(name) { for (const m of Process.enumerateModules()) { try { const e = m.findExportByName(name); if (e) return e; } catch (e) {} } return null; }

const types = {};                 // shader id -> 'vert'/'frag'
let n = 0;
const gcs = findExp('glCreateShader');
const gss = findExp('glShaderSource');
console.log(TAG, 'glCreateShader@' + gcs + ' glShaderSource@' + gss);

if (gcs) Interceptor.attach(gcs, {
  onEnter(a) { this.t = a[0].toInt32(); },
  onLeave(r) { const id = r.toInt32(); types[id] = (this.t === 0x8B31) ? 'vert' : (this.t === 0x8B30) ? 'frag' : ('t' + this.t.toString(16)); }
});

if (gss) Interceptor.attach(gss, {
  onEnter(args) {
    const shader = args[0].toInt32(), count = args[1].toInt32(), strings = args[2], lengths = args[3];
    let src = '';
    for (let i = 0; i < count; i++) {
      const p = strings.add(i * Process.pointerSize).readPointer();
      if (p.isNull()) continue;
      try {
        if (!lengths.isNull()) { const len = lengths.add(i * 4).readInt(); if (len >= 0) { src += p.readUtf8String(len); continue; } }
        src += p.readCString();
      } catch (e) {}
    }
    n++;
    const type = types[shader] || 'unk';
    send({ kind: 'shader', n: n, type: type, src: src });   // to PC, no device perms
    console.log(TAG, 'sent #' + n + ' ' + type + ' (len ' + src.length + ')');
  }
});

// path counters: which shader-load route does V203 use?
let progBin = 0, linkProg = 0, compShader = 0, vkMod = 0;
// NEUTRALIZE the program-binary cache: REPLACE glProgramBinary with a no-op so the binary is never
// loaded -> GL_LINK_STATUS is FALSE -> the engine recompiles from GLSL -> glShaderSource fires.
const gpb = findExp('glProgramBinary');
if (gpb) Interceptor.replace(gpb, new NativeCallback(function (program, fmt, bin, len) { progBin++; }, 'void', ['uint', 'uint', 'pointer', 'int']));
const glp = findExp('glLinkProgram');  if (glp) Interceptor.attach(glp, { onEnter() { linkProg++; } });
const gcsh = findExp('glCompileShader'); if (gcsh) Interceptor.attach(gcsh, { onEnter() { compShader++; } });
const vcsm = findExp('vkCreateShaderModule'); if (vcsm) Interceptor.attach(vcsm, { onEnter() { vkMod++; } });

console.log(TAG, gss ? 'hooked glShaderSource + path counters; waiting...' : 'glShaderSource NOT FOUND (Vulkan/SPIR-V path?)');
setTimeout(() => { console.log(TAG, 'DONE captured=' + n + ' glProgramBinary=' + progBin + ' glLinkProgram=' + linkProg + ' glCompileShader=' + compShader + ' vkCreateShaderModule=' + vkMod); }, 22000);
