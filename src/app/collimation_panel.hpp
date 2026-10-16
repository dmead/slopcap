// Collimate tab: analyses a defocused star live (analysis/collimation) and draws the fitted ring and shadow on the
// preview with an arrow to the ring's thin side, plus how far the star is from the frame centre.
#pragma once

#include "analysis/collimation.hpp"
#include "preview/latest_frame.hpp"

#include <QWidget>

#include <deque>

class QCheckBox;
class QLabel;

namespace slopcap::preview {
class PreviewWidget;
}

namespace slopcap::app {

/// Recent offset history (% of ring radius), newest on the right.
class OffsetHistory : public QWidget {
    Q_OBJECT
public:
    explicit OffsetHistory(QWidget* parent = nullptr) : QWidget(parent) { setMinimumHeight(70); }
    void add(double pct);
    void clear() { v_.clear(); update(); }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    std::deque<double> v_;
};

class CollimationPanel : public QWidget {
    Q_OBJECT
public:
    CollimationPanel(preview::PreviewWidget* preview, QWidget* parent = nullptr);
    void onFrame(const LatestFrame::Frame& frame, Bayer bayer);
    void setAnalysing(bool on);

private:
    void clearOverlay();

    preview::PreviewWidget* preview_;
    QCheckBox* enable_ = nullptr;
    QLabel* offset_ = nullptr;
    QLabel* detail_ = nullptr;
    OffsetHistory* history_ = nullptr;
    std::deque<analysis::DonutResult> recent_;
};

}  // namespace slopcap::app
