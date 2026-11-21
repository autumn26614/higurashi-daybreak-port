/* Plain C API over the stage reader, for Swift (bridging header) or any C caller.
 * Read-only; never modifies the archive. Strings are returned as raw cp932
 * (Shift-JIS) bytes plus a length: decode them on the caller side (on Apple
 * platforms: kCFStringEncodingDOSJapanese). Pointers stay valid until
 * db_stage_report_free(). */
#ifndef DAYBREAK_C_H
#define DAYBREAK_C_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DBStageReport DBStageReport;

/* Opens the archive and summarises one stage. Returns NULL on failure and writes a
 * NUL-terminated message into err (if err_len > 0). */
DBStageReport* db_stage_report_open(const char* dat_path, int32_t stage, char* err, int32_t err_len);
/* Same, over several archives searched in the given order; the first that has a
 * name wins. Kai: { "daybreak04.dat", "daybreak00.dat" }. */
DBStageReport* db_stage_report_open_set(const char* const* dat_paths, int32_t count, int32_t stage, char* err,
                                        int32_t err_len);
void db_stage_report_free(DBStageReport* r);

/* variant 0 = plain "%02d.x" set (game default), 1 = "%02db.x" set */
int32_t db_stage_mesh_count(const DBStageReport* r, int32_t variant);
int64_t db_stage_triangle_count(const DBStageReport* r, int32_t variant);
int32_t db_stage_error_count(const DBStageReport* r);

int32_t db_stage_texture_count(const DBStageReport* r);
int32_t db_stage_texture_resolved_count(const DBStageReport* r);
/* Texture i, in raw-byte sort order (same order as the stageinfo CLI). */
const char* db_stage_texture_name(const DBStageReport* r, int32_t i, int32_t* len);
const char* db_stage_texture_archive_name(const DBStageReport* r, int32_t i, int32_t* len);
int32_t db_stage_texture_found(const DBStageReport* r, int32_t i);
int32_t db_stage_texture_mips(const DBStageReport* r, int32_t i);

/* ---- Archives and images ------------------------------------------------------------
 * A set of archives opened once, searched in the given order (Kai: daybreak04, then
 * daybreak00). Not thread-safe: use one DBArchives from one thread at a time. */
typedef struct DBArchives DBArchives;
DBArchives* db_archives_open(const char* const* dat_paths, int32_t count, char* err, int32_t err_len);
void db_archives_free(DBArchives* a);

/* Decodes one image .cnv. name = raw cp932 bytes of the entry; a name not ending in
 * ".cnv" (the ".bmp" the game asks for) has its last three bytes swapped for "cnv",
 * as the engine does. Returns NULL with a message in err on failure. */
typedef struct DBImage DBImage;
DBImage* db_image_load(DBArchives* a, const char* name, int32_t name_len, char* err, int32_t err_len);
void db_image_free(DBImage* img);
int32_t db_image_width(const DBImage* img);  /* pixels per row */
int32_t db_image_height(const DBImage* img);
int32_t db_image_bpp(const DBImage* img);    /* 24 or 32 as stored; output is always RGBA */
/* width*height*4 bytes, R,G,B,A, top row first, straight (non-premultiplied) alpha. */
const uint8_t* db_image_rgba(const DBImage* img);

#ifdef __cplusplus
}
#endif
#endif
