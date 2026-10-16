import AppKit
import SiliconCore

protocol StationView: NSView {
    func reload(_ model: AppModel, _ event: ModelEvent)
}

/// Shown when the current source cannot reach this stage.
final class EmptyNote: FlippedView {
    var title = ""
    var message = ""
    override func draw(_ dirtyRect: NSRect) {
        Theme.panel.setFill()
        bounds.fill()
        let r = NSRect(x: 30, y: bounds.midY - 40, width: bounds.width - 60, height: 80)
        drawText(title, in: NSRect(x: r.minX, y: r.minY, width: r.width, height: 24), Fonts.heading, Theme.heading, align: .center)
        let p = NSMutableParagraphStyle()
        p.alignment = .center
        NSAttributedString(string: message, attributes: [.font: Fonts.body, .foregroundColor: Theme.secondary, .paragraphStyle: p])
            .draw(with: NSRect(x: r.minX, y: r.minY + 30, width: r.width, height: 50), options: [.usesLineFragmentOrigin])
    }
}

func tokenColor(_ k: TokenKind) -> (NSColor, NSColor) {
    switch k {
    case .keyword: return (Theme.keyword, NSColor(hex: 0xF3E4F5))
    case .identifier: return (Theme.text, NSColor(hex: 0xEEF0F5))
    case .number, .character: return (Theme.number, NSColor(hex: 0xE1EDF8))
    case .string: return (Theme.string, NSColor(hex: 0xF9E6DB))
    case .punct: return (Theme.punct, NSColor(hex: 0xF1EDE4))
    case .eof: return (Theme.secondary, NSColor(hex: 0xEEEEEE))
    }
}

// MARK: - Tokens

final class TokenCanvas: FlippedView {
    struct Chip {
        var rect: NSRect
        var token: Token
        var label: String
    }
    var chips: [Chip] = []
    var lineLabels: [(Int, CGFloat)] = []
    var height: CGFloat = 100
    var selectedLine: Int?
    var hovered: Int? { didSet { if hovered != oldValue { needsDisplay = true } } }
    var onClick: ((Token) -> Void)?
    var tokens: [Token] = []
    var summary = ""

    override init(frame: NSRect) {
        super.init(frame: frame)
        addTrackingArea(NSTrackingArea(rect: .zero, options: [.mouseMoved, .mouseEnteredAndExited, .activeInKeyWindow, .inVisibleRect], owner: self))
    }
    required init?(coder: NSCoder) { fatalError() }

    override var intrinsicContentSize: NSSize { NSSize(width: 300, height: height) }

    func layoutChips(width: CGFloat) {
        chips = []
        lineLabels = []
        var x: CGFloat = 54, y: CGFloat = 54
        var curLine = -1
        let font = Fonts.mono(11.5)
        for t in tokens where t.kind != .eof {
            var label = t.text
            if t.kind == .string { label = "\"" + CodeGen.escape(t.bytes) + "\"" }
            if label.count > 40 { label = String(label.prefix(38)) + "…\"" }
            let w = textSize(label, font).width + 12
            if t.line != curLine {
                if curLine != -1 { y += 30 }
                curLine = t.line
                x = 54
                lineLabels.append((t.line, y))
            } else if x + w > width - 12 {
                x = 54
                y += 30
            }
            chips.append(Chip(rect: NSRect(x: x, y: y, width: w, height: 22), token: t, label: label))
            x += w + 5
        }
        height = y + 50
        invalidateIntrinsicContentSize()
    }

