// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "NappleStudio",
    platforms: [.macOS("26.0")],
    products: [.executable(name: "NappleStudio", targets: ["NappleStudio"])],
    targets: [
        .target(name: "NappleStudioCore"),
        .executableTarget(name: "NappleStudio", dependencies: ["NappleStudioCore"]),
        .testTarget(name: "NappleStudioTests", dependencies: ["NappleStudioCore"]),
    ]
)
