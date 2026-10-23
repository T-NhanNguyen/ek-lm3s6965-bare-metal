"""Conservative linked Thumb dataflow witness for the actual boot entry.

This is NOT target execution. A bounded abstract interpreter follows compiled
entry instructions with typed ELF-resolved literals, AAPCS call arguments and
modeled *return categories* only. Unknown instructions, indirect calls, memory,
flags or provenance are INVALID, never inferred from symbol spellings. The
model of a successful aggregate return is a static path stimulus, not fabricated
HOST runtime evidence. HOST case1 independently provides the real return.
"""
import json
import re
import struct
from checked_support import ROOT, Invalid, Failure, require, instruction_rows

UNKNOWN=None
SP=0x30010000  # symbolic stack space, deliberately not a physical SRAM claim

ALIASES={'fp':'r11','ip':'r12','sl':'r10','lr':'r14','pc':'r15','sp':'r13'}
RESULT_NEGATIVES=(('reject-sequence-status',0,4),('reject-sequence-stage',4,0),
    ('reject-watchdog-wrapper-status',8,5),('reject-missing-runtime-valid',12,0x08),
    ('reject-missing-handoff-valid',12,0x02),('reject-nested-runtime-status',16,1),
    ('reject-handoff-status',220,1))

# Independently specified collaborator INPUTS, not snapshots of SUT memory.
# The exact whole returned input is the publication expectation. These byte
# patterns do not claim verifier/runtime success or native launch validity.
RESULT_POISON=bytes(range(252))
LAUNCH_POISON=bytes(range(256))+bytes(range(28))


def result_stimulus(category,dispatch):
    result=bytearray(RESULT_POISON)
    for offset,value in ((0,0),(4,3),(8,0),(12,10 if dispatch else 7),(16,0),(220,0)):
        struct.pack_into('<I',result,offset,value)
    if category in ('boot_sequence_launch','boot_sequence_run'): struct.pack_into('<I',result,0,4)
    for name,offset,value in RESULT_NEGATIVES:
        if category==name: struct.pack_into('<I',result,offset,value)
    return bytes(result)


def architectural_registers(text,form):
    """Thumb register masks are sets encoded by number, not source-list order.

    This deliberately does not implement arbitrary STMDB addressing modes.
    PUSH/POP use the wide architectural constraints; encoding availability is
    still the responsibility of the actual decoded instruction source.
    """
    if not re.fullmatch(r'\{[^{}]+\}',text.strip()): raise Invalid('register-list syntax')
    result=[]
    def reg(token):
        token=ALIASES.get(token,token)
        if not re.fullmatch(r'r(?:[0-9]|1[0-5])',token): raise Invalid('register-list register')
        return int(token[1:])
    for item in text.strip()[1:-1].split(','):
        fields=item.strip().split('-')
        if len(fields)==1: values=[reg(fields[0])]
        elif len(fields)==2:
            lo,hi=(reg(f.strip()) for f in fields)
            if lo>=hi: raise Invalid('register-list range')
            values=list(range(lo,hi+1))
        else: raise Invalid('register-list range')
        if any(v in result for v in values): raise Invalid('register-list duplicate')
        result.extend(values)
    if not result: raise Invalid('register-list empty')
    if 13 in result: raise Invalid('register-list prohibited SP')
    if form in ('stmdb','push','stm','stmia') and 15 in result:
        raise Invalid('register-list prohibited PC')
    if form=='stmdb' and len(result)<2: raise Invalid('STMDB encoding requires two registers')
    if form in ('ldm','ldmia','pop') and 14 in result and 15 in result:
        raise Invalid('register-list simultaneous LR/PC')
    names={13:'sp',14:'lr',15:'pc'}
    return [names.get(n,'r'+str(n)) for n in sorted(result)]


def stack_save(registers,text,store,form='stmdb',base='sp!'):
    if base!='sp!': raise Invalid('unsupported STMDB base/form')
    listed=architectural_registers(text,form)
    address=registers['sp']-4*len(listed)
    for j,r in enumerate(listed): store(address+4*j,registers[r])
    registers['sp']=address


# Literal source-contract MMIO paths. These are static inputs, not device facts.
RCC, RCC2, RIS, MISC, USECRL = 0x400fe060, 0x400fe070, 0x400fe050, 0x400fe058, 0x400fe140
CLOCK_EFFECTS=(
    ('read',RCC2,0x80000000), ('write',RCC2,0x80000800), ('read',RCC2,0x80000800),
    ('read',RCC,0), ('write',RCC,0x800), ('read',RCC,0x800),
    ('delay',0,524288), ('write',RCC,0xb80), ('read',RCC,0xb80),
    ('write',RCC2,0x800), ('read',RCC2,0x800), ('write',MISC,64),
    ('write',RCC,0xb80), ('read',RCC,0xb80), ('read',RIS,64),
    ('write',RCC,0x1c00b80), ('read',RCC,0x1c00b80),
    ('write',RCC,0x1c00380), ('delay',0,1), ('read',RCC,0x1c00380),
    ('read',RCC2,0x800), ('read',RIS,64), ('write',USECRL,49), ('read',USECRL,49))
# Independently transcribed callsites, relative to each typed function start.
# They bind the inputs to the observed CFG, not to an image hash or load address.
CLOCK_SITES_DISPATCH=(0xe,0x1c,0x20,0x32,0x46,0x4a,0x5a,0x64,0x68,
    0x78,0x7c,0x88,0x96,0x9a,0xa8,0xbe,0xc2,0xd4,0xda,0xe0,
    0xee,0xf8,0x104,0x108)
CLOCK_SITES_DECISION=(0x1e,0x2e,0x34,0x3e,0x54,0x5a,0x6e,0x76,0x7c,
    0x8c,0x92,0xa6,0xae,0xb4,0xc2,0xda,0xe0,0xf2,0xf8,0xfe,
    0x10e,0x11a,0x128,0x12e)


class MMIOTranscript:
    def __init__(self, clock_base, sites, flash_pc, category):
        self.expected=[(clock_base+off,kind,address,4,value)
                       for off,(kind,address,value) in zip(sites,CLOCK_EFFECTS)]
        if len(sites)!=len(CLOCK_EFFECTS): raise Invalid('MMIO callsite roster extent')
        self.expected.append((flash_pc,'read',USECRL,4,49))
        if category=='clock-readback-fail':
            pc,kind,address,width,_=self.expected[2]
            self.expected=self.expected[:3]
            self.expected[2]=(pc,kind,address,width,0x80000000)
        elif category=='clock-authority-rcc':
            self.expected=[(p,k,a,w,0 if a==RCC2 else v) for j,(p,k,a,w,v) in enumerate(self.expected) if j not in (1,2)]
        elif category=='flash-timing-fail':
            pc,kind,address,width,_=self.expected[-1]
            self.expected[-1]=(pc,kind,address,width,48)
        self.position=0; self.events=[]

    def effect(self,pc,kind,address,width,value=None):
        if self.position>=len(self.expected): raise Invalid('MMIO transcript exhausted')
        expected=self.expected[self.position]
        if (pc,kind,address,width)!=expected[:4]:
            raise Invalid('unmatched MMIO effect '+repr((pc,kind,address,width)))
        if kind!='read' and value!=expected[4]: raise Invalid('MMIO write/delay value')
        self.position+=1; self.events.append(expected)
        return expected[4]

    def finish(self):
        if self.position!=len(self.expected): raise Invalid('leftover MMIO transcript')


def verified_leaf(rows):
    """No symbol-name authority: exact narrow instruction semantics/CFG only."""
    body=[(op,args) for _,op,args in rows]
    contiguous=len(rows)==2 and rows[1][0]==rows[0][0]+2
    if contiguous and body==[('ldr','r0, [r0, #0]'),('bx','lr')]: return 'read'
    if contiguous and body==[('str','r1, [r0, #0]'),('bx','lr')]: return 'write'
    if len(rows)==11:
        p=rows[0][0]
        expected=[(0,'sub','sp, #8'),(2,'str','r0, [sp, #4]'),
            (4,'ldr','r3, [sp, #4]'),(6,'cbnz',f'r3, {p+12:x}'),
            (8,'add','sp, #8'),(10,'bx','lr'),(12,'nop',''),
            (14,'ldr','r3, [sp, #4]'),(16,'subs','r3, #1'),
            (18,'str','r3, [sp, #4]'),(20,'b',f'{p+4:x}')]
        actual=[(pc-p,op,re.sub(r' <[^>]+>','',args)) for pc,op,args in rows]
        if actual==expected: return 'delay'
    raise Invalid('unverified MMIO leaf instruction semantics')


