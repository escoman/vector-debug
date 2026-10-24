#pragma once
//
// Portable temp-path helpers for standalone test executables.
//
// Replaces the POSIX-only "/tmp/..." hardcodes and `mkdir()`/`system("rm -rf")`
// calls so the test suite builds and runs on Windows/MinGW as well as Linux.
//
// Reachable from both tests/ and agent/tests/ via the global
// include_directories(${DBG_TST_DIR}) entry in CMakeLists.txt.
//
#include <string>

#if __has_include(<filesystem>)
#  include <filesystem>
#  define VDB_TEST_HAS_FILESYSTEM 1
#else
#  include <experimental/filesystem>
#  define VDB_TEST_HAS_FILESYSTEM 0
#endif

// Root of the portable temp directory (Linux: /tmp, Windows: %TEMP%),
// with the trailing separator stripped.
inline const std::string &testTmpRoot()
{
    static const std::string root = [] {
#if VDB_TEST_HAS_FILESYSTEM
        std::string s = std::filesystem::temp_directory_path().string();
#else
        std::string s = std::experimental::filesystem::temp_directory_path().string();
#endif
        while (!s.empty() && (s.back() == '/' || s.back() == '\\')) {
            s.pop_back();
        }
        return s;
    }();
    return root;
}

// "<tmpdir>/<name>" — '/' separators are accepted by the Windows CRT too.
inline std::string testTmpPath(const std::string &name)
{
    return testTmpRoot() + "/" + name;
}

// Portable mkdir -p (replaces POSIX mkdir(path, 0755)).
inline bool testMakeDir(const std::string &path)
{
#if VDB_TEST_HAS_FILESYSTEM
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return !ec;
#else
    std::error_code ec;
    std::experimental::filesystem::create_directories(path, ec);
    return !ec;
#endif
}

// Portable recursive delete; also removes the path if it is a plain file.
// Replaces system("rm -rf <path>").
inline void testRemoveTree(const std::string &path)
{
#if VDB_TEST_HAS_FILESYSTEM
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    if (!ec) return;
    std::filesystem::remove(path, ec);  // path may be a file, not a dir
#else
    std::error_code ec;
    std::experimental::filesystem::remove_all(path, ec);
    if (!ec) return;
    std::experimental::filesystem::remove(path, ec);
#endif
}

// True if the file/directory exists (replaces stat()-based checks).
inline bool testFileExists(const std::string &path)
{
#if VDB_TEST_HAS_FILESYSTEM
    std::error_code ec;
    return std::filesystem::exists(path, ec) && !ec;
#else
    std::error_code ec;
    return std::experimental::filesystem::exists(path, ec) && !ec;
#endif
}
