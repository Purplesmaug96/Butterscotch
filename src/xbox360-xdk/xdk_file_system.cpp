#include <xtl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string.h>

// Core headers
#include "utils.h"
#include "xdk_file_system.h"
#include "stb_ds.h"

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
    return result;
}

static bool xdkFileExists(FileSystem* fs, const char* relativePath) {
    char* fullPath = xdkResolvePath(fs, relativePath);
    if (!fullPath) return false;

    DWORD attributes = GetFileAttributesA(fullPath);
    free(fullPath);

    // Xbox 360 XDK uses 0xFFFFFFFF instead of INVALID_FILE_ATTRIBUTES
    return (attributes != 0xFFFFFFFF && !(attributes & FILE_ATTRIBUTE_DIRECTORY));
}

static char* xdkReadFileText(FileSystem* fs, const char* relativePath) {
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
    char* fullPath = xdkResolvePath(fs, relativePath);
    if (!fullPath) return false;

    BOOL ok = DeleteFileA(fullPath);
    free(fullPath);
    return (ok != FALSE);
}

// ===[ Binary I/O ]===

static bool xdkReadFileBinary(FileSystem* fs, const char* relativePath, uint8_t** outData, int32_t* outSize) {
    char* fullPath = xdkResolvePath(fs, relativePath);
    if (!fullPath) return false;

    HANDLE hFile = CreateFileA(fullPath, GENERIC_READ, FILE_SHARE_READ,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(fullPath);

    if (hFile == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart == 0) {
        CloseHandle(hFile);
        return false;
    }

    *outData = (uint8_t*)malloc((size_t)fileSize.QuadPart);
    if (!*outData) {
        CloseHandle(hFile);
        return false;
    }

    DWORD bytesRead = 0;
    BOOL ok = ReadFile(hFile, *outData, (DWORD)fileSize.QuadPart, &bytesRead, NULL);
    CloseHandle(hFile);

    if (!ok) {
        free(*outData);
        *outData = NULL;
        return false;
    }

    *outSize = (int32_t)bytesRead;
    return true;
}

static bool xdkWriteFileBinary(FileSystem* fs, const char* relativePath, const uint8_t* data, int32_t size) {
    if (!data || size <= 0) return false;
    char* fullPath = xdkResolvePath(fs, relativePath);
    if (!fullPath) return false;

    HANDLE hFile = CreateFileA(fullPath, GENERIC_WRITE, 0,
                               NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    free(fullPath);

    if (hFile == INVALID_HANDLE_VALUE) return false;

    DWORD written;
    BOOL ok = WriteFile(hFile, data, (DWORD)size, &written, NULL);
    CloseHandle(hFile);
    return (ok && (int32_t)written == size);
}

// ===[ Streaming Binary I/O ]===

typedef struct {
    HANDLE hFile;
    char* fullPath;
} XdkBinaryHandle;

static XdkBinaryHandle* xdkBinaryHandleNew(HANDLE hFile, char* fullPath) {
    XdkBinaryHandle* h = (XdkBinaryHandle*)malloc(sizeof(XdkBinaryHandle));
    if (!h) { free(fullPath); return NULL; }
    h->hFile = hFile;
    h->fullPath = fullPath;
    return h;
}

static void* xdkBinaryOpen(FileSystem* fs, const char* relativePath, int32_t mode) {
    char* fullPath = xdkResolvePath(fs, relativePath);
    if (!fullPath) return NULL;

    HANDLE hFile = INVALID_HANDLE_VALUE;
    switch (mode) {
        case GML_FILE_BIN_READ:
            hFile = CreateFileA(fullPath, GENERIC_READ, FILE_SHARE_READ,
                                NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            break;
        case GML_FILE_BIN_WRITE:
            hFile = CreateFileA(fullPath, GENERIC_WRITE, 0,
                                NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            break;
        case GML_FILE_BIN_READWRITE:
            hFile = CreateFileA(fullPath, GENERIC_READ | GENERIC_WRITE, 0,
                                NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hFile == INVALID_HANDLE_VALUE) {
                hFile = CreateFileA(fullPath, GENERIC_READ | GENERIC_WRITE, 0,
                                    NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
            }
            break;
    }

    if (hFile == INVALID_HANDLE_VALUE) {
        free(fullPath);
        return NULL;
    }

    return xdkBinaryHandleNew(hFile, fullPath);
}

static void xdkBinaryClose(MAYBE_UNUSED FileSystem* fs, void* handle) {
    if (!handle) return;
    XdkBinaryHandle* h = (XdkBinaryHandle*)handle;
    if (h->hFile != INVALID_HANDLE_VALUE) CloseHandle(h->hFile);
    free(h->fullPath);
    free(h);
}

static int32_t xdkBinaryRead(MAYBE_UNUSED FileSystem* fs, void* handle, void* dst, int32_t n) {
    if (!handle || n <= 0) return 0;
    XdkBinaryHandle* h = (XdkBinaryHandle*)handle;
    DWORD bytesRead = 0;
    if (ReadFile(h->hFile, dst, (DWORD)n, &bytesRead, NULL)) {
        return (int32_t)bytesRead;
    }
    return 0;
}

static int32_t xdkBinaryWrite(MAYBE_UNUSED FileSystem* fs, void* handle, const void* src, int32_t n) {
    if (!handle || n <= 0) return 0;
    XdkBinaryHandle* h = (XdkBinaryHandle*)handle;
    DWORD written = 0;
    if (WriteFile(h->hFile, src, (DWORD)n, &written, NULL)) {
        return (int32_t)written;
    }
    return 0;
}

static int32_t xdkBinaryTell(MAYBE_UNUSED FileSystem* fs, void* handle) {
    if (!handle) return 0;
    XdkBinaryHandle* h = (XdkBinaryHandle*)handle;
    LARGE_INTEGER pos;
    pos.QuadPart = 0;
    if (SetFilePointerEx(h->hFile, pos, &pos, FILE_CURRENT)) {
        return (int32_t)pos.QuadPart;
    }
    return 0;
}

static bool xdkBinarySeek(MAYBE_UNUSED FileSystem* fs, void* handle, int32_t pos) {
    if (!handle) return false;
    XdkBinaryHandle* h = (XdkBinaryHandle*)handle;
    LARGE_INTEGER distance;
    distance.QuadPart = pos;
    return SetFilePointerEx(h->hFile, distance, NULL, FILE_BEGIN) != FALSE;
}

static int32_t xdkBinarySize(MAYBE_UNUSED FileSystem* fs, void* handle) {
    if (!handle) return 0;
    XdkBinaryHandle* h = (XdkBinaryHandle*)handle;
    LARGE_INTEGER size;
    if (GetFileSizeEx(h->hFile, &size)) {
        return (int32_t)size.QuadPart;
    }
    return 0;
}

static void xdkBinaryRewrite(MAYBE_UNUSED FileSystem* fs, void* handle) {
    if (!handle) return;
    XdkBinaryHandle* h = (XdkBinaryHandle*)handle;
    if (h->hFile != INVALID_HANDLE_VALUE) CloseHandle(h->hFile);
    h->hFile = CreateFileA(h->fullPath, GENERIC_READ | GENERIC_WRITE, 0,
                           NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

// ===[ Directory Operations ]===

static bool xdkDirectoryExists(FileSystem* fs, const char* relativePath) {
    char* fullPath = xdkResolvePath(fs, relativePath);
    if (!fullPath) return false;

    DWORD attributes = GetFileAttributesA(fullPath);
    free(fullPath);

    return (attributes != 0xFFFFFFFF && (attributes & FILE_ATTRIBUTE_DIRECTORY));
}

static bool xdkCreateDirectory(FileSystem* fs, const char* relativePath) {
    char* fullPath = xdkResolvePath(fs, relativePath);
    if (!fullPath) return false;

    BOOL ok = CreateDirectoryA(fullPath, NULL);
    free(fullPath);
    return (ok != FALSE);
}

static bool xdkDeleteDirectory(FileSystem* fs, const char* relativePath) {
    char* fullPath = xdkResolvePath(fs, relativePath);
    if (!fullPath) return false;

    BOOL ok = RemoveDirectoryA(fullPath);
    free(fullPath);
    return (ok != FALSE);
}

static FileSystemDirEntry* xdkListDirectory(FileSystem* fs, const char* relativeDirPath) {
    char* fullPath = xdkResolvePath(fs, relativeDirPath);
    if (!fullPath) return NULL;

    size_t dirLen = strlen(fullPath);
    char* search = (char*)malloc(dirLen + 3);
    if (!search) { free(fullPath); return NULL; }
    memcpy(search, fullPath, dirLen);
    search[dirLen] = '\\';
    search[dirLen + 1] = '*';
    search[dirLen + 2] = '\0';
    free(fullPath);

    WIN32_FIND_DATAA findData;
    HANDLE hFind = FindFirstFileA(search, &findData);
    free(search);

    if (hFind == INVALID_HANDLE_VALUE) return NULL;

    FileSystemDirEntry* list = NULL;
    do {
        const char* name = findData.cFileName;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

        bool dup = false;
        repeat(arrlen(list), i) {
            if (strcmp(list[i].name, name) == 0) { dup = true; break; }
        }
        if (dup) continue;

        FileSystemDirEntry entry = {0};
        entry.name = safeStrdup(name);
        entry.isDirectory = (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        arrput(list, entry);
    } while (FindNextFileA(hFind, &findData));

    FindClose(hFind);
    return list;
}

// ===[ Vtable ]===

static FileSystemVtable xdkFileSystemVtable = {
    xdkResolvePath,
    xdkFileExists,
    xdkReadFileText,
    xdkWriteFileText,
    xdkDeleteFile,
    xdkReadFileBinary,
    xdkWriteFileBinary,
    xdkBinaryOpen,
    xdkBinaryClose,
    xdkBinaryRead,
    xdkBinaryWrite,
    xdkBinaryTell,
    xdkBinarySeek,
    xdkBinarySize,
    xdkBinaryRewrite,
    xdkDirectoryExists,
    xdkCreateDirectory,
    xdkDeleteDirectory,
    xdkListDirectory,
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