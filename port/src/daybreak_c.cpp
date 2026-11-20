#include "daybreak_c.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <string>

#include "archive.h"
#include "stage.h"

struct DBStageReport {
    db::StageSummary summary;
};

namespace {
void set_err(char* err, int32_t err_len, const std::string& msg) {
    if (!err || err_len <= 0) return;
    size_t n = std::min(msg.size(), size_t(err_len - 1));
    memcpy(err, msg.data(), n);
    err[n] = 0;
}
bool valid(const DBStageReport* r, int32_t i) {
    return r && i >= 0 && size_t(i) < r->summary.textures.size();
}
}  // namespace

extern "C" {

DBStageReport* db_stage_report_open(const char* dat_path, int32_t stage, char* err, int32_t err_len) {
    if (!dat_path) {
        set_err(err, err_len, "no path");
        return nullptr;
    }
    db::Archive a;
    std::string e;
    if (!a.open(dat_path, &e)) {
        set_err(err, err_len, e);
        return nullptr;
    }
    auto* r = new (std::nothrow) DBStageReport;
    if (!r) {
        set_err(err, err_len, "out of memory");
        return nullptr;
    }
    r->summary = db::summarize_stage(a, stage);
    set_err(err, err_len, "");
    return r;
}

void db_stage_report_free(DBStageReport* r) { delete r; }

int32_t db_stage_mesh_count(const DBStageReport* r, int32_t variant) {
    return (r && (variant == 0 || variant == 1)) ? r->summary.meshes[variant] : -1;
}
int64_t db_stage_triangle_count(const DBStageReport* r, int32_t variant) {
    return (r && (variant == 0 || variant == 1)) ? int64_t(r->summary.tris[variant]) : -1;
}
int32_t db_stage_error_count(const DBStageReport* r) { return r ? int32_t(r->summary.errors.size()) : -1; }

int32_t db_stage_texture_count(const DBStageReport* r) { return r ? int32_t(r->summary.textures.size()) : -1; }
int32_t db_stage_texture_resolved_count(const DBStageReport* r) {
    if (!r) return -1;
    int32_t n = 0;
    for (const auto& t : r->summary.textures) n += t.status.found ? 1 : 0;
    return n;
}
const char* db_stage_texture_name(const DBStageReport* r, int32_t i, int32_t* len) {
    if (!valid(r, i)) return nullptr;
    const auto& s = r->summary.textures[size_t(i)].name;
    if (len) *len = int32_t(s.size());
    return s.data();
}
const char* db_stage_texture_archive_name(const DBStageReport* r, int32_t i, int32_t* len) {
    if (!valid(r, i)) return nullptr;
    const auto& s = r->summary.textures[size_t(i)].status.archive_name;
    if (len) *len = int32_t(s.size());
    return s.data();
}
int32_t db_stage_texture_found(const DBStageReport* r, int32_t i) {
    return valid(r, i) ? (r->summary.textures[size_t(i)].status.found ? 1 : 0) : -1;
}
int32_t db_stage_texture_mips(const DBStageReport* r, int32_t i) {
    return valid(r, i) ? r->summary.textures[size_t(i)].status.mips : -1;
}

}  // extern "C"
