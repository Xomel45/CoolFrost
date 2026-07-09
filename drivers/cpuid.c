#include <stdint.h>
#include "../drivers/screen.h"
#include "../libc/mem.h"
#include "../libc/stdio.h"

void cpuid_asm(uint32_t leaf, uint32_t* eax, uint32_t* ebx, uint32_t* ecx, uint32_t* edx) {
    uint32_t a = leaf, b, c, d;
    
    // GCC/Clang inline assembly syntax (AT&T syntax by default)
    __asm__ __volatile__ (
        "cpuid"
        : "=a" (a), "=b" (b), "=c" (c), "=d" (d) // Output operands: C variables receive register values
        : "a" (leaf)                             // Input operand: EAX is set to 'leaf' value
        : "memory"                               // Clobbered registers/memory
    );
    
    *eax = a;
    *ebx = b;
    *ecx = c;
    *edx = d;
}

void get_cpu_manufacturer(char *to) {
    uint32_t eax, ebx, edx, ecx;

    union {
        struct {
            uint32_t ebx;
            uint32_t edx;
            uint32_t ecx;
        } s;
        char str[13];
    } cpu = {0};

    cpuid_asm(0, &eax, &ebx, &ecx, &edx);

    // asm volatile(
    //     "cpuid"
    //     : "=b"(ebx), "=d"(edx), "=c"(ecx)
    //     : "a"(0)
    // );

    cpu.s.ebx = ebx;
    cpu.s.edx = edx;
    cpu.s.ecx = ecx;
    cpu.str[12] = 0;
    memcpy(to, cpu.str, 13);
}

uint8_t get_cpu_human_id(char *to) {
    uint32_t eax, ebx, ecx, edx;

    // Check the highest supported extended function
    cpuid_asm(0x80000000, &eax, &ebx, &ecx, &edx);
    unsigned int max_extended_leaf = eax;

    if (max_extended_leaf >= 0x80000004) {
        // Retrieve the brand string in three parts (leaves 0x80000002 to 0x80000004)
        
        // Part 1
        cpuid_asm(0x80000002, (uint32_t*)(to + 0), (uint32_t*)(to + 4), 
                  (uint32_t*)(to + 8), (uint32_t*)(to + 12));

        // Part 2
        cpuid_asm(0x80000003, (uint32_t*)(to + 16), (uint32_t*)(to + 20), 
                  (uint32_t*)(to + 24), (uint32_t*)(to + 28));
        
        // Part 3
        cpuid_asm(0x80000004, (uint32_t*)(to + 32), (uint32_t*)(to + 36), 
                  (uint32_t*)(to + 40), (uint32_t*)(to + 44));

        to[48] = '\0'; // Ensure null termination
        return 1;
    } else {
        return 0;
    }

    return 1;
}
