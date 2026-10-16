// Capture sidebar, under the mount: the raw readout per colour channel, in the sensor's ADU. Log-scaled curves for
// R / G / B (one for mono) over 0..full scale, a red edge when anything clips, and each channel's median, max and
// clipped share, so exposure and gain can be set against the ADC's real range rather than the stretched preview.
#pragma once

#include "preview/render_cpu.hpp"

#include <QWidget>

class QLabel;

namespace slopcap::app {

class RawHistogramPlot : public QWidget {
    Q_OBJECT
public:
    explicit RawHistogramPlot(QWidget* parent = nullptr);
    void setData(const preview::RawHistogram& h);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    preview::RawHistogram h_;
};

class RawHistogramSection : public QWidget {
    Q_OBJECT
public:
    explicit RawHistogramSection(QWidget* parent = nullptr);
    void setData(const preview::RawHistogram& h);
    void clear() { setData({}); }

private:
    QLabel* units_ = nullptr;
    RawHistogramPlot* plot_ = nullptr;
};

}  // namespace slopcap::app
