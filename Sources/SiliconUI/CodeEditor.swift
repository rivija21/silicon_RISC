import AppKit
import SiliconCore

/// The source editor: line numbers, syntax colours from the real lexer,
/// error underlines, and little stage badges showing which lines are in
/// the pipeline right now.
final class CodeEditor: NSView, NSTextViewDelegate {
    let scroll = NSScrollView()
    let textView: EditorTextView
    private let ruler: LineRuler
    var onChange: ((String) -> Void)?
    var onSelectLine: ((Int) -> Void)?
    private(set) var isAssembly = false
    private var suppress = false

    var errors: [Diagnostic] = [] { didSet { refreshMarks() } }
    var stages: [Int: [Int]] = [:] { didSet { if stages != oldValue { refreshMarks() } } }
    var selectedLine: Int? { didSet { if selectedLine != oldValue { refreshMarks() } } }

    override init(frame: NSRect) {
        let storage = NSTextStorage()
        let layout = NSLayoutManager()
        storage.addLayoutManager(layout)
        let container = NSTextContainer(size: NSSize(width: 1e6, height: 1e7))
        container.widthTracksTextView = false
        layout.addTextContainer(container)
        textView = EditorTextView(frame: frame, textContainer: container)
        ruler = LineRuler(frame: .zero)
        super.init(frame: frame)
        textView.isRichText = false
        textView.allowsUndo = true
        textView.isAutomaticQuoteSubstitutionEnabled = false
        textView.isAutomaticDashSubstitutionEnabled = false
        textView.isAutomaticTextReplacementEnabled = false
        textView.isAutomaticSpellingCorrectionEnabled = false
        textView.isContinuousSpellCheckingEnabled = false
        textView.smartInsertDeleteEnabled = false
        textView.font = Fonts.code
        textView.textColor = Theme.text
        textView.backgroundColor = Theme.panel
        textView.insertionPointColor = Theme.ink
        textView.selectedTextAttributes = [.backgroundColor: NSColor(hex: 0xD8E4F5)]
        textView.textContainerInset = NSSize(width: 6, height: 8)
        textView.isHorizontallyResizable = true
        textView.isVerticallyResizable = true
        textView.autoresizingMask = [.width]
        textView.maxSize = NSSize(width: 1e6, height: 1e7)
        textView.minSize = NSSize(width: 0, height: 0)
        textView.delegate = self
        textView.editor = self
        textView.typingAttributes = [.font: Fonts.code, .foregroundColor: Theme.text]
        let para = NSMutableParagraphStyle()
        para.lineSpacing = 2.5
        para.defaultTabInterval = 28
        para.tabStops = []
        textView.defaultParagraphStyle = para

        scroll.documentView = textView
        scroll.hasVerticalScroller = true
        scroll.hasHorizontalScroller = true
        scroll.autohidesScrollers = true
        scroll.drawsBackground = true
        scroll.backgroundColor = Theme.panel
        scroll.borderType = .noBorder
        ruler.editor = self
        ruler.scroll = scroll
        addSubview(scroll)
        addSubview(ruler)
        scroll.contentView.postsBoundsChangedNotifications = true
        NotificationCenter.default.addObserver(forName: NSView.boundsDidChangeNotification, object: scroll.contentView, queue: .main) { [weak self] _ in
            self?.ruler.needsDisplay = true
        }
    }

    required init?(coder: NSCoder) { fatalError() }

    override func layout() {
        super.layout()
        let gw: CGFloat = 58
        ruler.frame = NSRect(x: 0, y: 0, width: gw, height: bounds.height)
        scroll.frame = NSRect(x: gw, y: 0, width: bounds.width - gw, height: bounds.height)
        resetHorizontalScroll()
        ruler.needsDisplay = true
    }

    private func resetHorizontalScroll() {
        let o = scroll.contentView.bounds.origin
        if o.x != 0 {
            scroll.contentView.scroll(to: NSPoint(x: 0, y: o.y))
            scroll.reflectScrolledClipView(scroll.contentView)
        }
    }

    var text: String { textView.string }

    func setText(_ s: String, assembly: Bool) {
        isAssembly = assembly
        guard s != textView.string else {
            highlight()
            return
        }
        suppress = true
        textView.string = s
        textView.setSelectedRange(NSRange(location: 0, length: 0))
        suppress = false
        highlight()
        scroll.contentView.scroll(to: .zero)
        scroll.reflectScrolledClipView(scroll.contentView)
    }

    func textDidChange(_ notification: Notification) {
        guard !suppress else { return }
        highlight()
        onChange?(textView.string)
    }

    func textViewDidChangeSelection(_ notification: Notification) {
        guard !suppress, textView.window?.firstResponder === textView else { return }
        let loc = textView.selectedRange().location
        let lt = LineTable(textView.string)
        onSelectLine?(lt.line(of: loc))
    }

