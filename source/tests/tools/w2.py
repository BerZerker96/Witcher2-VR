import sys,re,struct,pefile
from capstone import *
import os
P=os.environ.get('W2_UNPACKED', 'witcher2_unpacked.exe')   # a locally decrypted copy of YOUR witcher2.exe
pe=pefile.PE(P,fast_load=True); D=open(P,'rb').read(); IB=0x400000
T=[s for s in pe.sections if s.Name.startswith(b'.text')][0]
text=D[T.PointerToRawData:T.PointerToRawData+T.Misc_VirtualSize]; TVA=IB+T.VirtualAddress
md=Cs(CS_ARCH_X86,CS_MODE_32)
def find(pat,buf=text,base=TVA):
    p=b''.join(b'.' if t in('??','?') else re.escape(bytes([int(t,16)])) for t in pat.split())
    return [base+m.start() for m in re.finditer(p,buf,re.S)]
def rd(va,n):
    for s in pe.sections:
        if s.VirtualAddress<=va-IB<s.VirtualAddress+max(s.Misc_VirtualSize,s.SizeOfRawData):
            o=s.PointerToRawData+va-IB-s.VirtualAddress; return D[o:o+n]
def dis(va,n=60,before=0):
    b=rd(va-before,n+before)
    out=[]
    for i in md.disasm(b,va-before): out.append('%s%08x: %-24s %s %s'%('>' if i.address==va else ' ',i.address,i.bytes.hex(' '),i.mnemonic,i.op_str))
    return '\n'.join(out)
