/* Exercises the C API exactly as the iOS app does, from plain C.
 * Usage: capi_test <archive.dat> [stage] [retail|fixture]
 *   retail  (default): expectations for the real daybreak00.dat, stage 01
 *   fixture          : expectations for port/ios/Fixtures/fixture.dat (port/tools/make_fixture.py)
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

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <archive.dat> [stage] [retail|fixture]\n", argv[0]);
        return 2;
    }
    int stage = argc > 2 ? atoi(argv[2]) : 1;
    const struct expect* ex = (argc > 3 && strcmp(argv[3], "fixture") == 0) ? &FIXTURE : &RETAIL;
    char err[256];
    DBStageReport* r = db_stage_report_open(argv[1], stage, err, sizeof err);
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
    int pass = stage != 1 || (m0 == ex->m0 && t0 == ex->t0 && m1 == ex->m1 && t1 == ex->t1 && nt == ex->textures &&
                              ok == ex->resolved && first_jp == ex->first_jp && name_ok);
    printf("%s (%s expectations)\n", pass ? "PASS" : "FAIL", ex == &FIXTURE ? "fixture" : "retail");
    db_stage_report_free(r);
    return pass ? 0 : 1;
}
