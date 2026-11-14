#pragma once

#include <QLabel>

// A one-line label that never widens the window: the text is cut to the width the layout gives it,
// by default with "..." at the START, so the end of a long path (the file name and the directories
// nearest to it) stays visible. A fixed prefix (e.g. "Current file: ") is kept whole. When the text is cut, the tooltip
// shows it in full.
class ElidedLabel : public QLabel
{
    Q_OBJECT

public:
    explicit ElidedLabel(QWidget *parent = nullptr);

    // Hide QLabel's setText()/clear() - called through an ElidedLabel pointer they keep the full text.
    void setText(const QString &text) { setText(QString(), text); }
    void setText(const QString &prefix, const QString &text);
    void clear() { setText(QString(), QString()); }
    QString fullText() const { return m_prefix + m_text; }
    // Where the text is cut: Qt::ElideLeft (default, for paths) or Qt::ElideRight.
    void setElideMode(Qt::TextElideMode mode);

    // As narrow as nothing: the width comes from the window, not from the text.
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void updateText();

    QString m_prefix;
    QString m_text;
    Qt::TextElideMode m_mode = Qt::ElideLeft;
};
