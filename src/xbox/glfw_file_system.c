#include "glfw_file_system.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <windows.h>
#include <hal/debug.h>
#include <hal/xbox.h>
#include <hal/video.h>

// ===[ Helpers ]===

// taken from stackoverflow
char* str_replace(char *target, const char *needle, const char *replacement)
{
    char* buffer = malloc(1024);
    char *insert_point = &buffer[0];
    const char *tmp = target;
    size_t needle_len = strlen(needle);
    size_t repl_len = strlen(replacement);

    while (1) {
        const char *p = strstr(tmp, needle);

        // walked past last occurrence of needle; copy remaining part
        if (p == nullptr) {
            strcpy(insert_point, tmp);
            break;
        }

        // copy part before needle
        memcpy(insert_point, tmp, p - tmp);
        insert_point += p - tmp;

        // copy replacement string
        memcpy(insert_point, replacement, repl_len);
        insert_point += repl_len;

        // adjust pointers, move on
        tmp = p + needle_len;
    }

    // write altered string back to target
    // strcpy(target, buffer);
    return buffer;
}

size_t strlen_s(char* p, size_t s) {
    char* _p = p;
    size_t l = 0;
    while (_p && l < s) {
        _p++; l++;
    }
    
    return l;
}

static char* buildFullPath(GlfwFileSystem* fs, const char* relativePath) {
    if (strstr(relativePath, fs->basePath) == relativePath) {
        return (char*)relativePath;
    }

    const char* fullPath = safeMalloc((sizeof(char) * strlen(fs->basePath)) + (sizeof(char) * strlen(relativePath)) + 1);
    strcpy((char*)fullPath, fs->basePath);
    strcat((char*)fullPath, relativePath);

    char* windowsFullPath = str_replace((char*)fullPath, "/", "\\");

    free((char*)fullPath);

    return windowsFullPath;
}

// ===[ Vtable Implementations ]===

static char* glfwResolvePath(FileSystem* fs, const char* relativePath) {
    return buildFullPath((GlfwFileSystem*) fs, relativePath);
}

static bool fileExists(FileSystem* fs, const char* path) {
    char* windowsPath = str_replace(path, "/", "\\");
    printf("Trying to open %s...\n", windowsPath);
    
    FILE *file = fopen(windowsPath, "r");
    bool exists = false;
    
    if (file != nullptr) {
        fclose(file);
        exists = true;
    }
    
    if (exists) {
        printf("File %s found.\n", windowsPath);
    } else {
        printf("File %s not found.\n", windowsPath);
    }
    
    free(windowsPath);
    return exists;
}

#define TITLE_ID 0x12345678
#define SAVE_DIR "E:\\UDATA\\0x12345678\\000000000000"

static bool glfwFileExists(FileSystem* fs, const char* path) {
    bool d_exists = fileExists(fs, path);

    if (d_exists) return true;

    char* savePath = str_replace(path, "D:", SAVE_DIR);
    bool e_exists = fileExists(fs, savePath);

    free(savePath);

    return e_exists;
}

static char* glfwReadFileText(FileSystem* fs, const char* relativePath) {
    char* savePath = str_replace(relativePath, "D:", SAVE_DIR);
    char* fullPath;
    
    if (fileExists(fs, savePath)) {
        fullPath = savePath;
        printf("Reading override file %s...\n", fullPath);
    } else {
        fullPath = buildFullPath((GlfwFileSystem*) fs, relativePath);
        printf("Reading base file %s...\n", fullPath);
        free(savePath); // FIXED: Prevent memory leak
    }

    Sleep(10);

    FILE* f = fopen(fullPath, "rb");
    free(fullPath); // Safely frees whichever string fullPath ended up pointing to

    if (f == nullptr) return nullptr;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* content = (char*)safeMalloc((size_t) size + 1);
    size_t bytesRead = fread(content, 1, (size_t) size, f);
    content[bytesRead] = '\0';
    fclose(f);

    return content;
}

static bool glfwWriteFileText(FileSystem* fs, const char* relativePath, const char* contents) {
    char* fullPath;
    
    // Check if the path actually contained D: and needs redirecting
    if (strstr(relativePath, "D:") != nullptr) {
        fullPath = str_replace(relativePath, "D:", SAVE_DIR);
    } else {
        // If it's a standard relative write, build the full path normally
        fullPath = buildFullPath((GlfwFileSystem*) fs, relativePath);
    }

    printf("Writing file %s...\n", fullPath);

    Sleep(10);
    
    FILE* f = fopen(fullPath, "wb");
    free(fullPath);

    if (f == nullptr) return false;

    size_t len = strlen(contents);
    size_t written = fwrite(contents, 1, len, f);
    fclose(f);

    return written == len;
}

static bool glfwDeleteFile(FileSystem* fs, const char* relativePath) {
    char* savePath = str_replace(relativePath, "D:", SAVE_DIR);
    char* fullPath;
    
    if (glfwFileExists(fs, savePath)) {
        fullPath = savePath;
        printf("Deleting override file %s...\n", savePath);
    } else {
        fullPath = buildFullPath((GlfwFileSystem*) fs, relativePath);
        printf("Deleting base file %s...\n", fullPath);
        free(savePath);
    }

    Sleep(10);
    
    int result = remove(fullPath);
    free(fullPath);
    
    return result == 0;
}

// ===[ Vtable ]===

static FileSystemVtable glfwFileSystemVtable = {
    .resolvePath = glfwResolvePath,
    .fileExists = glfwFileExists,
    .readFileText = glfwReadFileText,
    .writeFileText = glfwWriteFileText,
    .deleteFile = glfwDeleteFile,
};

// ===[ Lifecycle ]===

GlfwFileSystem* GlfwFileSystem_create(const char* dataWinPath) {
    GlfwFileSystem* fs = safeCalloc(1, sizeof(GlfwFileSystem));
    fs->base.vtable = &glfwFileSystemVtable;
    bool dataWinExists = glfwFileExists((FileSystem*) fs, dataWinPath);
    if (!dataWinExists) {
        debugPrint("Error: file %s not found! Rebooting in 10s...\n", dataWinPath);
        Sleep(10000);
        XReboot();
    }

    const char* lastSlash = strrchr(dataWinPath, '/');
    const char* lastBackslash = strrchr(dataWinPath, '\\');

    const char* lastSep = (lastSlash > lastBackslash) ? lastSlash : lastBackslash;

    if (lastSep != nullptr) {
        size_t dirLen = (size_t) (lastSep - dataWinPath + 1);
        fs->basePath = safeMalloc(dirLen + 1);
        memcpy(fs->basePath, dataWinPath, dirLen);
        fs->basePath[dirLen] = '\0';
    } else {
        fs->basePath = safeStrdup("./");
    }

    return fs;
}

void GlfwFileSystem_destroy(GlfwFileSystem* fs) {
    if (fs == nullptr) return;
    free(fs->basePath);
    free(fs);
}