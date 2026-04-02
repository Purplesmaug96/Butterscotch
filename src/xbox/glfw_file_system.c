#include "glfw_file_system.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ===[ Helpers ]===

static char* buildFullPath(GlfwFileSystem* fs, const char* relativePath) {
    if(strncmp(relativePath, fs->basePath, strlen(fs->basePath)) == 0) {
        return safeStrdup(relativePath);
    }

    size_t baseLen = strlen(fs->basePath);
    size_t relLen = strlen(relativePath);
    char* fullPath = safeMalloc(baseLen + relLen + 1);

    memcpy(fullPath, fs->basePath, baseLen);
    memcpy(fullPath + baseLen, relativePath, relLen);
    fullPath[baseLen + relLen] = '\0';

    return fullPath;
}

// ===[ Vtable Implementations ]===

static char* glfwResolvePath(FileSystem* fs, const char* relativePath) {
    return buildFullPath((GlfwFileSystem*) fs, relativePath);
}

static bool glfwFileExists(FileSystem* fs, const char* relativePath) {

    printf("Trying to open %s...\n", relativePath);
    FILE *file = fopen(relativePath, "r");
    bool exists = false;
    if (file != nullptr)
    {
        fclose(file);
        exists = true;
    }
    if (exists) {
        printf("File %s found.\n", relativePath);
    }
    else {
        printf("File %s not found.\n", relativePath);
    }
    return exists;
}

static char* glfwReadFileText(FileSystem* fs, const char* relativePath) {
    char* fullPath = buildFullPath((GlfwFileSystem*) fs, relativePath);
    FILE* f = fopen(fullPath, "rb");
    free(fullPath);

    if (f == nullptr) return nullptr;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* content = safeMalloc((size_t) size + 1);
    size_t bytesRead = fread(content, 1, (size_t) size, f);
    content[bytesRead] = '\0';
    fclose(f);

    return content;
}

static bool glfwWriteFileText(FileSystem* fs, const char* relativePath, const char* contents) {
    char* fullPath = buildFullPath((GlfwFileSystem*) fs, relativePath);
    FILE* f = fopen(fullPath, "wb");
    free(fullPath);

    if (f == nullptr) return false;

    size_t len = strlen(contents);
    size_t written = fwrite(contents, 1, len, f);
    fclose(f);

    return written == len;
}

static bool glfwDeleteFile(FileSystem* fs, const char* relativePath) {
    char* fullPath = buildFullPath((GlfwFileSystem*) fs, relativePath);
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