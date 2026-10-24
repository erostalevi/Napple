// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "NappleStudio",
    platforms: [.macOS("26.0")],
    products: [.executable(name: "NappleStudio", targets: ["NappleStudio"])],
    targets: [
        .target(name: "NappleStudioCore", swiftSettings: [.swiftLanguageMode(.v5)]),
        .executableTarget(name: "NappleStudio", dependencies: ["NappleStudioCore"],
                          swiftSettings: [.swiftLanguageMode(.v5)]),
        .testTarget(name: "NappleStudioTests", dependencies: ["NappleStudioCore"],
                    swiftSettings: [.swiftLanguageMode(.v5)]),
    ]
)
