"""Typed linked/static oracles, not physical entry or dynamic resource proof."""
import json
import re
import struct
from checked_support import (ROOT, BIN, GCC, DISPATCH, COMMON, FORBIDDEN, Elf,
                             Invalid, Failure, require, oracle, instruction_rows)
from test_handoff_target import check_sequence

SYMBOL_RE=re.compile(r'\s*\d+:\s+([0-9a-fA-F]+)\s+(\d+)\s+(\S+)\s+(\S+)\s+\S+\s+(\S+)(?:\s+(\S+))?\s*')
def readelf_symbols(text):
    rows=[]
    for line in text.splitlines():
        if not re.match(r'\s*\d+:',line): continue
        if re.fullmatch(r'\s*0:\s+0+\s+0\s+NOTYPE\s+LOCAL\s+DEFAULT\s+UND\s*',line): continue
        m=SYMBOL_RE.fullmatch(line)
        if not m: raise Invalid('unparsed readelf symbol: '+line[:160])
        # FILE/SECTION and UND records are bookkeeping, not authority. Keep
        # named tuples for the independent ELF comparison; permit empty names
        # only for these explicit non-authority categories.
        name=m[6]
        if not name:
            if m[3] in ('FILE','SECTION') or m[5]=='UND': continue
            raise Invalid('nameless readelf authority symbol: '+line[:160])
        row=(name,m[3],m[5],int(m[1],16),int(m[2]),m[4])
        rows.append(row)
    if not rows: raise Invalid('empty readelf symbol evidence')
    return rows

def static_selfchecks_legacy_unused():
    oracle(readelf_symbols(' 1: 00000101 4 FUNC GLOBAL DEFAULT 3 entry\n 2: 00000200 4 OBJECT LOCAL DEFAULT 4 state\n 3: 00000000 0 NOTYPE GLOBAL DEFAULT UND missing\n')==[
        ('entry','FUNC','3',0x101,4),('state','OBJECT','4',0x200,4),('missing','NOTYPE','UND',0,0)],'readelf type/section/value/size tuples')
    good=' 1: 00000101 4 FUNC GLOBAL DEFAULT 3 entry\n'
    bookkeeping=' 16: 00000000 0 FILE LOCAL DEFAULT ABS\n 17: 00000000 0 SECTION LOCAL DEFAULT 3\n 0: 00000000 0 NOTYPE LOCAL DEFAULT UND\n'
    oracle(readelf_symbols(bookkeeping+good)==[('entry','FUNC','3',0x101,4)],'captured unnamed bookkeeping preserves exact authority tuple')
    mapping=' 18: 00000100 0 NOTYPE LOCAL DEFAULT 3 $t\n 19: 00000200 0 NOTYPE LOCAL DEFAULT 3 $t\n'
    oracle(readelf_symbols(mapping)==[('$t','NOTYPE','3',0x100,0),('$t','NOTYPE','3',0x200,0)],'repeated ARM mapping tuples are not function authority')
    for bad in (' 1: 00000101 4 FUNC GLOBAL DEFAULT 3\n',
                ' 1: broken 4 FUNC GLOBAL DEFAULT 3 entry\n',good+good,
                good+' 2: 00000105 4 FUNC LOCAL DEFAULT 3 entry\n'):
        try: readelf_symbols(bad)
        except Invalid: pass
        else: raise Invalid('symbol parser admitted nameless/malformed/ambiguous authority')
    # Required-anchor resolution must still reject missing and duplicate ELF
    # records, independently of the readelf parser's bookkeeping handling.
    for symbols in ({}, {'entry':[('entry',2,'.text',0x101,4)]*2}):
        elf=object.__new__(Elf); elf.symbols=symbols
        try: elf.symbol('entry')
        except Invalid: pass
        else: raise Invalid('required ELF anchor admitted missing/duplicate symbol')

def static_selfchecks():
    sample=' 1: 00000101 4 FUNC LOCAL DEFAULT 3 put32\n 2: 00000201 8 FUNC LOCAL DEFAULT 3 put32\n 3: 00000301 4 FUNC GLOBAL DEFAULT 3 entry\n 4: 00000401 4 FUNC LOCAL DEFAULT 3 entry\n 5: 00000100 0 NOTYPE LOCAL DEFAULT 3 $t\n 6: 00000200 0 NOTYPE LOCAL DEFAULT 3 $t\n'
    expected=[('put32','FUNC','3',0x101,4,'LOCAL'),('put32','FUNC','3',0x201,8,'LOCAL'),('entry','FUNC','3',0x301,4,'GLOBAL'),('entry','FUNC','3',0x401,4,'LOCAL'),('$t','NOTYPE','3',0x100,0,'LOCAL'),('$t','NOTYPE','3',0x200,0,'LOCAL')]
    oracle(readelf_symbols(sample)==expected,'exact scoped repeated symbol tuples')
    bookkeeping=' 16: 00000000 0 FILE LOCAL DEFAULT ABS\n 17: 00000000 0 SECTION LOCAL DEFAULT 3\n 0: 00000000 0 NOTYPE LOCAL DEFAULT UND\n'
    oracle(readelf_symbols(bookkeeping+sample)==expected,'unnamed bookkeeping only')
    for bad in (' 1: 00000101 4 FUNC GLOBAL DEFAULT 3\n',' 1: broken 4 FUNC GLOBAL DEFAULT 3 entry\n'):
        try: readelf_symbols(bad)
        except Invalid: pass
        else: raise Invalid('malformed symbol admitted')
    elf=object.__new__(Elf); elf.sections={'.text':(0,1,6,0x100,0,0x400)}
    elf.symbols={'entry':[('entry',2,'.text',0x401,4),('entry',2,'.text',0x301,4)],'put32':[('put32',2,'.text',0x101,4),('put32',2,'.text',0x201,8)]}
    elf.bindings={'entry':[0,1],'put32':[0,0]}
    oracle(elf.symbol('entry')==('entry',2,'.text',0x301,4),'unique global not first local')
    oracle(elf.code('put32',0x100) and elf.code('put32',0x200) and not elf.code('put32'),'address-qualified locals')
    for rows,bindings in (([],[]),([('entry',2,'.text',0x301,4)]*2,[1,1])):
        elf.symbols['entry']=rows; elf.bindings['entry']=bindings
        try: elf.symbol('entry')
        except Invalid: pass
        else: raise Invalid('missing/duplicate required global admitted')
    elf.symbols['put32']*=2; elf.bindings['put32']*=2
    oracle(not elf.code('put32',0x100),'ambiguous address rejected')
    static_witness_selfchecks()

def inspect(s,path):
    elf=Elf(path.read_bytes())
    _,text=s.run([BIN/'arm-none-eabi-readelf','-sW',path])
    external=readelf_symbols(text)
    _,sections=s.run([BIN/'arm-none-eabi-readelf','-SW',path])
    # Explicit index-to-name table: never compare names-only nm as function proof.
    index={}
    for line in sections.splitlines():
        m=re.match(r'\s*\[\s*(\d+)\]\s+(\S+)\s+(\S+)\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+)',line)
        if m:
            i,name,kind,address,offset,size=m.groups(); index[i]=name
            if name in elf.sections:
                row=elf.sections[name]
                if (row[3],row[4],row[5])!=(int(address,16),int(offset,16),int(size,16)):
                    raise Invalid('official readelf section oracle disagreement '+name)
    for name,rows in elf.symbols.items():
        for _,kind,section,value,size in rows:
            typ={0:'NOTYPE',1:'OBJECT',2:'FUNC',3:'SECTION',4:'FILE'}.get(kind)
            if typ is None: continue
            # Empty SHN_UNDEF maps to UND; ABS is never an executable section.
            found=[r for r in external if r[0]==name and r[1]==typ and
                   (index.get(r[2],r[2])==section or r[2]=='UND' and section in ('','UNDEF')) and r[3:5]==(value,size) and r[5]=={0:'LOCAL',1:'GLOBAL',2:'WEAK'}.get(elf.bindings[name][rows.index((name,kind,section,value,size))],'UNKNOWN')]
            if not found: raise Invalid('official readelf typed-tuple oracle disagreement '+name)
    _,programs=s.run([BIN/'arm-none-eabi-readelf','-lW',path])
    loads=[]
    for line in programs.splitlines():
        m=re.match(r'\s*LOAD\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)',line)
        if m: loads.append(tuple(int(x,16) for x in m.groups()))
    if loads!=[p[1:6] for p in elf.loads]: raise Invalid('official physical LOAD oracle disagreement including data LMA')
    _,dis=s.run([BIN/'arm-none-eabi-objdump','-d',path])
    _,relocs=s.run([BIN/'arm-none-eabi-objdump','-r',path])
    (s.out/(path.name+'-static.json')).write_text(json.dumps({'sections':elf.sections,'symbols':elf.symbols,'loads':elf.loads},indent=2)+'\n')
    return elf,dis

def retained(elf,limit):
    for name,address,size in (('.boot_continuity',0x2000dbc0,1024),('.boot_diagnostic',0x2000dfc0,64),('.stack',0x2000e000,8192)):
        row=elf.sections.get(name)
        require(row is not None and (row[1],row[3],row[5])==(8,address,size),'exact retained NOBITS tuple '+name)
    c=elf.symbol('boot_startup_continuity')
    require(c[1] in (0,1) and c[2]=='ABS' and c[3]==0x2000dbc0,'absolute continuity ABI')
    require(elf.value('_estack')==0x20010000 and elf.value('_heap_end')==0x2000dbc0,'stack/heap ABI')
    require(elf.value('_sdata')<=elf.value('_edata')<=0x2000dbc0 and
            elf.value('_sbss')<=elf.value('_ebss')<=elf.value('_heap_static_end')<=0x2000dbc0,'ordinary data/BSS/static heap exclusion')
    require(all(not p[4] or p[3]+p[4]<=limit for p in elf.loads),'all file-backed physical LOAD bytes fit incl data LMA')
    for p in elf.loads:
        if p[4]: require(not (p[2]<0x20010000 and p[2]+p[4]>0x2000dbc0),'no file-backed retained SRAM initialization')
    return {'ordinary_static_sram_end':elf.value('_heap_static_end'),
            'heap_capacity_bytes':0x2000dbc0-elf.value('_heap_static_end'),
            'stack_reserved_bytes':8192,'dynamic_peaks_measured':False,
            'flash_load_end':max((p[3]+p[4] for p in elf.loads if p[4]),default=0)}

def contract_rows(spec):
    """Handwritten address/op/operand expectations, never derived from the ELF."""
    rows=[]
    for line in spec.strip().splitlines():
        for item in line.split(';'):
            if not item.strip(): continue
            fields=item.strip().split(' ',2)
            rows.append((int(fields[0],16),fields[1],fields[2] if len(fields)>2 else ''))
    return rows

def machine_contract(rows,spec,label):
    # Branch labels are annotations, not authority: compare actual addresses.
    actual=[(pc,op,re.sub(r' <[^>]+>','',args)) for pc,op,args in rows]
    expected=contract_rows(spec)
    if actual!=expected:
        mismatch=next(((a,e) for a,e in zip(actual,expected) if a!=e),None)
        raise Invalid(label+': unsupported decoded machine contract '+repr(mismatch or (len(actual),len(expected))))
    return actual

def elf_word(elf,address):
    sections=[s for s in elf.sections.values() if s[1]==1 and
              s[3]<=address and address+4<=s[3]+s[5]]
    if len(sections)!=1: raise Invalid('unresolved/aliased ELF word '+hex(address))
    s=sections[0]
    return struct.unpack_from('<I',elf.data,s[4]+address-s[3])[0]

def literal_contract(elf,rows,expected,label):
    found={}
    for pc,op,args in rows:
        m=re.fullmatch(r'(r\d+), \[pc, #(\d+)\]',args) if op=='ldr' else None
        if m:
            address=((pc+4)&~3)+int(m[2])
            found[pc]=(address,elf_word(elf,address))
    require(found==expected,label+': actual ELF PC-relative literal values')
    return found

