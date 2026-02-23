//
// Created by Tobias Hieta on 07/03/16.
//

#include "EventFilter.h"
#include "settings/SettingsComponent.h"
#include "input/InputKeyboard.h"
#include <QQuickWindow>
#include <QQuickItem>

#include <QKeyEvent>
#include <QWheelEvent>
#include <QObject>

static QStringList desktopWhiteListedKeys = { "Media Play",
                                              "Media Pause",
                                              "Media Stop",
                                              "Media Next",
                                              "Media Previous",
                                              "Media Rewind",
                                              "Media FastForward",
                                              "Back"};

// These just happen to be mostly the same.
static QStringList win32BlackListedKeys = { "Media Play",
                                                      "Media Pause",
                                                      "Media Stop",
                                                      "Media Next",
                                                      "Media Previous",
                                                      "Media Rewind",
                                                      "Media FastForward"};

///////////////////////////////////////////////////////////////////////////////////////////////////
static QString keyEventToKeyString(QKeyEvent *kevent)
{
  // We ignore the KeypadModifier here since it's practically useless
  QKeySequence modifiers(kevent->modifiers() &= ~Qt::KeypadModifier);

  QKeySequence keySeq(kevent->key());
  QString key = keySeq.toString();

  // Qt tends to make up something weird for keys which don't cleanly map to text.
  // See e.g. QKeySequencePrivate::keyName() in the Qt sources.
  // We can't really know for sure which names are "good" or "bad", so we simply
  // allow printable latin1 characters, and mangle everything else.
  if ((key.size() > 0 && (key[0].unicode() < 32 || key[0].unicode() > 255)))
  {
    if (kevent->nativeVirtualKey() != 0)
    {
      key = "0x" + QString::number(kevent->nativeVirtualKey(), 16) + "V";
    }
    else
    {
      QString properKey;
      for (int n = 0; n < key.size(); n++)
        properKey += QString(n > 0 ? "+" : "") + "0x" + QString::number(key[n].unicode(), 16) + "Q";
      key = properKey;
    }
  }

  return modifiers.toString() + key;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool EventFilter::eventFilter(QObject* watched, QEvent* event)
{
  QQuickWindow* window = qobject_cast<QQuickWindow*>(parent());

  if (window && window->property("webDesktopMode").toBool())
  {
    // For desktop mode we don't want fullblown keyboard handling in
    // the host yet. We just want to handle some specific keyboard
    // events.
    //
    if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease || event->type() == QEvent::ShortcutOverride)
    {
      QKeyEvent* key = dynamic_cast<QKeyEvent*>(event);

      if (key)
      {
        InputBase::InputkeyState keystatus;

        if (event->type() == QEvent::KeyPress)
          keystatus = InputBase::KeyDown;
        else
          keystatus = InputBase::KeyUp;

        QString seq = keyEventToKeyString(key);
#ifdef Q_OS_WIN32
        if (win32BlackListedKeys.contains(seq))
        {
          return true;
        }
#endif

        if (desktopWhiteListedKeys.contains(seq))
        {
          InputKeyboard::Get().keyPress(seq, keystatus);
          return true;
        }
      }
    }

    if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease)
    {
      QMouseEvent* mouseEvent = dynamic_cast<QMouseEvent*>(event);

      if (mouseEvent && (mouseEvent->button() == Qt::BackButton || mouseEvent->button() == Qt::ForwardButton)) {
        // Only trigger navigation on release, but block both press and release
        // to prevent WebEngine from interpreting press-without-release as a long-press
        if (event->type() == QEvent::MouseButtonRelease)
        {
          QQuickItem* webView = window->findChild<QQuickItem*>("web");

          if (mouseEvent->button() == Qt::BackButton)
            QMetaObject::invokeMethod(webView, "goBack");
          else
            QMetaObject::invokeMethod(webView, "goForward");
        }
        return true;
      }
    }

    // Block zoom keyboard shortcuts when zoom is disabled (but allow Ctrl+0 reset)
    bool allowZoom = SettingsComponent::Get().value(SETTINGS_SECTION_MAIN, "allowBrowserZoom").toBool();
    if (!allowZoom && (event->type() == QEvent::KeyPress || event->type() == QEvent::ShortcutOverride))
    {
      QKeyEvent* key = dynamic_cast<QKeyEvent*>(event);
      if (key && (key->modifiers() & Qt::ControlModifier))
      {
        if (key->key() == Qt::Key_Plus || key->key() == Qt::Key_Equal ||
            key->key() == Qt::Key_Minus)
          return true;
      }
    }

    // Block Ctrl+scroll zoom when zoom is disabled
    if (!allowZoom && event->type() == QEvent::Wheel)
    {
      QWheelEvent* wheel = dynamic_cast<QWheelEvent*>(event);
      if (wheel && (wheel->modifiers() & Qt::ControlModifier))
        return true;
    }

    return QObject::eventFilter(watched, event);
  }

  // ignore mouse events if mouse is disabled
  if  (SettingsComponent::Get().value(SETTINGS_SECTION_MAIN, "disablemouse").toBool() &&
       ((event->type() == QEvent::MouseMove) ||
        (event->type() == QEvent::MouseButtonPress) ||
        (event->type() == QEvent::MouseButtonRelease) ||
        (event->type() == QEvent::MouseButtonDblClick)))
  {
    return true;
  }

  if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease)
  {
    // In konvergo we intercept all keyboard events and translate them
    // into web client actions. We need to do this so that we can remap
    // keyboard buttons to different events.
    //

    InputBase::InputkeyState keystatus;

    if (event->type() == QEvent::KeyPress)
      keystatus = InputBase::KeyDown;
    else
      keystatus = InputBase::KeyUp;

    QKeyEvent* kevent = dynamic_cast<QKeyEvent*>(event);
    if (!kevent)
      return QObject::eventFilter(watched, event);

    QString keyName = keyEventToKeyString(kevent);

#ifdef Q_OS_WIN32
    // On Windows, ignore media keys as they are handled elsewhere.
    if (win32BlackListedKeys.contains(keyName))
      return true;
#endif

    if (keystatus == InputBase::KeyDown)
    {
      // Swallow auto-repeated keys (isAutoRepeat doesn't always work - QTBUG-57335)
      // Do this only for non-modifier keys (QKeyEvent::text serves as a heuristic
      // to distinguish them from normal key presses)
      if (kevent->text().size())
      {
        if (m_currentKeyDown)
          return true;
        m_currentKeyDown = true;
      }
    }
    else
      m_currentKeyDown = false;

    if (kevent->spontaneous() && !kevent->isAutoRepeat())
    {
      InputKeyboard::Get().keyPress(keyName, keystatus);
      return true;
    }
  }
  else if (event->type() == QEvent::Wheel)
  {
    return true;
  }
  else if (event->type() == QEvent::MouseButtonPress)
  {
    // ignore right clicks that would show context menu
    QMouseEvent *mouseEvent = dynamic_cast<QMouseEvent*>(event);
    if ((mouseEvent) && (mouseEvent->button() == Qt::RightButton))
      return true;
  }
  else if (event->type() == QEvent::Drop)
  {
    // QtWebEngine would accept the drop and unload web-client.
    return true;
  }

  return QObject::eventFilter(watched, event);
}
