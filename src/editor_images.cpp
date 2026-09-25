// Images in the editor: an image line laid out as a picture or a row of
// them, the caret stepping over them, typing and deleting beside them,
// moving them by mouse or key (with the animations that follow), and
// drawing them. The app gets them in and out (see app_images.cpp).
#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "editor.h"
#include "theme.h"
#include "util.h"

namespace {

// An image line's ![...](...) tokens (none if it isn't one), and a row
// rebuilt from them, one space apart.
std::vector<std::string> image_tokens(const std::string& line) {
    std::vector<ImageRef> refs;
    std::vector<std::string> out;
    if (md_image_row(line, &refs))
        for (auto& r : refs) out.push_back(line.substr(r.begin, r.end - r.begin));
    return out;
}

std::string join_images(const std::vector<std::string>& tokens) {
    std::string s;
    for (auto& t : tokens) s += (s.empty() ? "" : " ") + t;
    return s;
}

}  // namespace

bool Editor::image_rect(int line, int tile, ImVec2& a, ImVec2& b) const {
    int mode = reader_ ? 1 : 0;
    if (line < 0 || line >= (int)lay_[mode].size() || line >= (int)lines_.size()) return false;
    const Line& L = lay_[mode][line];
    if (!L.img || tile < 0 || tile >= (int)L.tiles.size()) return false;
    const auto& t = L.tiles[tile];
    float y = origin_y_ + L.dy + L.pad + t.y + (mode == 0 && line < (int)yoff_.size() ? yoff_[line] : 0.0f);
    a = ImVec2(std::round(col_x_ + t.x), std::round(y));
    b = ImVec2(a.x + t.w, a.y + t.h);
    return true;
}

bool Editor::image_at(ImVec2 screen, int& line, int& tile) const {
    int mode = reader_ ? 1 : 0;
    if (lay_[mode].empty()) return false;
    int l = line_at_y(mode, screen.y - origin_y_);
    if (l >= (int)lay_[mode].size() || !lay_[mode][l].img) return false;
    for (int k = 0; k < (int)lay_[mode][l].tiles.size(); k++) {
        ImVec2 a, b;
        if (image_rect(l, k, a, b) && screen.x >= a.x && screen.x < b.x && screen.y >= a.y && screen.y < b.y) {
            line = l;
            tile = k;
            return true;
        }
    }
    return false;
}

std::string Editor::image_src(int line, int tile) const {
    std::vector<ImageRef> refs;
    if (line < 0 || line >= (int)lines_.size() || !md_image_row(lines_[line], &refs) || tile < 0 || tile >= (int)refs.size()) return {};
    return refs[tile].src;
}

std::string Editor::image_markdown(int line, int tile) const {
    std::vector<ImageRef> refs;
    if (line < 0 || line >= (int)lines_.size() || !md_image_row(lines_[line], &refs) || tile < 0 || tile >= (int)refs.size()) return {};
    return lines_[line].substr(refs[tile].begin, refs[tile].end - refs[tile].begin);
}

std::vector<std::pair<int, int>> Editor::image_list() const {
    std::vector<std::pair<int, int>> out;
    for (int l = 0; l < (int)lines_.size() && l < (int)lay_[0].size(); l++)
        if (lay_[0][l].img)
            for (int k = 0; k < (int)lay_[0][l].tiles.size(); k++) out.push_back({l, k});
    return out;
}

int Editor::image_for_caret(int* tile) const {
    if (reader_) return -1;
    if (!has_selection()) {
        if (!is_img(cur_.line)) return -1;
        if (tile) *tile = img_tile(lay_[0][cur_.line], cur_.col);
        return cur_.line;
    }
    TextPos a = std::min(cur_, anchor_), b = std::max(cur_, anchor_);
    if (!is_img(a.line)) return -1;
    const Line& L = lay_[0][a.line];
    int len = (int)lines_[a.line].size();
    // The whole line, with or without its break, when it's one image...
    bool whole = a.col == 0 && ((b.line == a.line && b.col == len) || (b.line == a.line + 1 && b.col == 0));
    if (whole && L.tiles.size() == 1) {
        if (tile) *tile = 0;
        return a.line;
    }
    // ...or exactly one image of a row.
    if (b.line != a.line) return -1;
    for (int k = 0; k < (int)L.tiles.size(); k++)
        if (L.tiles[k].b0 == a.col && L.tiles[k].b1 == b.col) {
            if (tile) *tile = k;
            return a.line;
        }
    return -1;
}

// Takes one image out of its line; a row closes up around the gap, and a
// line left with nothing is left empty. The caret stays where it was.
void Editor::delete_tile(int line, int tile) {
    std::vector<std::string> keep = image_tokens(lines_[line]);
    if (tile < 0 || tile >= (int)keep.size()) return;
    keep.erase(keep.begin() + tile);
    lines_[line] = join_images(keep);
    // Where the gap is: before what followed the removed one.
    int col = 0;
    if (tile >= (int)keep.size()) col = (int)lines_[line].size();
    else for (int k = 0; k < tile; k++) col += (int)keep[k].size() + 1;
    cur_ = anchor_ = {line, col};
}

void Editor::delete_image(int line, int tile) {
    if (line < 0 || line >= (int)lines_.size()) return;
    begin_edit(Edit::Other);
    delete_tile(line, tile);
    end_edit();
}

// An image line that can be shown is one row: the image at its own pixel
// size (a screenshot shows 1:1), no wider than the column and no taller
// than most of the view, with a little air above and below. One that can't
// (missing file, a web address) keeps its text rows.
//
// A single image is the size asked for (|300 is 300 CSS px, scaled with
// zoom), or its own; several on a line are a row: justified, like a photo
// grid, every image in a row the same height and the row exactly the
// column's width, nothing cropped. Past four they wrap into more rows,
// balanced. Sizes asked for are ignored in a row (the row decides).
void Editor::layout_image(Line& L, const std::string& s, int index) {
    L.img = false;
    L.tiles.clear();
    std::vector<ImageRef> refs;
    if (P_.images && md_image_row(s, &refs)) {
        std::vector<Line::Tile> tiles;
        for (auto& r : refs) {
            Line::Tile t;
            if (!P_.images->size(r.src, t.iw, t.ih)) { tiles.clear(); break; }
            t.src = r.src;
            t.b0 = r.begin;
            t.b1 = r.end;
            tiles.push_back(t);
        }
        if (!tiles.empty()) {
            const float col = P_.column;
            const float max_h = std::max(P_.size * 6, (P_.max.y - P_.min.y) * 0.7f);
            const float gap = std::round(P_.size * 0.4f);
            L.img_m = std::round(P_.size * 0.5f);
            float y = L.img_m;
            int n = (int)tiles.size();
            if (n == 1) {
                Line::Tile& t = tiles[0];
                const ImageRef& r = refs[0];
                float w = (float)t.iw;
                if (r.w > 0 && r.h > 0) w = std::min(r.w * P_.px, (float)t.iw * (r.h * P_.px) / t.ih);  // fit the box
                else if (r.w > 0) w = r.w * P_.px;
                if (index == resize_line_ && resize_line_ >= 0) w = resize_w_;  // being resized: follow the handle
                w = std::min(w, col);
                float h = w * t.ih / t.iw;
                if (h > max_h) { h = max_h; w = h * t.iw / t.ih; }
                t.x = 0;
                t.y = y;
                t.w = std::max(1.0f, std::round(w));
                t.h = std::max(1.0f, std::round(h));
                y += t.h;
            } else {
                int rows = (n + 3) / 4, base = n / rows, extra = n % rows;
                int k = 0;
                for (int r = 0; r < rows; r++) {
                    int count = base + (r < extra ? 1 : 0);
                    float sum_a = 0;
                    for (int j = k; j < k + count; j++) sum_a += (float)tiles[j].iw / tiles[j].ih;
                    float h = std::min((col - gap * (count - 1)) / sum_a, max_h);
                    float x = 0;
                    for (int j = k; j < k + count; j++) {
                        Line::Tile& t = tiles[j];
                        float w = h * t.iw / t.ih;
                        t.x = std::round(x);
                        t.y = y;
                        // The last one takes up the rounding, so the row ends
                        // exactly at the column's edge.
                        float right = j == k + count - 1 && h < max_h ? col : std::round(x + w);
                        t.w = std::max(1.0f, right - t.x);
                        t.h = std::max(1.0f, std::round(h));
                        x += w + gap;
                    }
                    y += std::round(h) + (r + 1 < rows ? gap : 0);
                    k += count;
                }
            }
            L.img = true;
            L.tiles = std::move(tiles);
            L.rows.assign(1, 0);
            L.row_h = y + L.img_m;
            L.hang = 0;
            return;
        }
    }
    L.rows = L.text_rows;
    L.row_h = L.text_row_h;
}