def terminal_gate(dis,elf=None):
    wrapper=instruction_rows(dis,'boot_startup_dispatch')
    # Exact wrapper CFG: r4 preserves the ORIGINAL r0 across begin_dispatch;
    # the zero successor is nonreturning, never an unchecked terminal path.
    machine_contract(wrapper, '''
4d6e push {r3, lr}; 4d70 mov r4, r0
4d72 bl 4b7c; 4d76 cbnz r0, 4d7c; 4d78 bl 4d68
4d7c ldr r2, [r4, #24]; 4d7e ldr r1, [r4, #292]
4d82 ldr r0, [r4, #20]; 4d84 bl 6c60
''', 'dispatch original capsule/guard/terminal arguments')
    machine_contract(instruction_rows(dis,'boot_startup_stop'), '''
4d68 cpsid i; 4d6a nop; 4d6c b 4d6a
''', 'dispatch failed begin is nonreturning')
    begin=instruction_rows(dis,'boot_startup_begin_dispatch')
    machine_contract(begin,'''4b7c push {r0, r1, r4, lr}
4b7e movs r1, #1; 4b80 mov r4, r0; 4b82 bl 1144
4b86 cbz r0, 4b9a; 4b88 mrs r2, primask; 4b8c mrs r3, ipsr
4b90 cmp r2, r1; 4b92 beq 4ba0; 4b94 mov r0, r4; 4b96 bl 1134
4b9a movs r0, #0; 4b9c add sp, #8; 4b9e pop {r4, pc}
4ba0 cmp r3, #0; 4ba2 bne 4b94; 4ba4 ldr r3, [r4, #80]
4ba6 cmp r3, #0; 4ba8 beq 4b94; 4baa ldr r3, [r4, #84]
4bac cmp r3, #0; 4bae bne 4b94; 4bb0 ldr r3, [r4, #88]
4bb2 cmp r3, #2; 4bb4 bne 4b94; 4bb6 ldr r2, [r4, #92]
4bb8 ldr r3, [r4, #28]; 4bba cmp r2, r3; 4bbc bne 4b94
4bbe ldr r3, [r4, #96]; 4bc0 cmp r3, #0; 4bc2 bne 4b94
4bc4 mov r1, sp; 4bc6 mov r0, r4; 4bc8 bl 1200
4bcc cmp r0, #0; 4bce beq 4b94; 4bd0 ldrd r2, r3, [sp]
4bd4 strd r2, r3, [r4, #40]; 4bd8 ldr r3, [r4, #56]
4bda cmp r3, #0; 4bdc beq 4b94; 4bde ldr r3, [r4, #16]
4be0 cmp r3, #1; 4be2 bne 4b94; 4be4 movs r3, #0
4be6 str r3, [r4, #56]; 4be8 dmb sy; 4bec movs r3, #2
4bee str r3, [r4, #16]; 4bf0 b 4b9c
''','actual PREPARED claim/exclusion/dispatch publication guard')
    native=boot_native_witness(elf,dis,include_terminal=False)
    clock=machine_contract(instruction_rows(dis,'clock_read.lto_priv.1'),BOOT_CLOCK,
                           'begin-dispatch clock native dependency')
    architecture=terminal_architecture(dis)
    return {'wrapper':wrapper,'begin_dispatch_body':begin,'native_claim':native,
            'clock_dependency':clock,'parameters':{'vtor':20,'msp':292,'reset':24},
            'begin_dispatch':0x4b7c,'failure_stop':0x4d68,**architecture}


def terminal_architecture(dis):
    tail=instruction_rows(dis,'boot_startup_terminal')
    matches=[i for i,r in enumerate(tail) if r[1:]==('msr','msp, r1')]
    require(len(matches)==1,'exact single MSP switch')
    # Per-barrier obligations: each required terminal DSB/ISB is bound
    # individually so barrier->nop fragments fail here with a per-barrier
    # reason, not at the generic pre-switch allowlist. Non-barrier ops at
    # these PCs (e.g. ldr at 6c80) defer to the allowlist/suffix for the
    # no-old-frame obligation. The exact allowlist below still binds
    # unmutated code.
    by_pc={pc:(op,args) for pc,op,args in tail}
    for pc,op,args in ((0x6c60,'dsb','sy'),(0x6c68,'dsb','sy'),(0x6c6c,'isb','sy'),
                        (0x6c80,'isb','sy'),(0x6c88,'isb','sy')):
        actual=by_pc.get(pc)
        require(actual==(op,args) or (actual is not None and actual!=('nop','') and actual[0] not in ('dsb','isb')),'terminal barrier %x: exact barrier' % pc)
    # Labels split the literal pool/return trap into separate blocks; no old
    # frame memory access or call is permitted after the switch.
    try:
        check_sequence(tail[:matches[0]],[('dsb','sy'),('ldr',r'r3, \[pc, #[0-9]+\]'),
            ('str',r'r0, \[r3, #0\]'),('dsb','sy'),('isb','sy'),('addw',r'r3, pc, #[0-9]+'),
            ('orr','r3, r3, #1'),('mov','lr, r3'),('movs','r3, #0')],'terminal pre-switch exact allowlist')
        check_sequence(tail[matches[0]:],[('msr','msp, r1'),('isb','sy'),('msr','control, r3'),('isb','sy'),
            ('msr','basepri, r3'),('msr','faultmask, r3'),('msr','primask, r3'),('bx','r2')],'no-old-frame terminal suffix')
    except RuntimeError as exc: raise Failure(str(exc)) from exc
    trap=instruction_rows(dis,'boot_startup_return_trap')
    require(trap[0][1]=='b' and int(trap[0][2].split()[0],16)==trap[0][0],'terminal return trap self-loop')
    require(all(op in ('.word','.short') for _,op,_ in trap[1:]),'trap has no stack/load/call suffix')
    add=tail[5]
    immediate=re.fullmatch(r'r3, pc, #(\d+)',add[2])
    if not immediate: raise Invalid('unresolved terminal trap LR calculation')
    require(((((add[0]+4)&~3)+int(immediate[1]))|1)==(trap[0][0]|1),
            'terminal LR is exact Thumb trap, not arbitrary return')
    literal=tail[1]; off=int(re.search(r'#([0-9]+)',literal[2])[1])
    address=((literal[0]+4)&~3)+off
    require(re.search(r'^\s*'+format(address,'x')+r':\s+e000ed08\s+\.word\s+0xe000ed08\s*$',dis,re.M) is not None,
            'actual terminal literal is VTOR')
    obs=instruction_rows(dis,'boot_handoff_target_observe')
    machine_contract(obs,'''482c stmdb sp!, {r4, r5, r6, r7, r8, r9, lr}
4830 mov r4, r0; 4832 sub sp, #28; 4834 cbnz r0, 4840
4836 mov r0, #4294967295; 483a add sp, #28
483c ldmia sp!, {r4, r5, r6, r7, r8, r9, pc}
4840 mrs r9, ipsr; 4844 mrs r8, control; 4848 mrs r7, primask
484c mrs r6, basepri; 4850 mrs r5, faultmask; 4854 add r0, sp, #4
4856 strd r9, r8, [sp, #4]; 485a strd r7, r6, [sp, #12]
485e str r5, [sp, #20]; 4860 bl 17e; 4864 cmp r0, #0
4866 beq 4836; 4868 movs r0, #0; 486a strd r9, r8, [r4]
486e strd r7, r6, [r4, #8]; 4872 str r5, [r4, #16]; 4874 b 483a
''','actual core MRS capture and output provenance')
    registers=[args.split(', ',1)[1] for _,op,args in obs if op=='mrs']
    require(registers==['ipsr','control','primask','basepri','faultmask'],'actual observation exact ordered MRS tuple')
    barrier=instruction_rows(dis,'direct_barrier')
    machine_contract(barrier,'''1bc cbnz r1, 1cc; 1be cpsid i
1c0 dsb sy; 1c4 isb sy; 1c8 movs r0, #0; 1ca bx lr
1cc cmp r1, #1; 1ce beq 1c0; 1d0 mov r0, #4294967295; 1d4 bx lr
''','actual exclusion/barrier full CFG')
    ops=[(op,args) for _,op,args in barrier if op in ('cpsid','dsb','isb')]
    require(ops==[('cpsid','i'),('dsb','sy'),('isb','sy')],'actual exclusion/barrier tuple')
    require(tail[0][0]==0x6c60,'wrapper target is actual terminal start')
    return {'terminal':tail,'trap':trap,'observation_mrs':registers,'exclusion_barriers':ops}

def function_rows(dis,name,address):
    blocks=[m[0] for m in re.finditer(r'^\s*([0-9a-f]+) <([^>]+)>:\s*\n.*?(?=^\s*[0-9a-f]+ <|^Disassembly of section |\Z)',dis,re.M|re.S)
            if int(m[1],16)==address and m[2]==name]
    if len(blocks)!=1: raise Invalid('missing/ambiguous address-qualified instruction block '+name)
    return instruction_rows(blocks[0],name)

# Fully decoded retained sampling contract. The only memory domains in these
# bodies are g[0..40), caller output[0..24), current frame, and MMIO. matched's
# unsigned timer <=3 guard bounds (timer+0x40030)<<12 to 0x40030000..0x40033000;
# it checks frequency before the division. The direct callback bodies cannot
# mutate g, so those guards remain valid across the intervening calls.
RETAINED_READ='''
13e0 stmdb sp!, {r0, r1, r2, r3, r4, r5, r6, r7, r8, lr}
13e4 mov r4, r0; 13e6 mov r6, r1; 13e8 cbz r0, 1400
13ea ldr r2, [r0, #24]; 13ec ldr r3, [pc, #168]
13ee adds r3, r2, r3; 13f0 cmp r3, #2; 13f2 bhi 13fa
13f4 ldr r3, [pc, #164]; 13f6 str r3, [r4, #24]; 13f8 b 1400
13fa ldr r7, [pc, #164]; 13fc cmp r2, r7; 13fe beq 140a
1400 mov r0, #4294967295; 1404 add sp, #16
1406 ldmia sp!, {r4, r5, r6, r7, r8, pc}
140a ldr r1, [pc, #152]; 140c str r1, [r0, #24]
140e cmp r6, #0; 1410 beq 13f4; 1412 bl 1314
1416 cmp r0, #0; 1418 beq 13f4; 141a ldr r1, [r4, #20]
141c mov r0, r4; 141e add r1, r1, #262144; 1422 adds r1, #48
1424 lsls r5, r1, #12; 1426 add r2, sp, #12
1428 add r1, r5, #28; 142c bl 129e; 1430 cmp r0, #0; 1432 beq 13f4
1434 ldr r3, [sp, #12]; 1436 cmp r3, #0; 1438 bne 13f4
143a mov r0, r4; 143c add r2, sp, #8; 143e add r1, r5, #72
1442 bl 129e; 1446 cmp r0, #0; 1448 beq 13f4
144a mov r0, r4; 144c add r2, sp, #4; 144e add r1, r5, #28
1452 bl 129e; 1456 cmp r0, #0; 1458 beq 13f4
145a ldr r3, [sp, #4]; 145c cmp r3, #0; 145e bne 13f4
1460 mov r0, r4; 1462 ldr r1, [pc, #64]; 1464 bl 1314
1468 cmp r0, #0; 146a beq 13f4; 146c ldr r1, [sp, #8]
146e ldr r5, [r4, #16]; 1470 mvns r1, r1; 1472 ldr r0, [pc, #52]
1474 movs r3, #0; 1476 mov r2, r5; 1478 umull r0, r1, r1, r0
147c bl 6f50; 1480 ldr r3, [r4, #28]; 1482 strd r0, r1, [r6]
1486 str r3, [r6, #8]; 1488 ldr r3, [r4, #32]; 148a movs r0, #0
148c strd r3, r5, [r6, #12]; 1490 ldr r3, [pc, #24]
1492 str r3, [r6, #20]; 1494 str r7, [r4, #24]; 1496 b 1404
1498 .word 0xb8afabcd; 149c .word 0x47505436; 14a0 .word 0x47505431
14a4 .word 0x47505434; 14a8 .word 0x000f4240; 14ac .word 0x4750544d
'''
RETAINED_MATCHED='''
1314 push {r0, r1, r2, r4, r5, lr}; 1316 mov r4, r0
1318 cbnz r0, 1320; 131a movs r0, #0; 131c add sp, #12
131e pop {r4, r5, pc}; 1320 ldr r3, [r0, #24]
1322 cmp r3, r1; 1324 bne 131a; 1326 ldr r3, [r0, #20]
1328 cmp r3, #3; 132a bhi 131a; 132c ldr r3, [r0, #16]
132e subs r3, #1; 1330 cmp r3, #858993459; 1334 bcs 131a
1336 ldr r3, [r0, #4]; 1338 cmp r3, #0; 133a beq 131a
133c ldr r3, [r0, #8]; 133e cmp r3, #0; 1340 beq 131a
1342 ldr r1, [pc, #144]; 1344 add r2, sp, #4; 1346 bl 129e
134a cmp r0, #0; 134c beq 131a; 134e movs r3, #1
1350 ldr r5, [r4, #20]; 1352 add r2, r5, #16
1356 lsls r3, r2; 1358 ldr r2, [sp, #4]; 135a tst r3, r2
135c beq 131a; 135e add r5, r5, #262144; 1362 adds r5, #48
1364 lsls r5, r5, #12; 1366 movs r2, #0; 1368 mov r1, r5
136a mov r0, r4; 136c bl 12fc; 1370 cbnz r0, 137a
1372 movs r0, #0; 1374 and r0, r0, #1; 1378 b 131c
137a movs r2, #2; 137c mov r0, r4; 137e adds r1, r5, #4
1380 bl 12fc; 1384 cmp r0, #0; 1386 beq 1372
1388 movs r2, #1; 138a mov r0, r4; 138c add r1, r5, #12
1390 bl 12fc; 1394 cmp r0, #0; 1396 beq 1372
1398 movs r2, #0; 139a mov r0, r4; 139c add r1, r5, #24
13a0 bl 12fc; 13a4 cmp r0, #0; 13a6 beq 1372
13a8 mov r2, #4294967295; 13ac mov r0, r4; 13ae add r1, r5, #40
13b2 bl 12fc; 13b6 cmp r0, #0; 13b8 beq 1372
13ba mov r0, r4; 13bc ldr r2, [r4, #28]; 13be ldr r1, [pc, #24]
13c0 bl 12fc; 13c4 cmp r0, #0; 13c6 beq 1372
13c8 mov r0, r4; 13ca ldr r2, [r4, #32]; 13cc ldr r1, [pc, #12]
13ce bl 12fc; 13d2 b 1374; 13d4 .word 0x400fe104
13d8 .word 0x400fe060; 13dc .word 0x400fe070
'''
RETAINED_REGISTER='''
129e push {r3, r4, r5, lr}; 12a0 ldr r5, [r0, #24]
12a2 ldr r3, [pc, #32]; 12a4 mov r4, r0; 12a6 adds r3, r5, r3
12a8 cmp r3, #2; 12aa bhi 12bc; 12ac ldmia r0, {r0, r3}
12ae blx r3; 12b0 cbnz r0, 12b8; 12b2 ldr r3, [r4, #24]
12b4 cmp r3, r5; 12b6 beq 12c0; 12b8 ldr r3, [pc, #12]
12ba str r3, [r4, #24]; 12bc movs r0, #0
12be pop {r3, r4, r5, pc}; 12c0 movs r0, #1; 12c2 b 12be
12c4 .word 0xb8afabcd; 12c8 .word 0x47505436
'''
RETAINED_EQUAL='''
12fc push {r0, r1, r4, lr}; 12fe mov r4, r2
1300 add r2, sp, #4; 1302 bl 129e; 1306 cbz r0, 1310
1308 ldr r0, [sp, #4]; 130a subs r3, r0, r4
130c negs r0, r3; 130e adcs r0, r3; 1310 add sp, #8
1312 pop {r4, pc}
'''
RETAINED_DIV_WRAPPER='''
6f50 cbnz r3, 6f68; 6f52 cbnz r2, 6f68; 6f54 cmp r1, #0
6f56 it eq; 6f58 cmpeq r0, #0; 6f5a itt ne
6f5c movne r1, #4294967295; 6f60 movne r0, #4294967295
6f64 b 7298; 6f68 sub ip, sp, #8; 6f6c strd ip, lr, [sp, #-16]!
6f70 bl 6f80; 6f74 ldr lr, [sp, #4]; 6f78 ldrd r2, r3, [sp, #8]
6f7c add sp, #16; 6f7e bx lr
'''
RETAINED_CLAIM_PREFIX='''
4bf2 push {r0, r1, r2, r4, r5, r6, r7, lr}
4bf4 ldr r7, [sp, #32]; 4bf6 mov r4, r0; 4bf8 mov r5, r1
4bfa mov r6, r2; 4bfc cbz r0, 4c10; 4bfe ldr r1, [r0, #24]
4c00 ldr r2, [pc, #324]; 4c02 adds r2, r1, r2
4c04 cmp r2, #2; 4c06 bhi 4c0e; 4c08 ldr r3, [pc, #320]
4c0a str r3, [r4, #24]; 4c0c b 4c10; 4c0e cbnz r3, 4c16
4c10 movs r0, #0; 4c12 add sp, #12; 4c14 pop {r4, r5, r6, r7, pc}
4c16 cmp r7, #0; 4c18 beq 4c10; 4c1a cmp r5, #3
4c1c bhi 4c10; 4c1e subs r2, r6, #1; 4c20 cmp r2, #858993459
4c24 bcs 4c10; 4c26 cmp r1, #0; 4c28 bne 4c10
4c2a ldr r2, [r3, #4]; 4c2c cmp r2, #0; 4c2e beq 4c10
4c30 ldr r2, [r3, #8]; 4c32 cmp r2, #0; 4c34 beq 4c10
4c36 ldmia r3, {r0, r1, r2}; 4c3a strd r7, r6, [r4, #12]
4c3e ldr r3, [pc, #272]; 4c40 ldr r6, [pc, #272]
4c42 stmia r4, {r0, r1, r2}
'''


