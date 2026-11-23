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

/* ---- Stage geometry (src/geometry.h) ------------------------------------------------
 * Every visual mesh of one stage as draw batches: one per (slot 0-7, mesh, material).
 * Positions are Direct3D world coordinates (left-handed, y up); triangles keep the
 * original winding (front faces clockwise on screen). Pointers stay valid until
 * db_geometry_free; the DBArchives may be freed earlier. */
typedef struct DBStageGeometry DBStageGeometry;
DBStageGeometry* db_geometry_open(DBArchives* a, int32_t stage, int32_t variant, char* err, int32_t err_len);
void db_geometry_free(DBStageGeometry* g);
int32_t db_geometry_batch_count(const DBStageGeometry* g);
int64_t db_geometry_triangle_count(const DBStageGeometry* g);
int32_t db_geometry_error_count(const DBStageGeometry* g);

typedef struct {
    int32_t slot, mesh, material;
    int32_t vertex_count;
    const float* positions;  /* xyz per vertex */
    const float* uvs;        /* uv per vertex */
    const uint8_t* colors;   /* RGBA per vertex (MeshVertexColors, else 255s) */
    int32_t has_colors;
    int32_t index_count;
    const uint32_t* indices; /* triangles */
    float diffuse[4];        /* material faceColor RGBA */
    float power;             /* != 0: drawn additively by the game */
    float specular[3];
    float emissive[3];
    const char* texture;     /* raw cp932 archive entry (".cnv"); "" if none */
    int32_t texture_len;
    int32_t texture_found;
    const float* normals;    /* xyz per vertex for the outline pass (characters), or NULL */
} DBBatch;
int32_t db_geometry_batch(const DBStageGeometry* g, int32_t i, DBBatch* out); /* 1 = ok */

typedef struct {
    int32_t present;           /* env.dat was found */
    int32_t fog_start, fog_end;
    uint8_t fog_rgb[3];
    float spawn[4][3];         /* players 1-4 */
    float bounds_min[3], bounds_max[3]; /* all batches except the sky (slot 0) */
} DBStageEnv;
int32_t db_geometry_env(const DBStageGeometry* g, DBStageEnv* out);

/* 1 if the set has any visual mesh file for this stage (data/stageNN/00.x .. 07.x). */
int32_t db_archives_has_stage(DBArchives* a, int32_t stage);

/* Camera (src/camera.h): Direct3D convention, left-handed, y up. */
typedef struct {
    float eye[3], target[3];
    float fov_y, z_near, z_far;
} DBCamera;
/* A battle-like starting view over the arena (env.dat), or over the mesh bounds. */
int32_t db_geometry_default_camera(const DBStageGeometry* g, DBCamera* out);
/* D3DXMatrixLookAtLH / PerspectiveFovLH, row-major for row vectors (v' = v * M).
 * Loaded as a column-major float4x4 (Metal, simd) they are the transposes, so
 * M_metal * v == v * M_d3d: use them as they are with column vectors. */
void db_camera_matrices(const DBCamera* c, float aspect, float view[16], float proj[16]);
/* CPU reference render (src/raster.h) into width*height*4 RGBA bytes. Returns the
 * number of pixels drawn, or -1 on bad arguments. Textures are read from `a`. */
int64_t db_render_reference(DBArchives* a, const DBStageGeometry* g, const DBCamera* c, int32_t width, int32_t height,
                            uint8_t* rgba);

/* ---- Sound (src/audio.h) -------------------------------------------------------------
 * Music (.ogg files under data/bgm, with .sfl loop points) and effects (data/se, data/sysse) in the
 * set, sorted. A sound decodes to interleaved 16-bit PCM. */
int32_t db_archives_sound_count(DBArchives* a);
const char* db_archives_sound_name(DBArchives* a, int32_t i, int32_t* len);
typedef struct DBSound DBSound;
DBSound* db_sound_load(DBArchives* a, const char* name, int32_t name_len, char* err, int32_t err_len);
void db_sound_free(DBSound* s);
int32_t db_sound_channels(const DBSound* s);
int32_t db_sound_rate(const DBSound* s);
int64_t db_sound_frames(const DBSound* s);
const int16_t* db_sound_samples(const DBSound* s);
int64_t db_sound_loop_start(const DBSound* s); /* -1: play once */
int64_t db_sound_loop_end(const DBSound* s);
/* Archive entry for a scenario VOICE argument ("W05-08-1.wav"); writes "" if none.
 * Returns the length written. */
int32_t db_voice_entry(DBArchives* a, const char* voice, int32_t voice_len, char* out, int32_t out_len);

/* ---- Characters (src/character.h) ---------------------------------------------------
 * data/<name>/0N.X models across the set (first archive wins), sorted. A pose is
 * returned as a DBStageGeometry (batches in slot 1, lighting baked into the vertex
 * colours, no env) for the same renderers; free it with db_geometry_free. */
