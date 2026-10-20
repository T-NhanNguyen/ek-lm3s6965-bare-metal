#!/usr/bin/env python3
"""Focused new dispatch-mode checker roster, normal or -O (no compiler)."""
import struct
import sys
from checked_support import (ROOT, DISPATCH, FORBIDDEN, NESTED, Session, Elf,
                             arguments, selfchecks, oracle, require, Failure)
from test_boot_reset_image import fixture

SUCCESS='boot dispatch: exact vectors, Thumb entry, first cpsid, load/RAM bounds and closure checked\n'
DECISION='boot reset: exact vectors, Thumb entry, first cpsid, load/RAM bounds and closure checked\n'

def stimulus(remove=(), overrides=None):
    # Keep the old fixture as the only ELF constructor. Rebuild its symbol table
    # from scratch to actually remove symbols, not overwrite them with zero.
    original=fixture(overrides={n:0x111+2*i for i,n in enumerate(DISPATCH)})
    data=bytearray(original)
    h=struct.unpack_from('<16sHHIIIIIHHHHHH',data)
    sections=[list(struct.unpack_from('<10I',data,h[6]+i*40)) for i in range(h[12])]
    st,sy=sections[4],sections[5]
    strings=data[st[4]:st[4]+st[5]]
    symbols=[]
    for off in range(sy[4]+16,sy[4]+sy[5],16):
        row=list(struct.unpack_from('<IIIBBH',data,off)); no=row[0]
        name=strings[no:strings.index(0,no)].decode('ascii')
        if name=='boot_sequence_run' or name in remove: continue
        row[1]=(overrides or {}).get(name,row[1]); symbols.append((name,row[1]))
    for name,value in (overrides or {}).items():
        if name not in {n for n,_ in symbols}: symbols.append((name,value))
    new_strings=bytearray(b'\0'); new_symbols=bytearray(16)
    for name,value in symbols:
        no=len(new_strings); new_strings.extend(name.encode()+b'\0')
        new_symbols.extend(struct.pack('<IIIBBH',no,value,0,0,0,0))
    st[4],st[5]=len(data),len(new_strings); data.extend(new_strings)
    sy[4],sy[5]=len(data),len(new_symbols); data.extend(new_symbols)
    shoff=len(data)
    for row in sections: data.extend(struct.pack('<10I',*row))
    struct.pack_into('<I',data,32,shoff)
    return bytes(data)

def stimulus_selfcheck():
    elf=Elf(stimulus())
    for i,name in enumerate(DISPATCH):
        oracle(elf.symbol(name)==(name,0,'UNDEF',0x111+2*i,0),'synthesized dispatch symbol '+name)
    oracle('boot_sequence_run' not in elf.symbols,'no decision symbol')
    for name in DISPATCH:
        oracle(name not in Elf(stimulus(remove=(name,))).symbols,'actual symbol deletion '+name)

def main():
    def extra(p): p.add_argument('--python-mode',required=True,choices=('normal','-O'))
    a,roster=arguments(('dispatch-checker-closure',),extra=extra)
    s=Session('dispatch-checker-'+('optimized' if a.python_mode=='-O' else 'normal'),a,roster)
    def setup():
        s.freeze(); selfchecks(); stimulus_selfcheck()
        directory=s.root/('checker-'+a.python_mode.replace('-',''))
        directory.mkdir(exist_ok=False); s.directory=directory
    def check(name,data,error=None,dispatch=True):
        elf=s.directory/(name.replace(':','-')+'.elf'); elf.write_bytes(data)
        flags=['-O'] if a.python_mode=='-O' else []
        code,body=s.run([sys.executable,*flags,ROOT/'scripts/check-boot-reset-image.py',elf,*(['--dispatch'] if dispatch else [])],expected=None)
        try:
            if error is None:
                require(code==0 and body==(SUCCESS if dispatch else DECISION),'exact successful checker record '+name)
            else:
                # Exact final diagnostic, not arbitrary traceback text.
                require(code!=0 and body.splitlines()[-1:] == ['ValueError: '+error] and
                        'closure checked' not in body,'exact checker rejection '+name)
        except Failure as exc:
            s.failures.append(exc)
            s.manifest.setdefault('nested_status',{})[name]='fail'
        else:
            s.manifest.setdefault('nested_status',{})[name]='pass'
        s.nested('dispatch-checker-closure',name)
    def execute(case):
        s.failures=[]
        check('valid',stimulus())
        for name in DISPATCH: check('missing:'+name,stimulus(remove=(name,)), 'missing/out-of-boot closure: '+name)
        name='boot_handoff_target_operation'
        check('out-of-boot:'+name,stimulus(overrides={name:0x8000}), 'missing/out-of-boot closure: '+name)
        for name in FORBIDDEN:
            check('forbidden:'+name,stimulus(overrides={name:0x111}), 'decision/application entry leaked into dispatch: '+name)
        check('decision-without-flag',fixture(),dispatch=False)
        # The forbidden decision symbol is rejected before required closure.
        # Preserve exact rejection intent without changing production priority.
        check('decision-with-dispatch',fixture(),'decision/application entry leaked into dispatch: boot_sequence_run')
        if s.failures: raise s.failures[0]
    return s.execute(execute,setup)

if __name__=='__main__': raise SystemExit(main())