// ---- the caret among images

int Editor::img_stop(const Line& L, int col) {
    int k = 0;
    for (auto& t : L.tiles)
        if (t.b1 <= col) k++;
    return k;
}

int Editor::stop_col(const Line& L, int k, int len) {
    int n = (int)L.tiles.size();
    if (k <= 0) return 0;
    if (k >= n) return len;
    return L.tiles[k].b0;
}

int Editor::picked(int* tile) const {
    if (reader_ || !has_selection() || cur_.line != anchor_.line || !is_img(cur_.line)) return -1;
    int a = std::min(cur_.col, anchor_.col), b = std::max(cur_.col, anchor_.col);
    const Line& L = lay_[0][cur_.line];
    for (int k = 0; k < (int)L.tiles.size(); k++)
        if (L.tiles[k].b0 == a && L.tiles[k].b1 == b) {
            if (tile) *tile = k;
            return cur_.line;
        }
    return -1;
}

// From the text rather than the layout, so it works right after an edit.
void Editor::pick(int line, int tile, bool reveal) {
    std::vector<ImageRef> refs;
    if (line < 0 || line >= (int)lines_.size() || !md_image_row(lines_[line], &refs) || refs.empty()) return;
    const ImageRef& r = refs[std::clamp(tile, 0, (int)refs.size() - 1)];
    anchor_ = {line, r.begin};
    cur_ = {line, r.end};
    if (reveal) { reveal_ = true; reveal_mode_ = 0; }
    activity_ = ImGui::GetTime();
}

bool Editor::unpick() {
    if (picked() < 0) return false;
    cur_ = anchor_ = std::max(cur_, anchor_);
    return true;
}

// A caret left on an image line (a click beside one, an undo, a jump)
// picks the image before it, or the first.
void Editor::settle_on_images() {
    if (reader_ || has_selection() || !is_img(cur_.line) || img_drag_ || resize_line_ >= 0) return;
    const Line& L = lay_[0][cur_.line];
    const Line::Tile& t = L.tiles[img_tile(L, cur_.col)];
    anchor_ = {cur_.line, t.b0};
    cur_ = {cur_.line, t.b1};
}

// Left/right from a picked image: the next image in its row, or off the
// line (onto the next line's first image or text).
void Editor::step_picked(int line, int tile, int dir) {
    want_x_ = -1;
    if (tile + dir >= 0 && tile + dir < (int)lay_[0][line].tiles.size()) { pick(line, tile + dir); return; }
    int to = line + dir;
    if (to < 0 || to >= (int)lines_.size()) return;
    if (is_img(to)) pick(to, dir > 0 ? 0 : (int)lay_[0][to].tiles.size() - 1);
    else move_to({to, dir > 0 ? 0 : (int)lines_[to].size()}, false);
}

// Up/down from a picked image: the nearest image in the row above or below
// (rows of more than four wrap), else off the line.
void Editor::vertical_picked(int line, int tile, int dir) {
    const Line& L = lay_[0][line];
    const Line::Tile& t = L.tiles[tile];
    if (want_x_ < 0) want_x_ = t.x + t.w * 0.5f;
    float keep = want_x_;
    int best = -1;
    for (int k = 0; k < (int)L.tiles.size(); k++) {
        float y = L.tiles[k].y;
        if (dir > 0 ? y <= t.y : y >= t.y) continue;
        if (best < 0 || (dir > 0 ? y < L.tiles[best].y : y > L.tiles[best].y)) best = k;
    }
    if (best >= 0) {
        pick(line, nearest_tile(L, keep, L.tiles[best].y));
    } else {
        int to = line + dir;
        if (to < 0 || to >= (int)lines_.size()) return;
        const Line& N = lay_[0][to];
        if (is_img(to)) pick(to, nearest_tile(N, keep, dir < 0 ? N.tiles.back().y : N.tiles.front().y));
        else move_to({to, col_at_x(0, to, dir > 0 ? 0 : (int)lay_[0][to].rows.size() - 1, keep)}, false);
    }
    want_x_ = keep;
}

// Backspace (dir < 0) or delete on a picked image takes it out. Left in its
// row, the one before it (backspace) or the one that took its place
// (delete) is picked; a line left empty keeps the caret, where it was.
void Editor::remove_picked(int line, int tile, int dir) {
    begin_edit(Edit::Other);
    delete_tile(line, tile);
    std::vector<ImageRef> refs;
    if (md_image_row(lines_[line], &refs) && !refs.empty())
        pick(line, dir < 0 ? std::max(0, tile - 1) : std::min(tile, (int)refs.size() - 1));
    end_edit();
}

// The image in the row at row_y nearest x.
int Editor::nearest_tile(const Line& L, float x, float row_y) {
    float dist = 1e9f;
    int best = 0;
    for (int k = 0; k < (int)L.tiles.size(); k++) {
        const Line::Tile& t = L.tiles[k];
        float dx = std::fabs(t.x + t.w * 0.5f - x);
        if (t.y == row_y && dx < dist) { dist = dx; best = k; }
    }
    return best;
}

// Where the caret stands at `col` in an image line: just clear of the edge
// of the image it's beside, as tall as that image (relative to the line).
void Editor::img_caret(const Line& L, int col, float& x, float& y, float& h) const {
    int n = (int)L.tiles.size(), k = img_stop(L, col);
    float gap = std::round(P_.size * 0.3f);
    if (k >= n) {
        const auto& t = L.tiles[n - 1];
        x = t.x + t.w + gap;
        y = t.y;
        h = t.h;
        return;
    }
    const auto& t = L.tiles[k];
    bool same_row = k > 0 && L.tiles[k - 1].y == t.y;
    x = same_row ? std::round((L.tiles[k - 1].x + L.tiles[k - 1].w + t.x) * 0.5f) : t.x - gap;
    y = t.y;
    h = t.h;
}

int Editor::img_tile_at(const Line& L, float x, float y) const {
    for (int i = 0; i < (int)L.tiles.size(); i++) {
        const auto& t = L.tiles[i];
        if (x >= t.x && x < t.x + t.w && y >= t.y && y < t.y + t.h) return i;
    }
    return -1;
}