    override func draw(_ dirtyRect: NSRect) {
        Theme.panel.setFill()
        dirtyRect.fill()
        drawText(summary, at: NSPoint(x: 14, y: 10), Fonts.small, Theme.secondary)
        // Legend.
        var lx: CGFloat = 14
        for (name, k) in [("keyword", TokenKind.keyword), ("identifier", .identifier), ("number / char", .number), ("string", .string), ("punctuation", .punct)] {
            let (fg, bg) = tokenColor(k)
            let r = drawPill(name, at: NSPoint(x: lx, y: 28), font: Fonts.tiny, fill: bg, text: fg)
            lx = r.maxX + 6
        }
        _ = drawPill("from a #define", at: NSPoint(x: lx, y: 28), font: Fonts.tiny, fill: NSColor(hex: 0xFFF1C9), text: Theme.directive)
        for (l, y) in lineLabels where y < dirtyRect.maxY + 30 && y > dirtyRect.minY - 30 {
            let sel = l == selectedLine
            drawText("\(l)", in: NSRect(x: 8, y: y, width: 36, height: 22), Fonts.mono(10.5, sel ? .bold : .regular), sel ? Theme.heading : Theme.faint, align: .right)
        }
        let font = Fonts.mono(11.5)
        for (k, c) in chips.enumerated() where c.rect.intersects(dirtyRect.insetBy(dx: -4, dy: -4)) {
            var (fg, bg) = tokenColor(c.token.kind)
            if c.token.fromMacro != nil { bg = NSColor(hex: 0xFFF1C9) }
            fillRounded(c.rect, 5, bg)
            let sel = c.token.line == selectedLine
            if sel || hovered == k { strokeRounded(c.rect, 5, sel ? Theme.selectionStrong.blended(withFraction: 0.3, of: Theme.amber)! : Theme.borderStrong, 1.5) }
            if c.token.kind == .keyword { fg = Theme.keyword }
            drawText(c.label, in: c.rect.insetBy(dx: 6, dy: 0), c.token.kind == .keyword ? Fonts.mono(11.5, .semibold) : font, fg)
        }
        if let h = hovered, h < chips.count {
            let c = chips[h]
            var info = "\(c.token.kind.rawValue)  ·  line \(c.token.line), column \(c.token.column)"
            if c.token.kind == .number || c.token.kind == .character { info += "  ·  value \(c.token.value)" }
            if c.token.kind == .string { info += "  ·  \(c.token.bytes.count) bytes + terminating 0" }
            if let m = c.token.fromMacro { info += "  ·  expanded from #define \(m)" }
            let sz = textSize(info, Fonts.small)
            var r = NSRect(x: c.rect.minX, y: c.rect.maxY + 3, width: sz.width + 14, height: sz.height + 6)
            if r.maxX > bounds.width - 6 { r.origin.x = bounds.width - 6 - r.width }
            drawCard(r, radius: 5, fill: NSColor(hex: 0x2B2A33))
            drawText(info, at: NSPoint(x: r.minX + 7, y: r.minY + 3), Fonts.small, .white)
        }
    }

    override func mouseMoved(with event: NSEvent) {
        let p = convert(event.locationInWindow, from: nil)
        hovered = chips.firstIndex { $0.rect.contains(p) }
    }
    override func mouseExited(with event: NSEvent) { hovered = nil }
    override func mouseDown(with event: NSEvent) {
        let p = convert(event.locationInWindow, from: nil)
        if let c = chips.first(where: { $0.rect.contains(p) }) { onClick?(c.token) }
    }
}

final class TokensView: NSView, StationView {
    let canvas = TokenCanvas(frame: .zero)
    lazy var panel = CanvasPanel(canvas)
    let empty = EmptyNote()
    weak var model: AppModel?
    private var lastWidth: CGFloat = 0

    override init(frame: NSRect) {
        super.init(frame: frame)
        addSubview(panel)
        addSubview(empty)
        canvas.onClick = { [weak self] t in
            self?.model?.selection = Selection(line: t.line, range: t.range, origin: .tokens)
        }
    }
    required init?(coder: NSCoder) { fatalError() }

    override func layout() {
        super.layout()
        panel.frame = bounds
        empty.frame = bounds
        if abs(bounds.width - lastWidth) > 1 {
            lastWidth = bounds.width
            canvas.layoutChips(width: bounds.width - 16)
            panel.needsLayout = true
        }
    }

