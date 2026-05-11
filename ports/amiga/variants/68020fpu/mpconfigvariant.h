// 68020fpu variant: 68020 or 68030 CPU with 68881/68882 FPU coprocessor.
// Typical hardware: A2620/A2630 accelerator, A3000, accelerated A1200/A2000.

#define MICROPY_HW_MCU_NAME                 "68020/68881"

// Accelerator-card configurations typically ship with several MB of Fast
// RAM; bump the default heap to take advantage.
#define MICROPY_HEAP_SIZE                   (512 * 1024)