// The stop nearest a point: the row of images at that height (or nearest),
// then before or after the image under x by which half.
int Editor::img_col_at(const Line& L, int len, float x, float y) const {
    int n = (int)L.tiles.size();
    float best = 1e9f, row_y = L.tiles[0].y;
    for (auto& t : L.tiles) {
        float d = y < t.y ? t.y - y : (y > t.y + t.h ? y - (t.y + t.h) : 0.0f);
        if (d < best) { best = d; row_y = t.y; }
    }
    int k = -1;
    for (int i = 0; i < n; i++) {
        const auto& t = L.tiles[i];
        if (t.y != row_y) continue;
        if (k < 0) k = i;                 // first in the row
        if (x >= t.x + t.w * 0.5f) k = i + 1;
    }
    return stop_col(L, std::max(k, 0), len);
}

// Where each image is drawn now, before an edit changes the lines. A key
// is its path and which use of that path it is, in document order.
void Editor::shoot_tiles() {
    if (tile_shots_new_) return;  // a second edit this frame: what was drawn is already noted
    tile_shots_.clear();
    if (reader_ || !anim::enabled) return;
    std::unordered_map<std::string, int> seen;
    for (int i = 0; i < (int)lay_[0].size(); i++) {
        const Line& L = lay_[0][i];
        if (!L.img) continue;
        for (int k = 0; k < (int)L.tiles.size(); k++) {
            ImVec2 a, b;
            if (!tile_rect(i, k, a, b)) continue;
            float alpha = 1.0f;
            if (const TileMorph* m = tile_morph(i, k)) {
                a = ImVec2(a.x + m->da.x, a.y + m->da.y);
                b = ImVec2(b.x + m->db.x, b.y + m->db.y);
                alpha = m->alpha;
            }
            const std::string& src = L.tiles[k].src;
            tile_shots_.push_back({src + '\n' + std::to_string(seen[src]++), src, a, b, alpha});
        }
    }
    // Those still going carry on from where they are (they're in the shots).
    tile_morphs_.clear();
    tile_shots_new_ = true;
}

// The new layout is in place: each image eases from its shot to there.
void Editor::morph_tiles() {
    tile_shots_new_ = false;
    std::unordered_map<std::string, int> seen;
    std::unordered_map<std::string, const TileShot*> from;
    for (auto& s : tile_shots_) from[s.key] = &s;
    for (int i = 0; i < (int)lay_[0].size(); i++) {
        const Line& L = lay_[0][i];
        if (!L.img) continue;
        for (int k = 0; k < (int)L.tiles.size(); k++) {
            const std::string& src = L.tiles[k].src;
            auto it = from.find(src + '\n' + std::to_string(seen[src]++));
            ImVec2 a, b;
            if (!tile_rect(i, k, a, b)) continue;
            if (it == from.end()) {  // new here: it fades in
                tile_morphs_.push_back({i, k, ImVec2(0, 0), ImVec2(0, 0), 0.0f});
                continue;
            }
            const TileShot& s = *it->second;
            from.erase(it);
            if (i == land_line_ && k == land_tile_) continue;  // it has its own flight
            TileMorph m{i, k, ImVec2(s.a.x - a.x, s.a.y - a.y), ImVec2(s.b.x - b.x, s.b.y - b.y), s.alpha};
            if (std::fabs(m.da.x) + std::fabs(m.da.y) + std::fabs(m.db.x) + std::fabs(m.db.y) > 0.5f || m.alpha < 1.0f)
                tile_morphs_.push_back(m);
        }
    }
    for (auto& [key, s] : from) tile_gone_.push_back({s->src, s->a, s->b, s->alpha});  // taken out
    tile_shots_.clear();
}

void Editor::step_tile_morphs(float dt) {
    const float rate = 24.0f;  // as the landing flight
    for (auto& m : tile_morphs_) {
        m.da.x = anim::approach(m.da.x, 0.0f, rate, dt, 0.3f);
        m.da.y = anim::approach(m.da.y, 0.0f, rate, dt, 0.3f);
        m.db.x = anim::approach(m.db.x, 0.0f, rate, dt, 0.3f);
        m.db.y = anim::approach(m.db.y, 0.0f, rate, dt, 0.3f);
        m.alpha = anim::approach(m.alpha, 1.0f, rate, dt, 0.01f);
    }
    tile_morphs_.erase(std::remove_if(tile_morphs_.begin(), tile_morphs_.end(), [](const TileMorph& m) {
        return m.da.x == 0 && m.da.y == 0 && m.db.x == 0 && m.db.y == 0 && m.alpha >= 1.0f;
    }), tile_morphs_.end());
    for (auto& g : tile_gone_) g.alpha = anim::approach(g.alpha, 0.0f, 30.0f, dt, 0.01f);
    tile_gone_.erase(std::remove_if(tile_gone_.begin(), tile_gone_.end(), [](const TileGone& g) { return g.alpha <= 0.0f; }), tile_gone_.end());
}

// Images an edit took out: they fade where they were, settling a little.
void Editor::draw_tiles_gone(ImDrawList* dl) {
    float round = std::round(3 * P_.dpi);
    for (auto& g : tile_gone_) {
        float s = 0.06f * (1.0f - g.alpha);
        ImVec2 in((g.b.x - g.a.x) * s, (g.b.y - g.a.y) * s);
        ImVec2 a(std::round(col_x_ + g.a.x + in.x), std::round(origin_y_ + g.a.y + in.y));
        ImVec2 b(std::round(col_x_ + g.b.x - in.x), std::round(origin_y_ + g.b.y - in.y));
        if (b.y < P_.min.y || a.y > P_.max.y) continue;
        float shown = 0;
        ImTextureID tex = P_.images ? P_.images->texture(g.src, (int)std::ceil(g.b.x - g.a.x), shown) : ImTextureID{};
        if (tex) dl->AddImageRounded(ImTextureRef(tex), a, b, ImVec2(0, 0), ImVec2(1, 1), fade(col_alpha(IM_COL32_WHITE, g.alpha * shown)), round);
        else dl->AddRectFilled(a, b, fade(col_alpha(T.code_bg, g.alpha)), round);
    }
}

const Editor::TileMorph* Editor::tile_morph(int line, int tile) const {
    for (auto& m : tile_morphs_)
        if (m.line == line && m.tile == tile) return &m;
    return nullptr;
}

// Typing or pasting with the caret beside an image starts a line of its own
// on that side, so the image's line stays just the image; with an image
// picked, after it. Call inside an edit.
void Editor::step_off_image() {
    unpick();
    if (has_selection() || !is_img(cur_.line)) return;
    int l = cur_.line;
    const Line& L = lay_[0][l];
    int k = img_stop(L, cur_.col), n = (int)L.tiles.size();
    if (k == 0) {
        lines_.insert(lines_.begin() + l, std::string());
        cur_ = anchor_ = {l, 0};
    } else if (k >= n) {
        lines_.insert(lines_.begin() + l + 1, std::string());
        cur_ = anchor_ = {l + 1, 0};
    } else {
        // Between images in a row: the row splits, the text between.
        std::string s = lines_[l];
        int at = L.tiles[k].b0;
        lines_[l] = trim(s.substr(0, at));
        lines_.insert(lines_.begin() + l + 1, {std::string(), trim(s.substr(at))});
        cur_ = anchor_ = {l + 1, 0};
    }
}