    func reload(_ model: AppModel, _ event: ModelEvent) {
        self.model = model
        let c = model.compilation
        guard let lex = c?.lex, model.isAssembly == false else {
            empty.isHidden = false
            empty.title = "No tokens"
            empty.message = model.isAssembly ? "Hand-written assembly skips the compiler front end." : "The lexer could not run."
            empty.needsDisplay = true
            return
        }
        empty.isHidden = true
        if event == .compiled || canvas.tokens.isEmpty {
            canvas.tokens = lex.tokens
            let comments = lex.trivia.filter { $0.kind == .comment }.count
            let directives = lex.trivia.filter { $0.kind == .directive }.count
            let macroToks = lex.tokens.filter { $0.fromMacro != nil }.count
            canvas.summary = "\(lex.tokens.count - 1) tokens · \(comments) comment\(comments == 1 ? "" : "s") and \(directives) preprocessor lines thrown away · \(macroToks) tokens came from #define"
            canvas.layoutChips(width: bounds.width - 16)
            panel.needsLayout = true
        }
        canvas.selectedLine = model.selection.line
        canvas.needsDisplay = true
        if model.selection.origin != .tokens, let l = model.selection.line,
           let c = canvas.chips.first(where: { $0.token.line == l }) {
            canvas.scrollToVisible(c.rect.insetBy(dx: 0, dy: -40))
        }
    }
}

// MARK: - Syntax tree

struct TreeNode {
    var depth: Int
    var role: String
    var label: String
    var detail: String
    var category: Int   // 0 decl, 1 stmt, 2 expr, 3 type/struct
    var node: Int
    var range: SourceRange
    var line: Int
    var hasChildren: Bool
    var isLast: Bool
    var ancestorsLast: [Bool]
}

enum TreeBuilder {
    static func build(_ p: Program) -> [TreeNode] {
        var out: [TreeNode] = []
        func add(_ depth: Int, _ role: String, _ label: String, _ detail: String, _ cat: Int, _ id: Int, _ r: SourceRange, _ line: Int, _ kids: Int, _ last: Bool, _ anc: [Bool]) {
            out.append(TreeNode(depth: depth, role: role, label: label, detail: detail, category: cat, node: id, range: r, line: line,
                                hasChildren: kids > 0, isLast: last, ancestorsLast: anc))
        }
        func expr(_ e: Expr, _ d: Int, _ role: String, _ last: Bool, _ anc: [Bool]) {
            let (label, kids) = describe(e)
            add(d, role, label, ": " + e.type.description, 2, e.id, e.range, e.line, kids.count, last, anc)
            for (k, (r, c)) in kids.enumerated() { expr(c, d + 1, r, k == kids.count - 1, anc + [last]) }
        }
        func stmt(_ s: Stmt, _ d: Int, _ role: String, _ last: Bool, _ anc: [Bool]) {
            var kids: [(String, Any)] = []
            var label = ""
            switch s.kind {
            case let .block(items): label = "Block"; kids = items.map { ("", $0) }
            case let .decl(ds): label = "Declaration"; kids = ds.map { ("", $0) }
            case let .expr(e): label = "Expression statement"; kids = [("", e)]
            case let .ifElse(c, a, b):
                label = "If"
                kids = [("cond", c), ("then", a)]
                if let b { kids.append(("else", b)) }
            case let .whileLoop(c, b): label = "While"; kids = [("cond", c), ("body", b)]
            case let .doWhile(b, c): label = "Do-while"; kids = [("body", b), ("cond", c)]
            case let .forLoop(i, c, st, b):
                label = "For"
                if let i { kids.append(("init", i)) }
                if let c { kids.append(("cond", c)) }
                if let st { kids.append(("step", st)) }
                kids.append(("body", b))
            case let .switchCase(v, cases):
                label = "Switch"
                kids = [("value", v)] + cases.map { ("", $0) }
            case let .returnStmt(e): label = "Return"; if let e { kids = [("", e)] }
            case .breakStmt: label = "Break"
            case .continueStmt: label = "Continue"
            case .empty: label = "Empty"
            }
            add(d, role, label, "", 1, s.id, s.range, s.line, kids.count, last, anc)
            for (k, (r, c)) in kids.enumerated() {
                let l = k == kids.count - 1
                if let e = c as? Expr { expr(e, d + 1, r, l, anc + [last]) }
                else if let st = c as? Stmt { stmt(st, d + 1, r, l, anc + [last]) }
                else if let v = c as? VarDecl { varDecl(v, d + 1, r, l, anc + [last]) }
                else if let sc = c as? SwitchCase {
                    let lab = sc.value == nil ? "Default" : "Case \(sc.constant)"
                    add(d + 1, "", lab, "", 1, -sc.line, sc.range, sc.line, sc.body.count, l, anc + [last])
                    for (j, b) in sc.body.enumerated() { stmt(b, d + 2, "", j == sc.body.count - 1, anc + [last, l]) }
                }
            }
        }
        func varDecl(_ v: VarDecl, _ d: Int, _ role: String, _ last: Bool, _ anc: [Bool]) {
            var kids: [Expr] = []
            if case let .expr(e)? = v.initializer { kids = [e] }
            let isList: Bool = { if case .list? = v.initializer { return true }; return false }()
            add(d, role, "Variable \(v.name)", ": " + v.type.description + (isList ? " = {…}" : ""), 0, v.id, v.range, v.line, kids.count, last, anc)
            for e in kids { expr(e, d + 1, "init", true, anc + [last]) }
        }
        let items = p.items
        for (k, item) in items.enumerated() {
            let last = k == items.count - 1
            switch item {
            case let .function(f):
                let sig = "(" + f.params.map { $0.type.description }.joined(separator: ", ") + ") → " + f.returnType.description
                add(0, "", "Function \(f.name)", sig, 0, f.id, f.range, f.line, f.params.count + (f.body == nil ? 0 : 1), last, [])
                for (j, pd) in f.params.enumerated() {
                    add(1, "param", pd.name.isEmpty ? "Parameter" : "Parameter \(pd.name)", ": " + pd.type.description, 0, pd.id, pd.range, pd.line, 0,
                        j == f.params.count - 1 && f.body == nil, [last])
                }
                if let b = f.body { stmt(b, 1, "body", true, [last]) }
            case let .globals(gs):
                for (j, g) in gs.enumerated() { varDecl(g, 0, "global", last && j == gs.count - 1, []) }
            case let .structDef(s, r, l):
                add(0, "", "Struct \(s.name)", "\(s.size) bytes", 3, -1000 - s.id, r, l, s.fields.count, last, [])
                for (j, fld) in s.fields.enumerated() {
                    add(1, "field", fld.name, ": \(fld.type)  @ offset \(fld.offset)", 3, -2000 - s.id * 100 - j, r, l, 0, j == s.fields.count - 1, [last])
                }
            }
        }
        return out
    }

