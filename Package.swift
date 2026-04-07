// swift-tools-version: 6.1
// The swift-tools-version declares the minimum version of Swift required to build this package.

import PackageDescription

let package = Package(
    name: "vector",
    platforms: [.macOS(.v11), .iOS(.v11)],
    products: [
        .library(
            name: "vector",
            targets: ["vector"])
    ],
    targets: [
        .binaryTarget(
            name: "vectorBinary",
            url: "https://github.com/sqliteai/sqlite-vector/releases/download/0.9.95/vector-apple-xcframework-0.9.95.zip",
            checksum: "db4a3a733ff6d719c18a4692b5cbab80327daff004d8199cb53a198cb5072e85"
        ),
        .target(
            name: "vector",
            dependencies: ["vectorBinary"],
            path: "packages/swift"
        ),
    ]
)
