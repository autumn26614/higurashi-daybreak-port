#include "daybreak_c.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <string>

#include <memory>
#include <vector>

#include "archive.h"
#include "audio.h"
#include "cnv.h"
#include "geometry.h"
#include "camera.h"
#include "character.h"
#include "raster.h"
#include "recipe.h"
#include "identify.h"
#include "scenario.h"
#include "story.h"
#include "talk.h"

#include <map>

#include <cstdio>
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
    bool characters_listed = false;
    bool sounds_listed = false;
    std::vector<std::string> sounds;  // filled by db_archives_sound_count
    std::vector<std::string> characters;  // filled by db_archives_character_count
};

struct DBSound {
    db::Pcm pcm;
};

struct DBCharacter {
    db::Character c;
};

struct DBStageGeometry {
    db::StageGeometry geo;
};

struct DBStory {
    std::string name;
    db::story::Story story;
};

struct DBTalk {
    db::talk::Params params;
    db::talk::Layout layout;
    std::map<uint16_t, db::talk::Mask> masks;
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
db::Camera to_camera(const DBCamera& c) {
    db::Camera k;
    for (int i = 0; i < 3; i++) k.eye[i] = c.eye[i], k.target[i] = c.target[i];
    k.fov_y = c.fov_y;
    k.z_near = c.z_near;
    k.z_far = c.z_far;
    return k;
}
const db::scn::Page* page_at(const DBScript* s, int32_t p) {
    return s && p >= 0 && size_t(p) < s->pages.size() ? &s->pages[size_t(p)] : nullptr;
}
const db::scn::Token* token_at(const DBScript* s, int32_t p, int32_t t) {
    const db::scn::Page* pg = page_at(s, p);
    if (!pg || t < 0 || size_t(t) >= pg->text->tokens.size()) return nullptr;
    return &pg->text->tokens[size_t(t)];
}
const db::story::Page* story_page(const DBStory* s, int32_t i) {
    return s && i >= 0 && size_t(i) < s->story.pages.size() ? &s->story.pages[size_t(i)] : nullptr;
}
const char* out_str(const std::string& v, int32_t* len) {
    if (len) *len = int32_t(v.size());
    return v.c_str();
}
// Decodes an image entry; a .bmp name is mapped to .cnv as the engine does.
bool load_rgba(void* ctx, const std::string& name, std::vector<uint8_t>& rgba, int& w, int& h) {
    const db::ArchiveSet& set = static_cast<DBArchives*>(ctx)->set;
    const std::string low = db::lower_ascii(name);
    const std::string entry = low.size() > 4 && low.compare(low.size() - 4, 4, ".cnv") == 0 ? name : db::cnv_name(name);
    const db::Entry* e = set.find(entry);
    std::vector<uint8_t> d;
    db::Image img;
    std::string err;
    if (!e || !set.read(*e, d, &err) || !db::decode_image(d, img, &err)) return false;
    rgba = std::move(img.rgba);
    w = int(img.width);
    h = int(img.height);
    return true;
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

DBStageGeometry* db_geometry_open(DBArchives* a, int32_t stage, int32_t variant, char* err, int32_t err_len) {
    if (!a) {
        set_err(err, err_len, "no archives");
        return nullptr;
    }
    auto* g = new (std::nothrow) DBStageGeometry;
    if (!g) {
        set_err(err, err_len, "out of memory");
        return nullptr;
    }
    g->geo = db::load_stage_geometry(a->set, stage, variant);
    if (g->geo.batches.empty()) {
        char msg[64];
        snprintf(msg, sizeof msg, "stage %02d has no meshes", stage);
        set_err(err, err_len, g->geo.errors.empty() ? std::string(msg) : g->geo.errors.front());
        delete g;
        return nullptr;
    }
    set_err(err, err_len, "");
    return g;
}

void db_geometry_free(DBStageGeometry* g) { delete g; }
int32_t db_geometry_batch_count(const DBStageGeometry* g) { return g ? int32_t(g->geo.batches.size()) : 0; }
int64_t db_geometry_triangle_count(const DBStageGeometry* g) { return g ? int64_t(g->geo.triangles()) : 0; }
int32_t db_geometry_error_count(const DBStageGeometry* g) { return g ? int32_t(g->geo.errors.size()) : 0; }

int32_t db_geometry_batch(const DBStageGeometry* g, int32_t i, DBBatch* out) {
    if (!g || !out || i < 0 || size_t(i) >= g->geo.batches.size()) return 0;
    const db::GBatch& b = g->geo.batches[size_t(i)];
    out->slot = b.slot;
    out->mesh = b.mesh;
    out->material = b.material;
    out->vertex_count = int32_t(b.positions.size() / 3);
    out->positions = b.positions.data();
    out->uvs = b.uvs.data();
    out->colors = b.colors.data();
    out->has_colors = b.has_colors;
    out->index_count = int32_t(b.indices.size());
    out->indices = b.indices.data();
    for (int k = 0; k < 4; k++) out->diffuse[k] = b.mat.diffuse[k];
    out->power = b.mat.power;
    for (int k = 0; k < 3; k++) out->specular[k] = b.mat.specular[k], out->emissive[k] = b.mat.emissive[k];
    out->texture = b.mat.tex.archive_name.c_str();
    out->texture_len = int32_t(b.mat.tex.archive_name.size());
    out->texture_found = b.mat.tex.found;
    out->normals = b.normals.empty() ? nullptr : b.normals.data();
    return 1;
}

int32_t db_geometry_env(const DBStageGeometry* g, DBStageEnv* out) {
    if (!g || !out) return 0;
    const db::StageEnv& e = g->geo.env;
    out->present = e.present;
    out->fog_start = e.fog_start;
    out->fog_end = e.fog_end;
    for (int k = 0; k < 3; k++) out->fog_rgb[k] = e.fog_rgb[k];
    for (int p = 0; p < 4; p++)
        for (int k = 0; k < 3; k++) out->spawn[p][k] = e.spawn[p][k];
    const db::Box& bx = g->geo.bounds;
    for (int k = 0; k < 3; k++) {
        out->bounds_min[k] = bx.empty() ? 0.f : float(bx.lo[k]);
        out->bounds_max[k] = bx.empty() ? 0.f : float(bx.hi[k]);
    }
    return 1;
}

int32_t db_archives_has_stage(DBArchives* a, int32_t stage) {
    if (!a || stage < 0 || stage > 99) return 0;
    char name[32];
    for (int slot = 0; slot < 8; slot++)
        for (const char* fmt : {"data/stage%02d/%02d.x", "data/stage%02d/%02db.x"}) {
            snprintf(name, sizeof name, fmt, stage, slot);
            if (a->set.find(name)) return 1;
        }
    return 0;
}

int32_t db_geometry_default_camera(const DBStageGeometry* g, DBCamera* out) {
    if (!g || !out) return 0;
    const db::Camera c = db::default_camera(g->geo);
    for (int k = 0; k < 3; k++) out->eye[k] = c.eye[k], out->target[k] = c.target[k];
    out->fov_y = c.fov_y;
    out->z_near = c.z_near;
    out->z_far = c.z_far;
    return 1;
}


void db_camera_matrices(const DBCamera* c, float aspect, float view[16], float proj[16]) {
    if (!c) return;
    const db::Camera k = to_camera(*c);
    const db::Mat4f v = db::look_at_lh(k.eye, k.target);
    const db::Mat4f p = db::perspective_lh(k.fov_y, aspect, k.z_near, k.z_far);
    if (view) memcpy(view, v.m, sizeof v.m);
    if (proj) memcpy(proj, p.m, sizeof p.m);
}

int64_t db_render_reference(DBArchives* a, const DBStageGeometry* g, const DBCamera* c, int32_t width, int32_t height,
                            uint8_t* rgba) {
    if (!a || !g || !c || !rgba || width <= 0 || height <= 0 || width > 8192 || height > 8192) return -1;
    std::vector<uint8_t> out;
    const long drawn = db::render_reference(a->set, g->geo, to_camera(*c), width, height, out);
    memcpy(rgba, out.data(), out.size());
    return drawn;
}

int32_t db_archives_sound_count(DBArchives* a) {
    if (!a) return 0;
    if (!a->sounds_listed) {
        std::set<std::string> seen;
        for (const db::Archive* ar : a->set.archives())
            for (const db::Entry& e : ar->entries()) {
                const std::string low = db::lower_ascii(e.name);
                const bool bgm = low.rfind("data/bgm/", 0) == 0 && low.size() > 4 && low.compare(low.size() - 4, 4, ".ogg") == 0;
                const bool se = (low.rfind("data/se/", 0) == 0 || low.rfind("data/sysse/", 0) == 0) && low.size() > 4 &&
                                low.compare(low.size() - 4, 4, ".cnv") == 0;
                if ((bgm || se) && seen.insert(low).second) a->sounds.push_back(e.name);
            }
        std::sort(a->sounds.begin(), a->sounds.end(),
                  [](const std::string& x, const std::string& y) { return db::lower_ascii(x) < db::lower_ascii(y); });
        a->sounds_listed = true;
    }
    return int32_t(a->sounds.size());
}

const char* db_archives_sound_name(DBArchives* a, int32_t i, int32_t* len) {
    if (!a || i < 0 || i >= db_archives_sound_count(a)) return nullptr;
    const std::string& s = a->sounds[size_t(i)];
    if (len) *len = int32_t(s.size());
    return s.data();
}

DBSound* db_sound_load(DBArchives* a, const char* name, int32_t name_len, char* err, int32_t err_len) {
    if (!a || !name || name_len <= 0) {
        set_err(err, err_len, "no archive or name");
        return nullptr;
    }
    auto* s = new (std::nothrow) DBSound;
    if (!s) {
        set_err(err, err_len, "out of memory");
        return nullptr;
    }
    std::string msg;
    if (!db::load_sound(a->set, std::string(name, size_t(name_len)), s->pcm, &msg)) {
        set_err(err, err_len, msg);
        delete s;
        return nullptr;
    }
    set_err(err, err_len, "");
    return s;
}

void db_sound_free(DBSound* s) { delete s; }
int32_t db_sound_channels(const DBSound* s) { return s ? s->pcm.channels : 0; }
int32_t db_sound_rate(const DBSound* s) { return s ? s->pcm.rate : 0; }
int64_t db_sound_frames(const DBSound* s) {
    return s && s->pcm.channels > 0 ? int64_t(s->pcm.samples.size() / size_t(s->pcm.channels)) : 0;
}
const int16_t* db_sound_samples(const DBSound* s) { return s ? s->pcm.samples.data() : nullptr; }
int64_t db_sound_loop_start(const DBSound* s) { return s ? s->pcm.loop_start : -1; }
int64_t db_sound_loop_end(const DBSound* s) { return s ? s->pcm.loop_end : -1; }

int32_t db_voice_entry(DBArchives* a, const char* voice, int32_t voice_len, char* out, int32_t out_len) {
    if (!out || out_len <= 0) return 0;
    out[0] = 0;
    if (!a || !voice || voice_len <= 0) return 0;
    const std::string e = db::voice_entry(a->set, std::string(voice, size_t(voice_len)));
    const size_t n = std::min(e.size(), size_t(out_len - 1));
    memcpy(out, e.data(), n);
    out[n] = 0;
    return int32_t(n);
}

int32_t db_archives_character_count(DBArchives* a) {
    if (!a) return 0;
    if (!a->characters_listed) {
        std::set<std::string> seen;
        for (const db::Archive* ar : a->set.archives())
            for (const std::string& n : db::list_characters(*ar))
                if (seen.insert(db::lower_ascii(n)).second) a->characters.push_back(n);
        std::sort(a->characters.begin(), a->characters.end(),
                  [](const std::string& x, const std::string& y) { return db::lower_ascii(x) < db::lower_ascii(y); });
        a->characters_listed = true;
    }
    return int32_t(a->characters.size());
}

const char* db_archives_character_name(DBArchives* a, int32_t i, int32_t* len) {
    if (!a || i < 0 || i >= db_archives_character_count(a)) return nullptr;
    const std::string& s = a->characters[size_t(i)];
    if (len) *len = int32_t(s.size());
    return s.data();
}

DBCharacter* db_character_open(DBArchives* a, const char* name, int32_t name_len, char* err, int32_t err_len) {
    if (!a || !name || name_len <= 0) {
        set_err(err, err_len, "no archive or name");
        return nullptr;
    }
    auto* c = new (std::nothrow) DBCharacter;
    if (!c) {
        set_err(err, err_len, "out of memory");
        return nullptr;
    }
    std::string msg;
    if (!db::load_character(a->set, std::string(name, size_t(name_len)), c->c, &msg)) {
        set_err(err, err_len, msg);
        delete c;
        return nullptr;
    }
    set_err(err, err_len, "");
    return c;
}

void db_character_free(DBCharacter* c) { delete c; }
int32_t db_character_anim_count(const DBCharacter* c) { return c ? int32_t(c->c.anims.size()) : 0; }

const char* db_character_anim_name(const DBCharacter* c, int32_t i, int32_t* len) {
    if (!c || i < 0 || size_t(i) >= c->c.anims.size()) return nullptr;
    const std::string& s = c->c.anims[size_t(i)].name;
    if (len) *len = int32_t(s.size());
    return s.data();
}

double db_character_anim_seconds(const DBCharacter* c, int32_t i) {
    if (!c || i < 0 || size_t(i) >= c->c.anims.size()) return 0;
    return (c->c.anims[size_t(i)].ticks + 80) / c->c.ticks_per_second;  // the last key holds one frame
}

DBStageGeometry* db_character_pose(const DBCharacter* c, int32_t anim, double seconds) {
    if (!c) return nullptr;
    auto* g = new (std::nothrow) DBStageGeometry;
    if (g) g->geo = db::pose_character(c->c, anim, seconds);
    return g;
}

int32_t db_geometry_character_camera(const DBStageGeometry* g, DBCamera* out) {
    if (!g || !out) return 0;
    const db::Camera c = db::character_camera(g->geo);
    for (int k = 0; k < 3; k++) out->eye[k] = c.eye[k], out->target[k] = c.target[k];
    out->fov_y = c.fov_y;
    out->z_near = c.z_near;
    out->z_far = c.z_far;
    return 1;
}

int32_t db_stage_pass_count(void) { return db::kStagePassCount; }

int32_t db_stage_pass(int32_t i, DBPass* out) {
    if (!out || i < 0 || i >= db::kStagePassCount) return 0;
    const db::Pass& p = db::kStagePasses[i];
    *out = {p.slot, int32_t(p.blend), p.z_write, p.fog, p.alpha_test, p.point_sampling};
    return 1;
}

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

DBTalk* db_talk_layout(const char* text, int32_t len) {
    if (len < 0 || (!text && len > 0)) return nullptr;
    auto* t = new (std::nothrow) DBTalk;
    if (!t) return nullptr;
    t->layout = db::talk::layout(len > 0 ? std::string(text, size_t(len)) : std::string(), t->params);
    return t;
}

void db_talk_free(DBTalk* t) { delete t; }
int32_t db_talk_glyph_count(const DBTalk* t) { return t ? int32_t(t->layout.glyphs.size()) : 0; }
int32_t db_talk_text_bytes(const DBTalk* t) { return t ? int32_t(t->layout.text_bytes) : 0; }
int32_t db_talk_lines(const DBTalk* t) { return t ? t->layout.lines : 0; }
int32_t db_talk_stopped(const DBTalk* t) { return t ? t->layout.stopped : 0; }

int32_t db_talk_glyph(const DBTalk* t, int32_t i, uint32_t* code, int32_t* bytes, int32_t* x, int32_t* y) {
    if (!t || i < 0 || size_t(i) >= t->layout.glyphs.size()) return 0;
    const db::talk::Glyph& g = t->layout.glyphs[size_t(i)];
    if (code) *code = g.code;
    if (bytes) *bytes = g.bytes;
    if (x) *x = g.x;
    if (y) *y = g.y;
    return 1;
}

int32_t db_talk_set_mask(DBTalk* t, uint32_t code, const uint8_t* coverage, int32_t w, int32_t h, int32_t left,
                         int32_t top) {
    if (!t || w < 0 || h < 0 || w > 256 || h > 256 || (!coverage && w * h > 0)) return 0;
    db::talk::Mask m;
    m.width = w;
    m.height = h;
    m.left = left;
    m.top = top;
    m.coverage.assign(coverage, coverage + size_t(w) * size_t(h));
    t->masks[uint16_t(code)] = std::move(m);
    return 1;
}

DBStory* db_story_load(DBArchives* a, const char* name, int32_t name_len, char* err, int32_t err_len) {
    if (!a || !name || name_len <= 0) {
        set_err(err, err_len, "no archive or name");
        return nullptr;
    }
    const std::string entry(name, size_t(name_len));
    const db::Entry* e = a->set.find(entry);
    std::vector<uint8_t> data;
    std::string msg = "missing: " + entry;
    if (!e || !a->set.read(*e, data, &msg)) {
        set_err(err, err_len, msg);
        return nullptr;
    }
    auto* s = new (std::nothrow) DBStory;
    if (!s) {
        set_err(err, err_len, "out of memory");
        return nullptr;
    }
    s->name = entry;
    s->story = db::story::parse(std::string(data.begin(), data.end()));
    for (auto& p : s->story.pages) {
        // Story voices carry their voicedir; win quotes are bare names resolved like the
        // game's two voice folders (audio.h).
        if (p.voice.empty()) continue;
        if (p.voice.find('/') != std::string::npos) {
            const std::string cnv = db::cnv_name(p.voice);
            p.voice = a->set.find(cnv) ? cnv : "";
        } else {
            p.voice = db::voice_entry(a->set, p.voice);
        }
    }
    set_err(err, err_len, "");
    return s;
}

void db_story_free(DBStory* s) { delete s; }
int32_t db_story_page_count(const DBStory* s) { return s ? int32_t(s->story.pages.size()) : 0; }


const char* db_story_page_text(const DBStory* s, int32_t i, int32_t* len) {
    const auto* p = story_page(s, i);
    return p ? out_str(p->text, len) : nullptr;
}
const char* db_story_page_voice(const DBStory* s, int32_t i, int32_t* len) {
    const auto* p = story_page(s, i);
    return p ? out_str(p->voice, len) : nullptr;
}
const char* db_story_page_bgm(const DBStory* s, int32_t i, int32_t* len) {
    const auto* p = story_page(s, i);
    return p ? out_str(p->scene.bgm, len) : nullptr;
}
int32_t db_story_page_sleep_ms(const DBStory* s, int32_t i) {
    const auto* p = story_page(s, i);
    return p ? p->sleep_ms : 0;
}

int32_t db_story_background(DBArchives* a, const DBStory* s, int32_t i, uint8_t* out) {
    const auto* p = story_page(s, i);
    if (!a || !p || !out) return 0;
    const std::vector<uint8_t> bg = s->story.has_commands ? db::story::compose(p->scene, load_rgba, a)
                                                          : db::talk::background(s->name, load_rgba, a);
    memcpy(out, bg.data(), bg.size());
    return 1;
}

int32_t db_talk_background(DBArchives* a, const char* script, int32_t script_len, uint8_t* out) {
    if (!a || !script || script_len <= 0 || !out) return 0;
    auto load = [](void* ctx, const std::string& name, std::vector<uint8_t>& rgba, int& w, int& h) {
        const db::ArchiveSet& set = static_cast<DBArchives*>(ctx)->set;
        const db::Entry* e = set.find(name);
        std::vector<uint8_t> d;
        db::Image img;
        std::string err;
        if (!e || !set.read(*e, d, &err) || !db::decode_image(d, img, &err)) return false;
        rgba = std::move(img.rgba);
        w = int(img.width);
        h = int(img.height);
        return true;
    };
    const std::vector<uint8_t> bg = db::talk::background(std::string(script, size_t(script_len)), load, a);
    memcpy(out, bg.data(), bg.size());
    return 1;
}

int32_t db_talk_compose(DBTalk* t, const uint8_t* background, const uint8_t* window, int32_t ww, int32_t wh,
                        int32_t reveal, int32_t arrow, uint8_t* out) {
    if (!t || !out || ww < 0 || wh < 0) return 0;
    const size_t n = size_t(DB_TALK_WIDTH) * DB_TALK_HEIGHT * 4;
    std::vector<uint8_t> bg = background ? std::vector<uint8_t>(background, background + n) : std::vector<uint8_t>();
    std::vector<uint8_t> win = window ? std::vector<uint8_t>(window, window + size_t(ww) * size_t(wh) * 4)
                                      : std::vector<uint8_t>();
    const std::vector<uint8_t> text = db::talk::draw_text(t->layout, t->masks, t->params);
    const std::vector<uint8_t> frame =
        db::talk::compose(bg, win, ww, wh, text, t->params, reveal < 0 ? 0 : size_t(reveal), arrow != 0);
    memcpy(out, frame.data(), n);
    return 1;
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