def native_provenance(rows,word_read,mode):
    """Finite path-sensitive native seam proof, no guessed unknown reads.

    Every syntactic successor is visited. Values at joins are NOT combined:
    a target/context must be correct on each individual incoming state. The
    exact enclosing contracts bind flag predicates separately; overexploring
    their successors cannot hide an unsafe path. Unsupported effects INVALID.
    """
    code={pc:(op,re.sub(r' <[^>]+>','',args)) for pc,op,args in rows if not op.startswith('.')}
    order=sorted(code); following=dict(zip(order,order[1:]))
    work=[(order[0],{'r0':('g',0),'r3':('io',0),'sp':('stack',0)},())]
    seen=set(); seams=[]
    def reglist(text):
        rs=text.strip('{}').split(', ')
        if not rs or any(not re.fullmatch(r'r(?:[0-9]|1[0-2])|lr|pc',r) for r in rs):
            raise Invalid('unsupported native register list '+text)
        numbers=[14 if r=='lr' else 15 if r=='pc' else int(r[1:]) for r in rs]
        if numbers!=sorted(set(numbers)): raise Invalid('unordered/duplicate native register list')
        return rs
    while work:
        pc,tags,copied=work.pop(); key=(pc,tuple(sorted(tags.items())),copied)
        if key in seen: continue
        seen.add(key)
        if len(seen)>4096: raise Invalid('native provenance CFG state bound')
        if pc not in code: raise Invalid('unresolved native provenance successor '+hex(pc))
        op,args=code[pc]; tags=dict(tags); copied=dict(copied)
        args=re.sub(r'\bip\b','r12',args)
        def load(base,off):
            origin=tags.get(base)
            if base=='pc': return ('constant',word_read(((pc+4)&~3)+off))
            if origin is None: raise Invalid('unknown native load base at '+hex(pc))
            region,start=origin; off+=start
            if region=='g' and 0<=off<=36 and off%4==0:
                return ('field',off) if off<12 else ('scalar',0)
            if region=='io' and off in (0,4,8): return ('input',off)
            if region=='stack' and -32<=off<=4: return ('scalar',0)
            raise Invalid('unsupported native load dependency at '+hex(pc))
        def store(base,off,value):
            origin=tags.get(base)
            if origin is None: raise Invalid('unknown native store base at '+hex(pc))
            region,start=origin; off+=start
            if region=='g' and 0<=off<=36 and off%4==0:
                if off<12:
                    require(value==('input',off),'native copied field provenance at '+hex(pc))
                    copied[off]=value
            elif region!='stack' or not -32<=off<=0:
                raise Invalid('unsupported native store dependency at '+hex(pc))
        def address(text):
            m=re.fullmatch(r'\[(r\d+|sp|pc)(?:, #(-?\d+))?\]',text)
            if m is None: raise Invalid('unsupported native addressing at '+hex(pc))
            return m[1],int(m[2] or 0)
        if op in ('mov','movs'):
            dst,src=args.split(', ',1)
            tags[dst]=('constant',int(src[1:])) if src.startswith('#') else tags.get(src,('scalar',0))
        elif op=='push':
            rs=reglist(args); tags['sp']=('stack',tags['sp'][1]-4*len(rs))
        elif op=='pop':
            if 'pc' not in reglist(args): raise Invalid('unsupported native nonreturn pop')
            continue
        elif op=='ldr':
            dst,src=args.split(', ',1); base,off=address(src); tags[dst]=load(base,off)
        elif op=='str':
            src,dst=args.split(', ',1); base,off=address(dst); store(base,off,tags.get(src))
        elif op=='strd':
            a,b,dst=args.split(', ',2); base,off=address(dst)
            store(base,off,tags.get(a)); store(base,off+4,tags.get(b))
        elif op in ('ldmia','stmia'):
            base,listed=args.split(', ',1)
            if base.endswith('!'): raise Invalid('unsupported native multi writeback')
            rs=reglist(listed)
            if op=='ldmia':
                # All addresses use the OLD base even when base is in the list.
                values=[load(base,i*4) for i in range(len(rs))]
                tags.update(zip(rs,values))
            else:
                for i,r in enumerate(rs): store(base,i*4,tags.get(r))
        elif op in ('adds','subs','add'):
            parts=args.split(', '); dst=parts[0]
            if dst=='sp' and len(parts)==2 and parts[1].startswith('#') and op=='add':
                tags['sp']=('stack',tags['sp'][1]+int(parts[1][1:]))
            else: tags[dst]=('scalar',0)
        elif op in ('cmp','nop'): pass
        elif op=='blx':
            offset=8 if mode=='indirect-write' else 4
            require(mode in ('indirect','indirect-write') and tags.get(args)==('field',offset) and
                    tags.get('r0')==('field',0),
                    'native indirect target/context on every path at '+hex(pc))
            seams.append({'pc':pc,'target_field':offset,'context_field':0}); continue
        elif op in ('b','beq','bne','bhi','bcs','cbz','cbnz'):
            target=int(args.split(', ')[-1],16)
            work.append((target,dict(tags),tuple(sorted(copied.items()))))
            if op=='b': continue
        else: raise Invalid('unsupported native provenance instruction '+hex(pc)+' '+op)
        if mode=='copy' and pc==0x4c42:
            require(copied=={0:('input',0),4:('input',4),8:('input',8)},
                    'native descriptor copy preserves all original fields')
            seams.append({'pc':pc,'copy':copied}); continue
        if pc not in following: raise Invalid('native provenance falls out of bounded CFG')
        work.append((following[pc],tags,tuple(sorted(copied.items()))))
    require(bool(seams),'native provenance seam is reachable')
    return seams


def native_copy_suffix(claim):
    # Complete suffix, including every write/call and every failure successor.
    # r4 is never redefined: g+0..11 cannot be overwritten after the copy.
    # read_reg output arguments are sp, sp+4, g+28, g+32; write_reg's direct
    # leaf only writes MMIO, and delay has no memory effects.
    return machine_contract([r for r in claim if r[0]>0x4c42],'''
4c46 str r5, [r4, #20]; 4c48 mov r0, r4; 4c4a ldr r1, [pc, #268]
4c4c str r3, [r4, #36]; 4c4e str r6, [r4, #24]
4c50 add r2, sp, #4; 4c52 bl 129e; 4c56 cmp r0, #0; 4c58 beq 4c08
4c5a movs r3, #1; 4c5c ldr r2, [sp, #4]; 4c5e adds r5, #16
4c60 lsl r5, r3, r5; 4c64 mov r0, r4; 4c66 ldr r1, [pc, #240]
4c68 orrs r2, r5; 4c6a bl 12cc; 4c6e cmp r0, #0; 4c70 beq 4c08
4c72 ldr r2, [sp, #4]; 4c74 mov r0, r4; 4c76 ldr r1, [pc, #224]
4c78 orrs r2, r5; 4c7a bl 12fc; 4c7e cmp r0, #0; 4c80 beq 4c08
4c82 ldr r3, [r4, #12]; 4c84 ldr r0, [r4, #0]; 4c86 blx r3
4c88 ldr r3, [r4, #24]; 4c8a cmp r3, r6; 4c8c bne 4c08
4c8e ldr r5, [r4, #20]; 4c90 mov r2, sp
4c92 add r5, r5, #262144; 4c96 adds r5, #48
4c98 lsls r5, r5, #12; 4c9a add r7, r5, #12
4c9e mov r1, r7; 4ca0 mov r0, r4; 4ca2 bl 129e
4ca6 cmp r0, #0; 4ca8 beq 4c08; 4caa ldr r6, [sp, #0]
4cac cmp r6, #0; 4cae bne 4c08; 4cb0 mov r0, r4
4cb2 ldr r1, [pc, #168]; 4cb4 add r2, r4, #28; 4cb8 bl 129e
4cbc cmp r0, #0; 4cbe beq 4c08; 4cc0 mov r0, r4
4cc2 ldr r1, [pc, #156]; 4cc4 add r2, r4, #32; 4cc8 bl 129e
4ccc cmp r0, #0; 4cce beq 4c08; 4cd0 mov r2, r6
4cd2 mov r1, r5; 4cd4 mov r0, r4; 4cd6 bl 12cc
4cda cmp r0, #0; 4cdc beq 4c08; 4cde movs r2, #2
4ce0 mov r0, r4; 4ce2 adds r1, r5, #4; 4ce4 bl 12cc
4ce8 cmp r0, #0; 4cea beq 4c08; 4cec mov r2, r6
4cee mov r0, r4; 4cf0 add r1, r5, #24; 4cf4 bl 12cc
4cf8 cmp r0, #0; 4cfa beq 4c08; 4cfc mov r2, #4294967295
4d00 mov r0, r4; 4d02 add r1, r5, #40; 4d06 bl 12cc
4d0a cmp r0, #0; 4d0c beq 4c08; 4d10 movw r2, #1807
4d14 mov r0, r4; 4d16 add r1, r5, #36; 4d1a bl 12cc
4d1e cmp r0, #0; 4d20 beq 4c08; 4d24 movs r2, #1
4d26 mov r1, r7; 4d28 mov r0, r4; 4d2a bl 12cc
4d2e cmp r0, #0; 4d30 beq 4c08; 4d34 mov r0, r4
4d36 ldr r1, [pc, #28]; 4d38 bl 1314
4d3c cmp r0, #0; 4d3e beq 4c08; 4d42 ldr r3, [pc, #32]
4d44 str r3, [r4, #24]; 4d46 b 4c12
4d48 .word 0xb8afabcd; 4d4c .word 0x47505436; 4d50 .word 0x000013e1
4d54 .word 0x47505433; 4d58 .word 0x400fe104
4d5c .word 0x400fe060; 4d60 .word 0x400fe070; 4d64 .word 0x47505431
''','native copy remainder cannot replace descriptor')


