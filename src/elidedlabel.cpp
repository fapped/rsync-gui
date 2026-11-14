#include "elidedlabel.h"

#include <QEvent>
#include <QFontMetrics>

ElidedLabel::ElidedLabel(QWidget *parent)
    : QLabel(parent)
{
    setTextFormat(Qt::PlainText);
    setWordWrap(false);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
}

void ElidedLabel::setText(const QString &prefix, const QString &text)
{
    m_prefix = prefix;
    m_text = text;
    updateText();
}

void ElidedLabel::setElideMode(Qt::TextElideMode mode)
{
    m_mode = mode;
    updateText();
}

QSize ElidedLabel::sizeHint() const
{
    return {0, QLabel::sizeHint().height()};
}

QSize ElidedLabel::minimumSizeHint() const
{
    return {0, QLabel::minimumSizeHint().height()};
}

void ElidedLabel::resizeEvent(QResizeEvent *event)
{
    QLabel::resizeEvent(event);
    updateText();
}

void ElidedLabel::changeEvent(QEvent *event)
{
    QLabel::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange)
        updateText();
}

void ElidedLabel::updateText()
{
    const QString full = m_prefix + m_text;
    const QFontMetrics metrics = fontMetrics();
    const int available = contentsRect().width() - 2 * margin() - indent();
    QString shown = full;
    if (metrics.horizontalAdvance(full) > available) {
        const int prefixWidth = metrics.horizontalAdvance(m_prefix);
        if (prefixWidth < available)
            shown = m_prefix + metrics.elidedText(m_text, m_mode, available - prefixWidth);
        else // not even the prefix fits
            shown = metrics.elidedText(full, Qt::ElideRight, available);
    }
    QLabel::setText(shown);
    setToolTip(shown == full ? QString() : full);
}