def publication_barriers(events,state_address):
    """A ready store must follow a *fresh* full-system DMB after data/call."""
    last_boundary=-1; last_dmb=-1
    for index,e in enumerate(events):
        if e[0]=='barrier' and e[2:] == ('dmb','sy'): last_dmb=index
        elif e[0]=='call' or (e[0]=='store' and e[2]!=state_address): last_boundary=index
        elif e[0]=='store' and e[2]==state_address and e[4]!=1:
            require(e[3]==4 and last_dmb>last_boundary,'fresh DMB before checkpoint publication')
            last_boundary=index


def nzcv(value,carry=None,overflow=None):
    if value is None: return (None,None,carry,overflow)
    value &= 0xffffffff
    return (bool(value&0x80000000),value==0,carry,overflow)


def add_sub_flags(a,b,subtract=False):
    if a is None or b is None: return nzcv(None)
    a &= 0xffffffff; b &= 0xffffffff
    result=(a-b if subtract else a+b)&0xffffffff
    carry=a>=b if subtract else a+b>0xffffffff
    overflow=bool(((a^b) if subtract else ~(a^b))&(a^result)&0x80000000)
    return nzcv(result,carry,overflow)


def arm_condition(flags,code):
    if flags is None: raise Invalid('unresolved entry condition '+code)
    n,z,c,v=flags
    needed={'eq':(z,),'ne':(z,),'cs':(c,),'hs':(c,),'cc':(c,),'lo':(c,),
        'hi':(c,z),'ls':(c,z),'ge':(n,v),'gt':(n,v,z),'le':(n,v,z),
        'lt':(n,v),'pl':(n,),'mi':(n,)}
    if code not in needed or any(f is None for f in needed[code]):
        raise Invalid('unresolved architectural flags for '+code)
    return {'eq':z,'ne':not z,'cs':c,'hs':c,'cc':not c,'lo':not c,
        'hi':c and not z,'ls':not c or z,'ge':n==v,'gt':not z and n==v,
        'le':z or n!=v,'lt':n!=v,'pl':not n,'mi':n}[code]


def it_slots(op,condition):
    inverse={'eq':'ne','ne':'eq','cs':'cc','cc':'cs','hi':'ls','ls':'hi',
             'ge':'lt','lt':'ge','gt':'le','le':'gt','pl':'mi','mi':'pl'}
    if not re.fullmatch(r'it[te]{0,3}',op) or condition not in inverse:
        raise Invalid('unsupported IT condition/form')
    return [condition]+[condition if c=='t' else inverse[condition] for c in op[2:]]


def shift32(value,amount,left,carry):
    """ARM register shifts use low eight amount bits; immediate amounts supplied as decoded."""
    amount &= 255; value &= 0xffffffff
    if amount==0: return value,carry
    if amount>32: return 0,False
    if left: return (value<<amount)&0xffffffff,bool((value>>(32-amount))&1)
    return value>>amount,bool((value>>(amount-1))&1)


def entry_cfg(rows,dispatch):
    """Whole scoped entry graph: no data-pool fallthrough, unknown/tail bypass or retry.

    Calls are opaque only at the explicit collaborator boundary. Helper CFGs
    are separately interpreted; this graph covers the compiled entry itself.
    """
    code={p:(o,a) for p,o,a in rows if not o.startswith('.')}; graph={}; stops=set(); calls={}
    for p,(op,args) in code.items():
        target=re.search(r'([0-9a-f]+) <([^>]+)>$',args)
        nexts=sorted(q for q in code if q>p)
        # Instruction size is determined by the next decoded row, but may not
        # cross an embedded literal pool. Branches over pools have no fallthrough.
        nxt=nexts[0] if nexts else None
        if op=='bl':
            if not target: raise Invalid('unresolved entry CFG call')
            name=target[2]; calls.setdefault(name,[]).append(p)
            if name in ('stop','boot_startup_dispatch'): graph[p]=[]; stops.add(p)
            else: graph[p]=[nxt]
        elif op=='b':
            if not target: raise Invalid('unresolved entry CFG branch')
            graph[p]=[int(target[1],16)]
        elif op in ('cbz','cbnz') or re.fullmatch(r'b(eq|ne|cs|hs|cc|lo|hi|ls|ge|gt|le|lt|pl|mi)',op):
            if not target: raise Invalid('unresolved entry CFG conditional')
            graph[p]=[int(target[1],16),nxt]
        elif op in ('bx','pop','blx') or op.startswith('it'):
            raise Invalid('unsupported scoped entry CFG control '+op)
        else: graph[p]=[nxt]
    first=rows[0][0]
    def reachable(start,removed=None):
        seen=set(); todo=[start]
        while todo:
            p=todo.pop()
            if p in seen: continue
            if removed is not None and (p==removed or (isinstance(removed,(set,frozenset)) and p in removed)): continue
            if p not in graph: raise Invalid('entry CFG unresolved edge/literal fallthrough '+str(p))
            seen.add(p); todo.extend(graph[p])
        return seen
    reached=reachable(first)
    transaction='boot_sequence_launch' if dispatch else 'boot_sequence_run'
    require(len(calls.get(transaction,[]))==1,'one static entry transaction callsite')
    launch=calls[transaction][0]
    require(launch not in reachable(graph[launch][0]),'no CFG transaction retry/back-edge')
    forbidden='boot_sequence_run' if dispatch else 'boot_sequence_launch'
    require(forbidden not in calls,'mode-specific entry transaction exclusion')
    prerequisites=['boot_flash_read','register_read'] if dispatch else ['boot_clock_setup','boot_target_flash_init','boot_target_images_read','boot_target_sequence_bind']
    if dispatch: prerequisites+=['boot_handoff_target_init','boot_handoff_target_observe']
    for name in prerequisites:
        require(name in calls,'entry CFG prerequisite '+name)
        if name=='boot_flash_read':
            require(launch not in reachable(first,set(calls[name])),'entry prerequisite dominates transaction '+name)
        else:
            for p in calls[name]:
                require(launch not in reachable(first,p),'entry prerequisite dominates transaction '+name)
    if dispatch:
        for j,offset in enumerate(CLOCK_SITES_DISPATCH):
            if j in (1,2): continue  # guarded USERCC2 authority-only pair
            p=first+offset
            require(p in graph and code[p][0]=='bl' and launch not in reachable(first,p),
                    'required clock read/write/delay dominates transaction '+str(j))
        for name in ('boot_startup_prepare','boot_startup_dispatch'):
            require(len(calls.get(name,[]))==1,'one protected entry call '+name)
            require(calls[name][0] not in reachable(first,launch),'transaction dominates '+name)
        require(calls['boot_startup_dispatch'][0] not in reachable(first,calls['boot_startup_prepare'][0]),
                'prepare dominates dispatch')
    # All finite failure arms must remain terminal, with no joins back into a
    # protected path. Polling loops are retained as real cyclic edges, not
    # discarded after the concrete success sample.
    nonzero='nonzero'
    def stop_only(p,active,values=None,stored=None):
        if p in active: return False
        if p in stops: return code[p][1].endswith('<stop>')
        op,args=code[p]; values=dict(values or {}); stored=dict(stored or {})
        # Tiny failure-arm constant domain. It handles the *actual* inlined
        # flash category store/reload before its zero guard, without assuming
        # a structural path through that guard can rejoin success. No MMIO,
        # calls, unknown arithmetic or aliases are assigned successful values.
        if op=='bl': return False
        if op in ('mov','movs'):
            dst,src=args.split(', ',1)
            values[dst]=int(src[1:],0) if src.startswith('#') else values.get(src)
            stored={k:v for k,v in stored.items() if not k.startswith('['+dst+',')}
        elif op=='str':
            src,address=args.split(', ',1); stored[address]=values.get(src)
        elif op=='ldr':
            dst,address=args.split(', ',1); values[dst]=stored.get(address)
        successors=graph[p]
        if op in ('cbz','cbnz'):
            register=args.split(', ',1)[0]; value=values.get(register)
            if value is not None:
                zero=value==0
                successors=[successors[0 if zero==(op=='cbz') else 1]]
        return bool(successors) and all(stop_only(q,active|{p},values,stored) for q in successors)
    reject_edges=[]; selectors=[]
    for p in sorted(reached):
        if len(graph[p])!=2: continue
        op,args=code[p]; initial={}
        previous=max((q for q in code if q<p),default=None)
        for edge,q in enumerate(graph[p]):
            initial={}
            if op in ('cbz','cbnz'):
                r=args.split(', ',1)[0]
                initial[r]=0 if (edge==0)==(op=='cbz') else nonzero
                if previous is not None and code[previous][0]=='mov':
                    dst,src=code[previous][1].split(', ',1)
                    if src==r: initial[dst]=initial[r]
            if stop_only(q,set(),initial): reject_edges.append((p,q))
        if not any(edge[0]==p for edge in reject_edges):
            if not dispatch: raise Invalid('unclassified decision failure successor '+hex(p))
            # The only non-failure entry selectors in this image are the
            # signed USERCC2 authority choice and RIS bounded polling edge.
            predecessor=code.get(previous)
            if op=='bge' and predecessor==('subs','r2, r0, #0'):
                selectors.append((p,'USERCC2 authority'))
            elif op=='bpl' and predecessor==('lsls','r6, r0, #25'):
                countdown=graph[p][0]
                following=graph[countdown][0]
                require(code[countdown]==('subs','r3, #1') and code[following][0]=='bne' and
                        graph[following][0]<p and stop_only(graph[following][1],set()),
                        'bounded PLL poll back-edge and terminal timeout successor')
                selectors.append((p,'bounded PLL lock poll'))
            else: raise Invalid('unclassified scoped failure successor '+hex(p))
    require(bool(reject_edges),'classified terminal entry failure successors')
    require(len(selectors)==(2 if dispatch else 0),'exact scoped authority/poll selector classification')
    return {'edges':[(p,q) for p in sorted(reached) for q in graph[p]],
            'terminal_failure_edges':reject_edges,'nonfailure_selectors':selectors,'transaction':launch}