def division_dependency(rows):
    """Prove memory independence, not arithmetic correctness of libgcc divide.

    r2 is an invariant eight-byte caller-frame result pointer loaded once from
    incoming AAPCS stack slot. All other non-stack operations are scalar-only.
    Thus every possible arithmetic/branch path has the same memory boundary;
    no numeric result or success default is needed for this closure property.
    """
    required={0x6f80:('stmdb','sp!, {r4, r5, r6, r7, r8, r9, sl, lr}'),
              0x6f8e:('ldr','r2, [sp, #32]')}
    require(all((op,args)==required[pc] for pc,op,args in rows if pc in required) and
            all(pc in {r[0] for r in rows} for pc in required),
            'division stack argument derives original caller result pointer')
    scalar={'mov','movs','clz','rsb','lsls','lsl','lsr','lsrs','orr','orrs',
            'udiv','mls','uxth','mul','adds','add','subs','sub','sbc','sbcs',
            'umull','cmp'}
    branches={'b','beq','bne','bhi','bls','bcc','bcs','cbz','cbnz'}
    pcs={pc for pc,_,_ in rows}; edges=[]; memory=[]
    for pc,op,args in rows:
        clean=re.sub(r' <[^>]+>','',args)
        if pc in required: memory.append((pc,op,args)); continue
        if op in branches:
            target=int(clean.split(', ')[-1],16)
            require(target in pcs and target>0x6f8e,
                    'division branch remains inside scalar body, never repeats frame allocation')
            edges.append((pc,target)); continue
        if op=='ldmia':
            require(args=='sp!, {r4, r5, r6, r7, r8, r9, sl, pc}',
                    'division only restores its own frame')
            memory.append((pc,op,args)); continue
        if op=='strd':
            require(re.fullmatch(r'(r\d+|ip), (r\d+|ip), \[r2\]',args) is not None,
                    'division stores only eight bytes to caller result slot')
            memory.append((pc,op,args)); continue
        if op not in scalar: raise Invalid('unsupported division dependency '+hex(pc)+' '+op)
        require(not re.search(r'\b(sp|pc)\b',args),'division scalar cannot construct stack/PC dependency')
        # UMULL writes both RdLo and RdHi; checking only the first operand
        # would let its high result silently replace the caller result pointer.
        destinations=[] if op=='cmp' else [part.strip() for part in args.split(',')[:2 if op=='umull' else 1]]
        require('r2' not in destinations,'division result pointer remains invariant')
        if not re.fullmatch(r'[r0-9a-fxipls#, \-]+',args):
            raise Invalid('unsupported scalar division operand '+hex(pc))
    return {'memory':memory,'branch_edges':edges,'scope':'memory independence, not quotient correctness'}


def retained_literal_value(value,pc):
    require(not 0x20000000<=value<0x2000dbc0,
            'retained ordinary-RAM borrowing at '+hex(pc))
    return value