    static func describe(_ e: Expr) -> (String, [(String, Expr)]) {
        switch e.kind {
        case let .number(v): return ("Number \(v)", [])
        case let .string(b): return ("String \"\(CodeGen.escape(b).prefix(30))\"", [])
        case let .ident(n): return ("Name \(n)", [])
        case let .binary(op, a, b): return ("Binary \(op.rawValue)", [("", a), ("", b)])
        case let .unary(op, a):
            let names: [UnaryOp: String] = [.neg: "Negate -", .plus: "Plus +", .not: "Not !", .bitNot: "Complement ~", .deref: "Dereference *", .addr: "Address-of &"]
            return (names[op] ?? op.rawValue, [("", a)])
        case let .assign(op, a, b): return ("Assign \(op.map { $0.rawValue } ?? "")=", [("target", a), ("value", b)])
        case let .incDec(pre, inc, a): return ((pre ? "Prefix " : "Postfix ") + (inc ? "++" : "--"), [("", a)])
        case let .call(n, args): return ("Call \(n)", args.enumerated().map { ("arg \($0.offset + 1)", $0.element) })
        case let .index(a, i): return ("Index [ ]", [("array", a), ("index", i)])
        case let .member(a, f, arrow): return ("Member \(arrow ? "->" : ".")\(f)", [("", a)])
        case let .ternary(c, a, b): return ("Conditional ?:", [("cond", c), ("then", a), ("else", b)])
        case let .sizeofType(t): return ("sizeof(\(t))", [])
        case let .sizeofExpr(a): return ("sizeof", [("", a)])
        case let .cast(t, a): return ("Cast (\(t))", [("", a)])
        case let .comma(a, b): return ("Comma ,", [("", a), ("", b)])
        }
    }
}

final class TreeCanvas: FlippedView {
    var nodes: [TreeNode] = []
    var collapsed: Set<Int> = []
    private var visible: [Int] = []
    var selectedLine: Int?
    var selectedNode: Int?
    var hovered: Int? { didSet { if hovered != oldValue { needsDisplay = true } } }
    var onClick: ((TreeNode) -> Void)?
    let rowH: CGFloat = 23
    let indentW: CGFloat = 20

