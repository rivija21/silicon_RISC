import Foundation

/// The start-up code and the small C library every program is linked with.
public enum Runtime {
    /// crt0 ("C runtime zero"): the real entry point. It sets up the stack,
    /// calls main, and passes main's return value to the exit system call.
    public static let crt0 = """
    # crt0.s — program entry point
        .text
        .globl _start
    _start:
        andi sp, sp, -16        # the ABI wants a 16-byte aligned stack
        li   a0, 0              # argc = 0
        li   a1, 0              # argv = NULL
        call main
        li   a7, 93             # exit(a0): a0 still holds main's return value
        ecall
    """

    /// System-call wrappers. Each one puts its service number in a7 and traps
    /// into the "operating system" (the simulator) with ecall.
    public static let syscalls = """
    # syscalls.s — thin wrappers around ecall (service numbers as in RARS)
        .text
        .globl print_int
    print_int:
        li   a7, 1
        ecall
        ret

        .globl print_uint
    print_uint:
        li   a7, 36
        ecall
        ret

        .globl print_hex
    print_hex:
        li   a7, 34
        ecall
        ret

        .globl print_char
    print_char:
        li   a7, 11
        ecall
        ret

        .globl print_str
    print_str:
        li   a7, 4
        ecall
        ret

        .globl exit
    exit:
        li   a7, 93
        ecall

        .globl __sbrk
    __sbrk:                     # a0 = bytes; returns the old end of the heap
        li   a7, 9
        ecall
        ret
    """

    /// Library members written in C and compiled by this very compiler.
    public static let cMembers: [(name: String, summary: String, source: String)] = [
        ("stdio.o", "putchar, puts and the %x helper for printf", """
        void __printx(unsigned v, int upper) {
            char buf[8];
            int n = 0;
            do {
                int d = v & 15;
                if (d < 10) buf[n] = '0' + d;
                else if (upper) buf[n] = 'A' + d - 10;
                else buf[n] = 'a' + d - 10;
                n++;
                v = v >> 4;
            } while (v != 0);
            while (n > 0) {
                n--;
                print_char(buf[n]);
            }
        }

        int putchar(int c) {
            print_char(c);
            return c;
        }

        int puts(char *s) {
            print_str(s);
            print_char('\\n');
            return 0;
        }
        """),
        ("malloc.o", "malloc, calloc and free (a bump allocator on top of sbrk)", """
        void *__sbrk(unsigned n);

        void *malloc(unsigned n) {
            return __sbrk((n + 7) & ~7u);
        }

        void *calloc(unsigned n, unsigned size) {
            unsigned total = (n * size + 7) & ~7u;
            char *p = __sbrk(total);
            if (p) {
                for (unsigned i = 0; i < total; i++) p[i] = 0;
            }
            return p;
        }

        void free(void *p) {
        }
        """),
        ("string.o", "strlen, strcmp, strcpy, memset, memcpy", """
        unsigned strlen(char *s) {
            unsigned n = 0;
            while (s[n]) n++;
            return n;
        }

        int strcmp(char *a, char *b) {
            while (1) {
                int x = *a & 255, y = *b & 255;
                if (x != y || x == 0) return x - y;
                a++;
                b++;
            }
        }

        char *strcpy(char *d, char *s) {
            char *r = d;
            while (1) {
                char c = *s;
                *d = c;
                if (c == 0) break;
                d++;
                s++;
            }
            return r;
        }

        void *memset(void *p, int c, unsigned n) {
            char *q = p;
            for (unsigned i = 0; i < n; i++) q[i] = c;
            return p;
        }

        void *memcpy(void *dst, void *src, unsigned n) {
            char *d = dst;
            char *s = src;
            for (unsigned i = 0; i < n; i++) d[i] = s[i];
            return dst;
        }
        """),
        ("rand.o", "rand and srand (a linear congruential generator)", """
        unsigned __rand_next = 1;

        int rand(void) {
            __rand_next = __rand_next * 1103515245u + 12345u;
            return (int)((__rand_next >> 16) & 32767u);
        }

        void srand(unsigned seed) {
            __rand_next = seed;
        }
        """),
        ("abs.o", "abs", """
        int abs(int x) {
            return x < 0 ? -x : x;
        }
        """),
    ]

    public final class Library: @unchecked Sendable {
        public let crt0: ObjectFile
        public let members: [ObjectFile]
        public let compilations: [String: Compilation]
        init(crt0: ObjectFile, members: [ObjectFile], compilations: [String: Compilation]) {
            self.crt0 = crt0
            self.members = members
            self.compilations = compilations
        }
    }

    private static let lock = NSLock()
    nonisolated(unsafe) private static var cached: Library?

    /// The library, built once: assembly members assembled, C members compiled with every optimisation on.
    public static var library: Library {
        lock.lock()
        defer { lock.unlock() }
        if let c = cached { return c }
        let crt = Assembler.assemble(text: crt0, name: "crt0.o")
        crt.summary = "start-up code: sets up the stack and calls main"
        let sys = Assembler.assemble(text: syscalls, name: "syscalls.o")
        sys.summary = "print_int, print_char, print_str, exit, sbrk (ecall wrappers)"
        sys.isLibrary = true
        var members = [sys]
        var comps: [String: Compilation] = [:]
        for m in cMembers {
            let c = Compilation.compileObject(m.source, name: m.name, options: CompileOptions(passes: Set(PassID.allCases), isLibrary: true))
            if let o = c.object {
                o.isLibrary = true
                o.summary = m.summary
                members.append(o)
            }
            comps[m.name] = c
        }
        let lib = Library(crt0: crt, members: members, compilations: comps)
        cached = lib
        return lib
    }
}
