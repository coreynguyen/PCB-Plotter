// ============================================================================
// panelize.cpp — CircuitCAM-style panelization. See panelize.h.
// ============================================================================
#include "panelize.h"
#include <algorithm>
#include <cctype>
#include <cmath>

namespace pcbp {

static int norm_rot(int r) { r %= 360; if (r < 0) r += 360; return (r / 90) * 90; }

void PanelSetup::reset() {
    cells.clear();
    PanelCell c;
    c.root = true;
    c.assigned = true;
    cells.push_back(c);
}

PanelCell* PanelSetup::at(int x, int y) {
    for (auto& c : cells) if (c.x == x && c.y == y) return &c;
    return nullptr;
}
const PanelCell* PanelSetup::at(int x, int y) const {
    for (auto& c : cells) if (c.x == x && c.y == y) return &c;
    return nullptr;
}

PanelCell& PanelSetup::set(int x, int y, int rot) {
    PanelCell* c = at(x, y);
    if (!c) {
        PanelCell n;
        n.x = x; n.y = y;
        cells.push_back(n);
        c = &cells.back();
    }
    if (!c->root) { c->rot = norm_rot(rot); c->assigned = false; }
    return *c;
}

bool PanelSetup::remove(int x, int y) {
    for (size_t i = 0; i < cells.size(); ++i)
        if (cells[i].x == x && cells[i].y == y) {
            if (cells[i].root) return false;
            cells.erase(cells.begin() + i);
            return true;
        }
    return false;
}

bool PanelSetup::assign_root(int x, int y) {
    PanelCell* c = at(x, y);
    if (!c) return false;
    for (auto& k : cells) { k.root = false; k.assigned = false; }
    c->root = true;
    c->rot = 0;
    c->assigned = true;
    c->offset = {};
    // relative rotations stay as entered; all other images need re-panelizing
    return true;
}

int PanelSetup::max_x() const { int m = 0; for (auto& c : cells) m = std::max(m, c.x); return m; }
int PanelSetup::max_y() const { int m = 0; for (auto& c : cells) m = std::max(m, c.y); return m; }

PanelSetup PanelSetup::from_images(const std::vector<PanelImage>& v) {
    PanelSetup s;
    for (auto& im : v) {
        PanelCell c;
        c.x = im.x_index; c.y = im.y_index;
        c.rot = norm_rot((int)std::lround(im.rot));
        c.root = im.root;
        c.assigned = true;
        c.x_out = im.x_out;
        c.offset = im.offset;
        if (!s.at(c.x, c.y)) s.cells.push_back(c);
    }
    if (s.cells.empty()) s.reset();
    bool has_root = false;
    for (auto& c : s.cells) has_root |= c.root;
    if (!has_root) s.cells[0].root = true;
    return s;
}

// Cells in CircuitCAM scan order: X index outer, Y index inner.
static std::vector<const PanelCell*> scan(const PanelSetup& s) {
    std::vector<const PanelCell*> v;
    for (auto& c : s.cells) v.push_back(&c);
    std::sort(v.begin(), v.end(), [](const PanelCell* a, const PanelCell* b) {
        return a->x != b->x ? a->x < b->x : a->y < b->y;
    });
    return v;
}

PanelMarkers panel_markers(const PanelSetup& s) {
    PanelMarkers m;
    auto order = scan(s);
    for (auto* c : order) if (c->root) { m.root_x = c->x; m.root_y = c->y; break; }
    for (int i = 0; i < 4; ++i) {
        const int ang = i * 90;
        for (auto* c : order) {
            if (c->root || c->rot != ang) continue;
            if (i == 0 && c->x == m.root_x) continue;
            m.red_x[i] = c->x; m.red_y[i] = c->y;
            break;
        }
        bool found = false;
        for (auto* c : order) {
            if (c->root || c->rot != ang) continue;
            if (c->x == m.root_x && c->y == m.root_y) continue;
            if (c->x == m.red_x[i] || c->y == m.red_y[i]) continue;
            if (i == 0 && c->y == m.root_y) continue;
            m.blue_x[i] = c->x; m.blue_y[i] = c->y;
            found = true;
            break;
        }
        if (!found) {
            for (auto* c : order) {
                if (c->root || c->rot != ang) continue;
                if (c->x == m.root_x && c->y == m.root_y) continue;
                if (c->x == m.red_x[i] && c->y == m.red_y[i]) continue;
                if (i == 0 && c->y == m.root_y) continue;
                m.blue_x[i] = c->x; m.blue_y[i] = c->y;
                break;
            }
        }
    }
    return m;
}

bool panel_group_enabled(const PanelSetup& s, int i) {
    for (auto& c : s.cells) {
        if (c.rot != i * 90) continue;
        if (i == 0 && c.root) continue;
        return true;
    }
    return false;
}

std::vector<std::string> panel_requirements(const PanelSetup& s) {
    std::vector<std::string> v;
    PanelMarkers m = panel_markers(s);
    for (int i = 0; i < 4; ++i) {
        if (!panel_group_enabled(s, i)) continue;
        std::string g = std::to_string(i * 90) + " degree images: ";
        if (i == 0) {
            if (m.red_x[0] >= 0) v.push_back(g + "Yellow To Red X (root -> cell " + std::to_string(m.red_x[0]) + "," + std::to_string(m.red_y[0]) + ")");
            if (m.blue_x[0] >= 0) v.push_back(g + "Yellow To Blue Y (root -> cell " + std::to_string(m.blue_x[0]) + "," + std::to_string(m.blue_y[0]) + ")");
        } else {
            v.push_back(g + "Yellow To Red X,Y (root -> cell " + std::to_string(m.red_x[i]) + "," + std::to_string(m.red_y[i]) + ")");
            if (m.blue_x[i] >= 0)
                v.push_back(g + "Yellow To Blue X,Y (root -> cell " + std::to_string(m.blue_x[i]) + "," + std::to_string(m.blue_y[i]) + ")");
        }
    }
    return v;
}

bool panel_ref_pos(const Project& prj, const std::string& ref, Vec2& out) {
    for (auto& c : prj.comps) {
        if (c.ref.size() != ref.size()) continue;
        bool eq = true;
        for (size_t i = 0; i < ref.size(); ++i)
            if (std::toupper((unsigned char)c.ref[i]) != std::toupper((unsigned char)ref[i])) { eq = false; break; }
        if (eq) { out = c.pos; return true; }
    }
    return false;
}

static double pitch(double dist, int idx_a, int idx_b) {
    if (idx_a < 0 || idx_b < 0) return 0;
    int d = idx_b - idx_a;
    return d ? dist / d : dist;
}

bool panelize(PanelSetup& s, Vec2 ref, std::string& err) {
    PanelMarkers m = panel_markers(s);
    if (m.root_x < 0) { err = "no root image assigned"; return false; }
    bool rotated = false;
    for (auto& c : s.cells) rotated |= c.rot != 0;
    if (rotated && s.ref_location.empty()) { err = "Please select a reference location"; return false; }
    auto mm = [&](double v) { return from_display(v, s.units); };
    for (int i = 0; i < 4; ++i) {
        const RotGroupOffsets& g = s.groups[i];
        double px, py;
        Vec2 base;
        int ax, ay;     // anchor indices
        if (i == 0) {
            px = m.red_x[0] >= 0 ? pitch(mm(g.red_x), m.root_x, m.red_x[0]) : 0;
            py = m.blue_y[0] >= 0 ? pitch(mm(g.blue_y), m.root_y, m.blue_y[0]) : 0;
            ax = m.root_x; ay = m.root_y;
        } else {
            px = (m.blue_x[i] >= 0 && m.red_x[i] >= 0) ? pitch(mm(g.blue_x - g.red_x), m.red_x[i], m.blue_x[i]) : 0;
            py = (m.blue_y[i] >= 0 && m.red_y[i] >= 0) ? pitch(mm(g.blue_y - g.red_y), m.red_y[i], m.blue_y[i]) : 0;
            Vec2 r = rotate(ref, i * 90.0);
            base = ref - r + Vec2{mm(g.red_x), mm(g.red_y)};
            ax = m.red_x[i]; ay = m.red_y[i];
        }
        for (auto& c : s.cells) {
            if (c.root || c.rot != i * 90) continue;
            c.offset = {(c.x - ax) * px + base.x, (c.y - ay) * py + base.y};
            c.assigned = true;
        }
    }
    for (auto& c : s.cells) if (c.root) { c.offset = {}; c.assigned = true; }
    return true;
}

bool step_and_repeat_all(PanelSetup& s, int nx, int ny, double dx, double dy, std::string& err) {
    if (nx < 2 && ny < 2) { err = "X Number --OR-- Y Number must be greater than 1"; return false; }
    if (nx < 1) { err = "X Number invalid"; return false; }
    if (ny < 1) { err = "Y Number invalid"; return false; }
    if (nx > 1 && dx == 0) { err = "X Offset invalid"; return false; }
    if (ny > 1 && dy == 0) { err = "Y Offset invalid"; return false; }
    if (s.cells.empty()) s.reset();
    const double mdx = from_display(dx, s.units), mdy = from_display(dy, s.units);
    const int W = s.max_x() + 1, H = s.max_y() + 1;
    std::vector<PanelCell> base = s.cells;
    std::vector<PanelCell> add;
    for (int ix = 0; ix < nx; ++ix)
        for (int iy = 0; iy < ny; ++iy) {
            if (!ix && !iy) continue;
            for (auto& e : base) {
                PanelCell c = e;
                c.root = false;
                c.assigned = true;
                c.offset = e.offset + Vec2{ix * mdx, iy * mdy};
                c.x = ix * W + e.x;
                c.y = iy * H + e.y;
                add.push_back(c);
            }
        }
    for (auto& c : add) {
        if (PanelCell* ex = s.at(c.x, c.y)) *ex = c;
        else s.cells.push_back(c);
    }
    return true;
}

std::vector<PanelImage> panel_images(const PanelSetup& s) {
    std::vector<PanelImage> out;
    auto order = scan(s);
    for (auto* c : order)
        if (c->root) {
            PanelImage im;
            im.name = "1"; im.root = true; im.offset = {}; im.rot = 0;
            im.x_index = c->x; im.y_index = c->y; im.x_out = c->x_out;
            out.push_back(im);
        }
    int n = 2;
    for (auto* c : order) {
        if (c->root) continue;
        PanelImage im;
        im.name = std::to_string(n++);
        im.root = false;
        im.offset = c->offset;
        im.rot = c->rot;
        im.x_index = c->x; im.y_index = c->y; im.x_out = c->x_out;
        out.push_back(im);
    }
    return out;
}

// R22 (REVIEW_V21 §1.1/§2.1): never generates a board outline. The array is the reference circuit plus linked
// instances (images); outlines are physical data the user defines (Board > Outline, Gerber tab).
void apply_panel(Project& prj, const PanelSetup& s) {
    std::vector<PanelImage> v = panel_images(s);
    if (!prj.images.empty() && !v.empty()) {   // keep the reference image's metadata (name, legacy rot)
        PanelImage r = prj.images[0];
        r.root = true; r.offset = {}; r.x_index = v[0].x_index; r.y_index = v[0].y_index; r.x_out = v[0].x_out;
        if (r.name.empty()) r.name = "1";
        v[0] = r;
    }
    prj.images = std::move(v);
    prj.dirty = true;
}

void panel_backfill_offsets(PanelSetup& s, Vec2 ref) {
    const PanelMarkers m = panel_markers(s);
    auto d = [&](int x, int y, bool& ok) -> Vec2 {
        const PanelCell* c = s.at(x, y);
        ok = c && c->assigned;
        return ok ? rotate(ref, c->rot) + c->offset - ref : Vec2{};
    };
    for (int g = 0; g < 4; ++g) {
        bool ok = false;
        if (m.red_x[g] >= 0) {
            const Vec2 v = d(m.red_x[g], m.red_y[g], ok);
            if (ok) { s.groups[g].red_x = to_display(v.x, s.units); s.groups[g].red_y = to_display(v.y, s.units); }
        }
        if (m.blue_x[g] >= 0) {
            const Vec2 v = d(m.blue_x[g], m.blue_y[g], ok);
            if (ok) { s.groups[g].blue_x = to_display(v.x, s.units); s.groups[g].blue_y = to_display(v.y, s.units); }
        }
    }
}

void panel_setup_to_params(const PanelSetup& s, Project::ArrayParams& a) {
    a.nl_saved = true;
    a.nl_cells.clear();
    for (auto& c : s.cells) a.nl_cells.push_back({c.x, c.y, c.rot, c.root, c.x_out});
    for (int g = 0; g < 4; ++g) {
        a.nl_groups[g][0] = s.groups[g].red_x; a.nl_groups[g][1] = s.groups[g].red_y;
        a.nl_groups[g][2] = s.groups[g].blue_x; a.nl_groups[g][3] = s.groups[g].blue_y;
    }
    a.nl_units = s.units;
    a.nl_ref = s.ref_location;
}

bool panel_setup_from_params(const Project::ArrayParams& a, PanelSetup& s) {
    if (!a.nl_saved || a.nl_cells.empty()) return false;
    PanelSetup t;
    bool root = false;
    for (auto& c : a.nl_cells) {
        if (t.at(c.x, c.y)) continue;
        PanelCell k;
        k.x = c.x; k.y = c.y; k.rot = c.root ? 0 : norm_rot(c.rot); k.root = c.root && !root; k.x_out = c.x_out;
        k.assigned = k.root;   // offsets are recomputed on Apply
        root |= k.root;
        t.cells.push_back(k);
    }
    if (!root) t.cells[0].root = t.cells[0].assigned = true, t.cells[0].rot = 0;
    for (int g = 0; g < 4; ++g) {
        t.groups[g].red_x = a.nl_groups[g][0]; t.groups[g].red_y = a.nl_groups[g][1];
        t.groups[g].blue_x = a.nl_groups[g][2]; t.groups[g].blue_y = a.nl_groups[g][3];
    }
    t.units = a.nl_units;
    t.ref_location = a.nl_ref;
    s = std::move(t);
    return true;
}

} // namespace pcbp