    // MARK: Colouring

    func highlight() {
        guard let storage = textView.textStorage else { return }
        let s = textView.string
        let full = NSRange(location: 0, length: (s as NSString).length)
        storage.beginEditing()
        storage.setAttributes([.font: Fonts.code, .foregroundColor: Theme.text, .paragraphStyle: textView.defaultParagraphStyle!], range: full)
        func color(_ r: SourceRange, _ c: NSColor, font: NSFont? = nil) {
            guard r.isValid, r.end <= full.length, r.length > 0 else { return }
            let nr = NSRange(location: r.start, length: r.length)
            storage.addAttribute(.foregroundColor, value: c, range: nr)
            if let font { storage.addAttribute(.font, value: font, range: nr) }
        }
        if isAssembly {
            let lines = LineTable(s)
            for (k, l) in s.components(separatedBy: "\n").enumerated() {
                let start = lines.lineStarts[k]
                let attr = highlightAsm(l)
                attr.enumerateAttribute(.foregroundColor, in: NSRange(location: 0, length: attr.length)) { v, r, _ in
                    if let c = v as? NSColor, r.location + r.length <= (l as NSString).length {
                        storage.addAttribute(.foregroundColor, value: c, range: NSRange(location: start + r.location, length: r.length))
                    }
                }
            }
        } else {
            let lx = Lexer.lex(s)
            for t in lx.tokens where t.fromMacro == nil {
                switch t.kind {
                case .keyword:
                    let isType = ["int", "char", "void", "unsigned", "signed", "long", "struct", "const"].contains(t.text)
                    color(t.range, isType ? Theme.type : Theme.keyword, font: Fonts.mono(12.5, .semibold))
                case .number, .character: color(t.range, Theme.number)
                case .string: color(t.range, Theme.string)
                case .punct: color(t.range, Theme.punct)
                case .identifier:
                    if lx.macros[t.text] != nil { color(t.range, Theme.directive) }
                    else if Library.byName[t.text] != nil || t.text == "printf" { color(t.range, Theme.ink) }
                default: break
                }
            }
            for tr in lx.trivia {
                color(tr.range, tr.kind == .comment ? Theme.comment : Theme.directive,
                      font: tr.kind == .comment ? NSFontManager.shared.convert(Fonts.code, toHaveTrait: .italicFontMask) : nil)
            }
        }
        // Error underlines.
        for d in errors where d.severity == .error && d.range.isValid && d.range.end <= full.length {
            var r = NSRange(location: d.range.start, length: max(1, d.range.length))
            if r.location + r.length > full.length { r.length = max(0, full.length - r.location) }
            storage.addAttributes([.underlineStyle: NSUnderlineStyle.thick.rawValue | NSUnderlineStyle.patternDot.rawValue,
                                   .underlineColor: Theme.crimson], range: r)
        }
        storage.endEditing()
    }

    private func refreshMarks() {
        highlight()
        textView.needsDisplay = true
        ruler.needsDisplay = true
    }

    // MARK: Geometry

    /// Rect of a 1-based line in text-view coordinates.
    func rect(ofLine line: Int) -> NSRect? {
        guard let lm = textView.layoutManager, let tc = textView.textContainer else { return nil }
        let lt = LineTable(textView.string)
        guard line >= 1, line <= lt.lineCount else { return nil }
        let start = lt.lineStarts[line - 1]
        let len = (textView.string as NSString).length
        if start >= len {
            // Empty last line.
            var r = lm.extraLineFragmentRect
            if r == .zero, len > 0 {
                let g = lm.glyphIndexForCharacter(at: len - 1)
                r = lm.lineFragmentRect(forGlyphAt: g, effectiveRange: nil)
                r.origin.y += r.height
            }
            r.origin.y += textView.textContainerInset.height
            return r
        }
        let g = lm.glyphIndexForCharacter(at: start)
        var r = lm.lineFragmentRect(forGlyphAt: g, effectiveRange: nil)
        _ = tc
        r.origin.y += textView.textContainerInset.height
        r.origin.x = 0
        r.size.width = max(textView.bounds.width, 2000)
        return r
    }

    func reveal(line: Int) {
        guard let r = rect(ofLine: line) else { return }
        let visible = scroll.contentView.documentVisibleRect
        if r.minY < visible.minY + 20 || r.maxY > visible.maxY - 20 {
            textView.scroll(NSPoint(x: 0, y: max(0, r.midY - visible.height / 2)))
        }
    }

    func select(range: SourceRange) {
        guard range.isValid, range.end <= (textView.string as NSString).length else { return }
        textView.setSelectedRange(NSRange(location: range.start, length: range.length))
        textView.scrollRangeToVisible(NSRange(location: range.start, length: range.length))
    }
}

