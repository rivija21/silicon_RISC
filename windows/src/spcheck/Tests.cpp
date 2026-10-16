#include "Tests.h"

namespace spcheck {

/// Hand-written programs covering the C subset. Their expected output comes
/// from the native C compiler (see main.cpp), not from this compiler.
const std::vector<std::pair<std::string, std::string>>& handTests() {
    static const std::vector<std::pair<std::string, std::string>> tests = {
        // C leaves the order of argument evaluation unspecified (gcc on Windows goes right to left,
        // clang on the Mac left to right), so only one argument here has a side effect.
        {"printf_arg_eval", R"SRC(int calls = 0;
int noisy(int v) { calls++; printf("[noisy %d]", v); return v * 10; }
int main() {
    int a = noisy(1);
    printf("a=%d b=%d\n", a, noisy(2));
    printf("calls=%d\n", calls);
    printf("%s %c%c\n", "done", 'o', 'k');
    return calls;
})SRC"},
        {"recursion", R"SRC(int fact(int n) { return n <= 1 ? 1 : n * fact(n - 1); }
int fib(int n) { if (n < 2) return n; return fib(n - 1) + fib(n - 2); }
int ack(int m, int n) { if (m == 0) return n + 1; if (n == 0) return ack(m - 1, 1); return ack(m - 1, ack(m, n - 1)); }
int main() { printf("%d %d %d\n", fact(10), fib(20), ack(2, 3)); return 0; })SRC"},
        {"pointers", R"SRC(void swap(int *a, int *b) { int t = *a; *a = *b; *b = t; }
int main() {
    int x = 3, y = 4; swap(&x, &y);
    int a[5] = {1, 2, 3, 4, 5};
    int *p = a + 1, *q = &a[4];
    printf("%d %d %d %d %d\n", x, y, *p, q - p, *(q - 2));
    p += 2; *p = 99; p[-1] = 7;
    for (int i = 0; i < 5; i++) printf("%d ", a[i]);
    int **pp = &p; **pp = 42;
    printf("\n%d\n", a[3]);
    return 0;
})SRC"},
        {"arrays2d", R"SRC(int m[4][5];
int main() {
    int t[3][3] = {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}};
    for (int i = 0; i < 4; i++) for (int j = 0; j < 5; j++) m[i][j] = i * 10 + j;
    int s = 0;
    for (int i = 0; i < 3; i++) s += t[i][i] + t[i][2 - i];
    printf("%d %d %d\n", m[3][4], m[2][1], s);
    int (*row)[5] = m;
    row++;
    printf("%d %d\n", (*row)[3], row[1][2]);
    return sizeof(m) / sizeof(m[0]);
})SRC"},
        {"structs", R"SRC(struct point { int x; int y; };
struct rect { struct point lo; struct point hi; char name[8]; };
typedef struct rect Rect;
int area(Rect *r) { return (r->hi.x - r->lo.x) * (r->hi.y - r->lo.y); }
int main() {
    Rect r = {{1, 2}, {5, 7}, "box"};
    Rect copy;
    copy = r;
    copy.hi.x = 11;
    struct point pts[3] = {{1, 1}, {2, 4}, {3, 9}};
    int s = 0;
    for (int i = 0; i < 3; i++) s += pts[i].y;
    printf("%d %d %s %d %d\n", area(&r), area(&copy), copy.name, s, (int)sizeof(Rect));
    return 0;
})SRC"},
        {"linkedlist", R"SRC(struct node { int value; struct node *next; };
struct node *push(struct node *head, int v) {
    struct node *n = malloc(sizeof(struct node));
    n->value = v; n->next = head; return n;
}
struct node *reverse(struct node *h) {
    struct node *prev = NULL;
    while (h) { struct node *nx = h->next; h->next = prev; prev = h; h = nx; }
    return prev;
}
int main() {
    struct node *list = NULL;
    for (int i = 1; i <= 6; i++) list = push(list, i * i);
    list = reverse(list);
    int sum = 0;
    for (struct node *p = list; p != NULL; p = p->next) { printf("%d ", p->value); sum += p->value; }
    printf("\nsum=%d\n", sum);
    return 0;
})SRC"},
        {"strings", R"SRC(char greeting[] = "Hello";
char *names[3] = {"ada", "grace", "linus"};
int count(char *s, char c) { int n = 0; while (*s) { if (*s == c) n++; s++; } return n; }
void upper(char *s) { for (; *s; s++) if (*s >= 'a' && *s <= 'z') *s = *s - 32; }
int main() {
    char buf[32];
    strcpy(buf, names[1]);
    upper(buf);
    printf("%s %s %d %d\n", greeting, buf, strlen(names[2]), count("banana", 'a'));
    printf("%d %d %d\n", strcmp("abc", "abd") < 0, strcmp("b", "a") > 0, strcmp(names[0], "ada"));
    puts(greeting);
    putchar('!'); putchar('\n');
    return 0;
})SRC"},
        {"switch", R"SRC(int classify(int x) {
    int r = 0;
    switch (x) {
    case 0: r = 100;
    case 1: r += 10; break;
    case 2: case 3: r = 2; break;
    case -5: r = -5; break;
    default: r = 7;
    }
    return r;
}
int main() { for (int i = -6; i < 6; i++) printf("%d ", classify(i)); printf("\n"); return 0; })SRC"},
        {"unsigned_char", R"SRC(int main() {
    unsigned u = 4000000000u;
    int i = -1;
    char c = 200;
    char d = 'A';
    d += 100;
    unsigned big = u / 3 + (u >> 4);
    printf("%u %d %d %d %u\n", u, (int)u, c, d, big);
    printf("%d %d\n", i < 1u, (unsigned)i > 5);
    printf("%x %X %u\n", 48879, 3054, u % 7);
    unsigned m = 0xFFFFFFFF;
    printf("%u %d\n", m * m, -7 / 2 + -7 % 2);
    return 0;
})SRC"},
        {"control", R"SRC(int side = 0;
int bump(int v) { side++; return v; }
int main() {
    int n = 0;
    for (int i = 0; i < 20; i++) { if (i % 3 == 0) continue; if (i > 15) break; n += i; }
    int k = 10; do { k -= 3; } while (k > 0);
    int a = 0 && bump(1); int b = 1 || bump(1); int c = 1 && bump(5);
    int x = (n, k, 7);
    printf("%d %d %d %d %d %d %d\n", n, k, a, b, c, side, x);
    int t = 0;
    while (1) { t++; if (t * t > 50) break; }
    printf("%d %d\n", t, n > 50 ? n - 50 : 50 - n);
    return 0;
})SRC"},
        {"bigframe", R"SRC(int sum(int *a, int n) { int s = 0; for (int i = 0; i < n; i++) s += a[i]; return s; }
int main() {
    int big[1500];
    char pad[300];
    for (int i = 0; i < 1500; i++) big[i] = i % 13;
    for (int i = 0; i < 300; i++) pad[i] = i;
    big[1499] = 77;
    printf("%d %d %d\n", sum(big, 1500), big[1200], pad[299]);
    return 0;
})SRC"},
        {"manyargs", R"SRC(int f8(int a, int b, int c, int d, int e, int f, int g, int h) { return a - b + c * d - e + f * g - h; }
int pressure(int a, int b) {
    int c = a + b, d = a - b, e = a * b, f = a ^ b, g = a | b, h = a & b, i = c + d, j = e - f, k = g + h;
    int l = i * j, m = k - l, n = m + a, o = n - b, p = o * 3, q = p + c, r = q - d;
    return a + b + c + d + e + f + g + h + i + j + k + l + m + n + o + p + q + r + f8(c, d, e, f, g, h, i, j);
}
int main() { printf("%d %d\n", f8(1, 2, 3, 4, 5, 6, 7, 8), pressure(7, 3)); return 0; })SRC"},
        {"sorting", R"SRC(#define N 12
int data[N] = {9, -3, 7, 0, 22, 5, -8, 13, 1, 1, 64, -100};
void bubble(int *a, int n) {
    for (int i = 0; i < n - 1; i++)
        for (int j = 0; j < n - 1 - i; j++)
            if (a[j] > a[j + 1]) { int t = a[j]; a[j] = a[j + 1]; a[j + 1] = t; }
}
int bsearch(int *a, int n, int key) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) { int mid = (lo + hi) / 2; if (a[mid] == key) return mid; if (a[mid] < key) lo = mid + 1; else hi = mid - 1; }
    return -1;
}
int main() {
    bubble(data, N);
    for (int i = 0; i < N; i++) printf("%d ", data[i]);
    printf("\n%d %d\n", bsearch(data, N, 13), bsearch(data, N, 4));
    return 0;
})SRC"},
        {"matmul", R"SRC(#define N 8
int A[N][N], B[N][N], C[N][N];
int main() {
    for (int i = 0; i < N; i++) for (int j = 0; j < N; j++) { A[i][j] = i + j; B[i][j] = i - j; }
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) {
            int s = 0;
            for (int k = 0; k < N; k++) s += A[i][k] * B[k][j];
            C[i][j] = s;
        }
    int tr = 0;
    for (int i = 0; i < N; i++) tr += C[i][i];
    printf("%d %d %d\n", C[1][2], C[7][0], tr);
    return 0;
})SRC"},
        {"globals_init", R"SRC(int x = 5;
int *px = &x;
int arr[] = {10, 20, 30, 40};
int *mid = arr;
char letters[6] = {'a', 'b'};
struct pair { int a; char b; } pr = {7};
unsigned big = 0xDEADBEEF;
int main() {
    *px += 1;
    printf("%d %d %d %d %d %d %u\n", x, mid[2], letters[1], letters[3], pr.a, pr.b, big);
    return 0;
})SRC"},
        {"random_lib", R"SRC(int main() {
    srand(42);
    int h = 0;
    for (int i = 0; i < 20; i++) h = h * 31 + rand() % 100;
    int *buf = calloc(10, sizeof(int));
    for (int i = 0; i < 10; i++) buf[i] += i * abs(i - 5);
    char s[20];
    memset(s, 'z', 19); s[19] = 0;
    memcpy(s, "copy", 4);
    printf("%d %d %s\n", h, buf[9], s);
    return 0;
})SRC"},
        {"shadowing", R"SRC(int v = 1;
int f(int v) { { int v = 30; v++; } return v * 2; }
int main() {
    int r = f(5);
    int v = 2;
    { int v = 3; r += v; }
    for (int v = 0; v < 3; v++) r += v;
    printf("%d %d\n", r, v);
    return 0;
})SRC"},
        {"division_math", R"SRC(int isqrt(int n) { int x = n, y = (x + 1) / 2; while (y < x) { x = y; y = (x + n / x) / 2; } return x; }
int gcd(int a, int b) { while (b) { int t = a % b; a = b; b = t; } return a; }
int ipow(int b, int e) { int r = 1; while (e > 0) { if (e & 1) r *= b; b *= b; e >>= 1; } return r; }
int main() {
    int neg = -37;
    printf("%d %d %d %d\n", isqrt(1000000), gcd(1071, 462), ipow(3, 13), ipow(2, 31));
    printf("%d %d %d %d %d\n", neg / 4, neg % 4, neg / 8, neg >> 2, (neg * 16) / 16);
    unsigned un = 37;
    printf("%u %u %u\n", un / 4, un % 8, un * 1024u);
    return 0;
})SRC"},
    };
    return tests;
}

}  // namespace spcheck
