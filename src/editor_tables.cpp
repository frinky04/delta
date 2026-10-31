// Tables in the editor: which lines belong to one, and the reader's grid:
// columns sized from every row, each cell wrapped in its own column, the
// header ruled off, and what's under the pointer (for links in cells). The
// edit view shows tables as the markdown they are, their |s dimmed.
#include <algorithm>
#include <bit>
#include <cmath>

#include "editor.h"
#include "theme.h"
#include "util.h"

const std::vector<TableRole>& Editor::table_roles() const {
    if (roles_version_ != version_ || roles_.size() != lines_.size()) {
        if (syntax_ == Syntax::Markdown) roles_ = md_table_roles(lines_);
        else roles_.assign(lines_.size(), TableRole::None);
        roles_version_ = version_;
    }
    return roles_;
}

void Editor::layout_tables() {
    auto& V = lay_[1];
    const int n = (int)V.size();
    for (int i = 0; i < n; i++) {
        if (V[i].info.table != TableRole::Header) continue;
        int end = i + 1;
        while (end < n && (V[end].info.table == TableRole::Delim || V[end].info.table == TableRole::Body)) end++;

        // Sized already, for these rows at this width?
        uint64_t key = 1469598103934665603ull;
        auto mix = [&](uint64_t v) { key = (key ^ v) * 1099511628211ull; };
        mix((uint64_t)(end - i));
        for (int k = i; k < end; k++) mix(V[k].hash);
        mix(std::bit_cast<uint32_t>(P_.column));
        mix(std::bit_cast<uint32_t>(P_.size));
        if (V[i].table_key == key) { i = end - 1; continue; }
        V[i].table_key = key;

        std::vector<Align> align;
        md_table_delim(lines_[i + 1], &align);
        const int nc = (int)align.size();
        Metrics m = metrics(V[i].size);
        const float gap = std::round(P_.size * 1.5f);
        const float room = std::max(P_.size, P_.column - gap * (nc - 1));

        // What each column would like (its widest cell on one row) and the
        // least it can do with (its longest word, but a long url mustn't
        // claim the table). A row with too few cells gets empty ones; cells
        // past the header's aren't shown.
        std::vector<float> want(nc, 0), least(nc, 0);
        for (int k = i; k < end; k++) {
            Line& L = V[k];
            if (L.info.table == TableRole::Delim) continue;
            int len = (int)L.rtext.size();
            L.cells.resize(nc, Line::Cell{len, len});
            for (int c = 0; c < nc; c++) {
                Line::Cell& C = L.cells[c];
                C.align = align[c];
                want[c] = std::max(want[c], std::ceil(measure(L.rtext, L.rstyle, C.b0, C.b1, m)));
                for (int a = C.b0; a < C.b1;) {
                    int b = a;
                    while (b < C.b1 && L.rtext[b] != ' ') b++;
                    least[c] = std::max(least[c], std::ceil(measure(L.rtext, L.rstyle, a, b, m)));
                    a = b + 1;
                }
            }
        }
        for (int c = 0; c < nc; c++) {
            want[c] = std::max(want[c], P_.size);
            least[c] = std::min(least[c], std::min(want[c], std::round(room * 0.4f)));
        }
        // A column that wants no more than an even share of the room gets
        // it all, so short cells never wrap (repeated: each one settled
        // leaves more for the rest). The others take their least and share
        // what's left by how much more they wanted.
        std::vector<float> w(nc, -1);
        float left = room;
        for (bool settled = true; settled;) {
            settled = false;
            int open = 0;
            for (int c = 0; c < nc; c++) open += w[c] < 0;
            for (int c = 0; c < nc && open; c++)
                if (w[c] < 0 && want[c] <= left / open) {
                    w[c] = want[c];
                    left -= want[c];
                    settled = true;
                    break;
                }
        }
        float sum_want = 0, sum_least = 0;
        for (int c = 0; c < nc; c++)
            if (w[c] < 0) { sum_want += want[c]; sum_least += least[c]; }
        for (int c = 0; c < nc; c++) {
            if (w[c] >= 0) continue;
            if (sum_least >= left) w[c] = std::floor(least[c] * std::max(0.0f, left) / sum_least);
            else w[c] = std::floor(least[c] + (want[c] - least[c]) * (left - sum_least) / (sum_want - sum_least));
        }
        std::vector<float> x(nc, 0);
        for (int c = 1; c < nc; c++) x[c] = x[c - 1] + w[c - 1] + gap;
        float table_w = nc ? x[nc - 1] + w[nc - 1] : 0;

        for (int k = i; k < end; k++) {
            Line& L = V[k];
            L.table_w = table_w;
            L.table_sep = L.info.table == TableRole::Body && k > i + 2;
            if (L.info.table == TableRole::Delim) continue;
            size_t tallest = 1;
            for (int c = 0; c < nc; c++) {
                Line::Cell& C = L.cells[c];
                C.x = x[c];
                C.w = w[c];
                C.rows.clear();
                wrap_rows(L.rtext, L.rstyle, C.b0, C.b1, C.w + 0.5f, 0, m, C.rows);
                if (C.rows.size() > tallest) { tallest = C.rows.size(); L.rows = C.rows; }
            }
            if (tallest == 1) L.rows.assign(1, 0);
        }
        i = end - 1;
    }
}

