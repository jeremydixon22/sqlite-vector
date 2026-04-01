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
            url: "https://github.com/sqliteai/sqlite-vector/releases/download/0.9.94/vector-apple-xcframework-0.9.94.zip",
            checksum: "6d4d9064de2c525324128870730be8fb8e37857c180d828a87fb2af2be5b8546"
        ),
        .target(
            name: "vector",
            dependencies: ["vectorBinary"],
            path: "packages/swift"
        ),
    ]
)