// Backspace around images (no selection). Returns false to leave it to the
// normal backspace. Joining lines would pull text onto an image's line
// (turning it back into text), so instead:
//  - after an image: it's removed, its line left empty;
//  - before an image: an empty line above is removed; text above is just
//    stepped onto (the caret moves to its end);
//  - at the start of a line below an image: an empty line is removed; a line
//    with text steps onto the image (the next backspace removes it).
bool Editor::image_backspace() {
    int l = cur_.line;
    if (is_img(l) && cur_.col > 0) {  // the image before the caret
        begin_edit(Edit::Other);
        delete_tile(l, img_stop(lay_[0][l], cur_.col) - 1);
        end_edit();
        return true;
    }
    if (cur_.col != 0 || l == 0) return false;
    if (is_img(l)) {
        if (trim(lines_[l - 1]).empty()) {
            begin_edit(Edit::Other);
            lines_.erase(lines_.begin() + l - 1);
            cur_ = anchor_ = {l - 1, 0};
            end_edit();
        } else {
            move_to({l - 1, (int)lines_[l - 1].size()}, false);
        }
        return true;
    }
    if (is_img(l - 1)) {
        if (lines_[l].empty()) {
            begin_edit(Edit::Other);
            lines_.erase(lines_.begin() + l);
            cur_ = anchor_ = {l - 1, (int)lines_[l - 1].size()};
            end_edit();
        } else {
            move_to({l - 1, (int)lines_[l - 1].size()}, false);
        }
        return true;
    }
    return false;
}

// Delete around images, the mirror of image_backspace.
bool Editor::image_delete() {
    int l = cur_.line;
    int n = (int)lines_.size();
    if (is_img(l) && cur_.col < (int)lines_[l].size()) {  // the image after the caret
        begin_edit(Edit::Other);
        delete_tile(l, img_stop(lay_[0][l], cur_.col));
        end_edit();
        return true;
    }
    if (cur_.col != (int)lines_[l].size() || l + 1 >= n) return false;
    if (is_img(l)) {
        if (trim(lines_[l + 1]).empty()) {
            begin_edit(Edit::Other);
            lines_.erase(lines_.begin() + l + 1);
            end_edit();
        } else {
            move_to({l + 1, 0}, false);
        }
        return true;
    }
    if (is_img(l + 1)) {
        if (lines_[l].empty()) {
            begin_edit(Edit::Other);
            lines_.erase(lines_.begin() + l);
            cur_ = anchor_ = {l, 0};
            end_edit();
        } else {
            move_to({l + 1, 0}, false);
        }
        return true;
    }
    return false;
}

// The mouse around images (edit view): the resize handle, dragging an image
// to join a row or a line of its own, double-clicking one in a row. Returns
// true when it has taken the mouse, so text selection leaves it alone.
bool Editor::handle_image_mouse(bool hovered, ImVec2 d) {
    ImGuiIO& io = ImGui::GetIO();
    auto& V = lay_[0];
    float dpi = P_.dpi;

    // Resizing: follow the pointer, snapping to quarters of the column and
    // the image's own size; let go to keep it.
    if (resize_line_ >= 0) {
        if (resize_line_ >= (int)V.size() || !V[resize_line_].img || V[resize_line_].tiles.size() != 1) { resize_line_ = -1; return false; }
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const Line::Tile& t = V[resize_line_].tiles[0];
            float w = std::clamp(d.x - t.x, 24 * dpi, P_.column);
            struct Snap { float w; const char* name; };
            float own = std::min((float)t.iw, P_.column);
            Snap snaps[] = {{own, "own size"}, {P_.column, "full width"}, {P_.column * 0.75f, "3/4"}, {P_.column * 0.5f, "1/2"}, {P_.column * 0.25f, "1/4"}};
            resize_snap_.clear();
            for (auto& s : snaps)
                if (std::fabs(w - s.w) < 10 * dpi) { w = s.w; resize_snap_ = s.name; break; }
            resize_w_ = w;
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            activity_ = ImGui::GetTime();
        } else {
            finish_resize();
        }
        return true;
    }

    // Dragging an image: where it would go.
    if (press_img_line_ >= 0) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            bool was = img_drag_;
            if (img_drag_) let_go_image(true);
            press_img_line_ = press_img_tile_ = -1;
            img_drag_ = false;
            return was;
        }
        if (img_drag_ && ImGui::IsKeyPressed(ImGuiKey_Escape)) {  // changed your mind: it flies home
            let_go_image(false);
            img_drag_ = false;
            press_img_line_ = -1;  // (stays that way until the button is let go)
            return true;
        }
        if (!img_drag_ && std::fabs(d.x - press_at_.x) + std::fabs(d.y - press_at_.y) > 6 * dpi) {
            img_drag_ = true;
            anchor_ = cur_;  // not a text selection
            drag_mode_ = 0;
            // Picked up where it was grabbed, at full size.
            ImVec2 ta, tb;
            if (tile_rect(press_img_line_, press_img_tile_, ta, tb)) grab_off_ = ImVec2(press_at_.x - ta.x, press_at_.y - ta.y);
            ghost_at_ = press_at_;
            ghost_k_ = 1.0f;
            land_line_ = -1;
            // Kept as it was: the preview lays its line out without it.
            drag_tile_ = V[press_img_line_].tiles[press_img_tile_];
            drag_token_ = lines_[press_img_line_].substr(drag_tile_.b0, drag_tile_.b1 - drag_tile_.b0);
            Line own;
            layout_image(own, drag_token_, -1);
            drag_own_tile_ = own.img ? own.tiles[0] : drag_tile_;
            drag_own_h_ = own.img ? own.row_h : drag_tile_.h;
        }
        if (img_drag_) {
            drag_at_ = d;
            // Near the top or bottom of the view, the page scrolls to bring
            // more into reach.
            float my = io.MousePos.y, edge = 24 * dpi;
            if (my < P_.min.y + edge) scroll_target_ -= (P_.min.y + edge - my) * io.DeltaTime * 14;
            if (my > P_.max.y - edge) scroll_target_ += (my - P_.max.y + edge) * io.DeltaTime * 14;
            drop_line_ = drop_stop_ = drop_own_ = -1;
            if (!V.empty()) {
                // Where the lines really are (drag_real_y_), not the preview.
                bool real = drag_real_y_.size() == V.size() + 1;
                int l;
                if (real) l = std::clamp((int)(std::upper_bound(drag_real_y_.begin(), drag_real_y_.end() - 1, d.y) - drag_real_y_.begin()) - 1, 0, (int)V.size() - 1);
                else l = std::clamp(line_at_y(0, d.y), 0, (int)V.size() - 1);
                const Line& L = V[l];
                float top = real ? drag_real_y_[l] : L.dy + (l < (int)yoff_.size() ? yoff_[l] : 0.0f);
                float h = real ? drag_real_y_[l + 1] - drag_real_y_[l] : L.dh;
                float ly = d.y - top - L.pad;
                auto it = drag_real_.find(l);
                const std::vector<Line::Tile>& tiles = it != drag_real_.end() ? it->second : L.tiles;
                // Anywhere across a line of images (blank space beside them,
                // and between the rows it wraps into, too) joins the nearest
                // row; only a strip along the top of its first row and the
                // bottom of its last, and the space between lines, means a
                // line of its own.
                if (L.img && !tiles.empty()) {
                    float top = tiles.front().y, bottom = 0;
                    for (auto& t : tiles) bottom = std::max(bottom, t.y + t.h);
                    float edge_top = std::min(tiles.front().h * 0.2f, 22 * dpi), edge_bottom = std::min(tiles.back().h * 0.2f, 22 * dpi);
                    if (ly >= top + edge_top && ly <= bottom - edge_bottom) {
                        int first = 0;
                        float nearest = 1e9f;
                        for (int i = 0; i < (int)tiles.size(); i++) {
                            const Line::Tile& t = tiles[i];
                            float dist = ly < t.y ? t.y - ly : ly > t.y + t.h ? ly - (t.y + t.h) : 0.0f;
                            if (dist < nearest) { nearest = dist; first = i; }  // (the first of its row)
                        }
                        // That row: before the image whose middle is past x.
                        int k = first;
                        for (int j = first; j < (int)tiles.size() && tiles[j].y == tiles[first].y; j++)
                            if (d.x >= tiles[j].x + tiles[j].w * 0.5f) k = j + 1;
                        drop_line_ = l;
                        drop_stop_ = k;
                    }
                }
                if (drop_line_ < 0) drop_own_ = d.y < top + h * 0.5f ? l : l + 1;  // a line of its own
            }
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            anim::active = true;
            return true;
        }
        return false;
    }
    // After an Escape, the press is spent until the button comes up.
    if (img_drag_ == false && press_img_line_ < 0 && land_line_ >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left)) return true;

    // Hover: which image the pointer is over, and whether it's on a handle.
    int was_line = hover_img_line_, was_tile = hover_tile_;
    hover_img_line_ = -1;
    hover_handle_ = false;
    hover_tile_ = -1;
    int hover_tile = -1;
    if (!hovered || V.empty()) return false;
    int l = std::clamp(line_at_y(0, d.y), 0, (int)V.size() - 1);
    if (V[l].img) {
        const Line& L = V[l];
        hover_tile = img_tile_at(L, d.x, d.y - L.dy - L.pad - (l < (int)yoff_.size() ? yoff_[l] : 0.0f));
        if (hover_tile >= 0) hover_img_line_ = l;
    }
    for (int cand : {hover_img_line_, is_img(cur_.line) ? cur_.line : -1}) {
        ImVec2 a, b;
        float slop = 6 * dpi;
        if (cand >= 0 && resize_handle(cand, a, b) && d.x >= a.x - slop && d.x <= b.x + slop && d.y >= a.y - slop && d.y <= b.y + slop) {
            hover_handle_ = true;
            hover_img_line_ = cand;
            break;
        }
    }
    if (hover_handle_) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    else if (hover_tile >= 0) hover_tile_ = hover_tile;  // (the caller shows a hand: it can be picked up)
    if (hover_img_line_ != was_line || hover_tile_ != was_tile) hover_glow_ = 0.0f;

    if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left) || io.KeyShift) return hover_handle_;
    int clicks = io.MouseClickedCount[0];
    if (hover_handle_) {
        if (clicks >= 2) {
            // Double-click the handle: back to its own size.
            const Line::Tile& t = V[hover_img_line_].tiles[0];
            std::string token = lines_[hover_img_line_].substr(t.b0, t.b1 - t.b0);
            std::string sized = md_image_resized(token, 0);
            if (sized != token) {
                begin_edit(Edit::Other);
                lines_[hover_img_line_].replace(t.b0, t.b1 - t.b0, sized);
                cur_ = anchor_ = {hover_img_line_, (int)lines_[hover_img_line_].size()};
                end_edit(false);
            }
        } else {
            resize_line_ = hover_img_line_;
            resize_w_ = resize_from_ = V[resize_line_].tiles[0].w;
            resize_snap_.clear();
            cur_ = anchor_ = {resize_line_, (int)lines_[resize_line_].size()};
        }
        drag_mode_ = 0;
        return true;
    }
    if (hover_img_line_ >= 0 && hover_tile >= 0) {
        // A double-click opens the viewer (the app sees to it); nothing is
        // selected, so closing it leaves things as they were.
        if (clicks >= 2) {
            drag_mode_ = 0;
            return true;
        }
        if (clicks == 1) {
            press_img_line_ = hover_img_line_;
            press_img_tile_ = hover_tile;
            press_at_ = d;
            pick(hover_img_line_, hover_tile, false);  // a click picks it (and holding on drags it)
            drag_mode_ = 0;
            want_x_ = -1;
            return true;
        }
    }
    return false;  // the click still places the caret as usual
}

