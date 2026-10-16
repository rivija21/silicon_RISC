#include "Station.h"

namespace sp {

const char* stationTitle(Station s) {
    switch (s) {
    case Station::Source: return "Source";
    case Station::Tokens: return "Tokens";
    case Station::Tree: return "Syntax tree";
    case Station::Symbols: return "Symbols";
    case Station::IR: return "IR & optimiser";
    case Station::Asm: return "Assembly";
    case Station::Machine: return "Machine code";
    case Station::Link: return "Linking";
    case Station::Pipeline: return "Pipeline";
    case Station::Memory: return "Memory";
    }
    return "?";
}

const char* stationLayer(Station s) {
    switch (s) {
    case Station::Source: case Station::Tokens: case Station::Tree: case Station::Symbols: return "front end";
    case Station::IR: return "middle end";
    case Station::Asm: case Station::Machine: case Station::Link: return "back end";
    case Station::Pipeline: return "microarchitecture";
    case Station::Memory: return "memory hierarchy";
    }
    return "";
}

const std::vector<Example>& Examples::all() {
    static const std::vector<Example> examples = {
        {"Sum of an array", "A first look: a loop, an array, and the pipeline forwarding values between instructions.",
         R"SRC(// Sum of an array: a loop that the pipeline forwards values through.
int data[8] = {3, 1, 4, 1, 5, 9, 2, 6};

int sum(int *a, int n) {
    int total = 0;
    for (int i = 0; i < n; i++) {
        total += a[i];
    }
    return total;
}

int main() {
    int s = sum(data, 8);
    printf("sum = %d\n", s);
    return 0;
})SRC",
         false, Station::Pipeline},
        {"Fibonacci (recursion)", "Each call builds a stack frame: return address, saved registers, arguments in a0.",
         R"SRC(// Recursive Fibonacci: watch the stack frames and the calling convention.
int fib(int n) {
    if (n < 2)
        return n;
    return fib(n - 1) + fib(n - 2);
}

int main() {
    for (int i = 0; i <= 10; i++)
        printf("fib(%d) = %d\n", i, fib(i));
    return 0;
})SRC",
         false, Station::Asm},
        {"Matrix: row vs column order", "The same arithmetic, two memory access orders. Compare the D-cache miss rates.",
         R"SRC(// Row-major vs column-major traversal of the same matrix.
// C stores a[i][j] row by row, so walking along a row touches
// consecutive addresses (good spatial locality); walking down a
// column jumps N*4 bytes every time.
#define N 32
int a[N][N];

int sum_rows() {
    int s = 0;
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++)
            s += a[i][j];
    return s;
}

int sum_columns() {
    int s = 0;
    for (int j = 0; j < N; j++)
        for (int i = 0; i < N; i++)
            s += a[i][j];
    return s;
}

int main() {
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++)
            a[i][j] = i + j;
    int r = sum_rows();
    int c = sum_columns();
    printf("rows %d, columns %d\n", r, c);
    return 0;
})SRC",
         false, Station::Memory},
        {"Bubble sort", "Lots of data-dependent branches: try the branch predictors and see CPI change.",
         R"SRC(// Bubble sort: branches everywhere. Switch the predictor in the
// pipeline view and watch the number of flushed instructions.
#define N 16
int v[N] = {42, 7, 19, 3, 88, 1, 56, 23, 9, 61, 14, 70, 5, 33, 2, 47};

void sort(int *a, int n) {
    for (int i = 0; i < n - 1; i++) {
        for (int j = 0; j < n - 1 - i; j++) {
            if (a[j] > a[j + 1]) {
                int t = a[j];
                a[j] = a[j + 1];
                a[j + 1] = t;
            }
        }
    }
}

int main() {
    sort(v, N);
    for (int i = 0; i < N; i++)
        printf("%d ", v[i]);
    printf("\n");
    return 0;
})SRC",
         false, Station::Pipeline},
        {"Linked list on the heap", "malloc, structs and pointers: see struct layout, the heap, and pointer chasing.",
         R"SRC(// A linked list built with malloc. Each node is a struct on the heap.
struct node {
    int value;
    struct node *next;
};

struct node *push(struct node *head, int value) {
    struct node *n = malloc(sizeof(struct node));
    n->value = value;
    n->next = head;
    return n;
}

int main() {
    struct node *list = NULL;
    for (int i = 1; i <= 5; i++)
        list = push(list, i * 10);
    for (struct node *p = list; p != NULL; p = p->next)
        printf("%d -> ", p->value);
    printf("NULL\n");
    return 0;
})SRC",
         false, Station::Symbols},
        {"Hazards up close", "A load followed by its use, and a chain of dependent instructions.",
         R"SRC(// Data hazards. In the pipeline view, turn forwarding off and on.
int x[4] = {10, 20, 30, 40};

int main() {
    int a = x[0];        // load ...
    int b = a + 1;       // ... used straight away: load-use stall
    int c = b * 2;       // ALU result forwarded from EX/MEM
    int d = c - a;       // forwarded from MEM/WB
    x[1] = d;
    printf("%d %d %d %d\n", a, b, c, d);
    return 0;
})SRC",
         false, Station::Pipeline},
        {"What the optimiser does", "Constant folding, strength reduction (×8 → <<3), dead code. Toggle passes one by one.",
         R"SRC(// Step through the optimiser passes in the IR view.
int scale(int x) {
    int unused = x * 1000;      // never used: dead code
    int k = 4 * 2;              // constant folding: k = 8
    int y = x * k;              // strength reduction: x << 3
    int z = y / 4;              // signed /4 needs a rounding fix-up
    if (k > 100)                // always false: branch removed
        return -1;
    return z + 0;               // + 0 disappears
}

int main() {
    printf("%d\n", scale(21));
    return 0;
})SRC",
         false, Station::IR},
        {"Strings and printf", "String literals live in .data; printf is expanded into simple print calls.",
         R"SRC(// Strings are arrays of char ending in 0.
char greeting[] = "Hello";

void shout(char *s) {
    for (int i = 0; s[i] != 0; i++)
        if (s[i] >= 'a' && s[i] <= 'z')
            s[i] = s[i] - 32;
}

int main() {
    char name[16];
    strcpy(name, "risc-v");
    shout(name);
    printf("%s, %s! (%d letters, %x in hex)\n", greeting, name, strlen(name), 255);
    return 0;
})SRC",
         false, Station::Link},
        {"Register pressure", "Many values alive at once. Limit the registers in the Assembly view to force spills.",
         R"SRC(// Lots of values alive at the same time. Set "Registers" to 3
// in the assembly view and watch the allocator spill to the stack.
int mix(int a, int b) {
    int c = a + b, d = a - b, e = a * b, f = a ^ b;
    int g = c * d, h = e + f, i = g - h, j = c + e;
    return a + b + c + d + e + f + g + h + i + j;
}

int main() {
    printf("%d\n", mix(7, 3));
    return 0;
})SRC",
         false, Station::Asm},
        {"Cache: stride experiment", "Touch every 1st, 4th, then 16th word: spatial locality and block size.",
         R"SRC(// Read an array with different strides. With 16-byte blocks, a
// stride of 1 word gets 3 hits per miss; a stride of 4 words
// misses every time. Try changing the block size.
#define SIZE 1024
int big[SIZE];

int walk(int stride) {
    int s = 0;
    for (int i = 0; i < SIZE; i += stride)
        s += big[i];
    return s;
}

int main() {
    for (int i = 0; i < SIZE; i++)
        big[i] = i;
    printf("%d %d %d\n", walk(1), walk(4), walk(16));
    return 0;
})SRC",
         false, Station::Memory},
        {"Hand-written assembly", "Skip the compiler: write RISC-V assembly yourself, as in P&H chapter 2.",
         R"SRC(# Sum 1..10 in RISC-V assembly (Patterson & Hennessy style).
# Pseudo-instructions like li, la, mv and call are expanded
# by the assembler; see the machine-code view.
    .data
msg:    .string "sum = "

    .text
    .globl main
main:
    addi sp, sp, -16
    sw   ra, 12(sp)     # save the return address
    sw   s1, 8(sp)      # s1 is callee-saved: we must restore it

    li   t0, 0          # sum
    li   t1, 1          # i
    li   t2, 10         # limit
loop:
    add  t0, t0, t1     # sum += i
    addi t1, t1, 1      # i++
    ble  t1, t2, loop   # while (i <= 10)

    mv   s1, t0
    la   a0, msg
    call print_str
    mv   a0, s1
    call print_int
    li   a0, 10
    call print_char

    li   a0, 0          # return 0
    lw   s1, 8(sp)
    lw   ra, 12(sp)
    addi sp, sp, 16
    ret)SRC",
         true, Station::Machine},
    };
    return examples;
}

}  // namespace sp