int32_t db_archives_character_count(DBArchives* a);
const char* db_archives_character_name(DBArchives* a, int32_t i, int32_t* len);
typedef struct DBCharacter DBCharacter;
DBCharacter* db_character_open(DBArchives* a, const char* name, int32_t name_len, char* err, int32_t err_len);
void db_character_free(DBCharacter* c);
int32_t db_character_anim_count(const DBCharacter* c);
const char* db_character_anim_name(const DBCharacter* c, int32_t i, int32_t* len);
double db_character_anim_seconds(const DBCharacter* c, int32_t i);
DBStageGeometry* db_character_pose(const DBCharacter* c, int32_t anim, double seconds); /* anim -1: bind pose */
int32_t db_geometry_character_camera(const DBStageGeometry* g, DBCamera* out);

/* Draw passes in game order (src/recipe.h). blend: 0 = alpha (SRCALPHA, INVSRCALPHA),
 * 1 = multiply (DESTCOLOR, ZERO), 2 = additive (SRCALPHA, ONE). A batch whose material
 * power != 0 is drawn additively with no Z-write and no fog, whatever its pass says. */
typedef struct {
    int32_t slot, blend, z_write, fog, alpha_test, point_sampling;
    int32_t mode3; /* vertex colours as render mode 3 (src/recipe.h) when the geometry uses it */
} DBPass;
/* 1 if the stage is drawn in render mode 3 (the plain set; 0 for the "b" set, characters). */
int32_t db_geometry_mode3(const DBStageGeometry* g);
int32_t db_stage_pass_count(void);
int32_t db_stage_pass(int32_t i, DBPass* out); /* 1 = ok */

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

/* ---- Talk text box (src/talk.h) -----------------------------------------------------
 * One page of text laid out as the talk scene does (MS Gothic 24, fixed 12/24 px cells,
 * wrap 760, 5 lines, gradient). The caller rasterises each glyph code with a font and
 * hands in coverage masks (0..16, as GGO_GRAY4); db_talk_compose then draws the scene's
 * 800x600 frame: window at 50%, shadowed text revealed cell by cell, arrow. */
typedef struct DBTalk DBTalk;
enum { DB_TALK_WIDTH = 800, DB_TALK_HEIGHT = 600 };
DBTalk* db_talk_layout(const char* text, int32_t len); /* raw cp932 page text */
void db_talk_free(DBTalk* t);
int32_t db_talk_glyph_count(const DBTalk* t);
/* code: one byte, or lead << 8 | trail; x, y: cell origin in the text bitmap. */
int32_t db_talk_glyph(const DBTalk* t, int32_t i, uint32_t* code, int32_t* bytes, int32_t* x, int32_t* y);
int32_t db_talk_text_bytes(const DBTalk* t); /* strlen: where the reveal counter ends */
int32_t db_talk_lines(const DBTalk* t);
int32_t db_talk_stopped(const DBTalk* t);    /* 1 if text ran past the box */
int32_t db_talk_set_mask(DBTalk* t, uint32_t code, const uint8_t* coverage, int32_t width, int32_t height,
                         int32_t left, int32_t top);
/* The script's background (src/talk.h, talk::background) into 800x600 RGBA: win quotes
 * get the two characters' portraits, other scripts black. */
int32_t db_talk_background(DBArchives* a, const char* script, int32_t script_len, uint8_t* out_rgba);
/* background: 800x600 RGBA or NULL (black); window: decoded data/talk/window.cnv. */
int32_t db_talk_compose(DBTalk* t, const uint8_t* background, const uint8_t* window, int32_t window_w,
                        int32_t window_h, int32_t reveal, int32_t arrow, uint8_t* out_rgba);

/* ---- Story scenes (src/story.h) ------------------------------------------------------
 * Any scenario script as the talk scene plays it: pages of text, each with its voice
 * entry, music and scene (background + up to six portraits). Win quotes (no commands)
 * get the win screen's two portraits. Pointers live until db_story_free. */
typedef struct DBStory DBStory;
DBStory* db_story_load(DBArchives* a, const char* name, int32_t name_len, char* err, int32_t err_len);
void db_story_free(DBStory* s);
int32_t db_story_page_count(const DBStory* s);
const char* db_story_page_text(const DBStory* s, int32_t page, int32_t* len);  /* raw cp932 */
const char* db_story_page_voice(const DBStory* s, int32_t page, int32_t* len); /* entry or "" */
const char* db_story_page_bgm(const DBStory* s, int32_t page, int32_t* len);   /* entry or "" */
int32_t db_story_page_sleep_ms(const DBStory* s, int32_t page);
/* The page's 800x600 RGBA background, ready for db_talk_compose, once its portrait moves
 * have finished. _at: `frame` frames after the page opened (portraits mid-slide). */
int32_t db_story_background(DBArchives* a, const DBStory* s, int32_t page, uint8_t* out_rgba);
int32_t db_story_background_at(DBArchives* a, DBStory* s, int32_t page, int32_t frame, uint8_t* out_rgba);
/* Frames until the page's portrait moves finish (0: nothing moves). */
int32_t db_story_page_settle_frames(const DBStory* s, int32_t page);

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