// Let go of the resize handle: the width goes into the image's alt text
// ("|300", in CSS px); its own size means no size at all.
void Editor::finish_resize() {
    int l = resize_line_;
    resize_line_ = -1;
    if (l < 0 || l >= (int)lay_[0].size() || !lay_[0][l].img || lay_[0][l].tiles.size() != 1) return;
    if (std::fabs(resize_w_ - resize_from_) < 1.0f) return;  // a click, not a resize
    const Line::Tile& t = lay_[0][l].tiles[0];
    int w = resize_snap_ == "own size" ? 0 : (int)std::lround(resize_w_ / P_.px);
    std::string token = lines_[l].substr(t.b0, t.b1 - t.b0);
    std::string sized = md_image_resized(token, w);
    if (sized == token) return;
    begin_edit(Edit::Other);
    lines_[l].replace(t.b0, t.b1 - t.b0, sized);
    cur_ = anchor_ = {l, (int)lines_[l].size()};
    end_edit(false);
}

// alt+up/down with an image picked moves the image, not its line, one step
// through the places a drag can put it: into the row above (below) in a
// line that wraps into rows, by where it sits across; out of its row onto
// a line of its own; from a line of its own into the row of images above
// (below), at its end (start). False when it's alone on its line next to
// text: the line moves as any line does.
bool Editor::move_picked(int line, int tile, int dir) {
    const Line& L = lay_[0][line];
    const Line::Tile& t = L.tiles[tile];
    float cx = t.x + t.w * 0.5f;
    // Where it would go among a row's images (first..last): before the first
    // one whose middle is past its own.
    auto stop_in = [&](const Line& R, float row_y, int& first, int& last) {
        first = -1;
        int count = 0;
        for (int j = 0; j < (int)R.tiles.size(); j++) {
            if (R.tiles[j].y != row_y) continue;
            if (first < 0) first = j;
            last = j;
            if (R.tiles[j].x + R.tiles[j].w * 0.5f < cx) count++;
        }
        return first + count;
    };
    int to_line = -1, to_stop = -1, own = -1;
    float row_y = -1;  // the next row of this line, that way
    for (auto& o : L.tiles)
        if (dir < 0 ? o.y < t.y && (row_y < 0 || o.y > row_y) : o.y > t.y && (row_y < 0 || o.y < row_y)) row_y = o.y;
    if (row_y >= 0) {
        // The same number of images, so the same rows: it takes a place in
        // that row (the last of it, at most, or it would wrap back out).
        int first, last;
        int f = std::min(stop_in(L, row_y, first, last), last);
        to_line = line;
        to_stop = f >= tile ? f + 1 : f;  // (a stop counts it where it is)
    } else if (L.tiles.size() > 1) {
        own = dir < 0 ? line : line + 1;
    } else {
        // Joining the row above (below) keeps reading order: after its
        // images (before them). A line of its own sits at the column's left,
        // so where it sits across says nothing about where it should go.
        int m = line + dir;
        if (m < 0 || m >= (int)lines_.size() || !is_img(m)) return false;
        to_line = m;
        to_stop = dir < 0 ? (int)lay_[0][m].tiles.size() : 0;
    }
    // Dropped there, as a drag would, flying from where it is.
    ImVec2 a, b;
    if (tile_rect(line, tile, a, b)) {
        if (const TileMorph* mo = tile_morph(line, tile)) {
            a = ImVec2(a.x + mo->da.x, a.y + mo->da.y);
            b = ImVec2(b.x + mo->db.x, b.y + mo->db.y);
        }
        land_a_ = a;
        land_b_ = b;
    }
    land_drag_ = false;
    press_img_line_ = line;
    press_img_tile_ = tile;
    drop_line_ = to_line;
    drop_stop_ = to_stop;
    drop_own_ = own;
    uint64_t before = version_;
    move_image();
    press_img_line_ = press_img_tile_ = drop_line_ = drop_stop_ = drop_own_ = -1;
    if (version_ != before && land_line_ >= 0) pick(land_line_, land_tile_);
    else land_line_ = -1;
    return true;
}

