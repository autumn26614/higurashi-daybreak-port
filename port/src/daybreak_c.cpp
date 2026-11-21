#include "daybreak_c.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <string>

#include <memory>
#include <vector>

#include "archive.h"
#include "cnv.h"
#include "stage.h"

struct DBStageReport {
    db::StageSummary summary;
};

struct DBArchives {
    std::vector<std::unique_ptr<db::Archive>> archives;
    db::ArchiveSet set;
};

struct DBImage {
    db::Image image;
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

DBStageReport* db_stage_report_open_set(const char* const* dat_paths, int32_t count, int32_t stage, char* err,
                                        int32_t err_len) {
    if (!dat_paths || count <= 0) {
        set_err(err, err_len, "no path");
        return nullptr;
    }
    std::vector<db::Archive> archives(static_cast<size_t>(count));
    db::ArchiveSet set;
    for (int32_t i = 0; i < count; ++i) {
        std::string e;
        if (!dat_paths[i]) {
            set_err(err, err_len, "no path");
            return nullptr;
        }
        if (!archives[size_t(i)].open(dat_paths[i], &e)) {
            set_err(err, err_len, e);
            return nullptr;
        }
        set.attach(&archives[size_t(i)]);
    }
    auto* r = new (std::nothrow) DBStageReport;
    if (!r) {
        set_err(err, err_len, "out of memory");
        return nullptr;
    }
    r->summary = db::summarize_stage(set, stage);
    set_err(err, err_len, "");
    return r;
}

DBStageReport* db_stage_report_open(const char* dat_path, int32_t stage, char* err, int32_t err_len) {
    return db_stage_report_open_set(&dat_path, 1, stage, err, err_len);
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

DBArchives* db_archives_open(const char* const* dat_paths, int32_t count, char* err, int32_t err_len) {
    if (!dat_paths || count <= 0) {
        set_err(err, err_len, "no path");
        return nullptr;
    }
    std::unique_ptr<DBArchives> a(new (std::nothrow) DBArchives);
    if (!a) {
        set_err(err, err_len, "out of memory");
        return nullptr;
    }
    for (int32_t i = 0; i < count; ++i) {
        std::string e;
        if (!dat_paths[i]) {
            set_err(err, err_len, "no path");
            return nullptr;
        }
        auto archive = std::make_unique<db::Archive>();
        if (!archive->open(dat_paths[i], &e)) {
            set_err(err, err_len, e);
            return nullptr;
        }
        a->set.attach(archive.get());
        a->archives.push_back(std::move(archive));
    }
    set_err(err, err_len, "");
    return a.release();
}

void db_archives_free(DBArchives* a) { delete a; }

DBImage* db_image_load(DBArchives* a, const char* name, int32_t name_len, char* err, int32_t err_len) {
    if (!a || !name || name_len <= 0) {
        set_err(err, err_len, "no archive or name");
        return nullptr;
    }
    std::string entry(name, size_t(name_len));
    std::string lower = db::lower_ascii(entry);
    if (lower.size() < 4 || lower.compare(lower.size() - 4, 4, ".cnv") != 0) entry = db::cnv_name(entry);
    const db::Entry* e = a->set.find(entry);
    if (!e) {
        set_err(err, err_len, "missing: " + entry);
        return nullptr;
    }
    std::vector<uint8_t> data;
    std::string msg;
    if (!a->set.read(*e, data, &msg)) {
        set_err(err, err_len, msg);
        return nullptr;
    }
    auto* img = new (std::nothrow) DBImage;
    if (!img) {
        set_err(err, err_len, "out of memory");
        return nullptr;
    }
    if (!db::decode_image(data, img->image, &msg)) {
        delete img;
        set_err(err, err_len, msg);
        return nullptr;
    }
    set_err(err, err_len, "");
    return img;
}

void db_image_free(DBImage* img) { delete img; }
int32_t db_image_width(const DBImage* img) { return img ? int32_t(img->image.width) : -1; }
int32_t db_image_height(const DBImage* img) { return img ? int32_t(img->image.height) : -1; }
int32_t db_image_bpp(const DBImage* img) { return img ? int32_t(img->image.bpp) : -1; }
const uint8_t* db_image_rgba(const DBImage* img) { return img ? img->image.rgba.data() : nullptr; }

}  // extern "C"
