import AppKit
import SiliconUI

// uishot verify           check every built-in example
// uishot OUT.png cmd…     render the window after running the commands
let args = Array(CommandLine.arguments.dropFirst())
guard let first = args.first else {
    print("usage: uishot verify | uishot out.png [example:N] [station:N] [cycles:N] …")
    exit(1)
}
if first == "verify" {
    exit(Snapshot.verifyExamples() ? 0 : 1)
}
Snapshot.render(Array(args.dropFirst()), to: first)
print("wrote \(first)")