// Swap the image the caret is on with the one beside it in its row. It
// glides to its new place (the landing flight); the caret stays on it.
bool Editor::nudge_image(int dir) {
    int tile = 0;
    int line = image_for_caret(&tile);
    if (line < 0) return false;
    std::vector<std::string> tokens = image_tokens(lines_[line]);
    int other = tile + dir;
    if (other < 0 || other >= (int)tokens.size()) return false;  // at the end of the row
    ImVec2 a, b;
    bool fly = tile_rect(line, tile, a, b);
    std::swap(tokens[tile], tokens[other]);
    begin_edit(Edit::Other);
    std::string s;
    int col = 0;
    for (int k = 0; k < (int)tokens.size(); k++) {
        s += (k ? " " : "") + tokens[k];
        if (k == other) col = (int)s.size();  // just after it: the caret is on it
    }
    lines_[line] = s;
    cur_ = anchor_ = {line, col};
    end_edit();
    if (fly) {
        land_line_ = line;
        land_tile_ = other;
        land_drag_ = false;
        land_a_ = a;
        land_b_ = b;
    }
    return true;
}

bool Editor::tile_rect(int line, int tile, ImVec2& a, ImVec2& b) const {
    if (line < 0 || line >= (int)lay_[0].size() || !lay_[0][line].img) return false;
    const Line& L = lay_[0][line];
    if (tile < 0 || tile >= (int)L.tiles.size()) return false;
    const Line::Tile& t = L.tiles[tile];
    a = ImVec2(t.x, L.dy + L.pad + t.y + (line < (int)yoff_.size() ? yoff_[line] : 0.0f));
    b = ImVec2(a.x + t.w, a.y + t.h);
    return true;
}

// Would dropping at the current target change anything? (Beside itself,
// or back on the line it's alone on, it wouldn't.)
bool Editor::drop_changes() const {
    int from = press_img_line_, tile = press_img_tile_;
    std::vector<ImageRef> refs;  // (the text's, not the preview's)
    if (from < 0 || from >= (int)lines_.size() || !md_image_row(lines_[from], &refs)) return false;
    if (drop_line_ >= 0) return !(drop_line_ == from && (drop_stop_ == tile || drop_stop_ == tile + 1));
    if (drop_own_ < 0) return false;
    return !(refs.size() == 1 && (drop_own_ == from || drop_own_ == from + 1));
}

// While carried with somewhere new to go: how line `line` would read once
// it's dropped, and where it sits in it (`slot`, -1 if not there). False
// for lines it doesn't change.
bool Editor::drag_preview(int line, std::string& out, int* slot) const {
    if (!img_drag_ || !drop_changes()) return false;
    int from = press_img_line_, tile = press_img_tile_;
    if (line != from && line != drop_line_) return false;
    std::vector<std::string> rest = image_tokens(lines_[from]);  // its line without it
    if (tile < 0 || tile >= (int)rest.size()) return false;
    rest.erase(rest.begin() + tile);
    if (line == drop_line_) {
        std::vector<std::string> t = line == from ? rest : image_tokens(lines_[line]);
        int k = line == from ? (drop_stop_ > tile ? drop_stop_ - 1 : drop_stop_) : drop_stop_;
        k = std::clamp(k, 0, (int)t.size());
        t.insert(t.begin() + k, drag_token_);
        out = join_images(t);
        if (slot) *slot = k;
        return true;
    }
    if (rest.empty()) return false;  // alone on its line: the line stays, empty (drag_hides_source)
    out = join_images(rest);
    if (slot) *slot = -1;
    return true;
}

// Taken from a line of its own to somewhere new: its place stays until the
// drop (so nothing jumps under the pointer), with nothing in it.
bool Editor::drag_hides_source() const {
    if (!img_drag_ || !drop_changes() || drop_line_ == press_img_line_) return false;
    std::vector<ImageRef> refs;
    return md_image_row(lines_[press_img_line_], &refs) && refs.size() == 1;
}

// The carried image's place as shown: where it would land in a row, or
// where it came from when there's nowhere new (-1 when it's going to a line
// of its own, or its old place is hidden).
void Editor::drag_slot(int& line, int& tile) const {
    line = tile = -1;
    if (!img_drag_) return;
    std::string s;
    int k = -1;
    if (drop_changes()) {
        if (drop_line_ >= 0 && drag_preview(drop_line_, s, &k)) { line = drop_line_; tile = k; }
        return;
    }
    line = press_img_line_;
    tile = press_img_tile_;
}

// What the preview is showing, so a change can be morphed to.
int Editor::drag_signature() const {
    if (!img_drag_ || !drop_changes()) return 0;
    return 1 + (drop_line_ + 1) * 7919 + (drop_stop_ + 1) * 31 + (drop_own_ + 1) * 1000003;
}

// End a drag. Dropped somewhere new, it moves there; otherwise (or on
// Escape) it goes back. Either way it flies from where it was let go.
void Editor::let_go_image(bool drop) {
    // Where the carried copy is now: that's where the flight starts.
    const Line::Tile& t = drag_tile_;
    ImVec2 a(ghost_at_.x - grab_off_.x * ghost_k_, ghost_at_.y - grab_off_.y * ghost_k_);
    land_a_ = a;
    land_b_ = ImVec2(a.x + t.w * ghost_k_, a.y + t.h * ghost_k_);
    land_line_ = press_img_line_;
    land_tile_ = press_img_tile_;
    land_drag_ = true;
    if (drop && drop_changes()) move_image();  // sets land_* to where it went
}

