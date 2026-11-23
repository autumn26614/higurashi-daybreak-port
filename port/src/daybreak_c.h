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

/* ---- Scenario scripts (src/scenario.h) ---------------------------------------------
 * Scenario scripts (data/scenario/..., .txt) in the set, deduplicated (first archive wins), sorted
 * case-insensitively. Names are raw cp932; pointers live as long as the DBArchives. */
int32_t db_archives_script_count(DBArchives* a);
const char* db_archives_script_name(DBArchives* a, int32_t i, int32_t* len);

/* A parsed script: pages = text lines, each with the VOICE clip before it (if any).
 * Tokens of a page: kind DB_TOK_*, the exact source bytes (cp932), and for a colour
 * 0xRRGGBB. Pointers stay valid until db_script_free. */
typedef struct DBScript DBScript;
enum { DB_TOK_TEXT = 0, DB_TOK_COLOR = 1, DB_TOK_COLOR_RESET = 2, DB_TOK_NEWLINE = 3, DB_TOK_UNKNOWN = 4 };
DBScript* db_script_load(DBArchives* a, const char* name, int32_t name_len, char* err, int32_t err_len);
DBScript* db_script_parse(const uint8_t* bytes, int32_t len);
void db_script_free(DBScript* s);
int32_t db_script_page_count(const DBScript* s);
const char* db_script_page_voice(const DBScript* s, int32_t page, int32_t* len); /* "" if none */
int32_t db_script_token_count(const DBScript* s, int32_t page);
int32_t db_script_token_kind(const DBScript* s, int32_t page, int32_t tok);
const char* db_script_token_bytes(const DBScript* s, int32_t page, int32_t tok, int32_t* len);
uint32_t db_script_token_rgb(const DBScript* s, int32_t page, int32_t tok);

/* ---- Identifying a picked file -----------------------------------------------------
 * Roles, by entry count plus a known member (src/identify.h). */
enum {
    DB_ROLE_UNKNOWN = 0,
    DB_ROLE_BASE00 = 1, /* daybreak00.dat: stages, scenes, scripts */
    DB_ROLE_BASE01 = 2, /* daybreak01.dat: BGM */
    DB_ROLE_BASE02 = 3, /* daybreak02.dat: movies */
    DB_ROLE_KAI04 = 4,  /* daybreak04.dat: Kai data, searched before daybreak00 */
    DB_ROLE_KAI05 = 5,  /* daybreak05.dat: Kai BGM */
    DB_ROLE_KAI06 = 6   /* daybreak06.dat: Kai movies */
};
typedef struct {
    int32_t role;            /* DB_ROLE_* */
    int32_t entries;
    int64_t file_size;
    int32_t scripts;         /* scenario scripts in the archive */
    int32_t english_scripts; /* of those, ASCII-only (English fan translation applied) */
} DBArchiveInfo;
/* Opens and validates the archive at path, fills *out. Returns 1 on success, 0 with a
 * message in err if the file cannot be read or its index is corrupt. */
int32_t db_identify_archive(const char* path, DBArchiveInfo* out, char* err, int32_t err_len);
/* "daybreak00.dat" etc. for a role; "" for DB_ROLE_UNKNOWN. Static storage. */
const char* db_role_file_name(int32_t role);

#ifdef __cplusplus
}
#endif
#endif
