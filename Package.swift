// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "AstroLibrary",
    platforms: [.macOS(.v14)],
    products: [
        .executable(name: "AstroLibrary", targets: ["AstroLibrary"]),
    ],
    targets: [
        .executableTarget(
            name: "AstroLibrary",
            path: "macapp/Sources/AstroLibrary",
            linkerSettings: [
                .linkedFramework("AppKit"),
                .linkedFramework("WebKit"),
            ]
        ),
        .testTarget(name: "AstroLibraryTests", dependencies: ["AstroLibrary"], path: "macapp/Tests"),
    ]
)
