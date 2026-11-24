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
#include "chardef.h"
#include "fighter.h"
#include "movie.h"
#include "raster.h"
#include "recipe.h"
#include "identify.h"
#include "scenario.h"
#include "story.h"
#include "xfile.h"
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
    std::string definition;  // data/<dir>/NN.dat for battles; empty: the one next to the model
};

struct DBStageGeometry {
    db::StageGeometry geo;
};

struct DBMovie {
    db::Movie movie;
};

struct DBBattle {
    db::CharDef defs[4];
    std::vector<db::CharDef> bullets[4];  // by slot: data/bullet/<character id>/<slot>.dat
    int character_id[4] = {0, 0, 0, 0};
    db::battle::Battle battle;
};

struct DBStory {
    std::string name;
    db::story::Story story;
    struct Img {
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        bool ok = false;
    };
    std::map<std::string, Img> images;  // decoded once: backgrounds are redrawn per frame while moving
};

struct StoryLoad {
    DBArchives* a;
    DBStory* s;
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

// The definition next to a model: data/<name>/00.X -> data/<name>/00.dat. Costume models
// (data/<name>_B/, the loader's costume flag 2) use the base character's definition.
bool load_chardef(const db::Source& src, const db::Character& c, db::CharDef& out, std::string* err,
                  const std::string& definition = {}) {
    std::string path = c.path;
    const size_t dot = path.rfind('.');
    path = (dot == std::string::npos ? path : path.substr(0, dot)) + ".dat";
    if (!definition.empty()) path = definition;
    const db::Entry* e = src.find(path);
    const size_t slash = path.rfind('/');
    if (!e && slash != std::string::npos && slash >= 2 && (path.compare(slash - 2, 2, "_B") == 0 ||
                                                          path.compare(slash - 2, 2, "_b") == 0)) {
        path.erase(slash - 2, 2);
        e = src.find(path);
    }
    std::vector<uint8_t> bytes;
    if (!e) {
        if (err) *err = "no character definition " + path;
        return false;
    }
    if (!src.read(*e, bytes, err)) return false;
    if (!db::parse_chardef(bytes.data(), bytes.size(), out, err)) {
        if (err) *err = path + ": " + *err;
        return false;
    }
    return true;
}

// A character's bullet definitions (loader 0x58A070): slot n is data/bullet/<id>/<n>.dat;
// missing slots stay empty.
void load_bullets(const db::Source& src, int id, std::vector<db::CharDef>& out) {
    out.clear();
    if (id <= 0) return;
    for (int n = 0; n < 40; ++n) {
        char path[48];
        snprintf(path, sizeof path, "data/bullet/%03d/%03d.dat", id, n);
        db::CharDef d;
        std::vector<uint8_t> bytes;
        std::string err;
        if (const db::Entry* e = src.find(path))
            if (src.read(*e, bytes, &err) && !db::parse_bulletdef(bytes.data(), bytes.size(), d, &err)) d = {};
        out.push_back(std::move(d));
    }
    while (!out.empty() && out.back().moves.empty()) out.pop_back();
}

// Length of each animation set in game frames (the game advances 0.016 s per frame).
std::vector<int> anim_frames(const db::Character& c) {
    std::vector<int> out;
    for (const auto& a : c.anims) {
        const double seconds = (a.ticks + 80) / c.ticks_per_second;
        out.push_back(std::max(1, int(seconds / db::battle::kFrameSeconds + 0.5)));
    }
    return out;
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

DBStageGeometry* db_character_pose_at(const DBCharacter* c, int32_t anim, double seconds, const float pos[3],
                                      float yaw) {
    if (!c || !pos) return nullptr;
    auto* g = new (std::nothrow) DBStageGeometry;
    if (!g) return nullptr;
    db::place_character(g->geo, db::pose_character(c->c, anim, seconds), pos, yaw);
    for (const auto& b : g->geo.batches)
        for (size_t i = 0; i + 2 < b.positions.size(); i += 3) {
            const double p[3] = {b.positions[i], b.positions[i + 1], b.positions[i + 2]};
            g->geo.bounds.add(p);
        }
    return g;
}

float db_facing_yaw(const float from[3], const float to[3]) { return from && to ? db::facing_yaw(from, to) : 0.f; }

DBBattle* db_battle_open_teams(DBArchives* a, const DBCharacter* const* chars, const int32_t* teams,
                               const float* spawns, int32_t n, char* err, int32_t err_len) {
    if (!a || !chars || !teams || !spawns || (n != 2 && n != 4)) {
        set_err(err, err_len, "no archive, characters, teams or spawn points (2 or 4 fighters)");
        return nullptr;
    }
    for (int32_t i = 0; i < n; ++i)
        if (!chars[i]) {
            set_err(err, err_len, "missing character");
            return nullptr;
        }
    auto* b = new (std::nothrow) DBBattle;
    if (!b) {
        set_err(err, err_len, "out of memory");
        return nullptr;
    }
    std::vector<db::battle::Entrant> entrants(static_cast<size_t>(n));
    for (int32_t i = 0; i < n; ++i) {
        std::string msg;
        if (!load_chardef(a->set, chars[i]->c, b->defs[i], &msg, chars[i]->definition)) {
            set_err(err, err_len, msg);
            delete b;
            return nullptr;
        }
        db::battle::Entrant& e = entrants[size_t(i)];
        e.def = &b->defs[i];
        e.anim_frames = anim_frames(chars[i]->c);
        for (int k = 0; k < 3; ++k) e.spawn[k] = spawns[i * 3 + k];
        e.model = &chars[i]->c;
        e.team = teams[i];
    }
    b->battle.start(std::move(entrants));
    for (int32_t i = 0; i < n; ++i) {
        b->character_id[i] = b->defs[i].head[3];
        load_bullets(a->set, b->character_id[i], b->bullets[i]);
        b->battle.f[size_t(i)].bullet_defs = &b->bullets[i];
    }
    set_err(err, err_len, "");
    return b;
}

DBBattle* db_battle_open(DBArchives* a, const DBCharacter* p1, const DBCharacter* p2, const float spawn1[3],
                         const float spawn2[3], char* err, int32_t err_len) {
    if (!spawn1 || !spawn2) {
        set_err(err, err_len, "no spawn point");
        return nullptr;
    }
    const DBCharacter* chars[2] = {p1, p2};
    const int32_t teams[2] = {0, 1};
    const float spawns[6] = {spawn1[0], spawn1[1], spawn1[2], spawn2[0], spawn2[1], spawn2[2]};
    return db_battle_open_teams(a, chars, teams, spawns, 2, err, err_len);
}

int32_t db_chardef_model(DBArchives* a, const char* def, int32_t def_len, char* out, int32_t len) {
    if (out && len > 0) out[0] = 0;
    if (!a || !def || def_len <= 0) return 0;
    const std::string path(def, size_t(def_len));
    const db::Entry* e = a->set.find(path);
    std::vector<uint8_t> bytes;
    std::string err;
    db::CharDef d;
    if (!e || !a->set.read(*e, bytes, &err) || !db::parse_chardef(bytes.data(), bytes.size(), d, &err) ||
        d.model.empty())
        return 0;
    const size_t slash = path.rfind('/');
    const std::string model = (slash == std::string::npos ? std::string() : path.substr(0, slash + 1)) + d.model;
    if (out && len > 0) {
        const size_t n = std::min(model.size(), size_t(len - 1));
        std::memcpy(out, model.data(), n);
        out[n] = 0;
    }
    return int32_t(model.size());
}

void db_character_set_definition(DBCharacter* c, const char* def, int32_t def_len) {
    if (c) c->definition = def && def_len > 0 ? std::string(def, size_t(def_len)) : std::string();
}

int32_t db_battle_fighter_count(const DBBattle* b) { return b ? int32_t(b->battle.f.size()) : 0; }

int32_t db_battle_target(const DBBattle* b, int32_t i) {
    return b && i >= 0 && size_t(i) < b->battle.target.size() ? b->battle.target[size_t(i)] : -1;
}

int32_t db_battle_team(const DBBattle* b, int32_t i) {
    return b && i >= 0 && size_t(i) < b->battle.team.size() ? b->battle.team[size_t(i)] : -1;
}

void db_battle_step_pads(DBBattle* b, const DBPad* const* pads, int32_t n) {
    if (!b) return;
    std::vector<db::battle::Pad> buf(size_t(std::max(n, 0)));
    std::vector<const db::battle::Pad*> ptrs(buf.size(), nullptr);
    for (int32_t i = 0; i < n; ++i)
        if (pads && pads[i]) {
            buf[size_t(i)] = {pads[i]->x, pads[i]->y, pads[i]->buttons, pads[i]->tap};
            ptrs[size_t(i)] = &buf[size_t(i)];
        }
    b->battle.step(ptrs);
}

void db_battle_free(DBBattle* b) { delete b; }

int32_t db_battle_set_stage(DBBattle* b, DBArchives* a, int32_t stage) {
    if (!b || !a) return 0;
    char path[64];
    snprintf(path, sizeof path, "data/stage%02d/collision.x", int(stage));
    db::XFile x;
    std::string err;
    if (!db::load_x(a->set, path, x, &err)) return 0;
    db::StageGeometry g;
    char dir[32];
    snprintf(dir, sizeof dir, "data/stage%02d/", int(stage));
    db::append_x_batches(a->set, x, dir, 0, g);
    db::battle::Floor fl;
    for (const auto& batch : g.batches)
        for (size_t i = 0; i + 2 < batch.indices.size(); i += 3) {
            const uint32_t v0 = batch.indices[i];
            // A vertex colour with blue < 0x80 marks a surface that is not floor (0x433DA0).
            if (batch.has_colors && size_t(v0) * 4 + 2 < batch.colors.size() && batch.colors[size_t(v0) * 4 + 2] < 0x80)
                continue;
            const float* p0 = &batch.positions[size_t(batch.indices[i]) * 3];
            const float* p1 = &batch.positions[size_t(batch.indices[i + 1]) * 3];
            const float* p2 = &batch.positions[size_t(batch.indices[i + 2]) * 3];
            fl.add(p0, p1, p2);
        }
    const int32_t n = int32_t((fl.tris.size() + fl.walls.size()) / 9);
    if (n > 0) b->battle.set_floor(std::move(fl));
    return n;
}

void db_battle_step(DBBattle* b, const DBPad* p1, const DBPad* p2) {
    if (!b) return;
    db::battle::Pad pads[2];
    const DBPad* in[2] = {p1, p2};
    for (int i = 0; i < 2; ++i)
        if (in[i]) pads[i] = {in[i]->x, in[i]->y, in[i]->buttons, in[i]->tap};
    b->battle.step(p1 ? &pads[0] : nullptr, p2 ? &pads[1] : nullptr);
}

int32_t db_battle_fighter(const DBBattle* b, int32_t i, DBFighterState* out) {
    if (!b || !out || i < 0 || size_t(i) >= b->battle.f.size()) return 0;
    const db::battle::Fighter& f = b->battle.f[size_t(i)];
    for (int k = 0; k < 3; ++k) out->pos[k] = f.pos[k];
    out->yaw = f.body_yaw();
    out->anim = f.anim();
    out->anim_seconds = f.frame * db::battle::kFrameSeconds;
    out->state = f.state;
    out->step = f.step;
    out->frame = f.frame;
    out->hp = f.hp;
    out->hp_bar = f.hp_bar;
    out->stamina = f.stamina;
    out->grounded = f.ground ? 1 : 0;
    return 1;
}

int32_t db_battle_camera(const DBBattle* b, int32_t i, DBCamera* out) {
    if (!b || !out || i < 0 || size_t(i) >= b->battle.f.size()) return 0;
    const auto& me = b->battle.f[size_t(i)];
    const auto& them = b->battle.f[size_t(b->battle.target[size_t(i)])];
    db::battle::follow_camera(me.pos, them.pos, 300.f, out->eye, out->target);
    out->fov_y = 3.14159265f / 4;
    out->z_near = 50;
    out->z_far = 200000;
    return 1;
}

int32_t db_battle_take_sounds(DBBattle* b, DBSoundEvent* out, int32_t max) {
    if (!b) return 0;
    const std::vector<db::battle::SoundEvent> ev = b->battle.take_events();
    int32_t n = 0;
    for (const auto& e : ev) {
        if (!out || n >= max) break;
        out[n++] = {e.fighter, e.kind == db::battle::SoundEvent::Voice ? 0 : 1, e.id};
    }
    return n;
}

const char* db_voice_category(int32_t id) {
    // Alphabetical, as the exe lists them (0x644900, 16 bytes apart).
    static const char* const names[] = {
        "addattack", "attackA", "attackB",  "attackC",  "attackKO", "backattack", "backdamage", "battlestart",
        "damage",    "damagedown", "damageKO", "fire",   "guard",    "guarded",    "HPcheck",    "intercept",
        "missdamage", "nearLose", "nearWin", "oyashiro", "oyashiroB", "poison",    "provoke",    "recovery",
        "reloadNG",  "reloadOK",  "revival",  "shotA",    "shotB",    "shotC",      "shotFinal",  "spark",
        "specialA",  "specialB",  "tactics",  "tagback",  "tagfront", "tagOK",      "thank"};
    return id >= 0 && size_t(id) < sizeof names / sizeof names[0] ? names[id] : nullptr;
}

int32_t db_battle_bullet_count(const DBBattle* b) { return b ? int32_t(b->battle.shots.size()) : 0; }

int32_t db_battle_bullet(const DBBattle* b, int32_t i, DBBulletState* out) {
    if (!b || !out || i < 0 || size_t(i) >= b->battle.shots.size()) return 0;
    const db::battle::Bullet& s = b->battle.shots[size_t(i)];
    for (int k = 0; k < 3; ++k) out->pos[k] = s.pos[k];
    out->yaw = s.yaw;
    out->owner = s.owner;
    out->slot = s.def;
    out->age = s.age;
    return 1;
}

int32_t db_battle_bullet_model(const DBBattle* b, int32_t owner, int32_t slot, char* out, int32_t len) {
    std::string path;
    if (b && owner >= 0 && size_t(owner) < b->battle.f.size() && owner < 4 && slot >= 0 && size_t(slot) < b->bullets[owner].size() &&
        !b->bullets[owner][size_t(slot)].model.empty()) {
        char dir[32];
        snprintf(dir, sizeof dir, "data/bullet/%03d/", b->character_id[owner]);
        path = dir + b->bullets[owner][size_t(slot)].model;
    }
    if (out && len > 0) {
        const size_t n = std::min(path.size(), size_t(len - 1));
        std::memcpy(out, path.data(), n);
        out[n] = 0;
    }
    return int32_t(path.size());
}

DBMovie* db_movie_open(DBArchives* a, const char* name, int32_t name_len, char* err, int32_t err_len) {
    if (!a || !name || name_len <= 0) {
        set_err(err, err_len, "no archive or name");
        return nullptr;
    }
    auto* m = new (std::nothrow) DBMovie;
    if (!m) {
        set_err(err, err_len, "out of memory");
        return nullptr;
    }
    std::string msg;
    if (!m->movie.open(a->set, std::string(name, size_t(name_len)), &msg)) {
        set_err(err, err_len, msg);
        delete m;
        return nullptr;
    }
    set_err(err, err_len, "");
    return m;
}

void db_movie_free(DBMovie* m) { delete m; }
int32_t db_movie_width(const DBMovie* m) { return m ? m->movie.width() : 0; }
int32_t db_movie_height(const DBMovie* m) { return m ? m->movie.height() : 0; }
double db_movie_fps(const DBMovie* m) { return m ? m->movie.fps() : 0; }
int32_t db_movie_sample_rate(const DBMovie* m) { return m ? m->movie.sample_rate() : 0; }

int32_t db_movie_next_frame(DBMovie* m, uint8_t* rgba, int32_t len, double* time) {
    if (!m || !rgba) return 0;
    std::vector<uint8_t> frame;
    double t = 0;
    if (!m->movie.next_frame(frame, &t) || frame.size() > size_t(std::max(len, 0))) return 0;
    std::memcpy(rgba, frame.data(), frame.size());
    if (time) *time = t;
    return 1;
}

int32_t db_movie_next_audio(DBMovie* m, int16_t* out, int32_t max_frames) {
    if (!m || !out || max_frames <= 0) return 0;
    std::vector<int16_t> pcm;
    if (!m->movie.next_audio(pcm)) return 0;
    const size_t frames = std::min(pcm.size() / 2, size_t(max_frames));
    std::memcpy(out, pcm.data(), frames * 2 * sizeof(int16_t));
    return int32_t(frames);
}

int32_t db_team_load(DBArchives* a, int32_t team, DBTeam* out) {
    if (!a || !out) return 0;
    char path[48];
    snprintf(path, sizeof path, "data/scenario/team%02d.dat", int(team));
    const db::Entry* e = a->set.find(path);
    std::vector<uint8_t> bytes;
    std::string err;
    db::story::Team t;
    if (!e || !a->set.read(*e, bytes, &err) || !db::story::parse_team(bytes.data(), bytes.size(), t)) return 0;
    *out = DBTeam{};
    out->members[0] = t.members[0];
    out->members[1] = t.members[1];
    out->episodes = int32_t(std::min<size_t>(t.episodes.size(), 6));
    for (int32_t i = 0; i < out->episodes; ++i) {
        out->opponents[i][0] = t.episodes[size_t(i)].opponents[0];
        out->opponents[i][1] = t.episodes[size_t(i)].opponents[1];
        out->stage[i] = t.episodes[size_t(i)].stage;
        out->number[i] = t.episodes[size_t(i)].number;
    }
    const size_t n = std::min(t.movie.size(), sizeof out->movie - 1);
    std::memcpy(out->movie, t.movie.data(), n);
    out->movie[n] = 0;
    return 1;
}

const char* db_story_character_dir(int32_t n) { return db::story::character_dir(n); }

int32_t db_battle_frame(const DBBattle* b) { return b ? b->battle.frame : 0; }
void db_battle_set_time_limit(DBBattle* b, int32_t frames) {
    if (b) b->battle.time_limit = std::max(0, int(frames));
}
int32_t db_battle_time_limit(const DBBattle* b) { return b ? b->battle.time_limit : 0; }

int32_t db_battle_winner(const DBBattle* b) { return b ? b->battle.winner : -1; }

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
int32_t db_geometry_mode3(const DBStageGeometry* g) { return g && g->geo.mode3 ? 1 : 0; }

int32_t db_stage_pass(int32_t i, DBPass* out) {
    if (!out || i < 0 || i >= db::kStagePassCount) return 0;
    const db::Pass& p = db::kStagePasses[i];
    *out = {p.slot, int32_t(p.blend), p.z_write, p.fog, p.alpha_test, p.point_sampling, p.mode3};
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

int32_t db_story_page_settle_frames(const DBStory* s, int32_t i) {
    const auto* p = story_page(s, i);
    return p ? db::story::settle_frames(p->scene) : 0;
}

int32_t db_story_background_at(DBArchives* a, DBStory* s, int32_t i, int32_t frame, uint8_t* out) {
    const auto* p = story_page(s, i);
    if (!a || !p || !out) return 0;
    if (!s->story.has_commands) return db_story_background(a, s, i, out);
    auto cached = [](void* ctx, const std::string& name, std::vector<uint8_t>& rgba, int& w, int& h) {
        auto* l = static_cast<StoryLoad*>(ctx);
        auto it = l->s->images.find(name);
        if (it == l->s->images.end()) {
            DBStory::Img img;
            img.ok = load_rgba(l->a, name, img.rgba, img.w, img.h);
            it = l->s->images.emplace(name, std::move(img)).first;
        }
        if (!it->second.ok) return false;
        rgba = it->second.rgba;
        w = it->second.w;
        h = it->second.h;
        return true;
    };
    StoryLoad ctx{a, s};
    const std::vector<uint8_t> bg = db::story::compose(p->scene, cached, &ctx, frame);
    memcpy(out, bg.data(), bg.size());
    return 1;
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
