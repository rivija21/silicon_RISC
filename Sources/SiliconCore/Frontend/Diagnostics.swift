import Foundation

/// A span of the source text, in UTF-16 code units (so it maps straight onto an NSRange).
public struct SourceRange: Hashable, Sendable {
    public var start: Int
    public var end: Int
    public init(_ start: Int, _ end: Int) {
        self.start = start
        self.end = end
    }
    public static let none = SourceRange(-1, -1)
    public var isValid: Bool { start >= 0 }
    public var length: Int { max(0, end - start) }
    public func union(_ o: SourceRange) -> SourceRange {
        guard isValid else { return o }
        guard o.isValid else { return self }
        return SourceRange(min(start, o.start), max(end, o.end))
    }
}

/// Which part of the tool chain produced a message.
public enum Stage: String, Sendable {
    case lexer = "Lexer"
    case preprocessor = "Preprocessor"
    case parser = "Parser"
    case semantic = "Semantic analysis"
    case ir = "IR"
    case codegen = "Code generation"
    case assembler = "Assembler"
    case linker = "Linker"
    case runtime = "Runtime"
}

public struct Diagnostic: Error, Sendable, CustomStringConvertible {
    public enum Severity: Sendable { case error, warning, note }
    public var severity: Severity
    public var stage: Stage
    public var message: String
    public var range: SourceRange
    public var line: Int

    public init(_ severity: Severity, _ stage: Stage, _ message: String, range: SourceRange = .none, line: Int = 0) {
        self.severity = severity
        self.stage = stage
        self.message = message
        self.range = range
        self.line = line
    }

    public var description: String {
        let kind = severity == .error ? "error" : severity == .warning ? "warning" : "note"
        return line > 0 ? "line \(line): \(kind): \(message)" : "\(kind): \(message)"
    }
}

/// Maps UTF-16 offsets to 1-based line numbers and back.
public struct LineTable: Sendable {
    public let lineStarts: [Int]
    public let length: Int

    public init(_ text: String) {
        var starts = [0]
        var i = 0
        for u in text.utf16 {
            i += 1
            if u == 10 { starts.append(i) }
        }
        lineStarts = starts
        length = i
    }

    public var lineCount: Int { lineStarts.count }

    public func line(of offset: Int) -> Int {
        var lo = 0, hi = lineStarts.count - 1
        while lo < hi {
            let mid = (lo + hi + 1) / 2
            if lineStarts[mid] <= offset { lo = mid } else { hi = mid - 1 }
        }
        return lo + 1
    }

    public func column(of offset: Int) -> Int { offset - lineStarts[line(of: offset) - 1] + 1 }

    /// The range of a whole line (without its newline).
    public func range(ofLine line: Int) -> SourceRange {
        guard line >= 1, line <= lineStarts.count else { return .none }
        let s = lineStarts[line - 1]
        let e = line < lineStarts.count ? lineStarts[line] - 1 : length
        return SourceRange(s, max(s, e))
    }
}