// Drop a dragged image: into a row beside another image, or onto a line
// of its own. Rows are rebuilt from their images, one space apart.
void Editor::move_image() {
    int from = press_img_line_, tile = press_img_tile_;
    auto tokens = [&](int l) {
        return l >= 0 && l < (int)lines_.size() ? image_tokens(lines_[l]) : std::vector<std::string>{};
    };
    std::vector<std::string> src = tokens(from);
    if (tile < 0 || tile >= (int)src.size()) return;
    std::string moving = src[tile];

    if (drop_line_ >= 0) {
        if (drop_line_ == from && (drop_stop_ == tile || drop_stop_ == tile + 1)) return;  // where it already is
        begin_edit(Edit::Other);
        if (drop_line_ == from) {
            src.erase(src.begin() + tile);
            int k = drop_stop_ > tile ? drop_stop_ - 1 : drop_stop_;
            src.insert(src.begin() + k, moving);
            lines_[from] = join_images(src);
            int col = 0;
            for (int j = 0; j <= k; j++) col += (int)src[j].size() + (j ? 1 : 0);
            cur_ = anchor_ = {from, col};
            land_line_ = from;
            land_tile_ = k;
        } else {
            std::vector<std::string> dst = tokens(drop_line_);
            dst.insert(dst.begin() + std::clamp(drop_stop_, 0, (int)dst.size()), moving);
            lines_[drop_line_] = join_images(dst);
            int to = drop_line_;
            src.erase(src.begin() + tile);
            if (src.empty()) {
                lines_.erase(lines_.begin() + from);
                if (to > from) to--;
            } else {
                lines_[from] = join_images(src);
            }
            int at = std::clamp(drop_stop_, 0, (int)dst.size() - 1);
            int col = 0;
            for (int j = 0; j <= at; j++) col += (int)dst[j].size() + (j ? 1 : 0);
            cur_ = anchor_ = {to, col};
            land_line_ = to;
            land_tile_ = at;
        }
        end_edit();
        return;
    }
    if (drop_own_ < 0) return;
    int before = drop_own_;
    // Already alone on its line, dropped right where it is: nothing to do.
    if (src.size() == 1 && (before == from || before == from + 1)) return;
    begin_edit(Edit::Other);
    src.erase(src.begin() + tile);
    if (src.empty()) {
        lines_.erase(lines_.begin() + from);
        if (before > from) before--;
    } else {
        lines_[from] = join_images(src);
    }
    before = std::clamp(before, 0, (int)lines_.size());
    lines_.insert(lines_.begin() + before, moving);
    cur_ = anchor_ = {before, (int)moving.size()};
    land_line_ = before;
    land_tile_ = 0;
    end_edit();
}

// Moving images, drawn over the page. While carried: a faint copy in the
// room opening for a line of its own (in a row, the slot is drawn with the
// row), and the image itself, lifted, under the pointer. After a drop or a
// cancel: the image flying to its place.
void Editor::draw_image_drag(ImDrawList* dl) {
    float dt = anim::step(ImGui::GetIO().DeltaTime);
    auto ease = [&](float rate) { return anim::enabled ? 1.0f - std::exp(-rate * anim::speed * dt) : 1.0f; };
    auto toward = [](ImVec2& v, ImVec2 to, float k) { v.x += (to.x - v.x) * k; v.y += (to.y - v.y) * k; };
    float round = std::round(3 * P_.dpi);
    ImVec2 o(col_x_, origin_y_);  // doc -> screen
    auto screen = [&](ImVec2 p) { return ImVec2(std::round(p.x + o.x), std::round(p.y + o.y)); };
    auto picture = [&](const Line::Tile& t, ImVec2 a, ImVec2 b, float alpha) {
        float shown = 0;
        ImTextureID tex = P_.images ? P_.images->texture(t.src, (int)t.w, shown) : ImTextureID{};
        if (tex) dl->AddImageRounded(ImTextureRef(tex), a, b, ImVec2(0, 0), ImVec2(1, 1), fade(col_alpha(IM_COL32_WHITE, alpha)), round);
        else dl->AddRectFilled(a, b, fade(col_alpha(T.code_bg, alpha)), round);
    };
    // A soft shadow: a few widening, fainter rounds of the background's dark.
    auto shadow = [&](ImVec2 a, ImVec2 b, float alpha) {
        ImU32 dark = T.dark ? IM_COL32(0, 0, 0, 255) : IM_COL32(40, 40, 40, 255);
        for (int i = 3; i >= 1; i--) {
            float g = i * 3 * P_.dpi;
            dl->AddRectFilled(ImVec2(a.x - g, a.y - g + 2 * i * P_.dpi), ImVec2(b.x + g, b.y + g + 2 * i * P_.dpi),
                              fade(col_alpha(dark, alpha * 0.10f)), round + g);
        }
    };

    // Landing.
    if (land_line_ >= 0 && !img_drag_) {
        ImVec2 ta, tb;
        if (!tile_rect(land_line_, land_tile_, ta, tb)) {
            land_line_ = -1;
        } else {
            float k = ease(24.0f);
            toward(land_a_, ta, k);
            toward(land_b_, tb, k);
            bool there = std::fabs(land_a_.x - ta.x) + std::fabs(land_a_.y - ta.y) + std::fabs(land_b_.x - tb.x) + std::fabs(land_b_.y - tb.y) < 1.0f;
            const Line::Tile& t = lay_[0][land_line_].tiles[land_tile_];
            ImVec2 a = screen(land_a_), b = screen(land_b_);
            float lift = std::clamp((std::fabs(land_a_.x - ta.x) + std::fabs(land_a_.y - ta.y)) / (20 * P_.dpi), 0.0f, 1.0f);
            if (lift > 0) shadow(a, b, lift);
            picture(t, a, b, 1.0f);
            if (there) land_line_ = -1;
            else anim::active = true;
        }
    }

    if (!img_drag_ || press_img_line_ < 0 || press_img_line_ >= (int)lay_[0].size()) return;
    const Line::Tile& t = drag_tile_;

    // A line of its own opening between lines: a faint copy in the room
    // made for it, as the slot is in a row.
    for (auto& s : spacers_) {
        if (s.target <= 0.0f || s.h < 1.0f) continue;
        const Line::Tile& own = drag_own_tile_;
        float open = std::clamp(s.h / std::max(1.0f, drag_own_h_), 0.0f, 1.0f);
        ImVec2 a(own.x, s.dy + own.y + (s.h - drag_own_h_) * 0.5f);
        picture(own, screen(a), screen(ImVec2(a.x + own.w, a.y + own.h)), 0.22f * open);
    }

    // The image, carried: held where it was grabbed, easing after the
    // pointer, shrinking from full size to a size you can see past.
    float carry = std::min(1.0f, std::round(150 * P_.dpi) / std::max(t.w, t.h));
    ghost_k_ += (carry - ghost_k_) * ease(18.0f);
    toward(ghost_at_, drag_at_, ease(40.0f));
    ImVec2 a(ghost_at_.x - grab_off_.x * ghost_k_, ghost_at_.y - grab_off_.y * ghost_k_);
    ImVec2 b(a.x + t.w * ghost_k_, a.y + t.h * ghost_k_);
    ImVec2 sa = screen(a), sb = screen(b);
    shadow(sa, sb, 1.0f);
    picture(t, sa, sb, 0.92f);
    float th = std::max(1.0f, std::round(P_.dpi));
    dl->AddRect(ImVec2(sa.x - th, sa.y - th), ImVec2(sb.x + th, sb.y + th), fade(col_alpha(T.accent, 0.7f)), round + th, th);
    anim::active = true;
}

