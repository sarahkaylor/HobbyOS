ENTRY(_start)

SECTIONS
{
    . = 0x44000000;
    
    .text : {
        *(.text._start)
        *(.text*)
    }
    
    .rodata : {
        *(.rodata*)
    }
    
    .data : {
        *(.data*)
    }
    
    .bss : {
        *(.bss*)
        *(COMMON)
    }
}
