#ifndef _BS_BINARY_READER_H_
#define _BS_BINARY_READER_H_

#include "common.h"
#include <stdint.h>
#include <stddef.h>
#include "stdio_compat.h"
#include <stdbool.h>

#include "binary_utils.h"

typedef struct {
    FILE* file;
    size_t fileSize;

    // When non-null, reads come from this memory buffer instead of the FILE*
    // bufferBase is the absolute file offset that buffer[0] corresponds to
    uint8_t* buffer;
    size_t bufferBase;
    size_t bufferSize;
    size_t bufferPos; // current read position relative to bufferBase
} BinaryReader;

BinaryReader BinaryReader_create(FILE* file, size_t fileSize);

// Sets a memory buffer for bulk chunk reading
// All subsequent reads will come from this buffer until it is cleared
// baseOffset is the absolute file offset that buffer[0] corresponds to
void BinaryReader_setBuffer(BinaryReader* reader, uint8_t* buffer, size_t baseOffset, size_t size);

// Clears the memory buffer, reverting to FILE*-based reads
void BinaryReader_clearBuffer(BinaryReader* reader);

void BinaryReader_readCheck(BinaryReader* reader, void* dest, const size_t bytes);

static inline uint8_t BinaryReader_readUint8(BinaryReader* reader) {
    uint8_t value;
    BinaryReader_readCheck(reader, &value, 1);
    return value;
}

static inline int16_t BinaryReader_readInt16(BinaryReader* reader) {
    uint16_t value;
    BinaryReader_readCheck(reader, &value, sizeof(value));
    return (int16_t) BinaryUtils_toLittle16(value);
}

static inline uint16_t BinaryReader_readUint16(BinaryReader* reader) {
    uint16_t value;
    BinaryReader_readCheck(reader, &value, sizeof(value));
    return BinaryUtils_toLittle16(value);
}

static inline int32_t BinaryReader_readInt32(BinaryReader* reader) {
    uint32_t value;
    BinaryReader_readCheck(reader, &value, sizeof(value));
    return (int32_t) BinaryUtils_toLittle32(value);
}

static inline uint32_t BinaryReader_readUint32(BinaryReader* reader) {
    uint32_t value;
    BinaryReader_readCheck(reader, &value, sizeof(value));
    return BinaryUtils_toLittle32(value);
}

static inline float BinaryReader_readFloat32(BinaryReader* reader) {
    uint32_t bits;
    float value;
    BinaryReader_readCheck(reader, &bits, sizeof(bits));
    bits = BinaryUtils_toLittle32(bits);
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static inline uint64_t BinaryReader_readUint64(BinaryReader* reader) {
    uint64_t value;
    BinaryReader_readCheck(reader, &value, sizeof(value));
    return BinaryUtils_toLittle64(value);
}

static inline int64_t BinaryReader_readInt64(BinaryReader* reader) {
    uint64_t value;
    BinaryReader_readCheck(reader, &value, sizeof(value));
    return (int64_t) BinaryUtils_toLittle64(value);
}

static inline bool BinaryReader_readBool32(BinaryReader* reader) {
    return BinaryReader_readUint32(reader) != 0;
}

// Copies 'count' bytes from the current position into 'dest'.
void BinaryReader_readBytes(BinaryReader* reader, void* dest, size_t count);

// Reads 'count' bytes from position 'offset' into a newly allocated buffer.
// Caller must free the returned buffer.
uint8_t* BinaryReader_readBytesAt(BinaryReader* reader, size_t offset, size_t count);

void BinaryReader_skip(BinaryReader* reader, size_t bytes);
void BinaryReader_seek(BinaryReader* reader, size_t position);
size_t BinaryReader_getPosition(BinaryReader* reader);

#endif /* _BS_BINARY_READER_H_ */