    override init(frame: NSRect) {
        super.init(frame: frame)
        addTrackingArea(NSTrackingArea(rect: .zero, options: [.mouseMoved, .mouseEnteredAndExited, .activeInKeyWindow, .inVisibleRect], owner: self))
    }
    required init?(coder: NSCoder) { fatalError() }

    func recompute() {
        visible = []
        var skipDepth: Int?
        for (k, n) in nodes.enumerated() {
            if let s = skipDepth {
                if n.depth > s { continue }
                skipDepth = nil
            }
            visible.append(k)
            if collapsed.contains(n.node) && n.hasChildren { skipDepth = n.depth }
        }
        invalidateIntrinsicContentSize()
        needsDisplay = true
    }

    override var intrinsicContentSize: NSSize {
        let maxDepth = visible.map { nodes[$0].depth }.max() ?? 0
        return NSSize(width: 60 + CGFloat(maxDepth) * indentW + 520, height: CGFloat(visible.count) * rowH + 30)
    }

    func y(ofVisible v: Int) -> CGFloat { 14 + CGFloat(v) * rowH }

    func reveal(node: Int?, line: Int?) {
        guard let v = visible.firstIndex(where: { (node != nil && nodes[$0].node == node) || (node == nil && nodes[$0].line == line) }) else { return }
        scrollToVisible(NSRect(x: 0, y: y(ofVisible: v) - 60, width: 10, height: 140))
    }

    override func draw(_ dirtyRect: NSRect) {
        Theme.panel.setFill()
        dirtyRect.fill()
        let cats: [(NSColor, NSColor)] = [
            (NSColor(hex: 0xE6EEF9), Theme.ink),        // declarations
            (NSColor(hex: 0xF5E8DE), Theme.heading),    // statements
            (NSColor(hex: 0xE5F1EA), Theme.green),      // expressions
            (NSColor(hex: 0xF1E6F4), Theme.purple),     // structs
        ]
        for (v, k) in visible.enumerated() {
            let n = nodes[k]
            let y = y(ofVisible: v)
            guard y + rowH >= dirtyRect.minY - rowH, y <= dirtyRect.maxY + rowH else { continue }
            let x = 20 + CGFloat(n.depth) * indentW
            // Connectors: a vertical line for every open ancestor, and an elbow into this node.
            for (d, ancLast) in n.ancestorsLast.enumerated() where d > 0 && !ancLast {
                let ax = 20 + CGFloat(d - 1) * indentW + 8
                line(NSPoint(x: ax, y: y), NSPoint(x: ax, y: y + rowH), Theme.borderStrong)
            }
            if n.depth > 0 {
                let px = 20 + CGFloat(n.depth - 1) * indentW + 8
                line(NSPoint(x: px, y: y), NSPoint(x: px, y: n.isLast ? y + rowH / 2 : y + rowH), Theme.borderStrong)
                line(NSPoint(x: px, y: y + rowH / 2), NSPoint(x: x, y: y + rowH / 2), Theme.borderStrong)
            }
            let selected = (selectedNode != nil && n.node == selectedNode) || (selectedNode == nil && selectedLine == n.line && n.category != 3)
            let (bg, fg) = cats[n.category]
            var tx = x + 4
            if n.hasChildren {
                let tri = NSBezierPath()
                let cx = x + 2, cy = y + rowH / 2
                if collapsed.contains(n.node) {
                    tri.move(to: NSPoint(x: cx - 2, y: cy - 4)); tri.line(to: NSPoint(x: cx + 3, y: cy)); tri.line(to: NSPoint(x: cx - 2, y: cy + 4))
                } else {
                    tri.move(to: NSPoint(x: cx - 4, y: cy - 2)); tri.line(to: NSPoint(x: cx + 4, y: cy - 2)); tri.line(to: NSPoint(x: cx, y: cy + 3))
                }
                tri.close()
                Theme.secondary.setFill()
                tri.fill()
                tx += 8
            }
            if !n.role.isEmpty {
                let s = n.role.uppercased()
                let w = textSize(s, Fonts.mono(8.5, .semibold)).width
                drawText(s, at: NSPoint(x: tx, y: y + 6), Fonts.mono(8.5, .semibold), Theme.faint)
                tx += w + 5
            }
            let lw = textSize(n.label, Fonts.ui(12, .semibold)).width
            let box = NSRect(x: tx, y: y + 2.5, width: lw + 14, height: rowH - 5)
            fillRounded(box, 5, selected ? Theme.selection : bg)
            if selected { strokeRounded(box, 5, Theme.amber, 1.5) }
            if hovered == v && !selected { strokeRounded(box, 5, Theme.borderStrong) }
            drawText(n.label, at: NSPoint(x: box.minX + 7, y: y + 4.5), Fonts.ui(12, .semibold), fg)
            if !n.detail.isEmpty {
                drawText(n.detail, at: NSPoint(x: box.maxX + 6, y: y + 5), Fonts.mono(11), n.category == 2 ? Theme.type : Theme.secondary)
            }
            drawText("\(n.line)", in: NSRect(x: bounds.width - 46, y: y, width: 36, height: rowH), Fonts.mono(10), Theme.faint, align: .right)
        }
    }

