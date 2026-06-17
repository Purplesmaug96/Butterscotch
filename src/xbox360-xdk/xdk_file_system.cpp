#include <xtl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string.h>

// Core headers
#include "utils.h"
#include "xdk_file_system.h"

// Define max path for Xbox 360/Windows compatibility
#ifndef MAX_PATH
#define MAX_PATH 260
#endif

// Helper to normalize slashes for the Xbox 360 filesystem
static void xdkNormalizePath(char* path) {
    for (char* p = path; *p; p++) {
        if (*p == '/') *p = '\\';
    }
}

// ===[ Vtable Implementations ]===

static char* xdkResolvePath(FileSystem* fs, const char* relativePath) {
	diagLog("BS FS: xdkResolvePath called for: %s", relativePath ? relativePath : "NULL");
    XdkFileSystem* xfs = (XdkFileSystem*)fs;

    // If the path already contains a colon, it's an absolute Xbox 360 path
    // (e.g. "game:\lang\lang_en.json") — return it verbatim without prepending basePath.
    if (strchr(relativePath, ':') != nullptr) {
        char* result = (char*)malloc(MAX_PATH);
        if (!result) return NULL;
        strncpy(result, relativePath, MAX_PATH - 1);
        result[MAX_PATH - 1] = '\0';
        xdkNormalizePath(result);
        return result;
    }

    // Use snprintf to safely combine paths and respect MAX_PATH
    char* result = (char*)malloc(MAX_PATH);
    if (!result) return NULL;

    int written = snprintf(result, MAX_PATH, "%s%s", xfs->basePath, relativePath);

    if (written < 0 || written >= MAX_PATH) {
        free(result);
        return NULL;
    }

    xdkNormalizePath(result);

	diagLog("BS FS: Resolved absolute path to: %s", result);

    return result;
}

static bool xdkFileExists(FileSystem* fs, const char* relativePath) {
	diagLog("BS FS: xdkFileExists called for: %s", relativePath ? relativePath : "NULL");
    char* fullPath = xdkResolvePath(fs, relativePath);
    if (!fullPath) return false;

    DWORD attributes = GetFileAttributesA(fullPath);
    free(fullPath);

    // Xbox 360 XDK uses 0xFFFFFFFF instead of INVALID_FILE_ATTRIBUTES
    return (attributes != 0xFFFFFFFF && !(attributes & FILE_ATTRIBUTE_DIRECTORY));
}

static char* xdkReadFileText(FileSystem* fs, const char* relativePath) {
	diagLog("BS FS: xdkReadFileText called for: %s", relativePath ? relativePath : "NULL");
    char* fullPath = xdkResolvePath(fs, relativePath);
    if (!fullPath) return NULL;

    HANDLE hFile = CreateFileA(fullPath, GENERIC_READ, FILE_SHARE_READ,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(fullPath);

    if (hFile == INVALID_HANDLE_VALUE) return NULL;

    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart == 0) {
        CloseHandle(hFile);
        return NULL;
    }

    // Allocate buffer (ensure we don't overflow 32-bit size_t if file is massive)
    char* buffer = (char*)malloc((size_t)fileSize.QuadPart + 1);
    if (!buffer) {
        CloseHandle(hFile);
        return NULL;
    }

    DWORD bytesRead = 0;
    if (ReadFile(hFile, buffer, (DWORD)fileSize.QuadPart, &bytesRead, NULL)) {
        buffer[bytesRead] = '\0';
    } else {
        free(buffer);
        buffer = NULL;
    }

    CloseHandle(hFile);
    return buffer;
}

static bool xdkWriteFileText(FileSystem* fs, const char* relativePath, const char* contents) {
	diagLog("BS FS: xdkWriteFileText called for: %s", relativePath ? relativePath : "NULL");
    if (!contents) return false;
    char* fullPath = xdkResolvePath(fs, relativePath);
    if (!fullPath) return false;

    HANDLE hFile = CreateFileA(fullPath, GENERIC_WRITE, 0,
                               NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    free(fullPath);

    if (hFile == INVALID_HANDLE_VALUE) return false;

    DWORD len = (DWORD)strlen(contents);
    DWORD written;
    BOOL ok = WriteFile(hFile, contents, len, &written, NULL);
    CloseHandle(hFile);
    return (ok && written == len);
}

static bool xdkDeleteFile(FileSystem* fs, const char* relativePath) {
	diagLog("BS FS: xdkDeleteFile called for: %s", relativePath ? relativePath : "NULL");
    char* fullPath = xdkResolvePath(fs, relativePath);
    if (!fullPath) return false;

    BOOL ok = DeleteFileA(fullPath);
    free(fullPath);
    return (ok != FALSE);
}

// ===[ Vtable ]===

static FileSystemVtable xdkFileSystemVtable = {
    xdkResolvePath,
    xdkFileExists,
    xdkReadFileText,
    xdkWriteFileText,
    xdkDeleteFile,
};

// ===[ Public API ]===

XdkFileSystem* XdkFileSystem_create(const char* dataWinPath) {
    XdkFileSystem* xfs = (XdkFileSystem*)calloc(1, sizeof(XdkFileSystem));
    if (!xfs) return NULL;

    xfs->base.vtable = &xdkFileSystemVtable;

    // Safely copy and extract directory
    const char* lastSlash = strrchr(dataWinPath, '\\');
    const char* altSlash = strrchr(dataWinPath, '/');
    if (altSlash > lastSlash) lastSlash = altSlash;

    if (lastSlash) {
        size_t dirLen = (size_t)(lastSlash - dataWinPath + 1);
        if (dirLen >= sizeof(xfs->basePath)) dirLen = sizeof(xfs->basePath) - 1;
        memcpy(xfs->basePath, dataWinPath, dirLen);
        xfs->basePath[dirLen] = '\0';
    } else {
        strncpy(xfs->basePath, "game:\\", sizeof(xfs->basePath) - 1);
    }

    xdkNormalizePath(xfs->basePath);
    return xfs;
}