def entry_location(registers,text,pc):
    """Resolve the supported decoded addressing forms, including writeback."""
    m=re.fullmatch(r'\[(\w+)(?:, #(-?(?:0x[0-9a-f]+|\d+)))?\](!)?(?:, #(-?\d+))?',text)
    if not m: raise Invalid('unresolved entry addressing '+text)
    base=((pc+4)&~3) if m[1]=='pc' else registers.get(m[1])
    if base is None: raise Invalid('unresolved entry address base '+m[1])
    off=int(m[2] or '0',0); address=base+off
    if m[3]: registers[m[1]]=address
    if m[4]: registers[m[1]]=base+int(m[4])
    return address


def entry_memory_control(pc,op,args,registers,flags,load,store,events,jump):
    """Shared entry instruction semantics; return False for other instructions.

    Memory callbacks enforce the caller's provenance policy. jump resolves a
    taken target against the caller's instruction map; flags are current NZCV.
    """
    if op in ('dmb','dsb','isb'):
        if args not in ('sy','ish'): raise Invalid('unsupported entry barrier scope '+args)
        events.append(('barrier',pc,op,args))
    elif op in ('ldr','str','ldrb','strb','ldrh','strh'):
        dst,addr=args.split(', ',1); address=entry_location(registers,addr,pc)
        width=1 if op.endswith('b') else 2 if op.endswith('h') else 4
        if op.startswith('ldr'): registers[dst]=load(address,width)
        else: store(address,registers[dst],width)
    elif op in ('cbz','cbnz'):
        register,dest=args.split(', ',1); value=registers[register]
        if value is None: raise Invalid('unresolved entry CBZ input')
        if (value==0)==(op=='cbz'): jump(int(dest.split()[0],16),'entry CBZ leaves symbol')
    elif op=='b' or re.fullmatch(r'b(eq|ne|cs|hs|cc|lo|hi|ls|ge|gt|le|lt|pl|mi)',op):
        if op=='b' or arm_condition(flags,op[1:]):
            jump(int(args.split()[0],16),'unresolved entry branch '+args)
    else: return False
    return True


