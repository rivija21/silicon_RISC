import Foundation

/// The built-in C library. Its functions are known to the type checker
/// without any #include, and the linker pulls in their code on demand.
public enum Library {
    public struct Function: Sendable {
        public var name: String
        public var returnType: CType
        public var params: [CType]
        public var summary: String
    }

    public static let functions: [Function] = [
        .init(name: "print_int", returnType: .void, params: [.int], summary: "prints a signed integer (ecall 1)"),
        .init(name: "print_uint", returnType: .void, params: [.uint], summary: "prints an unsigned integer (ecall 36)"),
        .init(name: "print_hex", returnType: .void, params: [.uint], summary: "prints 0x… hexadecimal (ecall 34)"),
        .init(name: "print_char", returnType: .void, params: [.int], summary: "prints one character (ecall 11)"),
        .init(name: "print_str", returnType: .void, params: [.pointer(.char)], summary: "prints a string (ecall 4)"),
        .init(name: "putchar", returnType: .int, params: [.int], summary: "prints one character"),
        .init(name: "puts", returnType: .int, params: [.pointer(.char)], summary: "prints a string and a newline"),
        .init(name: "exit", returnType: .void, params: [.int], summary: "stops the program (ecall 93)"),
        .init(name: "malloc", returnType: .pointer(.void), params: [.uint], summary: "allocates heap memory with sbrk (ecall 9); never freed"),
        .init(name: "calloc", returnType: .pointer(.void), params: [.uint, .uint], summary: "allocates zeroed heap memory"),
        .init(name: "free", returnType: .void, params: [.pointer(.void)], summary: "does nothing (bump allocator)"),
        .init(name: "abs", returnType: .int, params: [.int], summary: "absolute value"),
        .init(name: "rand", returnType: .int, params: [], summary: "pseudo-random number 0…32767 (LCG)"),
        .init(name: "srand", returnType: .void, params: [.uint], summary: "seeds rand()"),
        .init(name: "strlen", returnType: .uint, params: [.pointer(.char)], summary: "length of a string"),
        .init(name: "strcmp", returnType: .int, params: [.pointer(.char), .pointer(.char)], summary: "compares two strings"),
        .init(name: "strcpy", returnType: .pointer(.char), params: [.pointer(.char), .pointer(.char)], summary: "copies a string"),
        .init(name: "memset", returnType: .pointer(.void), params: [.pointer(.void), .int, .uint], summary: "fills memory with a byte"),
        .init(name: "memcpy", returnType: .pointer(.void), params: [.pointer(.void), .pointer(.void), .uint], summary: "copies memory"),
    ]

    public static let byName: [String: Function] = Dictionary(uniqueKeysWithValues: functions.map { ($0.name, $0) })
}

/// A printf format string, split into literal text and conversions.
/// The compiler expands printf at compile time into calls to
/// print_str / print_int / print_char / print_hex / print_uint.
public enum PrintfPiece: Equatable, Sendable {
    case text([UInt8])
    case conversion(Character)
}

public enum PrintfFormat {
    public static let conversions: Set<Character> = ["d", "i", "u", "x", "X", "c", "s", "p"]

    /// Returns the pieces, or an error message.
    public struct FormatError: Error { public let message: String }

    public static func parse(_ bytes: [UInt8]) -> Result<[PrintfPiece], FormatError> {
        var pieces: [PrintfPiece] = []
        var text: [UInt8] = []
        var i = 0
        while i < bytes.count {
            let b = bytes[i]
            if b != UInt8(ascii: "%") {
                text.append(b)
                i += 1
                continue
            }
            guard i + 1 < bytes.count else { return .failure(FormatError(message: "the format string ends with a lone %")) }
            var j = i + 1
            // Accept and ignore flags/width/length such as %5d or %ld: we print without padding.
            while j < bytes.count, "-+ #0123456789.lhz".utf8.contains(bytes[j]) { j += 1 }
            guard j < bytes.count else { return .failure(FormatError(message: "the format string ends in the middle of a % conversion")) }
            let c = Character(UnicodeScalar(bytes[j]))
            if c == "%" {
                text.append(UInt8(ascii: "%"))
            } else if conversions.contains(c) {
                if !text.isEmpty { pieces.append(.text(text)); text = [] }
                pieces.append(.conversion(c))
            } else if c == "f" || c == "e" || c == "g" {
                return .failure(FormatError(message: "%\(c) needs floating point, which is not supported"))
            } else {
                return .failure(FormatError(message: "unknown printf conversion %\(c) (supported: %d %i %u %x %c %s %p %%)"))
            }
            i = j + 1
        }
        if !text.isEmpty { pieces.append(.text(text)) }
        return .success(pieces)
    }
}
