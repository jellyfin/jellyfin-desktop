#ifndef INPUTADAPTER_H
#define INPUTADAPTER_H

#include "ComponentManager.h"
#include "InputMapping.h"

#include <QThread>
#include <QVariantMap>
#include <QTimer>
#include <QElapsedTimer>

#include <functional>

class InputBase : public QObject
{
  Q_OBJECT
public:
  explicit InputBase(QObject* parent = nullptr) : QObject(parent) { qRegisterMetaType<InputBase::InputkeyState>("InputkeyState"); }
  virtual bool initInput() = 0;
  virtual const char* inputName() = 0;

  enum InputkeyState
  {
    KeyDown,
    KeyUp,
    KeyPressed
  };
  Q_ENUM(InputkeyState)

signals:
  void receivedInput(const QString& source, const QString& keycode, InputkeyState keystate);
};

///////////////////////////////////////////////////////////////////////////////////////////////////
// well known keys
///////////////////////////////////////////////////////////////////////////////////////////////////

#define INPUT_KEY_LEFT      "KEY_LEFT"
#define INPUT_KEY_RIGHT     "KEY_RIGHT"
#define INPUT_KEY_UP        "KEY_UP"
#define INPUT_KEY_DOWN      "KEY_DOWN"
#define INPUT_KEY_SELECT    "KEY_SELECT"
#define INPUT_KEY_MENU      "KEY_MENU"
#define INPUT_KEY_PLAY      "KEY_PLAY"
#define INPUT_KEY_PAUSE     "KEY_PAUSE"
#define INPUT_KEY_PLAY_PAUSE "KEY_PLAY_PAUSE"
#define INPUT_KEY_STOP      "KEY_STOP"
#define INPUT_KEY_BACK      "KEY_BACK"
#define INPUT_KEY_SEEKFWD   "KEY_SEEKFWD"
#define INPUT_KEY_SEEKBCK   "KEY_SEEKBCK"
#define INPUT_KEY_SUBTITLES "KEY_SUBTITLES"
#define INPUT_KEY_INFO      "KEY_INFO"
#define INPUT_KEY_NEXT      "KEY_NEXT"
#define INPUT_KEY_PREV      "KEY_PREV"
#define INPUT_KEY_RED       "KEY_RED"
#define INPUT_KEY_GREEN     "KEY_GREEN"
#define INPUT_KEY_BLUE      "KEY_BLUE"
#define INPUT_KEY_YELLOW    "KEY_YELLOW"
#define INPUT_KEY_HOME      "KEY_HOME"
#define INPUT_KEY_0         "KEY_NUMERIC_0"
#define INPUT_KEY_1         "KEY_NUMERIC_1"
#define INPUT_KEY_2         "KEY_NUMERIC_2"
#define INPUT_KEY_3         "KEY_NUMERIC_3"
#define INPUT_KEY_4         "KEY_NUMERIC_4"
#define INPUT_KEY_5         "KEY_NUMERIC_5"
#define INPUT_KEY_6         "KEY_NUMERIC_6"
#define INPUT_KEY_7         "KEY_NUMERIC_7"
#define INPUT_KEY_8         "KEY_NUMERIC_8"
#define INPUT_KEY_9         "KEY_NUMERIC_9"
#define INPUT_KEY_GUIDE     "KEY_GUIDE"

#define INPUT_KEY_LEFT_LONG    "KEY_LEFT_LONG"
#define INPUT_KEY_RIGHT_LONG   "KEY_RIGHT_LONG"
#define INPUT_KEY_UP_LONG      "KEY_UP_LONG"
#define INPUT_KEY_DOWN_LONG    "KEY_DOWN_LONG"
#define INPUT_KEY_SELECT_LONG  "KEY_SELECT_LONG"
#define INPUT_KEY_MENU_LONG    "KEY_MENU_LONG"
#define INPUT_KEY_PLAY_LONG    "KEY_PLAY_LONG"


struct ReceiverSlot
{
  std::function<void(void)> m_function;
  QObject* m_receiver;
  QByteArray m_slot;
  bool m_hasArguments;
};

class InputComponent : public ComponentBase
{
  Q_OBJECT
  DEFINE_SINGLETON(InputComponent);

public:
  const char* componentName() override { return "input"; }
  bool componentExport() override { return true; }
  bool componentInitialize() override;

  void registerHostCommand(const QString& command, QObject* receiver, const char* slot);
  void registerHostCommand(const QString& command, std::function<void(void)> function);

  // Called by web to actually execute pending actions. This is done in reaction
  // to hostInput(). The actions parameter contains a list of actions which
  // should be actually dispatched.
  Q_INVOKABLE void executeActions(const QStringList& actions);
  void cancelAutoRepeat();
  void sendAction(const QString action);

  // Volume, seek, and rate control
  Q_INVOKABLE void setVolume(int volume);
  Q_INVOKABLE void seekTo(qint64 position);
  Q_INVOKABLE void setRate(double rate);

signals:
  // Always emitted when any input arrives
  void receivedInput();

  // Emitted when new input arrives. Each entry is an action that matches
  // in the keymap, such as "host:fullscreen".
  void hostInput(const QStringList& actions);

  // Emitted when volume should be changed (0-100)
  void volumeChanged(int volume);

  // Emitted when position should be changed (milliseconds)
  void positionSeek(qint64 position);

  // Emitted when playback rate should be changed (1.0 = normal speed)
  void rateChanged(double rate);

private Q_SLOTS:
  void remapInput(const QString& source, const QString& keycode, InputBase::InputkeyState keyState);

private:
  explicit InputComponent(QObject *parent = nullptr);
  bool addInput(InputBase* base);
  void handleAction(const QString& action);

  QHash<QString, ReceiverSlot*> m_hostCommands;
  QList<InputBase*> m_inputs;
  InputMapping* m_mappings;

  QTimer* m_autoRepeatTimer;
  QStringList m_autoRepeatActions;

  QVariantMap m_currentLongPressAction;
  QElapsedTimer m_longHoldTimer;
};

#endif // INPUTADAPTER_H
