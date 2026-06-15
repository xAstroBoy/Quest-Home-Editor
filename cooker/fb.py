import struct
# Minimal FlatBuffers builder (back-to-front), enough for RENDMESH/MATL/ASMH.
class Builder:
    def __init__(self, size=1024):
        self.Bytes=bytearray(size); self.head=len(self.Bytes); self.minalign=1
        self.vt=None; self.objectEnd=None; self.vtables={}
    def Output(self): return bytes(self.Bytes[self.head:])
    def Offset(self): return len(self.Bytes)-self.head
    def _grow(self):
        old=self.Bytes; self.Bytes=bytearray(len(old)*2); self.Bytes[len(self.Bytes)-len(old):]=old
    def Pad(self,n):
        for _ in range(n): self.head-=1; self.Bytes[self.head]=0
    def Prep(self,size,extra):
        if size>self.minalign: self.minalign=size
        used=len(self.Bytes)-self.head
        alignsize=((~(used+extra))+1)&(size-1)
        while self.head < alignsize+size+extra:
            o=len(self.Bytes); self._grow(); self.head+=len(self.Bytes)-o
        self.Pad(alignsize)
    def Place(self,x,fmt):
        sz=struct.calcsize('<'+fmt); self.head-=sz; struct.pack_into('<'+fmt,self.Bytes,self.head,x)
    def PrependScalar(self,fmt,x): self.Prep(struct.calcsize('<'+fmt),0); self.Place(x,fmt)
    def PrependUOffset(self,off):
        self.Prep(4,0); self.Place(self.Offset()-off+4,'I')
    def CreateByteVector(self,data):
        self.Prep(4,len(data)); self.head-=len(data); self.Bytes[self.head:self.head+len(data)]=data
        self.PrependScalar('I',len(data)); return self.Offset()
    def CreateOffsetVector(self,offs,elem=4,align=4):
        # vector of uoffsets (or already-laid structs). offs: list of ints (offsets) for uoffset vec.
        self.Prep(4,elem*len(offs)); self.Prep(align,elem*len(offs))
        for o in reversed(offs): self.PrependUOffset(o)
        self.PrependScalar('I',len(offs)); return self.Offset()
    def CreateStructVector(self,raw,stride,count,align=4):
        # raw = bytes of count*stride structs (already little-endian). length-prefixed.
        assert len(raw)==stride*count
        self.Prep(4,len(raw)); self.Prep(align,len(raw))
        self.head-=len(raw); self.Bytes[self.head:self.head+len(raw)]=raw
        self.PrependScalar('I',count); return self.Offset()
    # tables
    def StartObject(self,n): self.vt=[0]*n; self.objectEnd=self.Offset()
    def Slot(self,s): self.vt[s]=self.Offset()
    def AddOffset(self,slot,off,default=0):
        if off!=default: self.PrependUOffset(off); self.Slot(slot)
    def AddScalar(self,fmt,slot,x,default=0):
        if x!=default: self.PrependScalar(fmt,x); self.Slot(slot)
    def EndObject(self):
        self.PrependScalar('i',0)             # soffset placeholder
        objOff=self.Offset()
        i=len(self.vt)-1
        while i>=0 and self.vt[i]==0: i-=1
        trimmed=i+1
        for fi in reversed(range(trimmed)):
            self.Place((objOff-self.vt[fi]) if self.vt[fi]!=0 else 0,'H')
        self.Place(objOff-self.objectEnd,'H')      # table size
        self.Place((trimmed+2)*2,'H')              # vtable size
        vtOff=self.Offset()
        vtbytes=bytes(self.Bytes[self.head:self.head+(trimmed+2)*2])
        ex=self.vtables.get(vtbytes)
        if ex is not None:
            self.head+=(trimmed+2)*2; vtOff=ex
        else:
            self.vtables[vtbytes]=vtOff
        struct.pack_into('<i',self.Bytes,len(self.Bytes)-objOff, vtOff-objOff)
        return objOff
    def Finish(self,root,fid=None):
        extra=4+(4 if fid else 0)
        self.Prep(self.minalign,extra)
        if fid:
            assert len(fid)==4
            for c in reversed(fid): self.head-=1; self.Bytes[self.head]=c if isinstance(c,int) else ord(c)
        self.PrependUOffset(root); return self.head
