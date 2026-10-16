import AppKit
import SiliconCore
import UniformTypeIdentifiers

public enum SiliconApp {
    public static func run() {
        let app = NSApplication.shared
        let delegate = AppDelegate()
        app.delegate = delegate
        app.setActivationPolicy(.regular)
        withExtendedLifetime(delegate) { app.run() }
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate, NSWindowDelegate {
    let model = AppModel()
    var window: NSWindow!
    var main: MainView!

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.appearance = NSAppearance(named: .aqua)
        buildMenu()
        let visible = NSScreen.main?.visibleFrame ?? NSRect(x: 0, y: 0, width: 1440, height: 900)
        let size = NSSize(width: min(1520, visible.width - 20), height: min(960, visible.height - 10))
        window = NSWindow(contentRect: NSRect(origin: .zero, size: size), styleMask: [.titled, .closable, .miniaturizable, .resizable],
                          backing: .buffered, defer: false)
        window.title = "Silicon Path"
        window.titlebarAppearsTransparent = true
        window.backgroundColor = Theme.background
        window.contentMinSize = NSSize(width: 1180, height: 720)
        window.delegate = self
        main = MainView(model: model)
        window.contentView = main
        window.center()
        window.setFrameAutosaveName("SiliconPathWindow")
        window.makeKeyAndOrderFront(nil)
        main.start()
        NSApp.activate(ignoringOtherApps: true)
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
    func applicationWillTerminate(_ notification: Notification) { model.save() }

    func application(_ sender: NSApplication, openFile filename: String) -> Bool {
        model.loadFile(URL(fileURLWithPath: filename))
        return true
    }

    // MARK: Menu

    private func item(_ title: String, _ action: Selector?, _ key: String = "", _ mods: NSEvent.ModifierFlags = .command, tag: Int = 0) -> NSMenuItem {
        let i = NSMenuItem(title: title, action: action, keyEquivalent: key)
        i.keyEquivalentModifierMask = mods
        i.tag = tag
        if let action, responds(to: action) { i.target = self }
        return i
    }

    private func buildMenu() {
        let main = NSMenu()
        let appMenu = NSMenu()
        appMenu.addItem(item("About Silicon Path", #selector(about)))
        appMenu.addItem(.separator())
        appMenu.addItem(withTitle: "Hide Silicon Path", action: #selector(NSApplication.hide(_:)), keyEquivalent: "h")
        appMenu.addItem(withTitle: "Quit Silicon Path", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        main.addItem(withTitle: "", action: nil, keyEquivalent: "").submenu = appMenu

        let file = NSMenu(title: "File")
        file.addItem(item("New C Program", #selector(newProgram), "n"))
        file.addItem(item("Open…", #selector(openFile), "o"))
        file.addItem(item("Save As…", #selector(saveFile), "s"))
        file.addItem(item("Export Assembly…", #selector(exportAsm), "e"))
        file.addItem(.separator())
        let ex = NSMenu(title: "Examples")
        for (k, e) in Examples.all.enumerated() { ex.addItem(item(e.name, #selector(loadExample(_:)), tag: k)) }
        let exItem = NSMenuItem(title: "Examples", action: nil, keyEquivalent: "")
        exItem.submenu = ex
        file.addItem(exItem)
        file.addItem(.separator())
        file.addItem(withTitle: "Close Window", action: #selector(NSWindow.performClose(_:)), keyEquivalent: "w")
        main.addItem(withTitle: "File", action: nil, keyEquivalent: "").submenu = file

        let edit = NSMenu(title: "Edit")
        edit.addItem(withTitle: "Undo", action: Selector(("undo:")), keyEquivalent: "z")
        let redo = NSMenuItem(title: "Redo", action: Selector(("redo:")), keyEquivalent: "z")
        redo.keyEquivalentModifierMask = [.command, .shift]
        edit.addItem(redo)
        edit.addItem(.separator())
        edit.addItem(withTitle: "Cut", action: #selector(NSText.cut(_:)), keyEquivalent: "x")
        edit.addItem(withTitle: "Copy", action: #selector(NSText.copy(_:)), keyEquivalent: "c")
        edit.addItem(withTitle: "Paste", action: #selector(NSText.paste(_:)), keyEquivalent: "v")
        edit.addItem(withTitle: "Select All", action: #selector(NSText.selectAll(_:)), keyEquivalent: "a")
        edit.addItem(.separator())
        edit.addItem(withTitle: "Find…", action: #selector(NSTextView.performFindPanelAction(_:)), keyEquivalent: "f").tag = Int(NSFindPanelAction.showFindPanel.rawValue)
        main.addItem(withTitle: "Edit", action: nil, keyEquivalent: "").submenu = edit

        let run = NSMenu(title: "Run")
        run.addItem(item("Run / Pause", #selector(runPause), "r"))
        run.addItem(item("Step One Cycle", #selector(stepCycle), "j"))
        run.addItem(item("Step One Instruction", #selector(stepInstruction), "k"))
        run.addItem(item("Step One C Line", #selector(stepLine), "l"))
        run.addItem(item("Step Back One Cycle", #selector(stepBack), "j", [.command, .shift]))
        run.addItem(item("Run to the End", #selector(runToEnd), "r", [.command, .option]))
        run.addItem(item("Reset Machine", #selector(reset), "r", [.command, .shift]))
        run.addItem(.separator())
        run.addItem(item("Compile Now", #selector(compile), "b"))
        main.addItem(withTitle: "Run", action: nil, keyEquivalent: "").submenu = run

        let view = NSMenu(title: "View")
        for (k, s) in Station.allCases.enumerated() {
            view.addItem(item("\(k + 1). \(s.title)", #selector(showStation(_:)), k < 9 ? "\(k + 1)" : "0", tag: k))
        }
        main.addItem(withTitle: "View", action: nil, keyEquivalent: "").submenu = view

        let win = NSMenu(title: "Window")
        win.addItem(withTitle: "Minimize", action: #selector(NSWindow.performMiniaturize(_:)), keyEquivalent: "m")
        win.addItem(withTitle: "Zoom", action: #selector(NSWindow.performZoom(_:)), keyEquivalent: "")
        main.addItem(withTitle: "Window", action: nil, keyEquivalent: "").submenu = win
        NSApp.windowsMenu = win
        NSApp.mainMenu = main
    }

    @objc func about() {
        let a = NSAlert()
        a.messageText = "Silicon Path"
        a.informativeText = "Follow a C program all the way down: tokens, syntax tree, symbols, IR and optimisation, RISC-V assembly, machine code and linking, then a 5-stage pipeline with caches.\n\nEverything here (compiler, assembler, linker, simulators) is implemented from scratch, following Patterson & Hennessy's RISC-V edition."
        a.runModal()
    }

    @objc func newProgram() {
        model.load(Example(name: "Untitled", goal: "", source: "int main() {\n    printf(\"hello, RISC-V\\n\");\n    return 0;\n}\n", station: .source))
    }

    @objc func openFile() {
        let p = NSOpenPanel()
        p.allowedContentTypes = [UTType(filenameExtension: "c")!, UTType(filenameExtension: "s")!, .plainText]
        p.allowsOtherFileTypes = true
        if p.runModal() == .OK, let u = p.url { model.loadFile(u) }
    }

    @objc func saveFile() {
        let p = NSSavePanel()
        p.nameFieldStringValue = model.isAssembly ? "program.s" : "program.c"
        if p.runModal() == .OK, let u = p.url { try? model.source.write(to: u, atomically: true, encoding: .utf8) }
    }

    @objc func exportAsm() {
        guard let c = model.shown else { return }
        let p = NSSavePanel()
        p.nameFieldStringValue = "program.s"
        if p.runModal() == .OK, let u = p.url {
            let text = c.asm.map { $0.text }.joined(separator: "\n") + "\n"
            try? text.write(to: u, atomically: true, encoding: .utf8)
        }
    }

    @objc func loadExample(_ s: NSMenuItem) { model.load(Examples.all[s.tag]) }
    @objc func runPause() { model.togglePlay() }
    @objc func stepCycle() { model.stepCycle() }
    @objc func stepInstruction() { model.stepInstruction() }
    @objc func stepLine() { model.stepLine() }
    @objc func stepBack() { model.stepBack() }
    @objc func runToEnd() { model.runToEnd() }
    @objc func reset() { model.resetMachine() }
    @objc func compile() { model.compileNow() }
    @objc func showStation(_ s: NSMenuItem) { model.setStation(Station.allCases[s.tag]) }
}