// An image line's pictures, its row's top at y. While one loads, a quiet
// placeholder holds its exact space; the picture fades in over it. `index`
// is its line in the editor (-1 in the reader, the morph or for a ghost):
// the image the caret is on is outlined, images inside a selection take its
// tint, and a lone image has a handle on its right edge to resize it by.
void Editor::draw_image(ImDrawList* dl, const Line& L, float x, float y, float alpha, int index) {
    if (alpha <= 0.0f) return;
    float round = std::round(3 * P_.dpi);
    bool editing = index >= 0 && !reader_;
    TextPos s0 = std::min(cur_, anchor_), s1 = std::max(cur_, anchor_);
    int on = -1;
    if (editing && picked(&on) != index) on = -1;
    int slot_line = -1, slot_tile = -1;
    if (editing) drag_slot(slot_line, slot_tile);
    bool hidden = editing && index == press_img_line_ && drag_hides_source();
    for (int k = 0; k < (int)L.tiles.size(); k++) {
        const Line::Tile& t = L.tiles[k];
        ImVec2 a(x + t.x, y + t.y), b(a.x + t.w, a.y + t.h);
        const TileMorph* m = editing ? tile_morph(index, k) : nullptr;
        if (m) {  // on its way from where an edit moved it
            a = ImVec2(a.x + m->da.x, a.y + m->da.y);
            b = ImVec2(b.x + m->db.x, b.y + m->db.y);
        }
        a = ImVec2(std::round(a.x), std::round(a.y));
        b = ImVec2(std::round(b.x), std::round(b.y));
        if (b.y < P_.min.y || a.y > P_.max.y) continue;
        if (editing && land_line_ == index && land_tile_ == k) continue;  // in flight (draw_image_drag)
        // Carried: a faint copy keeps its place, where it came from or where
        // it would land. From a line of its own, that line waits empty.
        if (hidden) continue;
        bool trace = index == slot_line && k == slot_tile;
        float tile_alpha = (trace ? alpha * 0.22f : alpha) * (m ? m->alpha : 1.0f);
        float shown = 0.0f;
        ImTextureID tex = P_.images ? P_.images->texture(t.src, (int)std::ceil(t.w), shown) : ImTextureID{};
        if (!tex) shown = 0.0f;
        if (shown < 1.0f) {
            dl->AddRectFilled(a, b, col_alpha(T.code_bg, tile_alpha * (1.0f - shown)), round);
            anim::active = true;  // waiting for it, or fading it in
            std::string label = P_.images && !tex ? P_.images->label(t.src) : std::string();
            if (!label.empty()) {
                float ls = std::round(P_.size * P_.small_size);
                ImVec2 sz = P_.mono->CalcTextSizeA(ls, FLT_MAX, 0, label.c_str());
                if (sz.x < t.w && ls < t.h)
                    dl->AddText(P_.mono, ls, ImVec2(std::round((a.x + b.x - sz.x) * 0.5f), std::round((a.y + b.y - ls) * 0.5f)),
                                col_alpha(T.dim, tile_alpha), label.c_str());
            }
        }
        if (tex) dl->AddImageRounded(ImTextureRef(tex), a, b, ImVec2(0, 0), ImVec2(1, 1), col_alpha(IM_COL32_WHITE, tile_alpha * shown), round);
        if (!editing || trace) continue;
        // Under the pointer (and free to pick up): a faint lift.
        if (index == hover_img_line_ && k == hover_tile_ && !img_drag_ && resize_line_ < 0) {
            hover_glow_ = anim::approach(hover_glow_, 1.0f, 20.0f, ImGui::GetIO().DeltaTime, 0.01f);
            dl->AddRectFilled(a, b, col_alpha(T.bright, alpha * 0.07f * hover_glow_), round);
        }
        bool selected = has_selection() && !(TextPos{index, t.b0} < s0) && !(s1 < TextPos{index, t.b1});
        if (k == on) {  // picked: a ring just clear of it (steps back while an image is carried)
            float th = std::max(1.0f, std::round(2 * P_.dpi)), off = th + std::max(1.0f, std::round(P_.dpi));
            dl->AddRect(ImVec2(a.x - off, a.y - off), ImVec2(b.x + off, b.y + off),
                        col_alpha(T.accent, alpha * (P_.keyboard ? 1.0f : 0.4f) * caret_shown_), round + off, th);
        } else if (selected) {
            dl->AddRectFilled(a, b, col_alpha(T.selection, alpha), round);
        }
    }

    // The resize handle: a lone image, with the caret on it, the pointer
    // over it, or being resized.
    ImVec2 ha, hb;
    if (!editing || !(resize_line_ == index || hover_img_line_ == index || on >= 0) || !resize_handle(index, ha, hb)) return;
    ha = ImVec2(std::round(ha.x + col_x_), std::round(ha.y + origin_y_));  // doc -> screen
    hb = ImVec2(std::round(hb.x + col_x_), std::round(hb.y + origin_y_));
    bool hot = resize_line_ == index || (hover_handle_ && hover_img_line_ == index);
    float pad = std::max(1.0f, std::round(P_.dpi));
    // While resizing, where it snaps: faint ticks at a quarter, half, three
    // quarters and all of the column, and at its own size; the one it's on
    // lit.
    if (resize_line_ == index) {
        const Line::Tile& t = L.tiles[0];
        float left = std::round(x + t.x), top = std::round(y + t.y), bottom = std::round(y + t.y + t.h);
        float own = std::min((float)t.iw, P_.column);
        struct Guide { float w; const char* name; };
        Guide guides[] = {{P_.column * 0.25f, "1/4"}, {P_.column * 0.5f, "1/2"}, {P_.column * 0.75f, "3/4"}, {P_.column, "full width"}, {own, "own size"}};
        float lw = std::max(1.0f, std::round(P_.dpi)), ext = std::round(6 * P_.dpi);
        for (auto& g : guides) {
            bool lit = resize_snap_ == g.name;
            float gx = std::round(left + g.w);
            dl->AddRectFilled(ImVec2(gx, top - ext), ImVec2(gx + lw, bottom + ext),
                              col_alpha(lit ? T.accent : T.faint, alpha * (lit ? 0.9f : 0.7f)));
        }
    }
    dl->AddRectFilled(ImVec2(ha.x - pad, ha.y - pad), ImVec2(hb.x + pad, hb.y + pad), col_alpha(T.bg, alpha * 0.9f), (hb.x - ha.x) * 0.5f + pad);
    dl->AddRectFilled(ha, hb, col_alpha(hot ? T.accent : T.bright, alpha * (hot ? 1.0f : 0.8f)), (hb.x - ha.x) * 0.5f);
    // While resizing: the width, and what it snapped to.
    if (resize_line_ == index) {
        const Line::Tile& t = L.tiles[0];
        std::string label = !resize_snap_.empty() ? resize_snap_ : std::to_string((int)std::lround(t.w / P_.px)) + "px";
        float ls = std::round(P_.size * P_.small_size);
        ImVec2 sz = P_.mono->CalcTextSizeA(ls, FLT_MAX, 0, label.c_str());
        float lx = std::round(hb.x + 8 * P_.dpi), ly = std::round((ha.y + hb.y - ls) * 0.5f);
        if (lx + sz.x > P_.max.x - 4 * P_.dpi) lx = std::round(ha.x - 8 * P_.dpi - sz.x);
        dl->AddText(P_.mono, ls, ImVec2(lx, ly), col_alpha(T.text, alpha), label.c_str());
    }
}

// Where a lone image's resize handle is (doc coords of the edit view): a
// pill straddling its right edge, halfway down.
bool Editor::resize_handle(int line, ImVec2& a, ImVec2& b) const {
    if (line < 0 || line >= (int)lay_[0].size() || !lay_[0][line].img || lay_[0][line].tiles.size() != 1) return false;
    const Line& L = lay_[0][line];
    const Line::Tile& t = L.tiles[0];
    float w = std::max(3.0f, std::round(4 * P_.dpi)), h = std::min(std::round(36 * P_.dpi), std::round(t.h * 0.4f));
    if (h < 8 * P_.dpi) return false;
    float cx = t.x + t.w, cy = L.dy + L.pad + t.y + t.h * 0.5f + (line < (int)yoff_.size() ? yoff_[line] : 0.0f);
    a = ImVec2(cx - w * 0.5f, cy - h * 0.5f);
    b = ImVec2(cx + w * 0.5f, cy + h * 0.5f);
    return true;
}