    private func visibleIndex(at p: NSPoint) -> Int? {
        let v = Int((p.y - 14) / rowH)
        return v >= 0 && v < visible.count ? v : nil
    }

    override func mouseMoved(with event: NSEvent) { hovered = visibleIndex(at: convert(event.locationInWindow, from: nil)) }
    override func mouseExited(with event: NSEvent) { hovered = nil }
    override func mouseDown(with event: NSEvent) {
        let p = convert(event.locationInWindow, from: nil)
        guard let v = visibleIndex(at: p) else { return }
        let n = nodes[visible[v]]
        let x = 20 + CGFloat(n.depth) * indentW
        if n.hasChildren && (p.x < x + 12 || event.clickCount == 2) {
            if collapsed.contains(n.node) { collapsed.remove(n.node) } else { collapsed.insert(n.node) }
            recompute()
            superview?.superview?.needsLayout = true
            return
        }
        onClick?(n)
    }
}

final class TreeView: NSView, StationView {
    let canvas = TreeCanvas(frame: .zero)
    lazy var panel = CanvasPanel(canvas)
    let empty = EmptyNote()
    weak var model: AppModel?
    let expand = TButton("Expand all", symbol: "chevron.down.2")
    let collapseB = TButton("Collapse expressions", symbol: "chevron.up.2")
    let legend = makeLabel("Declarations · Statements · Expressions (with their types from semantic analysis)", Fonts.small, Theme.secondary)

    override init(frame: NSRect) {
        super.init(frame: frame)
        addSubview(panel)
        addSubview(expand)
        addSubview(collapseB)
        addSubview(legend)
        addSubview(empty)
        canvas.onClick = { [weak self] n in
            self?.model?.selection = Selection(line: n.line, node: n.node, range: n.range, origin: .tree)
        }
        expand.action = { [weak self] in
            self?.canvas.collapsed = []
            self?.canvas.recompute()
            self?.panel.needsLayout = true
        }
        collapseB.action = { [weak self] in
            guard let self else { return }
            // Collapse every statement's expression subtree.
            self.canvas.collapsed = Set(self.canvas.nodes.filter { $0.category == 1 }.map { $0.node }).subtracting(
                self.canvas.nodes.filter { $0.label == "Block" || $0.label == "For" || $0.label == "While" || $0.label == "If" }.map { $0.node })
            self.canvas.collapsed = Set(self.canvas.nodes.filter { $0.label.hasPrefix("Expression") || $0.label == "Return" || $0.label == "Declaration" }.map { $0.node })
            self.canvas.recompute()
            self.panel.needsLayout = true
        }
    }
    required init?(coder: NSCoder) { fatalError() }

    override var isFlipped: Bool { true }

    override func layout() {
        super.layout()
        let e = expand.intrinsicContentSize, c = collapseB.intrinsicContentSize
        expand.frame = NSRect(x: 10, y: 8, width: e.width, height: 24)
        collapseB.frame = NSRect(x: expand.frame.maxX + 6, y: 8, width: c.width, height: 24)
        legend.frame = NSRect(x: collapseB.frame.maxX + 12, y: 12, width: bounds.width - collapseB.frame.maxX - 20, height: 16)
        panel.frame = NSRect(x: 0, y: 40, width: bounds.width, height: bounds.height - 40)
        empty.frame = bounds
    }

