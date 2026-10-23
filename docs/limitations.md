# Why this works and where proof stops

ARMv7-M is a stable core contract. The unsupported vendor layer is smaller than the whole toolchain. The project replaces that layer with auditable register definitions, explicit ownership and mapped validation. A passing model or link is evidence for its own boundary only.

## Find the contract

- [Stable core, obsolete vendor layer](#stable-core-obsolete-vendor-layer)
- [Evidence boundaries](#evidence-boundaries)


## Stable core, obsolete vendor layer

#### Core versus vendor: what actually decays

The intuition that a 2007 chip needs a 2007 toolchain conflates two layers:

| Layer | Defined by | Contents | Lifespan |
|---|---|---|---|
| Core | ARM | CPU, instruction set, exception model, debug architecture | Very long. A frozen architectural contract |
| Vendor | TI / Luminary | Peripheral inventory, IRQ numbering, SDK, IDE, flash utility | Dies with the product line |

The toolchain targets the core. `-mcpu=cortex-m3` in a current GCC produces the same Thumb-2
encoding it did in 2010, because ARMv7-M is a fixed specification. Only the vendor layer
decayed. The productive question is which layer is unsupported: the generic one or the vendor
one. The vendor layer is usually the smaller part to replace. Transcribe its
addresses and bit fields from the per-part datasheet. Datasheets are the real dependency, not SDKs.


## Evidence boundaries

| Evidence | Establishes | Does not establish |
|---|---|---|
| Native model | Policy, ownership and injected failure behavior | Physical register semantics or elapsed time |
| Final ELF/map/disassembly | Actual retained closure, addresses, vectors and static capacity | Runtime peaks or stable hardware clocks |
| QEMU | Core/startup behavior in the selected model | Full peripheral coverage or electrical behavior |
| Console marker | Startup text reception | Peripheral health or FTP transfer |
| PHY link | Negotiated readiness | Bidirectional packets, IP or FTP |
| READY and final 226 | Durable accepted update and transport completion boundaries | New-image execution or confirmation |

Compiler `.su` files are per-function frames, not whole-program stack bounds. Written watermarks can miss unwritten frames and sentinel collisions. Heap capacity is not peak use or fragmentation assurance. Crypto, flash and callback work units can remain synchronous even when input is bounded. Validate their physical watchdog and network service gaps separately.

A dated hardware or fixture outcome does not identify the currently installed board image. Revalidate image identity and ownership before any authorized operation. This manual records contracts and procedures, not a current hardware acceptance ledger.
