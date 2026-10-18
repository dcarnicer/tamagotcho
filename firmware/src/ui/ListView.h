#pragma once
// Scrolling list state for the menu screens (docs/ux-refresh.md §2.1: lists
// scroll instead of shrinking their rows). Holds the item count, the focused
// item and the first visible one, and turns a row geometry into what a list
// drawer needs: which rows fit, where each goes, whether rows are hidden above
// or below, and the scroll thumb. Pure C++, no Arduino (native-tested in
// test_ui); the drawing is render/Widgets (drawList).
//
// The geometry is set on every draw (it changes with the orientation) and the
// first visible row then moves as little as possible to keep the focus on
// screen, so moving the focus back and forth does not jump the window.
namespace tgc {

class ListView {
public:
    // Item count; the focus is clamped into it (0 when empty).
    void setCount(int n) {
        count_ = n > 0 ? n : 0;
        if (focus_ >= count_) focus_ = count_ > 0 ? count_ - 1 : 0;
        if (focus_ < 0) focus_ = 0;
        clampFirst();
    }

    // Rows of `rowH` px with `gap` px between them, in the band
    // [top, top + height). Re-clamps the window (e.g. after a rotation).
    void setGeometry(int top, int height, int rowH, int gap) {
        top_ = top;
        height_ = height > 0 ? height : 0;
        rowH_ = rowH > 0 ? rowH : 1;
        gap_ = gap > 0 ? gap : 0;
        clampFirst();
    }

    // Focus item `i` (clamped) and scroll just enough to show it.
    void setFocus(int i) {
        focus_ = i < 0 ? 0 : (i >= count_ ? (count_ > 0 ? count_ - 1 : 0) : i);
        clampFirst();
    }
    // A fresh window from the top with item `focus` focused (clamped). The
    // window is fitted to it by the next setGeometry(), which every draw does
    // first, so a list opened before its first draw starts at the top too.
    void reset(int focus = 0) {
        first_ = 0;
        focus_ = focus < 0 ? 0 : (focus >= count_ ? (count_ > 0 ? count_ - 1 : 0) : focus);
    }
    // The next / previous item, wrapping around at the ends.
    void next() { setFocus(count_ > 0 ? (focus_ + 1) % count_ : 0); }
    void prev() { setFocus(count_ > 0 ? (focus_ + count_ - 1) % count_ : 0); }

    int count() const { return count_; }
    int focus() const { return focus_; }
    int first() const { return first_; }
    int rowH() const { return rowH_; }
    int gap() const { return gap_; }
    int top() const { return top_; }

    // Rows that fit the band (at least 1, so a too-small band still shows the
    // focus).
    int fit() const {
        const int n = (height_ + gap_) / (rowH_ + gap_);
        return n > 0 ? n : 1;
    }
    // Rows on screen now.
    int visible() const {
        const int left = count_ - first_;
        const int f = fit();
        return left < f ? (left > 0 ? left : 0) : f;
    }
    bool overflows() const { return count_ > fit(); }
    bool moreAbove() const { return first_ > 0; }
    bool moreBelow() const { return first_ + visible() < count_; }
    bool isVisible(int i) const { return i >= first_ && i < first_ + visible(); }

    // Top of item `i`'s row (meaningful while it is visible).
    int rowY(int i) const { return top_ + (i - first_) * (rowH_ + gap_); }
    // One past the last pixel row of the visible rows (top() when none).
    int rowsBottom() const {
        const int v = visible();
        return v > 0 ? top_ + v * (rowH_ + gap_) - gap_ : top_;
    }

    // The scroll bar: the track spans the visible rows, the thumb's length is
    // the visible share (at least `minH`) and its position the scroll share.
    struct Thumb {
        int trackY, trackH, y, h;
    };
    Thumb thumb(int minH = 6) const {
        Thumb t{};
        t.trackY = top_;
        t.trackH = rowsBottom() - top_;
        if (count_ <= 0 || !overflows()) {
            t.y = t.trackY;
            t.h = t.trackH;
            return t;
        }
        const int v = visible();
        t.h = t.trackH * v / count_;
        if (t.h < minH) t.h = minH;
        if (t.h > t.trackH) t.h = t.trackH;
        const int range = count_ - v;  // > 0 while overflowing
        t.y = t.trackY + (t.trackH - t.h) * first_ / range;
        return t;
    }

private:
    void clampFirst() {
        const int f = fit();
        if (focus_ < first_) first_ = focus_;
        if (focus_ >= first_ + f) first_ = focus_ - f + 1;
        const int maxFirst = count_ > f ? count_ - f : 0;
        if (first_ > maxFirst) first_ = maxFirst;
        if (first_ < 0) first_ = 0;
    }

    int count_ = 0;
    int focus_ = 0;
    int first_ = 0;
    int top_ = 0;
    int height_ = 0;
    int rowH_ = 1;
    int gap_ = 0;
};

}  // namespace tgc