    override func draw(_ dirtyRect: NSRect) {
        Theme.panel.setFill()
        bounds.fill()
    }

    func reload(_ model: AppModel, _ event: ModelEvent) {
        self.model = model
        guard let prog = model.compilation?.program ?? (model.isAssembly ? nil : model.good?.program), !model.isAssembly else {
            empty.isHidden = false
            empty.title = "No syntax tree"
            empty.message = model.isAssembly ? "Hand-written assembly skips the parser." : "The parser stopped at a syntax error — see Problems under the editor."
            empty.needsDisplay = true
            return
        }
        empty.isHidden = true
        if event == .compiled || canvas.nodes.isEmpty {
            canvas.nodes = TreeBuilder.build(prog)
            canvas.recompute()
            panel.needsLayout = true
        }
        canvas.selectedLine = model.selection.line
        canvas.selectedNode = model.selection.origin == .tree ? model.selection.node : nil
        canvas.needsDisplay = true
        if model.selection.origin != .tree, model.selection.line != nil {
            canvas.reveal(node: nil, line: model.selection.line)
        }
    }
}

// MARK: - Symbols

final class SymbolsView: NSView, StationView {
    let panel = ListPanel()
    let empty = EmptyNote()
    weak var model: AppModel?

    override init(frame: NSRect) {
        super.init(frame: frame)
        addSubview(panel)
        addSubview(empty)
        panel.list.onClick = { [weak self] _, r in
            self?.model?.selection = Selection(line: r.line, origin: .symbols)
        }
        panel.list.highlight = { [weak self] r in
            guard let l = self?.model?.selection.line, r.line == l else { return nil }
            return Theme.selection
        }
    }
    required init?(coder: NSCoder) { fatalError() }

    override func layout() {
        super.layout()
        panel.frame = bounds
        empty.frame = bounds
    }