def retained_extent(elf,dis,name,address,rows):
    # libgcc's assembly __aeabi_uldivmod really has STT_FUNC size zero. A
    # zero-size *symbol* is insufficient, but its exact decoded contract plus
    # contiguous instruction bytes ending at the next typed entry is authority.
    symbols=[r for r in elf.symbols.get(name,[]) if r[3]&~1==address]
    if len(symbols)!=1: raise Invalid('ambiguous retained typed entry '+name)
    symbol=symbols[0]; section=elf.sections.get(symbol[2])
    require(symbol[1]==2 and section is not None and section[1]==1 and section[2]&4 and
            0<address<0x8000 and section[3]<=address<section[3]+section[5],
            'actual typed executable boot-flash dependency '+name)
    if symbol[4]: end=address+symbol[4]
    else:
        require((name,address)==('__aeabi_uldivmod',0x6f50),
                'only independently decoded zero-size assembly contract is supported')
        ends={r[3]&~1 for records in elf.symbols.values() for r in records
              if r[1]==2 and r[2]==symbol[2] and r[3]&~1>address}
        if not ends: raise Invalid('unresolved assembly closure extent')
        end=min(ends)
        require(end==0x6f80,'division wrapper ends at actual typed division entry')
    require(end<=min(0x8000,section[3]+section[5]),'whole retained extent stays in boot flash')
    block=[m[0] for m in re.finditer(
        r'^\s*([0-9a-f]+) <([^>]+)>:\s*\n.*?(?=^\s*[0-9a-f]+ <|^Disassembly of section |\Z)',
        dis,re.M|re.S) if int(m[1],16)==address and m[2]==name]
    if len(block)!=1: raise Invalid('unresolved encoded retained body')
    widths={}
    for line in block[0].splitlines()[1:]:
        m=re.fullmatch(r'\s*([0-9a-f]+):\s+((?:[0-9a-f]{4,8}\s+)+)[.a-z].*',line)
        if m: widths[int(m[1],16)]=sum(len(word)//2 for word in m[2].split())
    cursor=address
    for pc,op,args in rows:
        if pc>=end:
            require((op,args)==('nop','') and pc==end and widths.get(pc)==2,
                    'only unreachable assembler alignment outside typed extent')
            continue
        require(pc==cursor and widths.get(pc) in (2,4),
                'contiguous actual retained instruction/data bytes '+name)
        cursor+=widths[pc]
    require(cursor==end,'decoded retained body covers entire executable extent '+name)
    return address,end


def callbacks_gate(elf,dis,s):
    contracts={'boot_startup_gptm_read':RETAINED_READ,
        'read_reg.lto_priv.1':RETAINED_REGISTER,'equal':RETAINED_EQUAL,
        'matched':RETAINED_MATCHED,'__aeabi_uldivmod':RETAINED_DIV_WRAPPER,
        '__aeabi_idiv0':'7298 bx lr; 729a nop',
        'boot_startup_gptm_direct_read':'''14b0 ldr r3, [r1, #0]
            14b2 movs r0, #0; 14b4 str r3, [r2, #0]; 14b6 bx lr''',
        'boot_startup_gptm_direct_write':'''14b8 movs r0, #0
            14ba str r2, [r1, #0]; 14bc bx lr''',
        'boot_startup_gptm_direct_delay':'''164 nop; 166 nop; 168 nop; 16a bx lr'''}
    # Actual branch closure, including libgcc tail/zero paths; no unvisited
    # direct edge can be accepted merely because its name looks harmless.
    work=[('boot_startup_gptm_read',0x13e0),
          ('boot_startup_gptm_direct_read',0x14b0),
          ('boot_startup_gptm_direct_write',0x14b8),
          ('boot_startup_gptm_direct_delay',0x164)]
    visited=[]; bodies={}; indirect=[]; edges=[]; literals={}; extents={}
    while work:
        name,address=work.pop()
        if (name,address) in visited: continue
        rows=function_rows(dis,name,address)
        extent=retained_extent(elf,dis,name,address,rows); extents[name]=extent
        visited.append((name,address)); bodies[name]=rows
        if name=='__udivmoddi4': division=division_dependency(rows)
        elif name in contracts: machine_contract(rows,contracts[name],'retained '+name)
        else: raise Invalid('unsupported reachable retained dependency '+name)
        # Literal pools may be in FUNC size, but cannot be branch destinations.
        code_pcs={pc for pc,op,_ in rows if not op.startswith('.') and pc<extent[1]}
        for pc,op,args in rows:
            if op=='ldr' and re.fullmatch(r'r\d+, \[pc, #\d+\]',args):
                off=int(re.search(r'#(\d+)',args)[1]); la=((pc+4)&~3)+off
                value=elf_word(elf,la); literals[pc]=(la,value)
                retained_literal_value(value,pc)
            if op in ('bl','b'):
                m=re.fullmatch(r'([0-9a-f]+) <([^>]+)>',args)
                if m is None: raise Invalid('unresolved retained direct edge at '+hex(pc))
                target=int(m[1],16)
                if target in code_pcs: edges.append((pc,target)); continue
                # Tail/direct calls must resolve an exact typed entry, never a
                # symbol-plus-offset guessed from objdump annotation.
                if '+' in m[2]: raise Invalid('retained external interior edge')
                edges.append((pc,target)); work.append((m[2],target))
            elif op=='blx' or op=='bx' and args!='lr':
                require(name=='read_reg.lto_priv.1' and pc==0x12ae,
                        'only modeled retained native indirect seam')
                indirect.extend(native_provenance(rows,lambda a:elf_word(elf,a),'indirect'))
            elif op.startswith('b') and op not in ('bic','bfc','bx') or op in ('cbz','cbnz'):
                # Conditional branches cannot escape to unchecked blocks/data.
                m=re.search(r'([0-9a-f]+) <[^>]+>$',args)
                if m is None: raise Invalid('unresolved retained conditional edge')
                require(int(m[1],16) in code_pcs,'retained conditional edge stays in checked CFG')
                edges.append((pc,int(m[1],16)))
    claim=instruction_rows(dis,'boot_startup_gptm_claim')
    prefix=[r for r in claim if r[0]<=0x4c42]
    machine_contract(prefix,RETAINED_CLAIM_PREFIX,'native descriptor-copy guarded CFG')
    copy=native_provenance(prefix,lambda a:elf_word(elf,a),'copy')
    # The remainder cannot replace copied io[0..12). Inspect every subsequent
    # instruction, and require its known store sites rather than ignoring STR.
    suffix=native_copy_suffix(claim)
    write_rows=function_rows(dis,'write_reg.lto_priv.1',0x12cc)
    require(elf.code('write_reg.lto_priv.1',0x12cc),'typed native write dependency')
    machine_contract(write_rows,'''12cc push {r3, r4, r5, lr}
12ce ldr r5, [r0, #24]; 12d0 ldr r3, [pc, #32]; 12d2 mov r4, r0
12d4 adds r3, r5, r3; 12d6 cmp r3, #2; 12d8 bhi 12ec
12da ldr r3, [r0, #8]; 12dc ldr r0, [r0, #0]; 12de blx r3
12e0 cbnz r0, 12e8; 12e2 ldr r3, [r4, #24]
12e4 cmp r3, r5; 12e6 beq 12f0; 12e8 ldr r3, [pc, #12]
12ea str r3, [r4, #24]; 12ec movs r0, #0
12ee pop {r3, r4, r5, pc}; 12f0 movs r0, #1; 12f2 b 12ee
12f4 .word 0xb8afabcd; 12f8 .word 0x47505436
''','native claim write dependency')
    indirect.extend(native_provenance(write_rows,lambda a:elf_word(elf,a),'indirect-write'))
    # Actual claim pools bind the advertised producer and all MMIO constants.
    literal_contract(elf,claim,{
        0x4c00:(0x4d48,0xb8afabcd),0x4c08:(0x4d4c,0x47505436),
        0x4c3e:(0x4d50,0x13e1),0x4c40:(0x4d54,0x47505433),
        0x4c4a:(0x4d58,0x400fe104),0x4c66:(0x4d58,0x400fe104),
        0x4c76:(0x4d58,0x400fe104),0x4cb2:(0x4d5c,0x400fe060),
        0x4cc2:(0x4d60,0x400fe070),0x4d36:(0x4d54,0x47505433),
        0x4d42:(0x4d64,0x47505431)},'native claim')
    evidence={'visited':visited,'executable_extents':extents,'direct_edges':edges,'indirect_bindings':indirect,
        'native_descriptor_copy':copy,'native_copy_suffix':suffix,
        'native_write_dependency':write_rows,
        'native_delay_binding':{'pc':0x4c86,'target_field':12,'context_field':0},
        'literals':literals,'division':division,
        'closure_boundaries':{'context_bytes':40,'output_bytes':24,
            'timer_mmio_bases':[0x40030000,0x40031000,0x40032000,0x40033000],
            'system_mmio':[0x400fe104,0x400fe060,0x400fe070],
            'ordinary_boot_sram_dependencies':[],
            'requires':'entry-derived native direct read/write/delay/context bindings'},
        'machine_contracts':bodies}
    s.manifest['retained_direct_closure']=evidence
    (s.out/'retained-closure.json').write_text(json.dumps(evidence,indent=2)+'\n')
    return evidence


def app_fixture(s):
    """Historical FTPA only. No replacement application build is authorized."""
    from pathlib import Path
    from checked_support import FTPA_SHA, sha
    path=Path(s.args.ftpa_elf)
    if path.is_symlink() or not path.is_file() or sha(path)!=FTPA_SHA:
        raise Invalid('identified historical FTPA fixture missing/aliased/changed')
    elf,dis=inspect(s,path)
    bounds=(elf.value('_sdata'),elf.value('_edata'),elf.value('_sbss'),elf.value('_ebss'))
    require(bounds==(0x20000000,0x20000008,0x20000008,0x20009c48),
            'historical FTPA exact data/BSS symbol bounds')
    require(all(end<=0x2000dbc0 or start>=0x2000e000 for start,end in
                ((bounds[0],bounds[1]),(bounds[2],bounds[3]))),'FTPA initializer symbol exclusion')
    s.manifest['ftpa']={'path':str(path),'sha256':sha(path),'symbol_bounds':bounds,
        'source_freshness_claim':False,'physical_execution':False}
    evidence=ftpa_machine_witness(elf,dis)
    boot_path=s.root/'arm-public-repair-1/dispatch/src/lm3s6965_boot_dispatch'
    boot,boot_dis=inspect(s,boot_path)
    evidence['boot_native_contract']=boot_native_witness(boot,boot_dis)
    s.manifest['ftpa'].update(evidence)
    (s.out/'ftpa-static.json').write_text(json.dumps(s.manifest['ftpa'],indent=2)+'\n')
    return s.manifest['ftpa']

def ftpa_native_dependencies(elf,dis,evidence):
    controller='''
e414 mov r2, r0; e416 ldr r0, [r0, #80]; e418 cbz r0, e43e
e41a ldr r0, [r2, #84]; e41c cbnz r0, e438
e41e ldr r3, [r2, #88]; e420 cmp r3, #2; e422 bne e43e
e424 cbz r1, e43c; e426 ldr r1, [r2, #92]; e428 ldr r3, [r2, #28]
e42a cmp r1, r3; e42c bne e43e; e42e ldr r0, [r2, #96]
e430 clz r0, r0; e434 lsrs r0, r0, #5; e436 bx lr
e438 movs r0, #0; e43a bx lr; e43c movs r0, #1; e43e bx lr
'''
    clock='''
e51c push {r4, r5, r6, lr}; e51e sub sp, #24; e520 mov r4, r0
e522 bl e4ac; e526 mov r6, r1; e528 mov r5, r0; e52a cbnz r0, e534
e52c movs r5, #0; e52e mov r0, r5; e530 add sp, #24
e532 pop {r4, r5, r6, pc}; e534 ldr r3, [r4, #344]
e538 subs r2, r3, #1; e53a cmp r2, #858993459; e53e bcs e52c
e540 ldr r2, [r4, #336]; e544 cmp r2, #0; e546 beq e52c
e548 ldr r2, [r4, #340]; e54c cmp r2, #0; e54e beq e52c
e550 ldr r2, [r4, #348]; e554 cmp r2, #0; e556 beq e52c
e558 ldr r2, [r4, #32]; e55a cmp r3, r2; e55c bne e52c
e55e ldr r2, [r4, #36]; e560 add r3, r3, r3, lsl #2
e564 cmp r2, r3; e566 bne e52c; e568 ldr r3, [r4, #100]
e56a cmp r2, r3; e56c bne e52c; e56e mov r1, sp
e570 ldr r3, [r4, #320]; e574 ldr r0, [r4, #324]; e578 blx r3
e57a cmp r0, #0; e57c bne e52c; e57e ldr r3, [r4, #348]
e582 ldr r2, [sp, #20]; e584 cmp r2, r3; e586 bne e52c
e588 ldr r3, [r4, #32]; e58a ldr r2, [sp, #16]
e58c cmp r2, r3; e58e bne e52c; e590 ldr r2, [r4, #328]
e594 ldr r3, [sp, #8]; e596 eors r3, r2; e598 ldr r2, [r4, #336]
e59c tst r3, r2; e59e bne e52c; e5a0 ldr r2, [r4, #332]
e5a4 ldr r3, [sp, #12]; e5a6 eors r3, r2; e5a8 ldr r2, [r4, #340]
e5ac tst r3, r2; e5ae bne e52c; e5b0 ldrd r2, r3, [sp]
e5b4 strd r2, r3, [r6]; e5b8 b e52e
'''
    # GNU omits a trailing zero alignment halfword as '...'. It is outside
    # the executable CFG; obtain precisely the rows before that marker.
    block=re.search(r'^0000e51c <clock_read>:\n(.*?)(?=^0000e5bc)',dis,re.M|re.S)
    if block is None: raise Invalid('missing clock-read dependency')
    clock_dis='0000e51c <clock_read>:\n'+block[1].replace('\t...','')
    evidence['controller']=machine_contract(instruction_rows(dis,'controller'),controller,'native controller')
    evidence['clock_read']=machine_contract(instruction_rows(clock_dis,'clock_read'),clock,'native clock dependency')
    # No callback body is fabricated: ownership constrains this native call to
    # the retained boot-flash closure, whose binding is a composite dependency.
    evidence['clock_indirect']={'pc':0xe578,'target_field':320,'context_field':324,
        'requires':'entry-derived descriptor binding and retained-callback-closure'}


def boot_native_witness(elf,dis,include_terminal=True):
    claim='''
1144 push {r4, lr}; 1146 cbz r0, 1170; 1148 ldr r2, [r0, #0]
114a ldr r3, [pc, #52]; 114c cmp r2, r3; 114e bne 1170
1150 ldr r3, [r0, #4]; 1152 cmp r3, #1; 1154 bne 1170
1156 ldr r2, [r0, #8]; 1158 cmp r2, #1024; 115c bne 1170
115e ldr r4, [r0, #12]; 1160 ldr r2, [pc, #32]
1162 cmp r4, r2; 1164 bne 1170; 1166 ldr r2, [r0, #56]
1168 cbnz r2, 1170; 116a ldr r2, [r0, #16]; 116c cmp r2, r1
116e beq 1178; 1170 bl 1134; 1174 movs r0, #0
1176 pop {r4, pc}; 1178 str r3, [r0, #56]; 117a mov r0, r3
117c b 1176; 117e nop; 1180 .word 0x31534342; 1184 .word 0x4d330001
'''
    ownership='''
1194 mov r2, r0; 1196 push {r3, lr}; 1198 ldr r0, [r0, #68]
119a bl 1188; 119e cbz r0, 11f6; 11a0 ldr r0, [r2, #72]
11a2 bl 1188; 11a6 cbz r0, 11f6; 11a8 ldr r0, [r2, #320]
11ac bl 1188; 11b0 cbz r0, 11f6; 11b2 ldr r1, [r2, #64]
11b4 add r3, r2, #384; 11b8 cmp r1, r3; 11ba bne 11f8
11bc ldr r3, [r2, #324]; 11c0 cmp r3, r1; 11c2 bne 11f8
11c4 ldr r3, [r2, #132]; 11c8 cbnz r3, 11f8
11ca ldr r0, [r2, #136]; 11ce cbnz r0, 11f8
11d0 ldr r3, [r2, #140]; 11d4 cbnz r3, 11f6
11d6 ldrb r0, [r2, #151]; 11da cbz r0, 11f6
11dc ldrb r3, [r2, #150]; 11e0 cbnz r3, 11f8
11e2 ldr r3, [pc, #24]; 11e4 cmp r2, r3; 11e6 bne 11f8
11e8 ldr r0, [r3, #128]; 11ec add r3, r3, #1024
11f0 subs r3, r0, r3; 11f2 negs r0, r3; 11f4 adcs r0, r3
11f6 pop {r3, pc}; 11f8 movs r0, #0; 11fa b 11f6
11fc .word 0x2000dbc0
'''
    evidence={}
    for name,spec in {'claim':claim,'ownership':ownership,
            'boot_code.lto_priv.1':'''1188 bfc r0, #1, #14
                118c subs r3, r0, #1; 118e negs r0, r3
                1190 adcs r0, r3; 1192 bx lr''',
            'fail.isra.0':'''1134 cbz r0, 1142; 1136 movs r3, #0
                1138 str r3, [r0, #56]; 113a dmb sy; 113e movs r3, #5
                1140 str r3, [r0, #16]; 1142 bx lr'''}.items():
        address=contract_rows(spec)[0][0]
        if elf is not None:
            require(elf.code(name,address),'boot native executable dependency '+name)
        evidence[name]=machine_contract(function_rows(dis,name,address),spec,'boot native '+name)
    if elf is not None:
        literal_contract(elf,function_rows(dis,'claim',0x1144),{
            0x114a:(0x1180,0x31534342),0x1160:(0x1184,0x4d330001)},'boot ABI')
        literal_contract(elf,function_rows(dis,'ownership',0x1194),{
            0x11e2:(0x11fc,0x2000dbc0)},'boot ownership')
    evidence['prepare']=boot_prepare_witness(elf,dis)
    if include_terminal: evidence['terminal']=terminal_gate(dis,elf)
    return evidence


def boot_prepare_witness(elf,dis):
    rows=instruction_rows(dis,'boot_startup_prepare')
    decoded=machine_contract(rows,BOOT_PREPARE,'boot prepare native field publication')
    memcpy=machine_contract(instruction_rows(dis,'memcpy'),'''
6ea0 add r2, r1; 6ea2 cmp r1, r2; 6ea4 add r3, r0, #4294967295
6ea8 bne 6eac; 6eaa bx lr; 6eac push {r4, lr}
6eae ldrb r4, [r1], #1; 6eb2 cmp r1, r2
6eb4 strb r4, [r3, #1]!; 6eb8 bne 6eae; 6eba pop {r4, pc}
''','boot prepare actual 120-byte copy body')
    if elf is not None:
        require(elf.code('boot_startup_prepare') and elf.code('memcpy'),
                'typed boot prepare and actual copy dependency')
        literal_contract(elf,rows,{0x4a64:(0x4b74,0x1b314),
                                 0x4a90:(0x4b78,0x20010000)},'boot prepare')
    # Full CFG above preserves original r4=capsule, r5=launch and r7=launch+252
    # across AAPCS calls. It copies launch[0..120) to capsule[160..280),
    # launch[252..284) to capsule[288..320), and separately publishes +20/+24.
    # These are byte/field provenance equations, NOT current-header offsets.
    require(160+4==164 and 160+116==276 and 288+4==292,
            'native image/base/reset/MSP offset equations')
    return {'instructions':decoded,'memcpy':memcpy,
        'original_arguments':['capsule','runtime','launch'],
        'copies':[{'source':'launch','source_offset':0,'destination_offset':160,'bytes':120},
                  {'source':'launch','source_offset':252,'destination_offset':288,'bytes':32}],
        'field_sources':{20:252,24:260,28:280,164:4,276:116,292:256},
        'state_transition':[0,1],
        'requires':'entry witness binds original runtime/launch and retained producer descriptors'}


BOOT_PREPARE='''
497a push {r0, r1, r2, r4, r5, r6, r7, lr}
497c mov r3, r1; 497e mov r4, r0; 4980 mov r5, r2
4982 cbnz r1, 4990; 4984 mov r0, r4; 4986 bl 1134
498a movs r0, #0; 498c add sp, #12; 498e pop {r4, r5, r6, r7, pc}
4990 cmp r2, #0; 4992 beq 4984; 4994 cmp r0, #0; 4996 beq 4984
4998 cmp r1, r0; 499a bcc 4b46; 499e subs r2, r1, r0
49a0 cmp r2, #1024; 49a4 ite cs; 49a6 movcs r2, #0; 49a8 movcc r2, #1
49aa cmp r2, #0; 49ac bne 4984; 49ae cmp r4, r5; 49b0 bhi 4b52
49b4 subs r1, r5, r4; 49b6 cmp r1, #1024
49ba ite cs; 49bc movcs r1, #0; 49be movcc r1, #1
49c0 cmp r1, #0; 49c2 bne 4984; 49c4 ldr r2, [r3, #0]
49c6 cmp r2, #0; 49c8 bne 4984; 49ca ldr r2, [r3, #4]
49cc lsls r0, r2, #28; 49ce bpl 4984; 49d0 ldr r3, [r3, #212]
49d4 cmp r3, #0; 49d6 bne 4984; 49d8 mov r0, r4; 49da bl 1144
49de cmp r0, #0; 49e0 beq 498a; 49e2 cmp r4, r5; 49e4 bhi 4b5a
49e8 subs r3, r5, r4; 49ea cmp r3, #1024
49ee ite cs; 49f0 movcs r3, #0; 49f2 movcc r3, #1
49f4 cmp r3, #0; 49f6 bne 4984; 49f8 add r7, r5, #252
49fc cmp r4, r7; 49fe bhi 4b66; 4a02 subs r3, r7, r4
4a04 cmp r3, #1024; 4a08 ite cs; 4a0a movcs r3, #0; 4a0c movcc r3, #1
4a0e cmp r3, #0; 4a10 bne 4984; 4a12 ldr r3, [r4, #16]
4a14 cmp r3, #0; 4a16 bne 4984; 4a18 mrs r2, primask
4a1c mrs r3, ipsr; 4a20 cmp r2, #1; 4a22 bne 4984
4a24 cmp r3, #0; 4a26 bne 4984; 4a28 mov r0, r4; 4a2a bl 1194
4a2e cmp r0, #0; 4a30 beq 4984; 4a32 ldr r3, [r4, #80]
4a34 cmp r3, #0; 4a36 beq 4984; 4a38 ldr r3, [r4, #84]
4a3a cmp r3, #0; 4a3c bne 4984; 4a3e ldr r3, [r4, #88]
4a40 cmp r3, #2; 4a42 bne 4984; 4a44 ldr r3, [r4, #96]
4a46 cmp r3, #0; 4a48 bne 4984; 4a4a ldr r2, [r5, #0]
4a4c cmp r2, #1; 4a4e bhi 4984; 4a50 cmp r2, #0; 4a52 ite ne
4a54 movne r2, #147456; 4a58 moveq r2, #34816
4a5c ldr r3, [r5, #4]; 4a5e cmp r3, r2; 4a60 bne 4984
4a62 ldr r0, [r5, #8]; 4a64 ldr r2, [pc, #268]
4a66 sub r1, r0, #236; 4a6a cmp r1, r2; 4a6c bhi 4984
4a6e lsls r2, r0, #30; 4a70 bne 4984; 4a72 ldr r1, [r5, #116]
4a74 bic r2, r1, #1; 4a78 cmp r3, r2; 4a7a bhi 4984
4a7c subs r2, r2, r3; 4a7e cmp r0, r2; 4a80 bls 4984
4a82 ldr r2, [r5, #252]; 4a86 cmp r3, r2; 4a88 bne 4984
4a8c ldr r3, [r5, #256]; 4a90 ldr r2, [pc, #228]
4a92 cmp r3, r2; 4a94 bne 4984; 4a98 ldr r2, [r5, #112]
4a9a cmp r2, r3; 4a9c bne 4984; 4aa0 ldr r3, [r5, #260]
4aa4 cmp r1, r3; 4aa6 bne 4984; 4aaa lsls r3, r1, #31
4aac bpl 4984; 4ab0 ldr r2, [r5, #280]; 4ab4 ldr r3, [r4, #92]
4ab6 cmp r2, r3; 4ab8 bne 4984; 4abc ldr r3, [r5, #264]
4ac0 cmp r3, #0; 4ac2 bne 4984; 4ac6 ldr r3, [r5, #268]
4aca cmp r3, #0; 4acc bne 4984; 4ad0 ldr r3, [r5, #272]
4ad4 cmp r3, #0; 4ad6 bne 4984; 4ada ldr r3, [r5, #276]
4ade cmp r3, #0; 4ae0 bne 4984; 4ae4 movs r2, #120
4ae6 mov r1, r5; 4ae8 add r0, r4, #160; 4aec add r6, r4, #288
4af0 bl 6ea0; 4af4 ldmia r7!, {r0, r1, r2, r3}
4af6 stmia r6!, {r0, r1, r2, r3}; 4af8 ldmia r7, {r0, r1, r2, r3}
4afc stmia r6, {r0, r1, r2, r3}; 4b00 ldr r3, [r5, #252]
4b04 mov r1, sp; 4b06 str r3, [r4, #20]; 4b08 ldr r3, [r5, #260]
4b0c mov r0, r4; 4b0e str r3, [r4, #24]; 4b10 ldr r3, [r5, #280]
4b14 str r3, [r4, #28]; 4b16 ldr r3, [r4, #344]
4b1a str r3, [r4, #32]; 4b1c ldr r3, [r4, #100]
4b1e str r3, [r4, #36]; 4b20 bl 1200; 4b24 cmp r0, #0
4b26 beq 4984; 4b2a ldr r3, [r4, #56]; 4b2c cmp r3, #0
4b2e beq 4984; 4b32 ldr r3, [r4, #16]; 4b34 cmp r3, #0
4b36 bne 4984; 4b3a str r3, [r4, #56]; 4b3c dmb sy
4b40 movs r3, #1; 4b42 str r3, [r4, #16]; 4b44 b 498c
4b46 subs r2, r0, r1; 4b48 cmp r2, #243; 4b4a ite hi
4b4c movhi r2, #0; 4b4e movls r2, #1; 4b50 b 49aa
4b52 subs r1, r4, r5; 4b54 cmp r1, #284; 4b58 b 49ba
4b5a subs r3, r4, r5; 4b5c cmp r3, #119; 4b5e ite hi
4b60 movhi r3, #0; 4b62 movls r3, #1; 4b64 b 49f4
4b66 subs r3, r4, r7; 4b68 cmp r3, #31; 4b6a ite hi
4b6c movhi r3, #0; 4b6e movls r3, #1; 4b70 b 4a0e
4b72 nop; 4b74 .word 0x0001b314; 4b78 .word 0x20010000
'''


BOOT_CLOCK='''
1200 push {r4, r5, r6, lr}; 1202 sub sp, #24
1204 mov r4, r0; 1206 mov r6, r1; 1208 bl 1194
120c mov r5, r0; 120e cbnz r0, 1218; 1210 movs r5, #0
1212 mov r0, r5; 1214 add sp, #24; 1216 pop {r4, r5, r6, pc}
1218 ldr r3, [r4, #344]; 121c subs r2, r3, #1
121e cmp r2, #858993459; 1222 bcs 1210
1224 ldr r2, [r4, #336]; 1228 cmp r2, #0; 122a beq 1210
122c ldr r2, [r4, #340]; 1230 cmp r2, #0; 1232 beq 1210
1234 ldr r2, [r4, #348]; 1238 cmp r2, #0; 123a beq 1210
123c ldr r2, [r4, #32]; 123e cmp r3, r2; 1240 bne 1210
1242 ldr r2, [r4, #36]; 1244 add r3, r3, r3, lsl #2
1248 cmp r2, r3; 124a bne 1210; 124c ldr r3, [r4, #100]
124e cmp r2, r3; 1250 bne 1210; 1252 mov r1, sp
1254 ldr r3, [r4, #320]; 1258 ldr r0, [r4, #324]; 125c blx r3
125e cmp r0, #0; 1260 bne 1210; 1262 ldr r3, [r4, #348]
1266 ldr r2, [sp, #20]; 1268 cmp r2, r3; 126a bne 1210
126c ldr r3, [r4, #32]; 126e ldr r2, [sp, #16]
1270 cmp r2, r3; 1272 bne 1210; 1274 ldr r2, [r4, #328]
1278 ldr r3, [sp, #8]; 127a eors r3, r2; 127c ldr r2, [r4, #336]
1280 tst r3, r2; 1282 bne 1210; 1284 ldr r2, [r4, #332]
1288 ldr r3, [sp, #12]; 128a eors r3, r2; 128c ldr r2, [r4, #340]
1290 tst r3, r2; 1292 bne 1210; 1294 ldrd r2, r3, [sp]
1298 strd r2, r3, [r6]; 129c b 1212
'''

# These contracts intentionally specialize to the identified, optimized ELF.
# They are decoded instruction/CFG obligations, not digests or inferred source
# loops. In this binary GCC lowered the word loops to byte memcpy/memset calls.
# Their complete bodies below prove the write intervals, including the final
# byte and the zero-length exits. No call-name summary substitutes for a body.
FTPA_INITIALIZERS='''
9470 push {r3, lr}; 9472 ldr r2, [pc, #68]; 9474 ldr r3, [pc, #68]
9476 cmp r3, r2; 9478 bcs 948c; 947a subs r2, #1
947c subs r2, r2, r3; 947e bic r2, r2, #3; 9482 mov r0, r3
9484 ldr r1, [pc, #56]; 9486 adds r2, #4; 9488 bl 104dc
948c ldr r2, [pc, #52]; 948e ldr r3, [pc, #56]
9490 cmp r3, r2; 9492 bcs 94a6; 9494 subs r2, #1
9496 subs r2, r2, r3; 9498 bic r2, r2, #3; 949c movs r1, #0
949e mov r0, r3; 94a0 adds r2, #4; 94a2 bl 10454
94a6 bl 10480; 94aa bl 88ec; 94ae movs r2, #5
94b0 ldr r3, [pc, #24]; 94b2 str r2, [r3, #16]; 94b4 bl eaa8
94b8 .word 0x20000008; 94bc .word 0x20000000
94c0 .word 0x00010af4; 94c4 .word 0x20009c48
94c8 .word 0x20000008; 94cc .word 0x2000dbc0
'''
FTPA_MEMCPY='''
104dc add r2, r1; 104de cmp r1, r2
104e0 add r3, r0, #4294967295; 104e4 bne 104e8; 104e6 bx lr
104e8 push {r4, lr}; 104ea ldrb r4, [r1], #1
104ee cmp r1, r2; 104f0 strb r4, [r3, #1]!
104f4 bne 104ea; 104f6 pop {r4, pc}
'''
FTPA_MEMSET='''
10454 mov r3, r0; 10456 add r2, r0; 10458 cmp r3, r2
1045a bne 1045e; 1045c bx lr; 1045e strb r1, [r3], #1
10462 b 10458
'''
FTPA_CAPTURE='''
94d0 ldr r0, [pc, #80]; 94d2 mrs r1, msp; 94d6 str r1, [r0, #0]
94d8 mrs r1, control; 94dc str r1, [r0, #4]
94de mrs r1, ipsr; 94e2 str r1, [r0, #8]
94e4 mrs r1, primask; 94e8 str r1, [r0, #12]
94ea mrs r1, basepri; 94ee str r1, [r0, #16]
94f0 mrs r1, faultmask; 94f4 str r1, [r0, #20]
94f6 str lr, [r0, #24]; 94fa ldr r1, [pc, #44]
94fc ldr r1, [r1, #0]; 94fe str r1, [r0, #28]
'''
FTPA_WITNESS='''
9500 subw r1, pc, #4; 9504 str r1, [r0, #32]
9506 ldr r1, [pc, #36]; 9508 str r1, [r0, #36]
950a cpsid i; 950c ldr r0, [pc, #32]; 950e ldr r1, [pc, #28]
9510 ldr r2, [pc, #32]; 9512 bl e5bc; 9516 cmp r0, #0
9518 beq 951e; 951a b 9470; 951e b eaa8; 9522 .short 0x0000
9524 .word 0x2000dd80; 9528 .word 0xe000ed08; 952c .word 0x000094d1
9530 .word 0x2000dbc0; 9534 .word 0x00009500
'''
FTPA_ABI='''
e3e4 cbz r0, e40a; e3e6 ldr r2, [r0, #0]; e3e8 ldr r3, [pc, #32]
e3ea cmp r2, r3; e3ec bne e408; e3ee ldr r3, [r0, #4]
e3f0 cmp r3, #1; e3f2 bne e408; e3f4 ldr r3, [r0, #8]
e3f6 cmp r3, #1024; e3fa bne e408; e3fc ldr r0, [r0, #12]
e3fe ldr r3, [pc, #16]; e400 subs r3, r0, r3; e402 negs r0, r3
e404 adcs r0, r3; e406 bx lr; e408 movs r0, #0; e40a bx lr
e40c .word 0x31534342; e410 .word 0x4d330001
'''
FTPA_OWNERSHIP='''
e4ac ldr r3, [r0, #68]; e4ae ldr r2, [pc, #100]
e4b0 ands r3, r2; e4b2 cmp r3, #1; e4b4 bne e50e
e4b6 ldr r3, [r0, #72]; e4b8 ands r3, r2; e4ba cmp r3, #1
e4bc bne e50e; e4be ldr r3, [r0, #320]; e4c2 ands r3, r2
e4c4 cmp r3, #1; e4c6 bne e50e; e4c8 ldr r2, [r0, #64]
e4ca add r3, r0, #384; e4ce cmp r2, r3; e4d0 bne e50e
e4d2 ldr r3, [r0, #324]; e4d6 cmp r3, r2; e4d8 bne e50e
e4da ldr r2, [r0, #132]; e4de cbnz r2, e50e
e4e0 ldr r3, [r0, #136]; e4e4 cbnz r3, e50e
e4e6 ldr r2, [r0, #140]; e4ea cbnz r2, e50e
e4ec ldrb r3, [r0, #151]; e4f0 cbz r3, e50e
e4f2 ldrb r3, [r0, #150]; e4f6 cbnz r3, e50e
e4f8 ldr r3, [pc, #28]; e4fa cmp r0, r3; e4fc bne e50e
e4fe ldr r0, [r3, #128]; e502 add r3, r3, #1024
e506 subs r3, r0, r3; e508 negs r0, r3; e50a adcs r0, r3
e50c bx lr; e50e movs r0, #0; e510 bx lr; e512 nop
e514 .word 0xffff8001; e518 .word 0x2000dbc0
'''
FTPA_EARLY='''
e5bc push {r0, r1, r4, r5, r6, lr}; e5be mov r6, r1
e5c0 movs r1, #2; e5c2 mov r4, r0; e5c4 mov r5, r2
e5c6 bl e488; e5ca cbz r0, e5d8; e5cc bl e440; e5d0 cbnz r0, e5de
e5d2 mov r0, r4; e5d4 bl e456; e5d8 movs r0, #0
e5da add sp, #8; e5dc pop {r4, r5, r6, pc}
e5de movs r1, #1; e5e0 mov r0, r4; e5e2 bl e414
e5e6 cmp r0, #0; e5e8 beq e5d2; e5ea ldr r3, [r4, #24]
e5ec cmp r3, r6; e5ee bne e5d2; e5f0 ldr r2, [r4, #276]
e5f4 cmp r2, r3; e5f6 bne e5d2; e5f8 ldr r1, [r4, #20]
e5fa ldr r3, [r4, #164]; e5fe cmp r3, r1; e600 bne e5d2
e602 ldr r3, [r4, #484]; e606 cmp r3, r2; e608 bne e5d2
e60a ldr r2, [r4, #480]; e60e cmp r2, r5; e610 bne e5d2
e612 bic r3, r3, #1; e616 cmp r1, r3; e618 bhi e5d2
e61a ldr r2, [r4, #168]; e61e subs r3, r3, r1; e620 cmp r3, r2
e622 bcs e5d2; e624 cmp r1, r5; e626 bhi e5d2
e628 subs r5, r5, r1; e62a cmp r2, r5; e62c bls e5d2
e62e ldr r3, [r4, #476]; e632 cmp r1, r3; e634 bne e5d2
e636 ldr r2, [r4, #448]; e63a ldr r3, [pc, #92]
e63c cmp r2, r3; e63e bne e5d2
e640 ldr r3, [r4, #452]; e644 cmp r3, #0; e646 bne e5d2
e648 ldr r3, [r4, #456]; e64c cmp r3, #0; e64e bne e5d2
e650 ldr r3, [r4, #460]; e654 cmp r3, #0; e656 bne e5d2
e658 ldr r3, [r4, #464]; e65c cmp r3, #0; e65e bne e5d2
e660 ldr r3, [r4, #468]; e664 cmp r3, #0; e666 bne e5d2
e668 mov r1, sp; e66a mov r0, r4; e66c bl e51c
e670 cmp r0, #0; e672 beq e5d2; e674 ldrd r2, r3, [sp]
e678 ldrd r0, r1, [r4, #40]; e67c cmp r2, r0
e67e sbcs r1, r3, r1; e682 bcc e5d2; e684 strd r2, r3, [r4, #48]
e688 movs r1, #2; e68a movs r2, #3; e68c mov r0, r4
e68e add sp, #8; e690 ldmia sp!, {r4, r5, r6, lr}; e694 b e466
e698 .word 0x20010000
'''

# Native claim/complete are necessary dependencies: caller constants 2 and 3
# do not alone prove DISPATCHING -> ENTERED, or the busy/barrier protocol.
FTPA_CLAIM='''
e488 push {r4, lr}; e48a mov r4, r0; e48c bl e3e4
e490 cbz r0, e49c; e492 ldr r3, [r4, #56]; e494 cbnz r3, e49c
e496 ldr r3, [r4, #16]; e498 cmp r3, r1; e49a beq e4a6
e49c mov r0, r4; e49e bl e456; e4a2 movs r0, #0
e4a4 pop {r4, pc}; e4a6 movs r3, #1; e4a8 str r3, [r4, #56]
e4aa b e4a4
'''
FTPA_COMPLETE='''
e466 push {r3, lr}; e468 ldr r3, [r0, #56]; e46a cbz r3, e472
e46c ldr r3, [r0, #16]; e46e cmp r3, r1; e470 beq e47a
e472 bl e456; e476 movs r0, #0; e478 pop {r3, pc}
e47a movs r3, #0; e47c str r3, [r0, #56]; e47e dmb sy
e482 str r2, [r0, #16]; e484 movs r0, #1; e486 b e478
'''
FTPA_EXCLUDED='''
e440 mrs r3, primask; e444 mrs r0, ipsr; e448 cmp r0, #0
e44a it eq; e44c cmpeq r3, #1; e44e ite eq
e450 moveq r0, #1; e452 movne r0, #0; e454 bx lr
'''

# Captured-format positive control: literal expected addresses/encodings,
# independent of contract_rows, the CFG evaluator and any inspected ELF.
# It is parser/oracle support only, never executable-closure evidence.
STATIC_TERMINAL_CAPTURE='''
00006c60 <boot_startup_terminal>:
 6c60: f3bf 8f4f dsb sy
 6c64: 4b0e ldr r3, [pc, #56]
 6c66: 6018 str r0, [r3, #0]
 6c68: f3bf 8f4f dsb sy
 6c6c: f3bf 8f6f isb sy
 6c70: f20f 0327 addw r3, pc, #39
 6c74: f043 0301 orr.w r3, r3, #1
 6c78: 469e mov lr, r3
 6c7a: 2300 movs r3, #0
 6c7c: f381 8808 msr MSP, r1
 6c80: f3bf 8f6f isb sy
 6c84: f383 8814 msr CONTROL, r3
 6c88: f3bf 8f6f isb sy
 6c8c: f383 8811 msr BASEPRI, r3
 6c90: f383 8813 msr FAULTMASK, r3
 6c94: f383 8810 msr PRIMASK, r3
 6c98: 4710 bx r2
00006c9a <boot_startup_return_trap>:
 6c9a: f7ff bffe b.w 6c9a <boot_startup_return_trap>
 6c9e: 0000 .short 0x0000
 6ca0: e000ed08 .word 0xe000ed08
0000482c <boot_handoff_target_observe>:
 482c: e92d 43f0 stmdb sp!, {r4, r5, r6, r7, r8, r9, lr}
 4830: 4604 mov r4, r0
 4832: b087 sub sp, #28
 4834: b920 cbnz r0, 4840 <boot_handoff_target_observe+0x14>
 4836: f04f 30ff mov.w r0, #4294967295
 483a: b007 add sp, #28
 483c: e8bd 83f0 ldmia.w sp!, {r4, r5, r6, r7, r8, r9, pc}
 4840: f3ef 8905 mrs r9, IPSR
 4844: f3ef 8814 mrs r8, CONTROL
 4848: f3ef 8710 mrs r7, PRIMASK
 484c: f3ef 8611 mrs r6, BASEPRI
 4850: f3ef 8513 mrs r5, FAULTMASK
 4854: a801 add r0, sp, #4
 4856: e9cd 9801 strd r9, r8, [sp, #4]
 485a: e9cd 7603 strd r7, r6, [sp, #12]
 485e: 9505 str r5, [sp, #20]
 4860: f7fb fc8d bl 17e <core_valid>
 4864: 2800 cmp r0, #0
 4866: d0e6 beq.n 4836 <boot_handoff_target_observe+0xa>
 4868: 2000 movs r0, #0
 486a: e9c4 9800 strd r9, r8, [r4]
 486e: e9c4 7602 strd r7, r6, [r4, #8]
 4872: 6125 str r5, [r4, #16]
 4874: e7e1 b.n 483a <boot_handoff_target_observe+0xe>
000001bc <direct_barrier>:
 1bc: b931 cbnz r1, 1cc <direct_barrier+0x10>
 1be: b672 cpsid i
 1c0: f3bf 8f4f dsb sy
 1c4: f3bf 8f6f isb sy
 1c8: 2000 movs r0, #0
 1ca: 4770 bx lr
 1cc: 2901 cmp r1, #1
 1ce: d0f7 beq.n 1c0 <direct_barrier+0x4>
 1d0: f04f 30ff mov.w r0, #4294967295
 1d4: 4770 bx lr
'''


def static_witness_selfchecks():
    """Authored controls only; invocation belongs to test-merit-gate."""
    def reject(label,reason,callback):
        try: callback()
        except (Invalid,Failure) as exc:
            oracle(reason in str(exc),label+': rejected for wrong obligation: '+str(exc))
        else: raise Invalid('oracle selfcheck admitted '+label)
    def no_words(address): raise Invalid('unexpected literal dependency '+hex(address))
    read=[(0x100,'ldmia','r0, {r0, r3}'),(0x104,'blx','r3')]
    oracle(native_provenance(read,no_words,'indirect')==[
        {'pc':0x104,'target_field':4,'context_field':0}],
        'native LDM uses old base and exact independent target/context tuple')
    # Two real syntactic paths must both reach distinct seams. Ignoring taken
    # branches in the interpreter produces one record, not this handwritten two.
    branch=[(0x100,'cmp','r1, #0'),(0x102,'beq','10c'),
        (0x104,'ldmia','r0, {r0, r3}'),(0x106,'blx','r3'),
        (0x10c,'ldmia','r0, {r0, r3}'),(0x10e,'blx','r3')]
    oracle(native_provenance(branch,no_words,'indirect')==[
        {'pc':0x106,'target_field':4,'context_field':0},
        {'pc':0x10e,'target_field':4,'context_field':0}],
        'both CFG successors independently checked, not straight-line tags')
    reject('alternate-context path','target/context',lambda:native_provenance([
        *branch[:4],(0x10c,'ldr','r3, [r0, #4]'),
        (0x10e,'ldr','r0, [r0, #8]'),(0x110,'blx','r3')],no_words,'indirect'))
    reject('target from wrong descriptor','target/context',lambda:native_provenance([
        (0x100,'ldmia','r3, {r0, r3}'),(0x104,'blx','r3')],no_words,'indirect'))
    reject('unresolved memory borrowing','unknown native load base',lambda:native_provenance([
        (0x100,'ldr','r3, [r2, #0]'),(0x104,'blx','r3')],no_words,'indirect'))
    reject('unsupported conditional effect','unsupported native provenance',lambda:native_provenance([
        (0xfe,'it','eq'),*read],no_words,'indirect'))
    copy=[(0x4c30,'mov','r4, r0'),(0x4c36,'ldmia','r3, {r0, r1, r2}'),
          (0x4c42,'stmia','r4, {r0, r1, r2}')]
    oracle(native_provenance(copy,no_words,'copy')==[
        {'pc':0x4c42,'copy':{0:('input',0),4:('input',4),8:('input',8)}}],
        'all three native descriptor fields have independent copy provenance')
    reject('descriptor field substitution','copied field provenance',lambda:native_provenance([
        *copy[:2],(0x4c40,'mov','r2, r1'),copy[2]],no_words,'copy'))
    reject('descriptor store offset','unsupported native multi writeback',lambda:native_provenance([
        *copy[:2],(0x4c42,'stmia','r4!, {r0, r1, r2}')],no_words,'copy'))
    oracle(retained_literal_value(0x400fe104,0x1342)==0x400fe104,
           'retained MMIO dependency is permitted')
    reject('ordinary BSS pointer','ordinary-RAM borrowing',lambda:retained_literal_value(0x2000020c,0x1342))
    # Changed pointer construction has no source-spelling exemption.
    reject('constructed ordinary pointer','unsupported decoded machine contract',
        lambda:machine_contract([(0x164,'movw','r0, #524'),(0x168,'movt','r0, #8192')],
                                '164 nop; 166 nop; 168 nop; 16a bx lr','retained delay'))
    divide=[(0x6f80,'stmdb','sp!, {r4, r5, r6, r7, r8, r9, sl, lr}'),
        (0x6f8e,'ldr','r2, [sp, #32]'),(0x7024,'cbz','r2, 7030'),
        (0x702c,'strd','r3, r4, [r2]'),
        (0x7030,'ldmia','sp!, {r4, r5, r6, r7, r8, r9, sl, pc}')]
    oracle(division_dependency(divide)=={
        'memory':[(0x6f80,'stmdb','sp!, {r4, r5, r6, r7, r8, r9, sl, lr}'),
                  (0x6f8e,'ldr','r2, [sp, #32]'),(0x702c,'strd','r3, r4, [r2]'),
                  (0x7030,'ldmia','sp!, {r4, r5, r6, r7, r8, r9, sl, pc}')],
        'branch_edges':[(0x7024,0x7030)],
        'scope':'memory independence, not quotient correctness'},
        'division independent stack/result boundary and CFG records')
    reject('division ordinary-pointer substitution','result pointer remains invariant',
        lambda:division_dependency([*divide[:2],(0x7000,'mov','r2, r0'),*divide[2:]]))
    for operands in ('r2, r3, r0, r1','r3, r2, r0, r1'):
        reject('division UMULL result-pointer clobber','result pointer remains invariant',
            lambda:division_dependency([*divide[:2],(0x7000,'umull',operands),*divide[2:]]))
    oracle(division_dependency([*divide[:2],(0x7000,'umull','r3, r4, r2, r1'),*divide[2:]])==
           division_dependency(divide),'division UMULL may read invariant pointer without writing it')
    reject('division unknown load','unsupported division dependency',
        lambda:division_dependency([*divide[:2],(0x7000,'ldr','r0, [r3]'),*divide[2:]]))
    reject('division stack loop','never repeats frame allocation',
        lambda:division_dependency([*divide[:2],(0x7024,'cbz','r2, 6f80'),*divide[3:]]))
    terminal=terminal_architecture(STATIC_TERMINAL_CAPTURE)
    oracle(terminal['terminal'][9]==(0x6c7c,'msr','msp, r1') and
           terminal['trap'][0]==(0x6c9a,'b','6c9a <boot_startup_return_trap>') and
           terminal['observation_mrs']==['ipsr','control','primask','basepri','faultmask'] and
           terminal['exclusion_barriers']==[('cpsid','i'),('dsb','sy'),('isb','sy')],
           'captured terminal addresses/system registers/barrier event expectations')
    mutations=(
        ('terminal VTOR','e000ed08 .word 0xe000ed08','e000ed0c .word 0xe000ed0c','literal is VTOR'),
        ('terminal old stack','6c80: f3bf 8f6f isb sy','6c80: 9800 ldr r0, [sp, #0]','no-old-frame'),
        ('terminal barrier 6c60','6c60: f3bf 8f4f dsb sy','6c60: bf00 nop','terminal barrier 6c60'),
        ('terminal barrier 6c68','6c68: f3bf 8f4f dsb sy','6c68: bf00 nop','terminal barrier 6c68'),
        ('terminal barrier 6c6c','6c6c: f3bf 8f6f isb sy','6c6c: bf00 nop','terminal barrier 6c6c'),
        ('terminal barrier 6c80','6c80: f3bf 8f6f isb sy','6c80: bf00 nop','terminal barrier 6c80'),
        ('terminal barrier 6c88','6c88: f3bf 8f6f isb sy','6c88: bf00 nop','terminal barrier 6c88'),
        ('terminal mask','msr PRIMASK, r3','msr PRIMASK, r0','no-old-frame'),
        ('terminal trap LR','addw r3, pc, #39','addw r3, pc, #35','exact Thumb trap'),
        ('terminal reset target','6c98: 4710 bx r2','6c98: 4700 bx r0','no-old-frame'),
        ('actual MRS','mrs r7, PRIMASK','mrs r7, BASEPRI','core MRS'),
        ('exclusion barrier','1c4: f3bf 8f6f isb sy','1c4: bf00 nop','exclusion/barrier'))
    for label,old,new,reason in mutations:
        oracle(STATIC_TERMINAL_CAPTURE.count(old)==1,'unique handwritten terminal mutation '+label)
        reject(label,reason,lambda old=old,new=new:terminal_architecture(STATIC_TERMINAL_CAPTURE.replace(old,new)))
    wrapper=[(0x4d70,'mov','r4, r0'),(0x4d76,'cbnz','r0, 4d7c'),
             (0x4d7c,'ldr','r2, [r4, #24]'),(0x4d7e,'ldr','r1, [r4, #292]'),
             (0x4d82,'ldr','r0, [r4, #20]')]
    wrapper_spec='''4d70 mov r4, r0; 4d76 cbnz r0, 4d7c
4d7c ldr r2, [r4, #24]; 4d7e ldr r1, [r4, #292]; 4d82 ldr r0, [r4, #20]'''
    oracle(machine_contract(wrapper,wrapper_spec,'wrapper')==wrapper,'independent wrapper positive tuples')
    for index,operand in ((0,'r4, r1'),(1,'r0, 4d78'),(2,'r2, [r4, #20]'),
                           (3,'r1, [r4, #296]'),(4,'r0, [r4, #24]')):
        mutated=list(wrapper); pc,op,_=mutated[index]; mutated[index]=(pc,op,operand)
        reject('wrapper guard/pointer/offset','wrapper:',
               lambda mutated=mutated:machine_contract(mutated,wrapper_spec,'wrapper'))
    oracle(initializer_interval(0x20000000,0x20000008)==(0x20000000,0x20000008) and
           initializer_interval(0x20000008,0x20009c48)==(0x20000008,0x20009c48),
           'independent historical FTPA byte intervals, not symbol-only evidence')
    reject('initializer overlap','excludes retained',lambda:initializer_interval(0x20000008,0x2000dbc1))
    # A non-word aligned bound rounds UP in the actual compiled argument code.
    reject('initializer rounded overlap','excludes retained',lambda:initializer_interval(0x2000dbbf,0x2000dbc0))
    abi=[(0xe4be,'ldr','r3, [r0, #320]'),(0xe4c2,'ands','r3, r2'),
         (0xe4c4,'cmp','r3, #1'),(0xe4c6,'bne','e50e')]
    abi_spec='''e4be ldr r3, [r0, #320]; e4c2 ands r3, r2
 e4c4 cmp r3, #1; e4c6 bne e50e'''
    oracle(machine_contract(abi,abi_spec,'native ABI')==abi,'independent native callback ABI field/ownership guard')
    reject('native callback offset','native ABI:',lambda:machine_contract([
        (0xe4be,'ldr','r3, [r0, #324]'),*abi[1:]],abi_spec,'native ABI'))
    reject('native ownership bypass','native ABI:',lambda:machine_contract([
        *abi[:3],(0xe4c6,'nop','')],abi_spec,'native ABI'))
    # Check actual bytes for literals independently of disassembly .word text.
    fixture=object.__new__(Elf)
    fixture.sections={'.text':(0,1,6,0x94b8,0,4)}
    fixture.data=struct.pack('<I',0x20000008)
    load=[(0x9472,'ldr','r2, [pc, #68]')]
    oracle(literal_contract(fixture,load,{0x9472:(0x94b8,0x20000008)},'FTPA bounds')==
           {0x9472:(0x94b8,0x20000008)},'actual ELF initializer bound tuple')
    fixture.data=struct.pack('<I',0x2000dc00)
    reject('initializer ELF literal overlap','actual ELF PC-relative literal values',
        lambda:literal_contract(fixture,load,{0x9472:(0x94b8,0x20000008)},'FTPA bounds'))
    # Inject deliberate evaluator-output corruptions into the SAME handwritten
    # expectation checks, rather than generating expected data from the SUT.
    want_indirect=[{'pc':0x104,'target_field':4,'context_field':0}]
    wrong_indirect=[{'pc':0x104,'target_field':8,'context_field':0}]
    reject('field-offset evaluator corruption','oracle selfcheck:',
           lambda:oracle(wrong_indirect==want_indirect,'native field expectation'))
    want_copy=[{'pc':0x4c42,'copy':{0:('input',0),4:('input',4),8:('input',8)}}]
    wrong_copy=[{'pc':0x4c42,'copy':{0:('input',0),4:('input',4),8:('input',4)}}]
    reject('copy evaluator corruption','oracle selfcheck:',
           lambda:oracle(wrong_copy==want_copy,'native copy expectation'))
    reject('branch evaluator corruption','oracle selfcheck:',lambda:oracle(
        native_provenance(branch,no_words,'indirect')[:1]==[
            {'pc':0x106,'target_field':4,'context_field':0},
            {'pc':0x10e,'target_field':4,'context_field':0}],'both successor expectation'))
    reject('barrier recorder corruption','oracle selfcheck:',lambda:oracle(
        terminal['exclusion_barriers'][:-1]==[('cpsid','i'),('dsb','sy'),('isb','sy')],
        'barrier event expectation'))


def initializer_interval(start,end):
    require(0x20000000<=start<=end<=0x20010000,'FTPA initializer SRAM bounds')
    size=0 if start>=end else ((end-1-start)&0xfffffffc)+4
    require(start+size<=0x2000dbc0 or start>=0x2000e000,
            'FTPA compiled rounded byte interval excludes retained RAM')
    return start,start+size


def ftpa_machine_witness(elf,dis):
    contracts={'Reset_Handler':FTPA_INITIALIZERS,'memcpy':FTPA_MEMCPY,
        'memset':FTPA_MEMSET,'Application_Reset_Handler':FTPA_CAPTURE,
        'Application_Entry_Witness':FTPA_WITNESS,'abi':FTPA_ABI,
        'ownership':FTPA_OWNERSHIP,'boot_startup_early':FTPA_EARLY,
        'claim':FTPA_CLAIM,'complete':FTPA_COMPLETE,'excluded':FTPA_EXCLUDED,
        'fail.isra.0':'''e456 cbz r0, e464; e458 movs r3, #0
            e45a str r3, [r0, #56]; e45c dmb sy; e460 movs r3, #5
            e462 str r3, [r0, #16]; e464 bx lr''',
        'boot_startup_stop':'''eaa8 cpsid i; eaaa nop; eaac b eaaa'''}
    evidence={}
    for name,spec in contracts.items():
        rows=instruction_rows(dis,name)
        symbol=elf.symbol(name); section=elf.sections.get(symbol[2])
        if name=='Application_Entry_Witness':
            # This actual assembly witness is a zero-sized NOTYPE interior
            # label, not a counterfeit standalone function. Its instructions
            # are owned by the typed Application_Reset_Handler extent.
            owner=elf.symbol('Application_Reset_Handler')
            require(symbol[1]==0 and symbol[4]==0 and owner[1]==2 and
                    owner[2]==symbol[2] and (owner[3]&~1)<=symbol[3] and
                    0x9522==(owner[3]&~1)+owner[4],
                    'FTPA witness label belongs to typed pre-C reset body')
        else:
            require(symbol[1]==2,'FTPA typed executable function '+name)
        require(section is not None and section[1]==1 and section[2]&4 and
                symbol[3]&~1==rows[0][0],
                'FTPA actual executable instruction anchor '+name)
        evidence[name]=machine_contract(rows,spec,'FTPA '+name)
    literals={
        'Reset_Handler':{0x9472:(0x94b8,0x20000008),0x9474:(0x94bc,0x20000000),
            0x9484:(0x94c0,0x10af4),0x948c:(0x94c4,0x20009c48),
            0x948e:(0x94c8,0x20000008),0x94b0:(0x94cc,0x2000dbc0)},
        'Application_Reset_Handler':{0x94d0:(0x9524,0x2000dd80),0x94fa:(0x9528,0xe000ed08)},
        'Application_Entry_Witness':{0x9506:(0x952c,0x94d1),0x950c:(0x9530,0x2000dbc0),
            0x950e:(0x952c,0x94d1),0x9510:(0x9534,0x9500)},
        'abi':{0xe3e8:(0xe40c,0x31534342),0xe3fe:(0xe410,0x4d330001)},
        'ownership':{0xe4ae:(0xe514,0xffff8001),0xe4f8:(0xe518,0x2000dbc0)},
        'boot_startup_early':{0xe63a:(0xe698,0x20010000)}}
    for name,want in literals.items():
        literal_contract(elf,instruction_rows(dis,name),want,'FTPA '+name)
    # Given these exact compiled argument calculations, byte loops write N
    # bytes starting at D, not N words. Explicit arithmetic catches rounded-up
    # final-word writes even if symbol bounds themselves still look harmless.
    intervals=[]
    for start_at,end_at in ((0x94bc,0x94b8),(0x94c8,0x94c4)):
        intervals.append(initializer_interval(elf_word(elf,start_at),elf_word(elf,end_at)))
    # Ownership's mask means Thumb AND address <0x8000, not merely bit0.
    require((0xffff8001 & 0x7fff)==1,'native boot-flash Thumb ownership mask')
    # The controller and clock_read dependencies must be checked as machine
    # contracts too; an unexamined collaborator can invalidate all native ABI
    # results, even though the guards above match perfectly.
    ftpa_native_dependencies(elf,dis,evidence)
    return {'machine_contracts':evidence,'literal_bindings':literals,
            'initializer_write_intervals':intervals,'capture_base':0x2000dd80,
            'native_transition':[2,3], 'native_time_offsets':[40,48],
            'scope':'initializer exclusion and early native ABI; not later application writes'}

