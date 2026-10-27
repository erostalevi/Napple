// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "NappleStudio",
    platforms: [.macOS("26.0")],
    products: [.executable(name: "NappleStudio", targets: ["NappleStudio"]), .executable(name: "napple-studio-build", targets: ["NappleStudioBuild"])],
    targets: [
        .target(name: "NappleStudioCore"),
        .executableTarget(name: "NappleStudio", dependencies: ["NappleStudioCore"]),
        .executableTarget(name: "NappleStudioBuild", dependencies: ["NappleStudioCore"]),
        .testTarget(name: "NappleStudioTests", dependencies: ["NappleStudioCore"]),
    ]
)