namespace {

// Where a cell's row starts, after its alignment: the slack goes before it
// (right) or half before it (center).
float row_offset(float row_w, float cell_w, Align align) {
    float slack = std::max(0.0f, cell_w - row_w);
    if (align == Align::Right) return slack;
    if (align == Align::Center) return std::round(slack * 0.5f);
    return 0;
}

}  // namespace

int Editor::table_col_at(const Line& L, float x, float y) const {
    const std::string& s = L.rtext;
    const int none = (int)s.size();
    const Line::Cell* C = nullptr;
    for (auto& c : L.cells)
        if (x >= c.x) C = &c;
    if (!C || x >= C->x + C->w) return none;
    int r = (int)std::floor(y / L.row_h);
    if (r < 0 || r >= (int)C->rows.size()) return none;
    int a = C->rows[r], b = r + 1 < (int)C->rows.size() ? C->rows[r + 1] : C->b1;
    int e = b;
    while (e > a && s[e - 1] == ' ') e--;
    Metrics m = metrics(L.size);
    float cx = C->x + row_offset(measure(s, L.rstyle, a, e, m), C->w, C->align);
    const char* p = s.data();
    for (int i = a; i < b;) {
        int len;
        unsigned cp = utf8::decode(p + i, p + s.size(), &len);
        float w = advance(m, cp, L.rstyle[i]);
        if (x >= cx && x < cx + w) return i;
        cx += w;
        i += len;
    }
    return none;
}

void Editor::draw_table_row(ImDrawList* dl, const Line& L, const std::string& s, const std::vector<Style>& st,
                            float x0, float y) {
    float lw = std::max(1.0f, std::round(P_.dpi));
    // The header is ruled off; body rows are parted by a quieter line.
    if (L.info.table == TableRole::Delim) {
        dl->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + L.table_w, y + lw), fade(T.border));
        return;
    }
    if (L.table_sep) {
        float sy = std::round(y - L.pad);
        dl->AddRectFilled(ImVec2(x0, sy), ImVec2(x0 + L.table_w, sy + lw), fade(T.faint));
    }
    Metrics m = metrics(L.size);
    for (auto& C : L.cells) {
        for (int r = 0; r < (int)C.rows.size(); r++) {
            int a = C.rows[r], b = r + 1 < (int)C.rows.size() ? C.rows[r + 1] : C.b1;
            if (a >= b) continue;
            int e = b;
            while (e > a && s[e - 1] == ' ') e--;  // a wrapped row's break space
            float x = x0 + C.x + row_offset(measure(s, st, a, e, m), C.w, C.align);
            draw_row(dl, 1, L, m, s, st, a, b, x, y + r * L.row_h);
        }
    }
}
