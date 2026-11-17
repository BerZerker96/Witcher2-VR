import sys,os,struct,math; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from w2 import *
from unicorn import *
from unicorn.x86_const import *
def mk():
    mu=Uc(UC_ARCH_X86,UC_MODE_32)
    mu.mem_map(0x400000,0x2600000)
    for s in pe.sections:
        raw=D[s.PointerToRawData:s.PointerToRawData+s.SizeOfRawData]
        mu.mem_write(IB+s.VirtualAddress,raw)
    mu.mem_map(0x10000000,0x100000)  # stack/scratch
    # identity at 0x160fb20 (runtime-initialised global)
    mu.mem_write(0x160fb20,struct.pack('<16f',1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1))
    return mu
def call(mu,addr,regs,stack_args=(),scratch_init=None):
    sp=0x100f0000
    RET=0x10000000; mu.mem_write(RET,b'\xf4')  # hlt
    for a in reversed(stack_args): sp-=4; mu.mem_write(sp,struct.pack('<I',a))
    sp-=4; mu.mem_write(sp,struct.pack('<I',RET))
    mu.reg_write(UC_X86_REG_ESP,sp); mu.reg_write(UC_X86_REG_EBP,0x100f8000)
    for k,v in regs.items(): mu.reg_write(k,v)
    mu.emu_start(addr,RET,timeout=5_000_000,count=2_000_000)
def wf(mu,a,fs): mu.mem_write(a,struct.pack('<%df'%len(fs),*fs))
def rf(mu,a,n): return struct.unpack('<%df'%n,mu.mem_read(a,4*n))
