#include <stdio.h>

static volatile int src[32] = {
   1,  2,  3,  4,  5,  6,  7,  8,
   9, 10, 11, 12, 13, 14, 15, 16,
  17, 18, 19, 20, 21, 22, 23, 24,
  25, 26, 27, 28, 29, 30, 31, 32
};
static volatile int dst[32];
static volatile int result_sink;

#define READ(index)       src[(index)]
#define WRITE(index, val) dst[(index)] = (val)

__attribute__((noinline))
static int sequential_reads(void)
{
   return READ(0) ^ READ(1) ^ READ(2) ^ READ(3)
        ^ READ(4) ^ READ(5) ^ READ(6) ^ READ(7)
        ^ READ(8) ^ READ(9) ^ READ(10) ^ READ(11);
}

__attribute__((noinline))
static int random_reads(void)
{
   return READ(0) ^ READ(5) ^ READ(2) ^ READ(7)
        ^ READ(1) ^ READ(6) ^ READ(3) ^ READ(4)
        ^ READ(9) ^ READ(11) ^ READ(8) ^ READ(10);
}

__attribute__((noinline))
static int strided_reads(void)
{
   return READ(0) ^ READ(2) ^ READ(4) ^ READ(6)
        ^ READ(8) ^ READ(10) ^ READ(12) ^ READ(14)
        ^ READ(16) ^ READ(18) ^ READ(20) ^ READ(22);
}

__attribute__((noinline))
static void sequential_writes(void)
{
   WRITE(0, 1); WRITE(1, 1); WRITE(2, 1); WRITE(3, 1);
   WRITE(4, 1); WRITE(5, 1); WRITE(6, 1); WRITE(7, 1);
   WRITE(8, 1); WRITE(9, 1); WRITE(10, 1); WRITE(11, 1);
}

__attribute__((noinline))
static void random_writes(void)
{
   WRITE(0, 1); WRITE(5, 1); WRITE(2, 1); WRITE(7, 1);
   WRITE(1, 1); WRITE(6, 1); WRITE(3, 1); WRITE(4, 1);
   WRITE(9, 1); WRITE(11, 1); WRITE(8, 1); WRITE(10, 1);
}

__attribute__((noinline))
static void strided_writes(void)
{
   WRITE(0, 1); WRITE(2, 1); WRITE(4, 1); WRITE(6, 1);
   WRITE(8, 1); WRITE(10, 1); WRITE(12, 1); WRITE(14, 1);
   WRITE(16, 1); WRITE(18, 1); WRITE(20, 1); WRITE(22, 1);
}

int main(void)
{
   int value = sequential_reads() ^ random_reads() ^ strided_reads();
   sequential_writes();
   random_writes();
   strided_writes();
   result_sink = value;
   return 0;
}
