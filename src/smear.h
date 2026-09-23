#pragma once
// A rectangle that glides to its target, used for carets and selection bars.
//
// Each corner chases its own target. Without a trail they all move at the
// same rate: a plain glide. With a trail (the Neovide cursor), corners on the
// side the rect is heading towards move faster and the ones behind lag, so a
// long jump briefly stretches it into a smear between old and new spots,
// then it settles back into shape. Short hops look much like a glide.
#include <algorithm>
#include <cmath>

#include "imgui.h"
#include "util.h"

class SmearRect {
public:
    // Jump straight to a rect (first show, note switch, huge jumps).
    void snap(ImVec2 mn, ImVec2 mx) {
        corners(mn, mx, tgt_);
        for (int i = 0; i < 4; i++) cur_[i] = tgt_[i];
        valid_ = true;
    }

    // Aim at a new rect. `rate` is the glide speed (as in anim::approach).
    void set(ImVec2 mn, ImVec2 mx, bool trail, float rate) {
        if (!valid_) { snap(mn, mx); }
        ImVec2 t[4];
        corners(mn, mx, t);
        bool moved = false;
        for (int i = 0; i < 4; i++)
            if (std::fabs(t[i].x - tgt_[i].x) > 0.01f || std::fabs(t[i].y - tgt_[i].y) > 0.01f) moved = true;
        if (!moved) return;

        // Direction of travel, from where it is now to where it's going.
        ImVec2 from = center(cur_), to = center(t);
        float dx = to.x - from.x, dy = to.y - from.y, len = std::sqrt(dx * dx + dy * dy);
        static const float sx[4] = {-1, 1, 1, -1}, sy[4] = {-1, -1, 1, 1};  // TL TR BR BL
        for (int i = 0; i < 4; i++) {
            tgt_[i] = t[i];
            if (!trail || len < 1.0f) { rate_[i] = rate; continue; }
            // How much this corner faces the direction of travel: 0 = at the
            // back, 1 = at the front. The front runs ahead, the back lags.
            float lead = ((sx[i] * dx + sy[i] * dy) / (len * 1.41421356f) + 1.0f) * 0.5f;
            rate_[i] = rate * (0.3f + 1.3f * lead);
        }
    }

    void update(float dt) {
        for (int i = 0; i < 4; i++) {
            cur_[i].x = anim::approach(cur_[i].x, tgt_[i].x, rate_[i], dt, 0.2f);
            cur_[i].y = anim::approach(cur_[i].y, tgt_[i].y, rate_[i], dt, 0.2f);
        }
    }

    bool settled(float eps = 0.5f) const {
        for (int i = 0; i < 4; i++)
            if (std::fabs(cur_[i].x - tgt_[i].x) > eps || std::fabs(cur_[i].y - tgt_[i].y) > eps) return false;
        return true;
    }

    ImVec2 current_min() const { return cur_[0]; }
    ImVec2 corner(int i) const { return cur_[i]; }  // TL TR BR BL

    // Bounding box of where it is now, offset like draw().
    void bounds(ImVec2 off, ImVec2& mn, ImVec2& mx) const {
        mn = mx = cur_[0];
        for (int i = 1; i < 4; i++) {
            mn.x = std::min(mn.x, cur_[i].x);
            mn.y = std::min(mn.y, cur_[i].y);
            mx.x = std::max(mx.x, cur_[i].x);
            mx.y = std::max(mx.y, cur_[i].y);
        }
        mn = ImVec2(std::round(mn.x + off.x), std::round(mn.y + off.y));
        mx = ImVec2(std::round(mx.x + off.x), std::round(mx.y + off.y));
    }
    bool valid() const { return valid_; }
    void reset() { valid_ = false; }

    // Settled (or gliding in shape): a crisp, pixel-snapped rect with the
    // given rounding. Mid-smear: the stretched quad.
    void draw(ImDrawList* dl, ImVec2 off, ImU32 col, float rounding) const {
        if (!valid_) return;
        // In shape while the corners still line up as a rectangle.
        bool rect = std::fabs(cur_[0].y - cur_[1].y) < 0.5f && std::fabs(cur_[3].y - cur_[2].y) < 0.5f &&
                    std::fabs(cur_[0].x - cur_[3].x) < 0.5f && std::fabs(cur_[1].x - cur_[2].x) < 0.5f;
        if (rect) {
            ImVec2 a(std::round(cur_[0].x + off.x), std::round(cur_[0].y + off.y));
            ImVec2 b(std::round(cur_[2].x + off.x), std::round(cur_[2].y + off.y));
            if (b.x - a.x >= 1 && b.y - a.y >= 1) dl->AddRectFilled(a, b, col, rounding);
            return;
        }
        ImVec2 p[4];
        for (int i = 0; i < 4; i++) p[i] = ImVec2(cur_[i].x + off.x, cur_[i].y + off.y);
        dl->AddConvexPolyFilled(p, 4, col);
    }

private:
    static void corners(ImVec2 mn, ImVec2 mx, ImVec2* out) {
        out[0] = mn;
        out[1] = ImVec2(mx.x, mn.y);
        out[2] = mx;
        out[3] = ImVec2(mn.x, mx.y);
    }
    static ImVec2 center(const ImVec2* c) {
        return ImVec2((c[0].x + c[2].x) * 0.5f, (c[0].y + c[2].y) * 0.5f);
    }

    ImVec2 cur_[4] = {}, tgt_[4] = {};
    float rate_[4] = {50, 50, 50, 50};
    bool valid_ = false;
};
