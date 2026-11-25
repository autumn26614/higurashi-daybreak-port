/* Empty module tables, so the runtime links without the translated game (CI, and app builds
 * made without a copy of the game). They are weak: the generated files' tables replace them. */
#include "x86.h"

#define STUB(P)                                                                               \
    __attribute__((weak)) const uint32_t P##_count = 0;                                       \
    __attribute__((weak)) const FnEntry P##_table[1] = {{0, 0}};                              \
    __attribute__((weak)) const uint32_t P##_import_count = 0;                                \
    __attribute__((weak)) const ImportSlot P##_imports[1] = {{0, 0, 0}};                      \
    __attribute__((weak)) const uint32_t P##_export_count = 0;                                \
    __attribute__((weak)) const ExportSlot P##_exports[1] = {{0, 0, 0}};                      \
    __attribute__((weak)) const uint32_t P##_base = 0;                                        \
    __attribute__((weak)) const char P##_source[] = "";

STUB(F)
STUB(X)
