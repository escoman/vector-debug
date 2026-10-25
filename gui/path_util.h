#pragma once

// ---------------------------------------------------------------------------
// Portable path helpers.
//
// POSIX uses '/' as the directory separator; Windows uses '\'. The hand-rolled
// file browser (rom_file_dialog.cpp) and the File-menu path handling used to
// hardcode '/', which broke tree navigation on Windows: getcwd() returns
// "C:\Users\..." so rfind('/') found no separator and "go up" did nothing.
//
// PATH_SEP is the platform-preferred separator (what we *write*), while the
// queries below accept *both* '/' and '\' as separators on Windows — the Win32
// file APIs accept either, and a user may type '/' into the path field.
// ---------------------------------------------------------------------------

#include <cctype>
#include <cstddef>
#include <string>

namespace vpath {

#ifdef _WIN32
inline constexpr char PATH_SEP = '\\';
inline constexpr const char* PATH_SEP_STR = "\\";
inline constexpr const char* PATH_ROOT = "C:\\";

// On Windows both '\' and '/' are separators.
inline bool isSep(char c) { return c == '/' || c == '\\'; }
#else
inline constexpr char PATH_SEP = '/';
inline constexpr const char* PATH_SEP_STR = "/";
inline constexpr const char* PATH_ROOT = "/";

inline bool isSep(char c) { return c == '/'; }
#endif

// Index of the last separator in `p`, or npos if there is none.
inline std::size_t lastSep(const std::string& p) {
    for (std::size_t i = p.size(); i-- > 0;) {
        if (isSep(p[i])) return i;
    }
    return std::string::npos;
}

// Join a directory and a child name with exactly one separator.
inline std::string join(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    if (isSep(dir.back())) return dir + name;
    return dir + PATH_SEP + name;
}

// Filename portion after the last separator.
inline std::string basename(const std::string& p) {
    std::size_t i = lastSep(p);
    return (i == std::string::npos) ? p : p.substr(i + 1);
}

// True if the path is absolute: POSIX leading '/', Windows leading separator,
// a drive spec "X:", or a UNC root "\\server".
inline bool isAbsolute(const std::string& p) {
    if (p.empty()) return false;
    if (isSep(p[0])) return true;
#ifdef _WIN32
    if (p.size() >= 2 &&
        std::isalpha(static_cast<unsigned char>(p[0])) && p[1] == ':')
        return true;
#endif
    return false;
}

// Parent directory of a path.
//   "/home/user"    -> "/home"
//   "/home"         -> "/"            (the root itself)
//   "/"             -> ""             (already at root)
//   "C:\Users\roms" -> "C:\Users"
//   "C:\Users"      -> "C:\"          (drive root)
//   "C:\" or "C:"   -> ""             (already at root)
// A trailing separator on the input is ignored. Returns "" when there is no
// parent above the root, so callers can treat empty as "stay where you are".
inline std::string parentPath(const std::string& path) {
    if (path.empty()) return std::string();

    // Trim trailing separators, but never the sole leading one.
    std::size_t end = path.size();
    while (end > 1 && isSep(path[end - 1])) --end;
    const std::string p = path.substr(0, end);

    std::size_t i = lastSep(p);
    if (i == std::string::npos) return std::string();

#ifdef _WIN32
    // "C:\Users" -> i == 2, p[1] == ':' -> the parent is the drive root "C:\".
    if (i == 2 && p[1] == ':') return p.substr(0, 2) + PATH_SEP_STR;
    // "C:\" trimmed to "C:" hits the npos branch above (already at root).
    return p.substr(0, i);
#else
    // "/home" -> i == 0 -> parent is the root "/".
    if (i == 0) return PATH_SEP_STR;
    return p.substr(0, i);
#endif
}

}  // namespace vpath
