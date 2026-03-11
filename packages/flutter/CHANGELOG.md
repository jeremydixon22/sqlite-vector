## Unreleased

- Fix Flutter iOS simulator native-asset packaging by emitting an
  architecture-specific dylib for each simulator target.

## 0.9.85

- Initial release.
- Bundles pre-built sqlite-vector binaries for Android, iOS, macOS, Linux, and Windows.
- Provides `loadSqliteVectorExtension()` for use with `sqlite3` and `drift`.
