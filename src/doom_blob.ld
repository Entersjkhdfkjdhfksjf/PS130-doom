/* DOOM blob: flat image linked to run from the raw buffer, CACHED alias (0x00E80000).
   Phys 0x00E80000 is inside the raw buffer (base ~0x00E706A0). Running CACHED = code + zone
   memory go through the I/D caches = many times faster than the uncached alias (0x10E80000).
   The launcher loads DOOM.BIN here, cleans the D-cache (push code to RAM) and invalidates the
   I-cache (drop stale lines) before jumping, so the CPU fetches the freshly written code.
   All absolute addresses in the blob (function ptrs, header words) must match the execution
   alias, hence linking at 0x00E80000 and jumping there.
   doom_main (.text.entry) sits first = at the load address; launcher reads word0 = entry. */
ENTRY(doom_main)
SECTIONS {
  . = 0x00E80000;
  .text : {
    LONG(doom_main);     /* word0 = entry address; launcher reads + jumps it */
    LONG(_blob_end);     /* word1 = end of blob (incl bss) = start of the zone arena */
    *(.text.entry)
    *(.text*)
    *(.glue_7) *(.glue_7t)
    *(.rodata*)
  }
  . = ALIGN(8);
  .data : { *(.data*) *(.got) *(.got.plt) }
  . = ALIGN(8);
  __bss_start = .;
  .bss : { *(.bss*) *(COMMON) }
  . = ALIGN(8);
  __bss_end = .;
  _blob_end = .;
  /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) }
}