def entry_selfchecks():
    """Exercise actual shared semantics against independent literal expectations."""
    from checked_support import oracle
    for listed in ('{r7, fp, lr}','{lr, r11, r7}'):
        registers={'sp':0x30010000,'r7':0x77,'r11':0xbb,'lr':0xee}; stores=[]
        stack_save(registers,listed,lambda a,v:stores.append((a,v)))
        oracle(registers['sp']==0x3000fff4,'STMDB exact predecrement/writeback')
        oracle(stores==[(0x3000fff4,0x77),(0x3000fff8,0xbb),(0x3000fffc,0xee)],
               'STMDB architectural register order')
    for listed,reason in (('{r7, r7}','duplicate'),('{fp, r11}','duplicate'),
        ('{r7-r3}','range'),('{r7--r9}','range'),('{sp, lr}','prohibited SP'),
        ('{r7, pc}','prohibited PC'),('{r7}','two registers'),('r7, lr','syntax')):
        try: architectural_registers(listed,'stmdb')
        except Invalid as exc: oracle(reason in str(exc),'intended illegal-list rejection '+listed)
        else: raise Invalid('illegal STMDB accepted '+listed)
    try: stack_save({'sp':0},'{r7, lr}',lambda a,v:None,base='r0!')
    except Invalid as exc: oracle(str(exc)=='unsupported STMDB base/form','non-SP base reason')
    else: raise Invalid('arbitrary STMDB accepted')
    for value,amount,left,carry,expected in (
        (0x80000001,1,True,False,(2,True)),(0x80000001,1,False,False,(0x40000000,True)),
        (1,32,True,False,(0,True)),(0x80000000,32,False,False,(0,True)),
        (0xffffffff,33,False,True,(0,False)),(7,256,True,True,(7,True))):
        oracle(shift32(value,amount,left,carry)==expected,'literal 32-bit shift/flags')
    for a,b,subtract,expected in ((0x7fffffff,1,False,(True,False,False,True)),
            (0xffffffff,1,False,(False,True,True,False)),(0,1,True,(True,False,False,False)),
            (0x80000000,1,True,(False,False,True,True))):
        oracle(add_sub_flags(a,b,subtract)==expected,'literal NZCV arithmetic/signed overflow')
    oracle(it_slots('itee','eq')==['eq','ne','ne'],'literal IT mask order')
    oracle(it_slots('itt','mi')==['mi','mi'],'literal IT signed condition')
    oracle(arm_condition((True,False,False,True),'ge') is True,'signed GE uses N equals V')
    oracle(arm_condition((False,False,False,False),'lo') is True,'unsigned LO uses carry')
    try: arm_condition((False,False,None,False),'hi')
    except Invalid as exc: oracle(str(exc)=='unresolved architectural flags for hi','unknown carry stays INVALID')
    else: raise Invalid('unknown architectural condition admitted')
    oracle(verified_leaf([(0x134,'ldr','r0, [r0, #0]'),(0x136,'bx','lr')])=='read','typed-body read shape')
    oracle(verified_leaf([(0x138,'str','r1, [r0, #0]'),(0x13a,'bx','lr')])=='write','typed-body write shape')
    delay_rows=[(0x13c,'sub','sp, #8'),(0x13e,'str','r0, [sp, #4]'),
        (0x140,'ldr','r3, [sp, #4]'),(0x142,'cbnz','r3, 148 <clock_delay+0xc>'),
        (0x144,'add','sp, #8'),(0x146,'bx','lr'),(0x148,'nop',''),
        (0x14a,'ldr','r3, [sp, #4]'),(0x14c,'subs','r3, #1'),
        (0x14e,'str','r3, [sp, #4]'),(0x150,'b','140 <clock_delay+0x4>')]
    oracle(verified_leaf(delay_rows)=='delay','literal bounded delay CFG')
    for bad in (delay_rows[:8]+[(0x14c,'subs','r3, #2')]+delay_rows[9:],
                [(0x134,'ldrb','r0, [r0, #0]'),(0x136,'bx','lr')],
                [(0x138,'str','r1, [r0, #4]'),(0x13a,'bx','lr')],
                [(0x134,'ldr','r0, [r1, #0]'),(0x136,'bx','lr')],
                [(0x134,'ldr','r0, [r0, #0]'),(0x136,'blx','r3')]):
        try: verified_leaf(bad)
        except Invalid as exc: oracle(str(exc)=='unverified MMIO leaf instruction semantics','leaf control reason')
        else: raise Invalid('mutated MMIO leaf admitted')
    transcript=MMIOTranscript(0x68dc,CLOCK_SITES_DISPATCH,0x9e0,None)
    oracle(transcript.effect(0x68ea,'read',0x400fe070,4)==0x80000000,'literal RCC2 input at actual callsite')
    transcript.effect(0x68f8,'write',0x400fe070,4,0x80000800)
    oracle(transcript.effect(0x68fc,'read',0x400fe070,4)==0x80000800,'literal RCC2 readback')
    for effect,reason in (((0x690e,'read',0x400fe064,4),'unmatched MMIO effect'),
                         ((0x690e,'read',0x400fe060,1),'unmatched MMIO effect')):
        try: transcript.effect(*effect)
        except Invalid as exc: oracle(str(exc).startswith(reason),'unknown MMIO exact reason')
        else: raise Invalid('unknown MMIO control admitted')
    try: transcript.finish()
    except Invalid as exc: oracle(str(exc)=='leftover MMIO transcript','leftover read rejection')
    else: raise Invalid('leftover transcript admitted')
    exhausted=MMIOTranscript(0x68dc,CLOCK_SITES_DISPATCH,0x9e0,'clock-readback-fail')
    exhausted.effect(0x68ea,'read',0x400fe070,4)
    try: exhausted.effect(0x68f8,'write',0x400fe070,4,0x80000801)
    except Invalid as exc: oracle(str(exc)=='MMIO write/delay value','wrong MMIO write value rejection')
    else: raise Invalid('wrong MMIO value admitted')
    exhausted.effect(0x68f8,'write',0x400fe070,4,0x80000800)
    exhausted.effect(0x68fc,'read',0x400fe070,4)
    exhausted.finish()
    try: exhausted.effect(0x690e,'read',0x400fe060,4)
    except Invalid as exc: oracle(str(exc)=='MMIO transcript exhausted','exhausted MMIO rejection')
    else: raise Invalid('exhausted transcript admitted')
    state=0x20000050
    positive=[('store',0x6c34,0x20000530,1,37),('barrier',0x6c38,'dmb','sy'),
              ('store',0x6c46,state,4,11),('call',0x6c48,'boot_startup_prepare'),
              ('barrier',0x6c52,'dmb','sy'),('store',0x6c5a,state,4,13)]
    publication_barriers(positive,state)
    for bad in (positive[:1]+positive[2:],positive[:4]+positive[5:],
                [positive[1],positive[0],*positive[2:]],
                [positive[0],('barrier',0x6c38,'dsb','sy'),*positive[2:]]):
        try: publication_barriers(bad,state)
        except Failure as exc: oracle(str(exc)=='fresh DMB before checkpoint publication','barrier control reason')
        else: raise Invalid('missing/moved/wrong barrier admitted')
    # Small instruction stimuli use the same memory/control dispatcher as the
    # linked-entry interpreter. They test these semantics, not full CFG/ELF or
    # full-runner acceptance. Expectations remain independently written literals.
    instructions=[(0x100,'ldr','r2, [r0, #8]'),(0x102,'ldr','r3, [r0, #16]')]
    for offset in range(8):
        instructions.extend([(0x104+offset*4,'ldrb',f'r6, [r4, #{offset}]'),
                             (0x106+offset*4,'strb',f'r6, [r5, #{offset}]')])
    instructions.extend([(0x124,'dmb','sy'),(0x128,'str','r7, [r8, #0]'),
                         (0x12a,'beq','200'),(0x12c,'bne','204')])
    def observe(stimulus):
        registers={'r0':0x1000,'r2':None,'r3':None,'r4':0x1100,'r5':0x1200,
                   'r6':None,'r7':11,'r8':0x1300}
        memory=dict(enumerate(bytes([0,1,2,3,4,5,6,7]),0x1100))
        memory.update(dict(enumerate(struct.pack('<I',5),0x1008)))
        memory.update(dict(enumerate(struct.pack('<I',1),0x1010)))
        events=[]; targets=[]
        def load(address,width):
            return int.from_bytes(bytes(memory[address+j] for j in range(width)),'little')
        def store(address,value,width):
            for j in range(width): memory[address+j]=(value>>(j*8))&255
            events.append(('store',pc,address,width,value))
        for pc,op,args in stimulus:
            oracle(entry_memory_control(pc,op,args,registers,(False,True,True,False),
                   load,store,events,lambda address,reason:targets.append(address)),
                   'selfcheck instruction has shared semantics')
        return registers,memory,events,targets
    def expectations(observed):
        registers,memory,events,targets=observed
        oracle((registers['r2'],registers['r3'])==(5,1),'interpreter field-offset expectation: instruction LDR +8/+16')
        oracle(bytes(memory.get(0x1200+j,255) for j in range(8))==bytes([0,1,2,3,4,5,6,7]),
               'interpreter copy expectation: instruction byte-copy')
        oracle(('barrier',0x124,'dmb','sy') in events,'interpreter barrier-recording expectation: instruction DMB')
        publication_barriers(events,0x1300)
        oracle(targets==[0x200],'interpreter branch expectation: instruction EQ/NE taken-target')
    expectations(observe(instructions))
    for old,new,reason in (
        ((0x100,'ldr','r2, [r0, #8]'),(0x100,'ldr','r2, [r0, #16]'),'interpreter field-offset expectation: instruction LDR +8/+16'),
        ((0x120,'ldrb','r6, [r4, #7]'),(0x120,'ldrb','r6, [r4, #6]'),'interpreter copy expectation: instruction byte-copy'),
        ((0x124,'dmb','sy'),(0x124,'dsb','sy'),'interpreter barrier-recording expectation: instruction DMB'),
        ((0x12a,'beq','200'),(0x12a,'bne','200'),'interpreter branch expectation: instruction EQ/NE taken-target')):
        oracle(old in instructions,'corruption stimulus replaces an actual instruction')
        observed=observe([new if row==old else row for row in instructions])
        try: expectations(observed)
        except Invalid as exc:
            oracle(str(exc)=='oracle selfcheck: '+reason,reason+' corruption rejected for intended reason')
        else: raise Invalid('instruction corruption admitted: '+reason)

