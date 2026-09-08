/* Independent SDK-shaped declarations compiled for BOTH test architectures. */
#include <stdint.h>
#include <stdbool.h>
#pragma pack(push, 1)
typedef struct { bool bState, bActive; } Digital;
typedef struct { int eMode; float x, y; bool bActive; } Analog;
typedef struct { float values[10]; } Motion;
typedef struct { uint8_t address[16]; int type; } IP;
typedef struct {
    uint64_t controller; int kind;
    union {
        struct { uint64_t action; Analog data; } analog;
        struct { uint64_t action; Digital data; } digital;
    } value;
} Event;
#pragma pack(pop)
#pragma pack(push, 4)
typedef struct { int type; uint64_t id; } Location;
typedef struct { const char **strings; int count; } Tags;
#pragma pack(pop)
#define DEVICE UINT64_C(0xfedcba9876543210)
#define ACTION UINT64_C(0xabcdef0123456789)
