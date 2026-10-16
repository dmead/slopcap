// Collimate mode: analyses a defocused star live (analysis/collimation) and draws the fitted ring and shadow on the
// preview with an arrow to the ring's thin side. left() holds the method and the switch, right() the readouts.
#pragma once

#include "analysis/collimation.hpp"
#include "preview/latest_frame.hpp"

#include <QObject>
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
    explicit OffsetHistory(QWidget* parent = nullptr) : QWidget(parent) { setMinimumHeight(110); }
    void add(double pct);
    void clear() {
        v_.clear();
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    std::deque<double> v_;
};

class CollimationPanel : public QObject {
    Q_OBJECT
public:
    explicit CollimationPanel(preview::PreviewWidget* preview, QObject* parent = nullptr);
    QWidget* left() const { return left_; }
    QWidget* right() const { return right_; }
    void onFrame(const LatestFrame::Frame& frame, Bayer bayer);
    void setAnalysing(bool on);

private:
    void clearOverlay();

    preview::PreviewWidget* preview_;
    QWidget* left_ = nullptr;
    QWidget* right_ = nullptr;
    QCheckBox* enable_ = nullptr;
    QLabel* offset_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel *thin_ = nullptr, *ring_ = nullptr, *round_ = nullptr, *centre_ = nullptr;
    OffsetHistory* history_ = nullptr;
    std::deque<analysis::DonutResult> recent_;
};

}  // namespace slopcap::app
