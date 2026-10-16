// Log-scaled 256-bin histogram with the black and white points marked.
#pragma once

#include "preview/render_cpu.hpp"

#include <QWidget>

#include <array>

namespace slopcap::preview {

class HistogramWidget : public QWidget {
    Q_OBJECT
public:
    explicit HistogramWidget(QWidget* parent = nullptr);
    void setData(const std::array<uint32_t, 256>& histogram, const Levels& levels);
    QSize sizeHint() const override { return {256, 90}; }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    std::array<uint32_t, 256> hist_{};
    Levels levels_;
};

}  // namespace slopcap::preview
