/* Exercises the C API exactly as the iOS app does, from plain C.
 * Usage: capi_test <archive.dat> [stage] [retail|fixture]
 *        capi_test <daybreak00.dat> 16 kai <daybreak04.dat>
 *   retail  (default): expectations for the real daybreak00.dat, stage 01
 *   fixture          : expectations for port/ios/Fixtures/fixture.dat (port/tools/make_fixture.py)
 *   kai              : daybreak04 searched before daybreak00 (db_stage_report_open_set), stage 16
 * The first Japanese name is printed as hex so the check does not depend on the
 * terminal's encoding. Exit status 0 = PASS. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/daybreak_c.h"

struct expect {
    int m0, m1, textures, resolved, first_jp;
    long long t0, t1;
    const char* jp_cp932; /* raw bytes of the first Japanese texture name */
};

/* くさむら06.bmp */
static const struct expect RETAIL = {27, 26, 85, 85, 3, 28372, 28148,
                                     "\x82\xad\x82\xb3\x82\xde\x82\xe7" "06.bmp"};
/* テスト草01.bmp */
static const struct expect FIXTURE = {1, 0, 1, 1, 0, 1, 0, "\x83\x65\x83\x58\x83\x67\x91\x90" "01.bmp"};
/* Kai, daybreak04 then daybreak00, stage 16 (Kai-only): アスファルト05.bmp */
static const struct expect KAI = {40, 40, 38, 38, 1, 23961, 23961,
                                  "\x83\x41\x83\x58\x83\x74\x83\x40\x83\x8b\x83\x67" "05.bmp"};

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <archive.dat> [stage] [retail|fixture]\n", argv[0]);
        return 2;
    }
    int stage = argc > 2 ? atoi(argv[2]) : 1;
    const char* mode = argc > 3 ? argv[3] : "retail";
    int kai = strcmp(mode, "kai") == 0;
    if (kai && argc < 5) {
        fprintf(stderr, "kai mode: %s <daybreak00.dat> 16 kai <daybreak04.dat>\n", argv[0]);
        return 2;
    }
    const struct expect* ex = kai ? &KAI : strcmp(mode, "fixture") == 0 ? &FIXTURE : &RETAIL;
    int checked_stage = kai ? 16 : 1;
    char err[256];
    /* Kai search order, as the app passes it: daybreak04 first, then daybreak00. */
    const char* paths[2] = {kai ? argv[4] : argv[1], argv[1]};
    DBStageReport* r = kai ? db_stage_report_open_set(paths, 2, stage, err, sizeof err)
                           : db_stage_report_open(argv[1], stage, err, sizeof err);
    if (!r) {
        fprintf(stderr, "open failed: %s\n", err);
        return 1;
    }
    int m0 = db_stage_mesh_count(r, 0), m1 = db_stage_mesh_count(r, 1);
    long long t0 = (long long)db_stage_triangle_count(r, 0), t1 = (long long)db_stage_triangle_count(r, 1);
    int nt = db_stage_texture_count(r), ok = db_stage_texture_resolved_count(r);
    printf("stage %02d plain: %d meshes / %lld tris\n", stage, m0, t0);
    printf("stage %02d b:     %d meshes / %lld tris\n", stage, m1, t1);
    printf("textures: %d referenced, %d resolved, %d parse errors\n", nt, ok, db_stage_error_count(r));
    int first_jp = -1, name_ok = 0;
    for (int i = 0; i < nt && first_jp < 0; i++) {
        int len = 0;
        const unsigned char* s = (const unsigned char*)db_stage_texture_name(r, i, &len);
        for (int k = 0; k < len; k++)
            if (s[k] >= 0x80) { first_jp = i; break; }
    }
    if (first_jp >= 0) {
        int len = 0, alen = 0;
        const char* s = db_stage_texture_name(r, first_jp, &len);
        db_stage_texture_archive_name(r, first_jp, &alen);
        printf("first Japanese name: index %d, cp932 hex", first_jp);
        for (int k = 0; k < len; k++) printf(" %02x", (unsigned char)s[k]);
        printf("\n  archive name bytes: %d, found %d, mips %d\n", alen, db_stage_texture_found(r, first_jp),
               db_stage_texture_mips(r, first_jp));
        name_ok = (size_t)len == strlen(ex->jp_cp932) && memcmp(s, ex->jp_cp932, (size_t)len) == 0;
    }
    int pass = stage != checked_stage || (m0 == ex->m0 && t0 == ex->t0 && m1 == ex->m1 && t1 == ex->t1 && nt == ex->textures &&
                              ok == ex->resolved && first_jp == ex->first_jp && name_ok);
    /* Voice categories: the game's alphabetical table. */
    int voices_ok = strcmp(db_voice_category(1), "attackA") == 0 && strcmp(db_voice_category(8), "damage") == 0 &&
                    strcmp(db_voice_category(38), "thank") == 0 && db_voice_category(39) == NULL &&
                    db_voice_category(-1) == NULL;
    if (!voices_ok) printf("voice category table wrong\n");
    pass = pass && voices_ok;
    printf("%s (%s expectations)\n", pass ? "PASS" : "FAIL", mode);
    db_stage_report_free(r);
    return pass ? 0 : 1;
}
