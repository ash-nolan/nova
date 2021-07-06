#include <sys/mman.h>
#include <stdio.h>

int
main(void)
{
    int const prot = PROT_READ | PROT_WRITE;
    int const flags = MAP_PRIVATE | MAP_ANONYMOUS;
    void* const mapped = mmap((void*)0xDEADBEEF, 4096, prot, flags, -1, 0);
    printf("prot %#x\n", prot); // 0x3
    printf("flags %#x\n", flags); // 0x22
    printf("%p\n", mapped); // 0xdeadb000
}
