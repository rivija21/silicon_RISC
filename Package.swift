// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "SiliconPath",
    platforms: [.macOS("15.0")],
    targets: [
        // The compiler, assembler, linker and simulators. No AppKit.
        .target(name: "SiliconCore", path: "Sources/SiliconCore",
                swiftSettings: [.unsafeFlags(["-Ounchecked"], .when(configuration: .release))]),
        // Views and the app model.
        .target(name: "SiliconUI", dependencies: ["SiliconCore"], path: "Sources/SiliconUI"),
        // The app itself.
        .executableTarget(name: "SiliconPath", dependencies: ["SiliconUI"], path: "Sources/SiliconPath"),
        // Test driver: hand-written tests and random differential testing.
        .executableTarget(name: "spcheck", dependencies: ["SiliconCore"], path: "Sources/spcheck"),
        // Renders UI scenarios to PNG files for checking the layout.
        .executableTarget(name: "uishot", dependencies: ["SiliconUI"], path: "Sources/uishot"),
    ]
)
