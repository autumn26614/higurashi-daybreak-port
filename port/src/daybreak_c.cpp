#include "daybreak_c.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <string>

#include <memory>
#include <vector>

#include "archive.h"
#include "cnv.h"
#include "identify.h"
#include "scenario.h"
#include "stage.h"

#include <set>

struct DBStageReport {
    db::StageSummary summary;
};

struct DBArchives {
    std::vector<std::unique_ptr<db::Archive>> archives;
    db::ArchiveSet set;
    bool scripts_listed = false;
    std::vector<std::string> scripts;  // filled by db_archives_script_count
};

struct DBScript {
    db::scn::Script script;
    std::vector<db::scn::Page> pages;  // point into script.lines
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

int32_t db_archives_script_count(DBArchives* a) {
    if (!a) return 0;
    if (!a->scripts_listed) {
        std::set<std::string> seen;  // lower-case names; the first archive's entry wins
        for (const db::Archive* ar : a->set.archives())
            for (const db::Entry& e : ar->entries()) {
                const std::string low = db::lower_ascii(e.name);
                if (low.rfind("data/scenario/", 0) == 0 && low.size() > 4 &&
                    low.compare(low.size() - 4, 4, ".txt") == 0 && seen.insert(low).second)
                    a->scripts.push_back(e.name);
            }
        std::sort(a->scripts.begin(), a->scripts.end(),
                  [](const std::string& x, const std::string& y) { return db::lower_ascii(x) < db::lower_ascii(y); });
        a->scripts_listed = true;
    }
    return int32_t(a->scripts.size());
}

const char* db_archives_script_name(DBArchives* a, int32_t i, int32_t* len) {
    if (!a || i < 0 || i >= db_archives_script_count(a)) return nullptr;
    const std::string& s = a->scripts[size_t(i)];
    if (len) *len = int32_t(s.size());
    return s.data();
}

DBScript* db_script_parse(const uint8_t* bytes, int32_t len) {
    if (len < 0 || (!bytes && len > 0)) return nullptr;
    auto* s = new (std::nothrow) DBScript;
    if (!s) return nullptr;
    s->script = db::scn::parse(len > 0 ? std::string(reinterpret_cast<const char*>(bytes), size_t(len)) : std::string());
    s->pages = db::scn::pages(s->script);
    return s;
}

DBScript* db_script_load(DBArchives* a, const char* name, int32_t name_len, char* err, int32_t err_len) {
    if (!a || !name || name_len <= 0) {
        set_err(err, err_len, "no archive or name");
        return nullptr;
    }
    const std::string entry(name, size_t(name_len));
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
    DBScript* s = db_script_parse(data.data(), int32_t(data.size()));
    if (!s) set_err(err, err_len, "out of memory");
    return s;
}

void db_script_free(DBScript* s) { delete s; }

int32_t db_script_page_count(const DBScript* s) { return s ? int32_t(s->pages.size()) : 0; }

namespace {
const db::scn::Page* page_at(const DBScript* s, int32_t p) {
    return s && p >= 0 && size_t(p) < s->pages.size() ? &s->pages[size_t(p)] : nullptr;
}
const db::scn::Token* token_at(const DBScript* s, int32_t p, int32_t t) {
    const db::scn::Page* pg = page_at(s, p);
    if (!pg || t < 0 || size_t(t) >= pg->text->tokens.size()) return nullptr;
    return &pg->text->tokens[size_t(t)];
}
}  // namespace

const char* db_script_page_voice(const DBScript* s, int32_t page, int32_t* len) {
    const db::scn::Page* p = page_at(s, page);
    if (!p) return nullptr;
    if (len) *len = int32_t(p->voice_file.size());
    return p->voice_file.data();
}

int32_t db_script_token_count(const DBScript* s, int32_t page) {
    const db::scn::Page* p = page_at(s, page);
    return p ? int32_t(p->text->tokens.size()) : 0;
}

int32_t db_script_token_kind(const DBScript* s, int32_t page, int32_t tok) {
    const db::scn::Token* t = token_at(s, page, tok);
    return t ? int32_t(t->kind) : -1;
}

const char* db_script_token_bytes(const DBScript* s, int32_t page, int32_t tok, int32_t* len) {
    const db::scn::Token* t = token_at(s, page, tok);
    if (!t) return nullptr;
    if (len) *len = int32_t(t->raw.size());
    return t->raw.data();
}

uint32_t db_script_token_rgb(const DBScript* s, int32_t page, int32_t tok) {
    const db::scn::Token* t = token_at(s, page, tok);
    return t ? (uint32_t(t->r) << 16 | uint32_t(t->g) << 8 | t->b) : 0;
}

int32_t db_identify_archive(const char* path, DBArchiveInfo* out, char* err, int32_t err_len) {
    if (!path || !out) {
        set_err(err, err_len, "no path");
        return 0;
    }
    db::ArchiveIdent id;
    std::string e;
    if (!db::identify_archive(path, id, &e)) {
        set_err(err, err_len, e);
        return 0;
    }
    out->role = int32_t(id.role);
    out->entries = int32_t(id.entries);
    out->file_size = int64_t(id.file_size);
    out->scripts = id.scripts;
    out->english_scripts = id.english_scripts;
    return 1;
}

const char* db_role_file_name(int32_t role) { return db::role_file_name(db::ArchiveRole(role)); }

}  // extern "C"