    func reload(_ model: AppModel, _ event: ModelEvent) {
        self.model = model
        guard !model.isAssembly, let c = model.shown, let sema = c.sema, let prog = c.program else {
            empty.isHidden = false
            empty.title = "No symbol table"
            empty.message = model.isAssembly ? "Hand-written assembly has labels instead — see the Linking view." : "Semantic analysis did not run."
            empty.needsDisplay = true
            return
        }
        empty.isHidden = true
        guard event == .compiled || panel.list.rows.isEmpty || event == .selection else { return }
        if event == .selection { panel.list.needsDisplay = true; return }
        var rows: [Row] = []
        let cols: [CGFloat] = [130, 80, 170, 60]
        func headerRow() -> Row {
            var r = Row([Cell("NAME", Fonts.mono(9.5, .semibold), Theme.faint, width: cols[0]),
                         Cell("KIND", Fonts.mono(9.5, .semibold), Theme.faint, width: cols[1]),
                         Cell("TYPE", Fonts.mono(9.5, .semibold), Theme.faint, width: cols[2]),
                         Cell("SIZE", Fonts.mono(9.5, .semibold), Theme.faint, width: cols[3]),
                         Cell("WHERE IT LIVES (after code generation)", Fonts.mono(9.5, .semibold), Theme.faint)], height: 18)
            r.kind = .section
            return r
        }
        func symRow(_ s: Symbol, indent: CGFloat = 0) -> Row {
            let kindColor: NSColor = [Symbol.Kind.global: Theme.ink, .local: Theme.green, .param: Theme.amber, .function: Theme.heading][s.kind] ?? Theme.text
            let name = s.uniqueName == s.name ? s.name : "\(s.name)  (\(s.uniqueName))"
            var loc = s.location
            if s.kind == .function {
                loc = c.executable?.address(of: s.name).map { "code at \(hexAddr($0))" } ?? (s.isDefined ? "" : "declared only")
            } else if s.kind == .global, let a = c.executable?.address(of: s.uniqueName) {
                loc = ".data at \(hexAddr(a))"
            }
            var r = Row([Cell(name, Fonts.mono(12, .semibold), Theme.text, width: cols[0]),
                         Cell(NSAttributedString(s.kind.rawValue, Fonts.small, kindColor), width: cols[1], badge: kindColor.alpha(0.1)),
                         Cell(s.type.description, Fonts.mono(11.5), Theme.type, width: cols[2]),
                         Cell(s.kind == .function ? "" : "\(s.type.size) B", Fonts.mono(11), Theme.secondary, width: cols[3]),
                         Cell(loc, Fonts.mono(11), loc.hasPrefix("register") ? Theme.green : loc.hasPrefix("optimised") ? Theme.faint : Theme.text)])
            r.line = s.line
            r.indent = indent
            r.height = 21
            if s.addressTaken { r.tooltip = "Its address is taken (&\(s.name)), so it must live in memory." }
            return r
        }
        let fileScope = sema.scopes.first
        rows.append(.header("File scope", detail: "globals and functions — visible everywhere after their declaration"))
        rows.append(headerRow())
        for s in fileScope?.symbols ?? [] { rows.append(symRow(s)) }
        for fd in prog.functions {
            rows.append(.spacer(6))
            let callNote = fd.hasCalls ? "makes calls" : "leaf function (makes no calls)"
            rows.append(.header("function \(fd.name)", detail: "\(fd.locals.count) parameters and locals · \(callNote)"))
            rows.append(headerRow())
            // Order by scope depth, then declaration.
            for s in fd.locals { rows.append(symRow(s, indent: CGFloat(max(0, s.depth - 1)) * 14)) }
            if fd.locals.isEmpty { rows.append(Row([Cell("(no parameters or locals)", Fonts.small, Theme.faint)])) }
        }
        let structs = prog.structs.filter { $0.isComplete }
        if !structs.isEmpty {
            rows.append(.spacer(6))
            rows.append(.header("Struct layouts", detail: "fields are aligned to their size, so the compiler may insert padding"))
            for st in structs {
                var hr = Row([Cell("struct \(st.name)", Fonts.mono(12, .semibold), Theme.purple, width: 260),
                              Cell("\(st.size) bytes, aligned to \(st.align)", Fonts.small, Theme.secondary)], height: 22)
                hr.kind = .section
                rows.append(hr)
                var last = 0
                for f in st.fields {
                    if f.offset > last {
                        rows.append(Row([Cell("", width: 20), Cell("(padding)", Fonts.mono(11), Theme.faint, width: 170),
                                         Cell("offset \(last)–\(f.offset - 1)", Fonts.mono(11), Theme.faint)]))
                    }
                    rows.append(Row([Cell("", width: 20), Cell(f.name, Fonts.mono(11.5, .semibold), Theme.text, width: 170),
                                     Cell(f.type.description, Fonts.mono(11), Theme.type, width: 170),
                                     Cell("offset \(f.offset), \(f.type.size) B", Fonts.mono(11), Theme.secondary)]))
                    last = f.offset + f.type.size
                }
                if st.size > last {
                    rows.append(Row([Cell("", width: 20), Cell("(tail padding)", Fonts.mono(11), Theme.faint, width: 170),
                                     Cell("offset \(last)–\(st.size - 1)", Fonts.mono(11), Theme.faint)]))
                }
            }
        }
        if !sema.libraryUsed.isEmpty {
            rows.append(.spacer(6))
            rows.append(.header("Library functions called", detail: "built in: no #include needed; the linker pulls in their code"))
            for n in sema.libraryUsed.sorted() {
                let f = Library.byName[n]
                let sig = f.map { CType.function($0.returnType, $0.params).spelled(n) } ?? n
                rows.append(Row([Cell(sig, Fonts.mono(11.5), Theme.ink, width: 300), Cell(f?.summary ?? "", Fonts.small, Theme.secondary)]))
            }
        }
        if !sema.strings.isEmpty {
            rows.append(.spacer(6))
            rows.append(.header("String literals", detail: "stored once each in the .data section, ending with a zero byte"))
            for s in sema.strings {
                rows.append(Row([Cell(s.label, Fonts.mono(11.5, .semibold), Theme.label, width: 80),
                                 Cell("\"" + CodeGen.escape(s.bytes) + "\"", Fonts.mono(11.5), Theme.string, width: 360),
                                 Cell("\(s.bytes.count + 1) bytes", Fonts.mono(11), Theme.secondary)]))
            }
        }
        panel.set(rows)
    }
}