final class EditorTextView: NSTextView {
    weak var editor: CodeEditor?

    override func drawBackground(in rect: NSRect) {
        super.drawBackground(in: rect)
        guard let e = editor else { return }
        // Lines with instructions in the pipeline get a soft band in the colour of the furthest stage.
        for (line, st) in e.stages {
            guard let r = e.rect(ofLine: line), r.intersects(rect), let k = st.max() else { continue }
            Theme.stageFill(k).alpha(0.55).setFill()
            NSRect(x: 0, y: r.minY, width: bounds.width, height: r.height).fill()
        }
        if let l = e.selectedLine, let r = e.rect(ofLine: l) {
            Theme.selection.setFill()
            NSRect(x: 0, y: r.minY, width: bounds.width, height: r.height).fill()
            Theme.selectionStrong.setFill()
            NSRect(x: 0, y: r.minY, width: 3, height: r.height).fill()
        }
        for d in e.errors where d.severity == .error && d.line > 0 {
            if let r = e.rect(ofLine: d.line) {
                Theme.errorFill.alpha(0.6).setFill()
                NSRect(x: 0, y: r.minY, width: bounds.width, height: r.height).fill()
            }
        }
    }

    override func insertNewline(_ sender: Any?) {
        // Keep the indentation of the current line, and indent after "{".
        let s = string as NSString
        let loc = selectedRange().location
        let lineRange = s.lineRange(for: NSRange(location: max(0, loc - 1 < 0 ? 0 : loc), length: 0))
        let lineText = s.substring(with: NSRange(location: lineRange.location, length: max(0, loc - lineRange.location)))
        var indent = String(lineText.prefix { $0 == " " || $0 == "\t" })
        if lineText.trimmingCharacters(in: .whitespaces).hasSuffix("{") { indent += "    " }
        super.insertNewline(sender)
        insertText(indent, replacementRange: selectedRange())
    }

    override func insertTab(_ sender: Any?) {
        insertText("    ", replacementRange: selectedRange())
    }
}

/// Line numbers, error marks and pipeline stage badges, beside the text.
final class LineRuler: NSView {
    weak var editor: CodeEditor?
    weak var scroll: NSScrollView?

    override var isFlipped: Bool { true }

    override func draw(_ rect: NSRect) {
        Theme.panelAlt.setFill()
        bounds.fill()
        line(NSPoint(x: bounds.maxX - 0.5, y: rect.minY), NSPoint(x: bounds.maxX - 0.5, y: rect.maxY), Theme.border)
        guard let e = editor, let scroll else { return }
        let tv = e.textView
        let lt = LineTable(tv.string)
        let visible = scroll.contentView.bounds
        let offset = -visible.origin.y
        let errLines = Set(e.errors.filter { $0.severity == .error }.map { $0.line })
        let warnLines = Set(e.errors.filter { $0.severity == .warning }.map { $0.line })
        let stageNames = ["F", "D", "X", "M", "W"]
        for n in 1...max(1, lt.lineCount) {
            guard let r = e.rect(ofLine: n) else { continue }
            if r.maxY < visible.minY - 20 { continue }
            if r.minY > visible.maxY + 20 { break }
            let y = r.minY + offset
            let isSel = e.selectedLine == n
            let s = "\(n)"
            let font = isSel ? Fonts.mono(10.5, .bold) : Fonts.mono(10.5)
            let sz = textSize(s, font)
            drawText(s, at: NSPoint(x: bounds.maxX - 8 - sz.width, y: y + (r.height - sz.height) / 2), font, isSel ? Theme.text : Theme.faint)
            if errLines.contains(n) {
                fillRounded(NSRect(x: 4, y: y + r.height / 2 - 4, width: 8, height: 8), 4, Theme.crimson)
            } else if warnLines.contains(n) {
                fillRounded(NSRect(x: 4, y: y + r.height / 2 - 4, width: 8, height: 8), 4, Theme.amber)
            }
            if let st = e.stages[n] {
                var x: CGFloat = 3
                for k in Set(st).sorted() {
                    let b = NSRect(x: x, y: y + (r.height - 12) / 2, width: 12, height: 12)
                    fillRounded(b, 3, Theme.stage[k])
                    drawText(stageNames[k], in: b, Fonts.mono(8.5, .bold), .white, align: .center)
                    x += 7
                }
            }
        }
    }

    override func mouseDown(with event: NSEvent) {
        guard let e = editor, let scroll else { return }
        let p = convert(event.locationInWindow, from: nil)
        let offset = -scroll.contentView.bounds.origin.y
        let lt = LineTable(e.textView.string)
        for n in 1...max(1, lt.lineCount) {
            if let r = e.rect(ofLine: n), p.y >= r.minY + offset, p.y < r.maxY + offset {
                e.onSelectLine?(n)
                return
            }
        }
    }
}
