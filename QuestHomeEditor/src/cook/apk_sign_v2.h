// ── Self-contained APK signer — NO Java (no keytool, no apksigner) ─────────────────────────────────────────
// Implements APK Signature Scheme v2 (id 0x7109871a) with a baked-in debug RSA-2048 key, plus a 4-byte
// zipalign pass. Everything is C++: SHA-256, a big-integer modexp for RSA, PKCS#1 v1.5 padding, the v2 block.
// Quest (API 29+) accepts v2-only signatures, so no v1 (JAR) signing is needed.
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <algorithm>
#include "cook/apk_debug_key.h"

namespace hslcook { namespace sign {

// ── SHA-256 (public-domain, compact) ──────────────────────────────────────────────────────────────────────
struct Sha256 {
    uint32_t s[8]; uint64_t len; uint8_t buf[64]; size_t n;
    static uint32_t ror(uint32_t x,int c){ return (x>>c)|(x<<(32-c)); }
    void init(){ static const uint32_t iv[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
                 memcpy(s,iv,32); len=0; n=0; }
    void block(const uint8_t* p){
        static const uint32_t K[64]={
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
        uint32_t w[64];
        for(int i=0;i<16;i++) w[i]=(p[i*4]<<24)|(p[i*4+1]<<16)|(p[i*4+2]<<8)|p[i*4+3];
        for(int i=16;i<64;i++){ uint32_t a=ror(w[i-15],7)^ror(w[i-15],18)^(w[i-15]>>3), b=ror(w[i-2],17)^ror(w[i-2],19)^(w[i-2]>>10);
                                w[i]=w[i-16]+a+w[i-7]+b; }
        uint32_t a=s[0],b=s[1],c=s[2],d=s[3],e=s[4],f=s[5],g=s[6],h=s[7];
        for(int i=0;i<64;i++){ uint32_t S1=ror(e,6)^ror(e,11)^ror(e,25), ch=(e&f)^(~e&g), t1=h+S1+ch+K[i]+w[i];
                               uint32_t S0=ror(a,2)^ror(a,13)^ror(a,22), mj=(a&b)^(a&c)^(b&c), t2=S0+mj;
                               h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2; }
        s[0]+=a;s[1]+=b;s[2]+=c;s[3]+=d;s[4]+=e;s[5]+=f;s[6]+=g;s[7]+=h;
    }
    void update(const uint8_t* p, size_t l){ len+=l;
        while(l){ size_t k=64-n; if(k>l)k=l; memcpy(buf+n,p,k); n+=k; p+=k; l-=k; if(n==64){ block(buf); n=0; } } }
    void final(uint8_t out[32]){ uint64_t bits=len*8; uint8_t pad=0x80; update(&pad,1);
        uint8_t z=0; while(n!=56) update(&z,1);
        uint8_t L[8]; for(int i=0;i<8;i++) L[i]=(uint8_t)(bits>>(56-i*8)); update(L,8);
        for(int i=0;i<8;i++){ out[i*4]=s[i]>>24; out[i*4+1]=s[i]>>16; out[i*4+2]=s[i]>>8; out[i*4+3]=s[i]; } }
};
inline void sha256(const uint8_t* p, size_t l, uint8_t out[32]){ Sha256 h; h.init(); h.update(p,l); h.final(out); }

// ── big-integer (base 2^32, little-endian limbs) modexp for RSA-2048 ────────────────────────────────────────
// Fixed 64 limbs for the modulus; products use 128 limbs. Enough for RSA-2048. One sign per APK → speed is fine.
struct Big {                       // little-endian: v[0] is least significant
    static const int W = 128;      // limbs (support up to 4096-bit intermediates)
    uint32_t v[W];
    Big(){ memset(v,0,sizeof v); }
    static Big fromBE(const uint8_t* b, int nbytes){ Big r; for(int i=0;i<nbytes;i++){ r.v[(nbytes-1-i)/4] |= (uint32_t)b[i] << (8*((nbytes-1-i)%4)); } return r; }
    void toBE(uint8_t* out, int nbytes) const { for(int i=0;i<nbytes;i++){ int j=nbytes-1-i; out[i]=(uint8_t)(v[j/4] >> (8*(j%4))); } }
    int bits() const { for(int i=W-1;i>=0;--i) if(v[i]){ uint32_t x=v[i]; int b=0; while(x){x>>=1;++b;} return i*32+b; } return 0; }
    bool ge(const Big& o) const { for(int i=W-1;i>=0;--i){ if(v[i]!=o.v[i]) return v[i]>o.v[i]; } return true; }
    void sub(const Big& o){ uint64_t br=0; for(int i=0;i<W;i++){ uint64_t x=(uint64_t)v[i]-o.v[i]-br; v[i]=(uint32_t)x; br=(x>>63)&1; } }
    void shl1(){ uint32_t c=0; for(int i=0;i<W;i++){ uint32_t nc=v[i]>>31; v[i]=(v[i]<<1)|c; c=nc; } }
    bool bit(int i) const { return (v[i/32]>>(i%32))&1; }
};
// r = (a*b) mod m  (schoolbook multiply into 128 limbs, then binary reduce)
inline Big mulmod(const Big& a, const Big& b, const Big& m){
    uint64_t prod[Big::W]; memset(prod,0,sizeof prod);   // 64-bit accumulators, then normalize
    // schoolbook (only lower 128 limbs matter for our sizes)
    static uint32_t P[Big::W*2]; memset(P,0,sizeof P);
    for(int i=0;i<Big::W;i++){ if(!a.v[i]) continue; uint64_t carry=0;
        for(int j=0;j<Big::W;j++){ if(i+j>=Big::W*2) break; uint64_t cur=(uint64_t)P[i+j]+(uint64_t)a.v[i]*b.v[j]+carry; P[i+j]=(uint32_t)cur; carry=cur>>32; }
    }
    (void)prod;
    // reduce P (up to 256 limbs) mod m by binary long division on the high part; but our operands are < m (<2048b),
    // so P < 2^4096 fits in 128 limbs. Copy low 128 limbs into a Big and reduce.
    Big x; for(int i=0;i<Big::W;i++) x.v[i]=P[i];
    // binary reduction: align m to x's top bit, subtract down.
    int xb=x.bits(), mb=m.bits(); if(xb<mb) return x;
    Big ms=m; int sh=xb-mb; for(int i=0;i<sh;i++) ms.shl1();
    for(int i=0;i<=sh;i++){ if(x.ge(ms)) x.sub(ms);
        // shift ms right by 1
        uint32_t c=0; for(int k=Big::W-1;k>=0;--k){ uint32_t nc=ms.v[k]&1; ms.v[k]=(ms.v[k]>>1)|(c<<31); c=nc; } }
    return x;
}
// result = base^exp mod m  (left-to-right binary)
inline Big modexp(const Big& base, const Big& exp, const Big& m){
    Big r; r.v[0]=1; int eb=exp.bits();
    for(int i=eb-1;i>=0;--i){ r=mulmod(r,r,m); if(exp.bit(i)) r=mulmod(r,base,m); }
    return r;
}

// RSA-2048 PKCS#1 v1.5 signature over SHA-256(msg). Returns 256 bytes big-endian.
inline std::vector<uint8_t> rsaSignSha256(const uint8_t* msg, size_t len){
    uint8_t hash[32]; sha256(msg,len,hash);
    // DigestInfo(SHA-256) ASN.1 prefix (RFC 8017)
    static const uint8_t PREFIX[19]={0x30,0x31,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01,0x05,0x00,0x04,0x20};
    uint8_t T[51]; memcpy(T,PREFIX,19); memcpy(T+19,hash,32);   // 19+32
    uint8_t EM[256]; EM[0]=0x00; EM[1]=0x01; int psLen=256-3-51; memset(EM+2,0xff,psLen); EM[2+psLen]=0x00; memcpy(EM+3+psLen,T,51);
    Big m = Big::fromBE(EM,256), n = Big::fromBE(kDebugModulusBE,256), d = Big::fromBE(kDebugPrivExpBE,256);
    Big sig = modexp(m,d,n);
    std::vector<uint8_t> out(256); sig.toBE(out.data(),256); return out;
}

// ── little ZIP helpers (read the EOCD; used by both zipalign + v2) ──────────────────────────────────────────
inline uint32_t rd32(const uint8_t* p){ return p[0]|(p[1]<<8)|(p[2]<<16)|((uint32_t)p[3]<<24); }
inline uint16_t rd16(const uint8_t* p){ return p[0]|(p[1]<<8); }
inline void put32(std::vector<uint8_t>& b, uint32_t v){ b.push_back(v); b.push_back(v>>8); b.push_back(v>>16); b.push_back(v>>24); }
inline void put64(std::vector<uint8_t>& b, uint64_t v){ for(int i=0;i<8;i++) b.push_back((uint8_t)(v>>(8*i))); }

// find End Of Central Directory record (0x06054b50), searching back from the end.
inline int findEOCD(const std::vector<uint8_t>& z){
    if (z.size()<22) return -1;
    for (int i=(int)z.size()-22; i>=0 && i>=(int)z.size()-22-65536; --i)
        if (z[i]==0x50&&z[i+1]==0x4b&&z[i+2]==0x05&&z[i+3]==0x06) return i;
    return -1;
}

// ── zipalign: rebuild the ZIP so every STORED (uncompressed) entry's DATA starts at a 4-byte boundary ────────
// (Android 11+ requires resources.arsc + uncompressed libs aligned; splicing shifts offsets, so re-align here.)
inline bool zipalign4(const std::vector<uint8_t>& in, std::vector<uint8_t>& out){
    int eocd = findEOCD(in); if (eocd<0) return false;
    uint16_t nrec = rd16(&in[eocd+10]);
    uint32_t cdOff = rd32(&in[eocd+16]);
    // walk central directory, copy each local entry to `out` with alignment padding for STORED entries.
    // Read the AUTHORITATIVE crc/compressed/uncompressed sizes from the CENTRAL DIRECTORY (always correct),
    // NOT the local header — miniz writes its local headers with the general-purpose DATA-DESCRIPTOR bit
    // (0x08) set and the three size fields ZERO, deferring them to a descriptor after the data. The old code
    // read comp from lo+18 (=0 there) and copied ZERO data bytes for every entry → a 3KB gutted APK (central
    // dir intact so `unzip -l` "works", but no file data → won't install / won't load). We now rewrite each
    // local header with the real sizes and CLEAR the data-descriptor bit (dropping the trailing descriptor),
    // producing a canonical, descriptor-free APK exactly like real zipalign/apksigner. Byte-identical result
    // for inputs that already had correct local headers.
    struct CE { uint32_t lho, crc, csz, usz; uint16_t method, flag; };
    std::vector<uint32_t> newLocal(nrec);
    out.clear(); out.reserve(in.size()+nrec*4);
    size_t cp = cdOff;
    std::vector<CE> ces; ces.reserve(nrec);
    for (int i=0;i<nrec;i++){ if (cp+46>in.size()||rd32(&in[cp])!=0x02014b50) return false;
        CE c; c.flag=rd16(&in[cp+8]); c.method=rd16(&in[cp+10]);
        c.crc=rd32(&in[cp+16]); c.csz=rd32(&in[cp+20]); c.usz=rd32(&in[cp+24]); c.lho=rd32(&in[cp+42]);
        uint16_t nl=rd16(&in[cp+28]), el=rd16(&in[cp+30]), cl=rd16(&in[cp+32]); ces.push_back(c); cp+=46+nl+el+cl; }
    for (int i=0;i<nrec;i++){ size_t lo=ces[i].lho; if (lo+30>in.size()||rd32(&in[lo])!=0x04034b50) return false;
        uint16_t nl=rd16(&in[lo+26]), el=rd16(&in[lo+28]);
        size_t dataStart=lo+30+nl+el; if (dataStart+ces[i].csz>in.size()) return false;
        newLocal[i]=(uint32_t)out.size();
        // for STORED entries, pad the extra field so the data offset is 4-aligned (mmap of uncompressed assets)
        size_t pad = 0;
        if (ces[i].method==0){ size_t curDataOff = out.size()+30+nl+el; pad=(4-(curDataOff&3))&3; }
        // copy the fixed 30-byte header + name + extra (+pad), then the compressed data (no descriptor)
        size_t hdrStart = out.size();
        out.insert(out.end(), in.begin()+lo, in.begin()+lo+30+nl+el);
        if (pad){ out.insert(out.begin()+hdrStart+30+nl+el, pad, 0);
                  uint16_t nel=(uint16_t)(el+pad); out[hdrStart+28]=(uint8_t)nel; out[hdrStart+29]=(uint8_t)(nel>>8); }
        // patch this fresh local header: clear data-descriptor bit, write real crc + sizes from the CD
        uint16_t nflag = (uint16_t)(ces[i].flag & ~0x0008);
        out[hdrStart+6]=(uint8_t)nflag; out[hdrStart+7]=(uint8_t)(nflag>>8);
        auto put32=[&](size_t o,uint32_t v){ out[o]=(uint8_t)v; out[o+1]=(uint8_t)(v>>8); out[o+2]=(uint8_t)(v>>16); out[o+3]=(uint8_t)(v>>24); };
        put32(hdrStart+14, ces[i].crc); put32(hdrStart+18, ces[i].csz); put32(hdrStart+22, ces[i].usz);
        out.insert(out.end(), in.begin()+dataStart, in.begin()+dataStart+ces[i].csz);
    }
    // rebuild central directory with fixed local-header offsets
    uint32_t newCdOff=(uint32_t)out.size(); cp=cdOff;
    for (int i=0;i<nrec;i++){ uint16_t nl=rd16(&in[cp+28]), el=rd16(&in[cp+30]), cl=rd16(&in[cp+32]);
        size_t start=out.size(); out.insert(out.end(), in.begin()+cp, in.begin()+cp+46+nl+el+cl);
        // clear the data-descriptor bit here too, so the CD flag matches the rewritten local header
        uint16_t cflag=(uint16_t)(rd16(&in[cp+8]) & ~0x0008); out[start+8]=(uint8_t)cflag; out[start+9]=(uint8_t)(cflag>>8);
        out[start+42]=(uint8_t)newLocal[i]; out[start+43]=(uint8_t)(newLocal[i]>>8); out[start+44]=(uint8_t)(newLocal[i]>>16); out[start+45]=(uint8_t)(newLocal[i]>>24);
        cp+=46+nl+el+cl; }
    uint32_t newCdSize=(uint32_t)out.size()-newCdOff;
    // EOCD
    std::vector<uint8_t> e(in.begin()+eocd, in.begin()+eocd+22);
    uint16_t commentLen=rd16(&e[20]); if (commentLen) e.insert(e.end(), in.begin()+eocd+22, in.begin()+eocd+22+commentLen);
    e[12]=(uint8_t)newCdSize; e[13]=newCdSize>>8; e[14]=newCdSize>>16; e[15]=newCdSize>>24;
    e[16]=(uint8_t)newCdOff; e[17]=newCdOff>>8; e[18]=newCdOff>>16; e[19]=newCdOff>>24;
    out.insert(out.end(), e.begin(), e.end());
    return true;
}

// ── APK Signature Scheme v2 ─────────────────────────────────────────────────────────────────────────────────
// Digest = chunked SHA-256: split (ZIP entries | central dir | EOCD') into <=1MB chunks; d_i = SHA256(0xa5 ||
// u32(len) || chunk); top = SHA256(0x5a || u32(nchunks) || d_0..d_n). EOCD' has its CD-offset field pointing at
// the signing block start (== original CD start, since the block sits between entries and CD).
inline void sha256Chunked(const uint8_t* data, size_t len, std::vector<uint8_t>& digestsOut, uint32_t& nchunks){
    const size_t CH=1024*1024; nchunks=0;
    for (size_t off=0; off<len || (len==0&&off==0); off+=CH){ if (len==0 && off>0) break;
        size_t clen = (len-off<CH)?(len-off):CH; if (len==0) clen=0;
        uint8_t pre[5]={0xa5,(uint8_t)clen,(uint8_t)(clen>>8),(uint8_t)(clen>>16),(uint8_t)(clen>>24)};
        Sha256 h; h.init(); h.update(pre,5); if(clen) h.update(data+off,clen);
        uint8_t dg[32]; h.final(dg); digestsOut.insert(digestsOut.end(),dg,dg+32); nchunks++;
        if (len==0) break;
    }
}
inline bool signApkV2(const std::vector<uint8_t>& alignedApk, std::vector<uint8_t>& out){
    int eocd = findEOCD(alignedApk); if (eocd<0) return false;
    uint32_t cdOff = rd32(&alignedApk[eocd+16]);
    uint32_t cdSize = rd32(&alignedApk[eocd+12]);
    size_t entriesEnd = cdOff, cdStart=cdOff, cdEnd=cdStart+cdSize;
    if (cdEnd> (size_t)eocd) return false;
    // EOCD copy with CD-offset rewritten to the signing-block start (which will equal the ORIGINAL cdOff since the
    // block sits exactly where the CD was; the CD then moves DOWN by the block size). For digest purposes the spec
    // wants EOCD' with its cd-offset field = (start of signing block). That start = cdOff after we insert. But the
    // block is inserted at `entriesEnd`, so the signing block starts at entriesEnd, and the new CD starts at
    // entriesEnd+blockSize. The EOCD used in the DIGEST must have cd-offset = entriesEnd (block start).
    std::vector<uint8_t> eocdCopy(alignedApk.begin()+eocd, alignedApk.end());
    // will patch its cd-offset (bytes 16..19) to entriesEnd below (it already equals cdOff==entriesEnd here).
    eocdCopy[16]=(uint8_t)entriesEnd; eocdCopy[17]=entriesEnd>>8; eocdCopy[18]=entriesEnd>>16; eocdCopy[19]=entriesEnd>>24;

    // 1) chunked digest over the three sections
    std::vector<uint8_t> digests; uint32_t nc=0, t=0;
    sha256Chunked(alignedApk.data(), entriesEnd, digests, t); nc+=t;
    sha256Chunked(alignedApk.data()+cdStart, cdSize, digests, t); nc+=t;
    sha256Chunked(eocdCopy.data(), eocdCopy.size(), digests, t); nc+=t;
    uint8_t top[32]; { uint8_t pre[5]={0x5a,(uint8_t)nc,(uint8_t)(nc>>8),(uint8_t)(nc>>16),(uint8_t)(nc>>24)};
        Sha256 h; h.init(); h.update(pre,5); h.update(digests.data(),digests.size()); h.final(top); }

    const uint32_t SIG_ALGO = 0x0103;   // RSASSA-PKCS1-v1.5 with SHA2-256
    // 2) signed data = seq{ digests, certs, additional-attrs }
    auto lp = [](std::vector<uint8_t>& b, const std::vector<uint8_t>& x){ put32(b,(uint32_t)x.size()); b.insert(b.end(),x.begin(),x.end()); };
    std::vector<uint8_t> digestSeq; { std::vector<uint8_t> one; put32(one,SIG_ALGO); std::vector<uint8_t> d32(top,top+32); lp(one,d32);
        put32(digestSeq,(uint32_t)one.size()); digestSeq.insert(digestSeq.end(),one.begin(),one.end()); }   // len-prefixed list of len-prefixed
    std::vector<uint8_t> certSeq; { std::vector<uint8_t> c(kDebugCertDer, kDebugCertDer+sizeof kDebugCertDer);
        put32(certSeq,(uint32_t)c.size()); certSeq.insert(certSeq.end(),c.begin(),c.end()); }
    std::vector<uint8_t> attrs;   // empty (0-length sequence)
    std::vector<uint8_t> signedData; lp(signedData,digestSeq); lp(signedData,certSeq); lp(signedData,attrs);

    // 3) signature over signedData
    std::vector<uint8_t> sig = rsaSignSha256(signedData.data(), signedData.size());
    std::vector<uint8_t> sigSeq; { std::vector<uint8_t> one; put32(one,SIG_ALGO); lp(one,sig);
        put32(sigSeq,(uint32_t)one.size()); sigSeq.insert(sigSeq.end(),one.begin(),one.end()); }
    std::vector<uint8_t> pubkey(kDebugPubKeyDer, kDebugPubKeyDer+sizeof kDebugPubKeyDer);

    // 4) signer = seq{ signed-data, signatures, public-key }
    std::vector<uint8_t> signer; lp(signer,signedData); lp(signer,sigSeq); lp(signer,pubkey);
    // signers = len-prefixed list of len-prefixed signer
    std::vector<uint8_t> signers; { std::vector<uint8_t> tmp; lp(tmp,signer); put32(signers,(uint32_t)tmp.size()); signers.insert(signers.end(),tmp.begin(),tmp.end()); }

    // 5) v2 block value = signers; wrapped as an id-value pair in the APK Signing Block
    std::vector<uint8_t> v2val = signers;
    std::vector<uint8_t> pair; put32(pair, 0); /*placeholder*/ ; // we build pair manually below
    // APK Signing Block: uint64 sizeOfBlock | pairs(uint64 len | uint32 id | value) | uint64 sizeOfBlock | magic
    std::vector<uint8_t> pairs; put64(pairs, (uint64_t)(4 + v2val.size())); put32(pairs, 0x7109871a); pairs.insert(pairs.end(), v2val.begin(), v2val.end());
    uint64_t blockLen = pairs.size() + 8 /*trailing size*/ + 16 /*magic*/;
    std::vector<uint8_t> block; put64(block, blockLen); block.insert(block.end(), pairs.begin(), pairs.end()); put64(block, blockLen);
    const char* MAGIC="APK Sig Block 42"; block.insert(block.end(), MAGIC, MAGIC+16);
    // the signing block must be 4096-... no: only 8-byte-aligned by spec via the pairs; total block already 8-aligned since all fields are 8/4. (entriesEnd is 4-aligned from zipalign; block starts there.)

    // 6) assemble: [entries][signing block][central dir][EOCD with cd-offset += blockLen]
    out.clear(); out.reserve(alignedApk.size()+block.size());
    out.insert(out.end(), alignedApk.begin(), alignedApk.begin()+entriesEnd);
    out.insert(out.end(), block.begin(), block.end());
    out.insert(out.end(), alignedApk.begin()+cdStart, alignedApk.begin()+cdEnd);
    size_t newEocd=out.size(); out.insert(out.end(), alignedApk.begin()+eocd, alignedApk.end());
    uint32_t newCdOff=(uint32_t)(entriesEnd + block.size());
    out[newEocd+16]=(uint8_t)newCdOff; out[newEocd+17]=newCdOff>>8; out[newEocd+18]=newCdOff>>16; out[newEocd+19]=newCdOff>>24;
    return true;
}

}} // namespace hslcook::sign