def entry_gate(session,elf,dis,dispatch):
    names=('STARTING','CLOCK_FAILED','FLASH_FAILED','IMAGES_FAILED','BIND_FAILED','SEQUENCE_FAILED','DECISION',
           'HANDOFF_INIT_FAILED','OBSERVE_FAILED','LAUNCH_FAILED','LAUNCH_READY','PREPARE_FAILED','DISPATCH_READY')
    source=(ROOT/'src/boot_entry/boot_decision_entry.c').read_text()
    enum=re.search(r'enum\s*\{(.*?)\};',source,re.S)
    if not enum: raise Invalid('entry checkpoint enum absent')
    clean=re.sub(r'/\*.*?\*/','',enum[1],flags=re.S)
    tokens=[t.strip() for t in clean.split(',')]
    require(tokens==['BOOT_ENTRY_STARTING = 1',*['BOOT_ENTRY_'+n for n in names[1:]]],'append-only checkpoint constants 1..7 and 8..13')
    # Objdump may abbreviate zero alignment padding with `...`; it is not an
    # executable instruction. A reached gap still has no CFG row and is INVALID.
    dis=re.sub(r'^\s*\.\.\.\s*$', '', dis, flags=re.M)
    def typed_rows(name,address=None):
        if address is None: address=elf.value(name)&~1
        if not elf.code(name,address): raise Invalid('undefined/untyped entry callee '+name)
        matches=[r for r in elf.symbols[name] if (r[3]&~1)==address and r[1]==2]
        if len(matches)!=1: raise Invalid('ambiguous address-qualified callee '+name)
        extent=matches[0][4]
        labels=re.findall(r'^\s*([0-9a-f]+) <([^>]+)>:',dis,re.M)
        labels=[label for raw,label in labels if int(raw,16)==address]
        if len(labels)!=1: raise Invalid('missing/ambiguous disassembly body at typed address')
        result=instruction_rows(dis,labels[0])
        if result[0][0]!=address: raise Invalid('disassembly/typed callee address mismatch')
        return [r for r in result if address<=r[0]<address+extent]
    rows=typed_rows('boot_decision_entry')
    entry_rows=list(rows)
    cfg=entry_cfg(entry_rows,dispatch)
    interpreted={'boot_clock_setup','boot_target_flash_init','boot_target_flash_io',
        'boot_flash_init','boot_target_images_read','boot_target_sequence_bind',
        'boot_target_reset_adapter','boot_flash_store_adapter','boot_sequence_init',
        'boot_runtime_watchdog_init','boot_runtime_init','register_read','memcpy'}
    interpreted.update(name for name in elf.symbols if name=='aligned' or name.startswith('aligned.'))
    leaf_names={name for name in elf.symbols if name in ('clock_read','clock_write','clock_delay') or name.startswith('clock_read.lto_priv.0')}
    leaves={name:verified_leaf(typed_rows(name)) for name in leaf_names}
    # Check stop and terminal against actual typed instructions/literals, not
    # a callee-name assertion. The literal relocation is checked below.
    stop_rows=typed_rows('stop'); terminal_rows=typed_rows('Boot_Terminal_Handler')
    if [(o,a) for _,o,a in terminal_rows]!=[('cpsid','i'),('b',f'{terminal_rows[0][0]+2:x} <boot_terminal_handler+0x2>')]:
        raise Invalid('unverified terminal IRQ-disable/nonreturn loop')
    for name in sorted(interpreted):
        if name in elf.symbols: rows+=typed_rows(name)
    unique={}
    for row in rows:
        if row[0] in unique and unique[row[0]]!=row: raise Invalid('conflicting aliased instruction bodies')
        unique[row[0]]=row
    # Keep the entry first, then address order for helper fallthrough. Each
    # function ends in an interpreted return/tail; gaps never supply semantics.
    entry_pcs={p for p,_,_ in entry_rows}
    rows=entry_rows+[unique[p] for p in sorted(unique) if p not in entry_pcs]
    # Instructions have actual linked addresses. Literal pools are read from ELF
    # sections, not guessed from comments or symbol-name proximity.
    bypc={r[0]:i for i,r in enumerate(rows)}
    widths={}
    for line in dis.splitlines():
        m=re.fullmatch(r'\s*([0-9a-f]+):\s+((?:[0-9a-f]{4,8}\s+)+)([.a-z][.a-z0-9]*)\s*(.*?)\s*',line)
        if m: widths[int(m[1],16)]=sum(len(part)//2 for part in m[2].split())
    if any(p not in widths or widths[p] not in (2,4) for p,_,_ in rows):
        raise Invalid('unresolved decoded entry instruction width')
    def word(address):
        for s in elf.sections.values():
            if s[1]==1 and s[3]<=address and address+4<=s[3]+s[5]:
                return struct.unpack_from('<I',elf.data,s[4]+address-s[3])[0]
        raise Invalid('unresolved ELF literal '+hex(address))
    def symbol(name): return elf.value(name)
    objects={'sequence':24,'parameters':40,'flash':36,'images':328,'registers':12,
             'store':16,'producer':12,'boot_entry_sequence_result':252,
             'boot_entry_state':4,'boot_entry_flash_result':4}
    objects.update({'target':136,'handoff':24,'observed':20,'launch_parameters':48,
                    'launch':284,'boot_entry_launch':284} if dispatch else {'decision':252,'boot_entry_decision':252})
    spans=[]
    for name,size in objects.items():
        _,kind,section,address,extent=elf.symbol(name)
        s=elf.sections.get(section)
        require(kind==1 and extent==size and address%4==0 and s is not None and s[1]==8 and
                s[2]&1 and s[3]<=address and address+size<=s[3]+s[5] and
                0x20000000<=address and address+size<=0x2000dbc0,
                'typed aligned bounded ordinary entry object '+name)
        spans.append((address,address+size,name))
    spans.sort()
    require(all(a[1]<=b[0] for a,b in zip(spans,spans[1:])),'disjoint actual entry object extents')
    require(symbol('boot_startup_continuity')==0x2000dbc0,'linked native continuity binding')
    for section,address,size in (('.boot_continuity',0x2000dbc0,1024),('.boot_diagnostic',0x2000dfc0,64)):
        s=elf.sections.get(section)
        require(s is not None and s[1]==8 and s[2]&1 and s[3]==address and s[5]==size and address%64==0,
                'bounded aligned retained entry binding '+section)
    for callback in ('boot_startup_gptm_direct_read','boot_startup_gptm_direct_write',
            'boot_startup_gptm_direct_delay','reset_read','reset_write','store_read','store_erase',
            'store_program','register_read','register_write','flash_read','boot_target_verify_read'):
        pointer=symbol(callback)
        require(pointer&1 and elf.code(callback,pointer&~1),'typed Thumb entry binding '+callback)
    def validate_bindings(load):
        common=(0x2000dbc0,0x2000dfc0,50000000,symbol('producer'),
            symbol('boot_startup_gptm_direct_delay'),symbol('registers'),symbol('store'),
            symbol('images'),symbol('boot_target_verify_read'),symbol('flash'))
        for name,expected in [('parameters',common)]+([('launch_parameters',common+(symbol('handoff'),symbol('observed')))] if dispatch else []):
            require(tuple(load(symbol(name)+4*j) for j in range(len(expected)))==expected,
                    'complete original descriptor/snapshot bindings '+name)
        for name,expected in (
            ('producer',(0x2000dbc0+384,symbol('boot_startup_gptm_direct_read'),symbol('boot_startup_gptm_direct_write'))),
            ('registers',(0,symbol('reset_read'),symbol('reset_write'))),
            ('store',(symbol('flash'),symbol('store_read'),symbol('store_erase'),symbol('store_program')))):
            require(tuple(load(symbol(name)+4*j) for j in range(len(expected)))==expected,
                    'complete adapter/context bindings '+name)
        require(tuple(load(symbol('flash')+off,width) for off,width in
                ((0,4),(4,4),(8,4),(12,4),(16,4),(20,4),(24,1),(28,4),(32,4)))==
                (0,symbol('register_read'),symbol('register_write'),symbol('flash_read'),50,32768,0,0,0),
                'complete flash IO/timing/poll/atomic bindings without padding guesses')
        require(load(symbol('images')+320)==160 and load(symbol('images')+324)==160,
                'both original manifest extents bound')
    def run(failing=None,mutation=None):
        regs={'r'+str(i):UNKNOWN for i in range(13)}; regs.update(sp=SP,lr=UNKNOWN,pc=UNKNOWN)
        memory={}; cmp=None; calls=[]; checkpoints=[]; result_address=None; launch_address=None
        events=[]; returned=None; launched=None; frames=[]; image_reads=0
        clock_base=symbol('boot_decision_entry' if dispatch else 'boot_clock_setup')&~1
        flash_pc=(symbol('register_read')&~1)+(0x38 if dispatch else 0x34)
        transcript=MMIOTranscript(clock_base,CLOCK_SITES_DISPATCH if dispatch else CLOCK_SITES_DECISION,flash_pc,failing)
        i=0; steps=0; pending_it=[]; outcome=None
        def verify_stop():
            body=[(o,a) for _,o,a in stop_rows if not o.startswith('.') and o!='nop']
            if mutation=='stop-barrier': body[1]=('nop','')
            require(len(body)==5 and body[0]==('push','{r3, lr}') and body[1]==('dmb','sy') and
                    body[3]==('str','r0, [r3, #0]'),'verified stop DMB-before-state instruction order')
            m=re.fullmatch(r'r3, \[pc, #(\d+)\]',body[2][1])
            require(body[2][0]=='ldr' and m is not None,'verified stop state literal addressing')
            require(word(((stop_rows[2][0]+4)&~3)+int(m[1]))==symbol('boot_entry_state'),
                    'verified stop publishes original state object')
            require(body[4]==('bl',f'{symbol("Boot_Terminal_Handler")&~1:x} <boot_terminal_handler>'),
                    'verified stop calls actual terminal')
            events.append(('barrier',stop_rows[1][0],'dmb','sy'))
            store(symbol('boot_entry_state'),known(regs['r0'],'stop state'),event_pc=stop_rows[3][0])
            events.append(('terminal',terminal_rows[0][0],'cpsid i; nonreturn self-loop'))
        def return_from_callee(target):
            nonlocal i,cmp
            if not frames: raise Invalid('unexpected returning boot entry')
            expected,caller_sp,saved=frames.pop()
            if target!=expected or regs['sp']!=caller_sp: raise Invalid('callee return/stack provenance')
            require(all(regs[r]==v for r,v in saved.items()),'AAPCS callee-saved register preservation')
            if target&~1 not in bypc: raise Invalid('unresolved callee return PC')
            i=bypc[target&~1]
        def number(text):
            text=text.strip()
            if text.startswith('#'): return int(text[1:],0)
            if text in regs: return regs[text]
            raise Invalid('unparsed entry operand '+text)
        def known(value,why):
            if value is None: raise Invalid('unresolved entry '+why)
            return value
        def location(text,pc): return entry_location(regs,text,pc)
        def jump(address,reason):
            nonlocal i
            if address not in bypc: raise Invalid(reason)
            i=bypc[address]
        def load(a,width=4):
            values=[memory.get(a+j) for j in range(width)]
            if all(a+j in memory for j in range(width)):
                return int.from_bytes(bytes(values),'little') if all(v is not None for v in values) else UNKNOWN
            if 0x40000000<=a<0x60000000:
                return transcript.effect(pc,'read',a,width)
            # Only initialized ELF PROGBITS data, not guessed zero RAM/MMIO.
            for section in elf.sections.values():
                if section[1]==1 and section[2]&2 and section[3]<=a and a+width<=section[3]+section[5]:
                    return int.from_bytes(elf.data[section[4]+a-section[3]:section[4]+a-section[3]+width],'little')
            raise Invalid('unknown entry memory read '+hex(a))
        def store(a,v,width=4,event_pc=None):
            if mutation and mutation.startswith('binding:'):
                _,object_name,offset=mutation.split(':')
                if a==symbol(object_name)+int(offset): v=known(v,'binding mutation original')^4
            if mutation=='producer-context' and a==symbol('producer'): v=symbol('target') if dispatch else symbol('sequence')
            if mutation=='descriptor' and a==symbol('parameters')+36: v=symbol('producer')
            if mutation=='publication-byte' and a==symbol('boot_entry_sequence_result')+251: v=0
            if mutation=='launch-publication-byte' and dispatch and a==symbol('boot_entry_launch')+283: v=0
            if 0x40000000<=a<0x60000000:
                transcript.effect(pc,'write',a,width,known(v,'MMIO write')); return
            if not (SP-4096<=a and a+width<=SP or any(s[1]==8 and s[2]&1 and
                    s[3]<=a and a+width<=s[3]+s[5] for s in elf.sections.values())):
                raise Invalid('unknown entry write provenance '+hex(a))
            for j in range(width): memory[a+j]=None if v is None else (v>>(8*j))&255
            events.append(('store',pc if event_pc is None else event_pc,a,width,v))
            if a==symbol('boot_entry_state'):
                checkpoints.append(known(v,'checkpoint value'))
        flags=nzcv; arithmetic_flags=add_sub_flags
        def cond(code): return arm_condition(cmp,code)
        def reglist(text,form): return architectural_registers(text,form)
        while 0<=i<len(rows):
            steps+=1
            if steps>20000: raise Invalid('entry abstract dataflow budget exhausted')
            pc,op,args=rows[i]; i+=1
            if op not in ('b','bl','bx','pop') and not (op.startswith('ldm') and 'pc' in args):
                if i>=len(rows) or rows[i][0]!=pc+widths[pc]:
                    raise Invalid('unresolved instruction fallthrough/gap '+hex(pc))
            events.append(('instruction',pc,op,args))
            args=re.sub(r'\b(?:ip|fp|sl)\b',lambda m:{'ip':'r12','fp':'r11','sl':'r10'}[m[0]],args)
            if dispatch and result_address is not None and op=='ldr':
                offsets={'reject-sequence-status':0,'reject-watchdog-wrapper-status':8,
                         'reject-nested-runtime-status':16,'reject-handoff-status':220}
                if mutation in offsets and args==f'r{2 if offsets[mutation] in (8,16,220) else 3}, [r5, #{offsets[mutation]}]':
                    op='mov'; args=args.split(', ',1)[0]+', #0'
            if dispatch and mutation in ('reject-sequence-stage','reject-missing-runtime-valid','reject-missing-handoff-valid'):
                offset=0x2be if mutation=='reject-sequence-stage' else 0x2c8
                if pc==(symbol('boot_decision_entry')&~1)+offset: op='nop'; args=''
            if mutation in ('ready11-barrier','ready13-barrier') and dispatch and pc==(symbol('boot_decision_entry')&~1)+(0x35c if mutation=='ready11-barrier' else 0x376):
                op='nop'; args=''
            if op=='b' and int(args.split()[0],16) not in bypc:
                # AAPCS tail calls have the same argument provenance.
                op='bl'
            if pending_it:
                slot=pending_it.pop(0)
                # Validate objdump's condition suffix even on a skipped slot;
                # evaluate current architectural flags, not frozen IT booleans.
                if not op.endswith(slot): raise Invalid('inconsistent IT instruction condition')
                op=op[:-len(slot)]
                if not cond(slot): continue
            if re.fullmatch(r'it[te]{0,3}',op):
                if pending_it: raise Invalid('nested IT')
                pending_it=it_slots(op,args); continue
            if entry_memory_control(pc,op,args,regs,cmp,load,store,events,jump): continue
            if op=='nop': continue
            if op in ('mov','movs','movw','movt','mvn','mvns'):
                dst,src=args.split(', ',1); value=number(src)
                if op=='movt': value=(known(regs[dst],'movt lower')&0xffff)|(known(value,'movt upper')<<16)
                if op.startswith('mvn'): value=None if value is None else (~value)&0xffffffff
                regs[dst]=value
                if op in ('movs','mvns'): cmp=flags(value,cmp[2] if cmp else None,cmp[3] if cmp else None)
            elif op in ('add','adds','addw','sub','subs','subw','and','ands','orr','orrs','bic','bics','lsl','lsls','lsr','lsrs','rsb','rsbs','ubfx'):
                parts=args.split(', '); dst=parts[0]
                if len(parts)==2: a=regs[dst]; b=number(parts[1])
                else:
                    a=((pc+4)&~3) if parts[1]=='pc' else number(parts[1]); b=number(parts[2])
                value=None
                if a is not None and b is not None:
                    if op.startswith('add'): value=a+b
                    elif op.startswith('sub'): value=a-b
                    elif op.startswith('and'): value=a&b
                    elif op.startswith('orr'): value=a|b
                    elif op.startswith('bic'): value=a&~b
                    elif op.startswith('lsl'): value=shift32(a,b,True,cmp[2] if cmp else None)[0]
                    elif op.startswith('lsr'): value=shift32(a,b,False,cmp[2] if cmp else None)[0]
                    elif op.startswith('rsb'): value=b-a
                    elif op=='ubfx': value=(a>>b)&((1<<number(parts[3]))-1)
                regs[dst]=None if value is None else value&0xffffffff
                if op.endswith('s'):
                    if op in ('adds','subs'): cmp=arithmetic_flags(a,b,op=='subs')
                    elif op=='rsbs': cmp=arithmetic_flags(b,a,True)
                    else:
                        carry=cmp[2] if cmp else None
                        if op in ('lsls','lsrs'):
                            carry=None if a is None or b is None else shift32(a,b,op=='lsls',carry)[1]
                        elif parts[-1].startswith('#'): carry=None  # unknown modified-immediate expansion
                        cmp=flags(regs[dst],carry,cmp[3] if cmp else None)
            elif op=='mls':
                dst,left,right,accumulator=args.split(', ')
                regs[dst]=(known(number(accumulator),'MLS accumulator')-
                           known(number(left),'MLS multiplicand')*known(number(right),'MLS multiplier'))&0xffffffff
            elif op in ('udiv','muls','clz','negs','adcs'):
                parts=args.split(', '); dst=parts[0]
                a=known(number(parts[1] if len(parts)>2 else (parts[-1] if op in ('clz','negs') else dst)),'integer input')
                b=known(number(parts[-1]),'integer input')
                if op=='udiv':
                    if b==0: raise Invalid('UDIV zero divisor')
                    regs[dst]=a//b
                elif op=='muls': regs[dst]=(a*b)&0xffffffff; cmp=flags(regs[dst],cmp[2] if cmp else None,cmp[3] if cmp else None)
                elif op=='clz': regs[dst]=32-b.bit_length()
                elif op=='negs': regs[dst]=(-b)&0xffffffff; cmp=arithmetic_flags(0,b,True)
                else:
                    if cmp is None or cmp[2] is None: raise Invalid('unknown ADC carry')
                    carry=int(cmp[2]); total=a+b+carry; result=total&0xffffffff
                    regs[dst]=result; cmp=flags(result,total>0xffffffff,bool((~(a^b)&(a^result))&0x80000000))
            elif op in ('cmp','cmn','tst'):
                a,b=args.split(', ',1); x=number(a)
                rotated=re.fullmatch(r'(r\d+), ror #(\d+)',b)
                if rotated:
                    y=known(number(rotated[1]),'ROR operand'); shift=int(rotated[2])
                    if not 0<shift<32: raise Invalid('unsupported ROR amount')
                    y=((y>>shift)|(y<<(32-shift)))&0xffffffff
                else: y=number(b)
                cmp=flags(None if x is None or y is None else x&y,None,cmp[3] if cmp else None) if op=='tst' else arithmetic_flags(x,y,op=='cmp')
            elif op in ('ldrd','strd'):
                first,second,addr=args.split(', ',2); a=location(addr,pc)
                if op=='ldrd': regs[first],regs[second]=load(a),load(a+4)
                else: store(a,regs[first]); store(a+4,regs[second])
            elif op in ('ldmia','stmia','ldm','stm'):
                base,listed=args.split(', ',1); wb=base.endswith('!'); base=base.rstrip('!')
                a=known(regs[base],'multiple-register base'); listed=reglist(listed,op)
                if wb and base in listed: raise Invalid('multiple-register writeback base in list')
                for j,r in enumerate(listed):
                    if op.startswith('ldm'): regs[r]=load(a+j*4)
                    else: store(a+j*4,regs[r])
                if wb: regs[base]=a+len(listed)*4
                if op.startswith('ldm') and 'pc' in listed: return_from_callee(known(regs['pc'],'LDM return'))
            elif op=='stmdb':
                fields=args.split(', ',1)
                if len(fields)!=2: raise Invalid('unsupported STMDB base/form')
                stack_save(regs,fields[1],store,base=fields[0])
            elif op=='push':
                stack_save(regs,args,store,form='push')
            elif op=='pop':
                listed=reglist(args,'pop')
                for j,r in enumerate(listed): regs[r]=load(regs['sp']+4*j)
                regs['sp']+=len(listed)*4
                if 'pc' in listed: return_from_callee(known(regs['pc'],'POP return'))
            elif op=='bx':
                if args!='lr': raise Invalid('unsupported indirect return')
                return_from_callee(known(regs['lr'],'BX return'))
            elif op in ('bl','blx'):
                if op=='blx':
                    pointer=known(number(args),'indirect call pointer')
                    if not pointer&1: raise Invalid('non-Thumb indirect entry call')
                    address=pointer&~1
                    candidates=[n for n in interpreted|leaf_names if n in elf.symbols and elf.code(n,address)]
                    if len(candidates)!=1: raise Invalid('unknown/ambiguous indirect call provenance')
                    name=candidates[0]
                else:
                    match=re.fullmatch(r'([0-9a-f]+) <([^>]+)>',args)
                    if not match: raise Invalid('unresolved entry call '+args)
                    address=int(match[1],16)
                    candidates=[n for n in elf.symbols if n.lower()==match[2] and elf.code(n,address)]
                    if len(candidates)!=1: raise Invalid('actual defined typed executable entry callee '+args)
                    name=candidates[0]
                require(regs['sp']%8==0,'AAPCS call stack alignment')
                calls.append({'name':name,'pc':pc,'args':[regs['r'+str(j)] for j in range(4)]})
                events.append(('call',pc,name))
                if name=='stop':
                    state=known(regs['r0'],'stop checkpoint'); verify_stop()
                    outcome=('stop',state); break
                if name=='Boot_Terminal_Handler': raise Invalid('entry bypasses stop publication')
                if name in leaves:
                    kind=leaves[name]
                    if mutation=='unknown-mmio' and kind=='read': regs['r0']=0x400fe064
                    if kind=='delay':
                        count=known(regs['r0'],'delay iterations')
                        if not 0<count<=524288: raise Invalid('delay summary bound')
                        transcript.effect(pc,'delay',0,4,count)
                        store(regs['sp']-4,0); regs['r3']=0; cmp=arithmetic_flags(1,1,True)
                    elif kind=='read': regs['r0']=transcript.effect(pc,'read',known(regs['r0'],'MMIO address'),4)
                    else: transcript.effect(pc,'write',known(regs['r0'],'MMIO address'),4,known(regs['r1'],'MMIO value'))
                    # Verified leaves preserve other caller-saved registers and
                    # flags (except the exact counted delay's R3/NZCV effects).
                    regs['lr']=(pc+widths[pc])|1
                    continue
                if name in interpreted:
                    if len(frames)>=12: raise Invalid('entry call-depth bound')
                    saved={r:regs[r] for r in ('r4','r5','r6','r7','r8','r9','r10','r11')}
                    frames.append(((pc+widths[pc])|1,regs['sp'],saved)); regs['lr']=(pc+widths[pc])|1
                    if address not in bypc: raise Invalid('callee CFG unresolved')
                    i=bypc[address]; continue
                if name=='boot_startup_dispatch':
                    if mutation=='dispatch-pointer': regs['r0']=symbol('producer')
                    require(regs['r0']==0x2000dbc0,'actual dispatch continuity argument'); outcome=('dispatch',13); break
                if name=='boot_flash_read':
                    image_reads+=1
                    require(image_reads<=2 and regs['r0']==symbol('flash') and
                            regs['r1']==(0x23c00 if image_reads==1 else 0x3f400) and regs['r3']==160,
                            'image read exact flash/manifest/extent arguments')
                    destination=known(regs['r2'],'manifest output')
                    require(destination%4==0 and SP-2048<=destination<=SP-160,'image read stack output provenance')
                    ret=4 if failing==('image-first-read-fail' if image_reads==1 else 'image-second-read-fail') else 0
                    if ret==0:
                        for off in range(160): store(destination+off,(off+image_reads*37)&255,1)
                elif name in ('boot_handoff_target_init','boot_handoff_target_observe'):
                    if name=='boot_handoff_target_init':
                        if mutation=='target-init-pointer': regs['r1']=symbol('target')
                        require(regs['r0']==symbol('target') and regs['r1']==0x2000dbc0 and regs['r2']==0 and regs['r3']==symbol('handoff'),'actual target init binding')
                    if name=='boot_handoff_target_observe':
                        if mutation=='observe-pointer': regs['r0']=symbol('target')
                        require(regs['r0']==symbol('observed'),'actual observed storage')
                    ret=1 if failing==name else 0
                elif name in ('boot_sequence_launch','boot_sequence_run'):
                    require(not any(c['name'] in ('boot_sequence_launch','boot_sequence_run') for c in calls[:-1]),'no launch/run retry')
                    validate_bindings(load)
                    if mutation=='original-result-pointer': regs['r0']=symbol('boot_entry_sequence_result')
                    result_address=known(regs['r0'],'AAPCS aggregate return pointer')
                    require(result_address%4==0 and SP-2048<=result_address<=SP-252,'AAPCS original aggregate stack provenance')
                    require(regs['r1']==symbol('sequence'),'same actual sequence argument')
                    require(regs['r2']==symbol('launch_parameters' if dispatch else 'parameters'),'actual snapshotted descriptor argument')
                    launch_address=known(regs['r3'],'output pointer')
                    require(launch_address==symbol('launch' if dispatch else 'decision'),'actual launch output storage')
                    # Static aggregate/output input stimulus, including distinct
                    # non-status bytes. Provenance is the actual aggregate return
                    # destination, never a fabricated HOST or verifier result.
                    result_size=elf.symbol('boot_entry_sequence_result')[4]
                    if result_size!=252: raise Invalid('aggregate extent differs from literal 252-byte contract')
                    returned=result_stimulus(failing,dispatch)
                    for off,value in enumerate(returned): store(result_address+off,value,1)
                    extent=284 if dispatch else elf.symbol('decision')[4]
                    if extent>284: raise Invalid('decision output exceeds bounded input stimulus')
                    launched=LAUNCH_POISON[:extent]
                    for off,value in enumerate(launched): store(launch_address+off,value,1)
                    ret=None
                elif name=='boot_startup_prepare':
                    if mutation=='runtime-pointer': regs['r1']=symbol('boot_entry_sequence_result')+8
                    if mutation=='launch-pointer': regs['r2']=symbol('boot_entry_launch')
                    require(regs['r0']==0x2000dbc0 and regs['r1']==result_address+8 and regs['r2']==launch_address,
                            'prepare consumes exact returned result.runtime and launch addresses')
                    ret=0 if failing==name else 1
                else: raise Invalid('unmodeled compiled entry callee '+name)
                for r in ('r0','r1','r2','r3','r12','lr'): regs[r]=None
                regs['r0']=ret; cmp=None
                if mutation=='launch-retry' and name in ('boot_sequence_launch','boot_sequence_run'): i=bypc[pc]
            else: raise Invalid('unresolved compiled entry instruction '+op+' '+args)
        if outcome is None: raise Invalid('compiled entry did not terminate in bounded static witness')
        transcript.finish()
        publication_barriers(events,symbol('boot_entry_state'))
        if mutation=='publication-order' and dispatch:
            events.insert(0,('store',0,symbol('boot_entry_state'),4,11))
            checkpoints.insert(0,11)
        if dispatch and 11 in checkpoints:
            ready=next(j for j,e in enumerate(events) if e[0]=='store' and e[2]==symbol('boot_entry_state') and e[4]==11)
            launch_stores=[j for j,e in enumerate(events) if e[0]=='store' and symbol('boot_entry_launch')<=e[2]<symbol('boot_entry_launch')+284]
            prepares=[j for j,e in enumerate(events) if e[:1]==('call',) and e[2]=='boot_startup_prepare']
            require(launch_stores and max(launch_stores)<ready and len(prepares)==1 and ready<prepares[0],
                    'complete launch publication before ready11 before prepare')
        if dispatch and 13 in checkpoints:
            ready=next(j for j,e in enumerate(events) if e[0]=='store' and e[2]==symbol('boot_entry_state') and e[4]==13)
            prepare=next(j for j,e in enumerate(events) if e[0]=='call' and e[2]=='boot_startup_prepare')
            transfer=next(j for j,e in enumerate(events) if e[0]=='call' and e[2]=='boot_startup_dispatch')
            require(prepare<ready<transfer,'prepare before ready13 before dispatch')
        if result_address is not None:
            publication=symbol('boot_entry_sequence_result')
            require(bytes(known(load(publication+j,1),'result publication byte') for j in range(len(returned)))==returned,
                    'exact original aggregate publication including poison remainder')
            if dispatch and outcome!=('stop',10):
                publication=symbol('boot_entry_launch')
                require(bytes(known(load(publication+j,1),'launch publication byte') for j in range(284))==launched,
                        'exact original launch publication including poison remainder')
            if not dispatch:
                publication=symbol('boot_entry_decision')
                written=any(e[0]=='store' and publication<=e[2]<publication+252 for e in events)
                require(written==(outcome==('stop',7)),'decision publication only on success')
                if written:
                    require(bytes(known(load(publication+j,1),'decision publication byte') for j in range(252))==launched,
                            'exact original decision publication including poison remainder')
            if dispatch and outcome==('stop',10):
                require(not any(c['name'] in ('boot_startup_prepare','boot_startup_dispatch') for c in calls),
                        'independent result rejection excludes prepare and dispatch')
                require(not any(e[0]=='store' and symbol('boot_entry_launch')<=e[2]<symbol('boot_entry_launch')+284 for e in events),
                        'independent result rejection excludes launch publication')
        require(sum(c['name']=='boot_sequence_launch' for c in calls)<=1,'no dynamic launch retry')
        return {'static_path_stimulus':failing or 'success','outcome':outcome,'checkpoints':checkpoints,'calls':calls,'steps':steps,'memory':memory,'events':events,'mmio':transcript.events}
    successful=run()
    expected=('boot_handoff_target_init','boot_handoff_target_observe','boot_sequence_launch','boot_startup_prepare','boot_startup_dispatch') if dispatch else ('boot_sequence_run',)
    relevant=[r['name'] for r in successful['calls'] if r['name'] in ('boot_handoff_target_init','boot_handoff_target_observe','boot_sequence_launch','boot_startup_prepare','boot_startup_dispatch','boot_sequence_run')]
    require(relevant==list(expected),'compiled actual init-observe-one-launch-prepare-dispatch / old run-only order')
    require(successful['outcome']==('dispatch',13) if dispatch else successful['outcome']==('stop',7),'mode-specific compiled terminal outcome')
    require(sum(c['name']==('boot_sequence_launch' if dispatch else 'boot_sequence_run') for c in successful['calls'])==1,
            'exactly one successful entry transaction')
    require(successful['checkpoints']==([1,11,13] if dispatch else [1,7]),'compiled success publication checkpoint values')
    # Prove actual entry stores retained-only callback identities/context into
    # the descriptor passed to the one genuine sequence call.
    mem=successful['memory']; p=symbol('producer')
    for off,name in ((4,'boot_startup_gptm_direct_read'),(8,'boot_startup_gptm_direct_write')):
        require(int.from_bytes(bytes(mem[p+off+j] for j in range(4)),'little')==elf.value(name),'linked producer callback pointer dataflow '+name)
    def value(address):
        data=[mem.get(address+j) for j in range(4)]
        if any(v is None for v in data): raise Invalid('unknown descriptor bytes')
        return int.from_bytes(bytes(data),'little')
    require(value(p)==0x2000dbc0+384,'linked producer context points only to retained native context')
    descriptor=symbol('launch_parameters' if dispatch else 'parameters')
    require(value(descriptor)==0x2000dbc0 and value(descriptor+12)==p,
            'linked descriptor binds original continuity and producer')
    if dispatch:
        require(value(descriptor+40)==symbol('handoff') and value(descriptor+44)==symbol('observed'),
                'linked launch descriptor binds actual handoff/observation storage')
    # Bind failure may be optimized away. Final descriptor provenance, not a
    # nonexistent callee return category, is the required compiled obligation.
    failures=[('clock-readback-fail',2),('flash-timing-fail',3),
              ('image-first-read-fail',4),('image-second-read-fail',4)]
    failures += [('boot_handoff_target_init',8),('boot_handoff_target_observe',9),('boot_sequence_launch',10),('boot_startup_prepare',12)] if dispatch else [('boot_sequence_run',6)]
    if dispatch: failures += [(category,10) for category,_,_ in RESULT_NEGATIVES]
    witnesses=[successful,run('clock-authority-rcc')]
    require(witnesses[-1]['outcome']==successful['outcome'],'inactive RCC2 authority success path')
    for callee,state in failures:
        witness=run(callee); require(witness['outcome']==('stop',state),'compiled fail-stop control-flow checkpoint '+str(state))
        require(witness['checkpoints']==([1,11,12] if state==12 else [1,state]),'exact failure publication checkpoint path')
        protected=[c['name'] for c in witness['calls'] if c['name'] in ('boot_sequence_launch','boot_sequence_run','boot_startup_prepare','boot_startup_dispatch')]
        require(protected==(['boot_sequence_launch','boot_startup_prepare'] if state==12 else
                ['boot_sequence_launch'] if state==10 else ['boot_sequence_run'] if state==6 else []),
                'failure excludes all subsequent protected operations')
        witnesses.append(witness)
    # These controls run only when the caller explicitly executes this gate.
    # Each must fail its intended assertion, not merely throw some exception.
    controls=[]
    mutations=[('producer-context',None,'complete adapter/context bindings producer'),
        ('descriptor',None,'complete original descriptor/snapshot bindings parameters'),
        ('publication-byte',None,'exact original aggregate publication including poison remainder'),
        ('launch-retry',None,'no launch/run retry'),
        ('stop-barrier','clock-readback-fail','verified stop DMB-before-state instruction order'),
        ('unknown-mmio',None,'unmatched MMIO effect')]
    for object_name,extent,reason in [('parameters',40,'complete original descriptor/snapshot bindings parameters'),
            ('producer',12,'complete adapter/context bindings producer'),
            ('registers',12,'complete adapter/context bindings registers'),
            ('store',16,'complete adapter/context bindings store')]+(
            [('launch_parameters',48,'complete original descriptor/snapshot bindings launch_parameters')] if dispatch else []):
        mutations += [('binding:'+object_name+':'+str(off),None,reason) for off in range(0,extent,4)]
    if dispatch:
        mutations += [('target-init-pointer',None,'actual target init binding'),
                      ('observe-pointer',None,'actual observed storage'),
                      ('dispatch-pointer',None,'actual dispatch continuity argument')]
        mutations += [(category,category,'independent predicate control '+category) for category,_,_ in RESULT_NEGATIVES]
        mutations += [('runtime-pointer',None,'prepare consumes exact returned result.runtime and launch addresses'),
            ('launch-pointer',None,'prepare consumes exact returned result.runtime and launch addresses'),
            ('original-result-pointer',None,'AAPCS original aggregate stack provenance'),
            ('publication-order',None,'complete launch publication before ready11 before prepare'),
            ('launch-publication-byte',None,'exact original launch publication including poison remainder'),
            ('ready11-barrier',None,'fresh DMB before checkpoint publication'),
            ('ready13-barrier',None,'fresh DMB before checkpoint publication')]
    for mutation,category,reason in mutations:
        try:
            changed=run(category,mutation)
            if mutation in {name for name,_,_ in RESULT_NEGATIVES}:
                require(changed['outcome']==('stop',10),'independent predicate control '+mutation)
        except (Failure,Invalid) as exc:
            if str(exc)!=reason and not (reason=='unmatched MMIO effect' and str(exc).startswith(reason)):
                raise Invalid('entry mutation failed unrelated obligation '+mutation+': '+str(exc)) from exc
            controls.append({'mutation':mutation,'rejected_by':reason})
        else: raise Invalid('entry semantic mutation escaped '+mutation)
    for witness in witnesses: witness.pop('memory')
    report={'scope':'bounded static path stimuli; not HOST/runtime/device proof',
            'cfg':cfg,'paths':witnesses,'semantic_controls':controls}
    (session.out/('dispatch-entry-witness.json' if dispatch else 'decision-entry-witness.json')).write_text(json.dumps(report,indent=2)+'\n')
    return report
