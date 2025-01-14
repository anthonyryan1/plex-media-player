#include "QsLog.h"
#include "InputComponent.h"
#include "settings/SettingsComponent.h"
#include "system/SystemComponent.h"
#include "power/PowerComponent.h"
#include "InputKeyboard.h"
#include "InputSocket.h"
#include "InputRoku.h"

#ifdef Q_OS_MAC
#include "apple/InputAppleRemote.h"
#include "apple/InputAppleMediaKeys.h"
#endif

#ifdef HAVE_SDL
#include "InputSDL.h"
#endif

#ifdef HAVE_LIRC
#include "InputLIRC.h"
#endif

#ifdef HAVE_CEC
#include "InputCEC.h"
#endif

#define LONG_HOLD_MSEC 500
#define INITAL_AUTOREPEAT_MSEC 650

// Synthetic key repeat events are emitted at 60ms intervals. The interval is
// half of the fastest press-and-release time. Empirical key repeat intervals:
//   * Key hold on macOS wireless USB keyboard with fastest key repeat preference: ~35s
//   * Press and release on macOS wireless USB keyboard (like a meth monkey): ~120ms
//   * Key hold on macOS Apple TV IR remote + FLiRC (3.8 firmware) with fastest key repeat preference: ~35s
//   * Press and release on macOS Apple TV IR remote + FLiRC (3.8 firmware): ~150ms
//
#define AUTOREPEAT_MSEC 60

///////////////////////////////////////////////////////////////////////////////////////////////////
InputComponent::InputComponent(QObject* parent) : ComponentBase(parent)
{
  m_mappings = new InputMapping(this);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool InputComponent::addInput(InputBase* base)
{
  if (!base->initInput())
  {
    QLOG_WARN() << "Failed to init input:" << base->inputName();
    return false;
  }

  QLOG_INFO() << "Successfully inited input:" << base->inputName();
  m_inputs.push_back(base);

  // we connect to the provider receivedInput signal, then we check if the name
  // needs to be remaped in remapInput and then finally send it out to JS land.
  //
  connect(base, &InputBase::receivedInput, this, &InputComponent::remapInput);

  // for auto-repeating inputs
  //
  m_autoRepeatTimer = new QTimer(this);
  connect(m_autoRepeatTimer, &QTimer::timeout, [=]()
  {
    if (!m_autoRepeatActions.isEmpty())
    {
      QLOG_DEBUG() << "Emit input action (autorepeat):" << m_autoRepeatActions;
      emit hostInput(m_autoRepeatActions);
    }

    m_autoRepeatTimer->setInterval(AUTOREPEAT_MSEC);
  });

  return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool InputComponent::componentInitialize()
{
  // load our input mappings
  m_mappings->loadMappings();

  addInput(&InputKeyboard::Get());
  addInput(new InputSocket(this));
  addInput(new InputRoku(this));

#ifdef Q_OS_MAC
  addInput(new InputAppleRemote(this));
  addInput(new InputAppleMediaKeys(this));
#endif
#ifdef HAVE_SDL
  if (SettingsComponent::Get().value(SETTINGS_SECTION_MAIN, "sdlEnabled").toBool())
    addInput(new InputSDL(this));
#endif
#ifdef HAVE_LIRC
  addInput(new InputLIRC(this));
#endif
#ifdef HAVE_CEC
  if (SettingsComponent::Get().value(SETTINGS_SECTION_CEC, "enable").toBool())
  {
    InputCEC* cec = new InputCEC(this);
    addInput(cec);
    connect(&InputKeyboard::Get(), &InputKeyboard::receivedInput,
            cec, &InputCEC::activateSource);
  }
#endif

  return true;
}

/////////////////////////////////////////////////////////////////////////////////////////
void InputComponent::handleAction(const QString& action)
{
  if (action.startsWith("host:"))
  {
    QStringList argList = action.mid(5).split(" ");
    QString hostCommand = argList.value(0);
    QString hostArguments;

    if (argList.size() > 1)
    {
      argList.pop_front();
      hostArguments = argList.join(" ");
    }

    QLOG_DEBUG() << "Got host command:" << hostCommand << "arguments:" << hostArguments;
    if (m_hostCommands.contains(hostCommand))
    {
      ReceiverSlot* recvSlot = m_hostCommands.value(hostCommand);
      if (recvSlot)
      {
        if (recvSlot->m_function)
        {
          QLOG_DEBUG() << "Invoking anonymous function";
          recvSlot->m_function();
        }
        else
        {
          QLOG_DEBUG() << "Invoking slot" << qPrintable(recvSlot->m_slot.data());
          QGenericArgument arg0 = QGenericArgument();

          if (recvSlot->m_hasArguments)
            arg0 = Q_ARG(const QString&, hostArguments);

          if (!QMetaObject::invokeMethod(recvSlot->m_receiver, recvSlot->m_slot.data(),
                                         Qt::AutoConnection, arg0))
          {
            QLOG_ERROR() << "Invoking slot" << qPrintable(recvSlot->m_slot.data()) << "failed!";
          }
        }
      }
    }
    else
    {
      QLOG_WARN() << "No such host command:" << hostCommand;
    }
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void InputComponent::remapInput(const QString &source, const QString &keycode, InputBase::InputkeyState keyState)
{
  QLOG_DEBUG() << "Input received: source:" << source << "keycode:" << keycode << ":" << keyState;

  emit receivedInput();

  if (keyState == InputBase::KeyUp)
  {
    cancelAutoRepeat();

    if (!m_currentLongPressAction.isEmpty())
    {
      QString type;
      if (m_longHoldTimer.elapsed() > LONG_HOLD_MSEC)
        type = "long";
      else
        type = "short";

      QString action = m_currentLongPressAction.value(type).toString();

      m_currentLongPressAction.clear();

      QLOG_DEBUG() << "Emit input action (" + type + "):" << action;
      emit hostInput(QStringList{action});
    }

    return;
  }

  QStringList queuedActions;
  m_autoRepeatActions.clear();

  auto actions = m_mappings->mapToAction(source, keycode);
  for (auto action : actions)
  {
    if (action.type() == QVariant::String)
    {
      queuedActions.append(action.toString());
      m_autoRepeatActions.append(action.toString());
    }
    else if (action.type() == QVariant::Map)
    {
      QVariantMap map = action.toMap();
      if (map.contains("long"))
      {
        // Don't overwrite long actions if there was no key up event yet.
        // (It could be a key autorepeated by Qt.)
        if (m_currentLongPressAction.isEmpty())
        {
          m_longHoldTimer.start();
          m_currentLongPressAction = map;
        }
      }
      else if (map.contains("short"))
      {
        queuedActions.append(map.value("short").toString());
      }
    }
    else if (action.type() == QVariant::List)
    {
      queuedActions.append(action.toStringList());
    }
  }

  if (!m_autoRepeatActions.isEmpty() && keyState != InputBase::KeyPressed)
    m_autoRepeatTimer->start(INITAL_AUTOREPEAT_MSEC);

  if (!queuedActions.isEmpty())
  {
    if (SystemComponent::Get().isWebClientConnected())
    {
      QLOG_DEBUG() << "Emit input action:" << queuedActions;
      emit hostInput(queuedActions);
    }
    else
    {
      QLOG_DEBUG() << "Web Client has not connected, handling input in host instead.";
      executeActions(queuedActions);
    }
  }
}

/////////////////////////////////////////////////////////////////////////////////////////
void InputComponent::executeActions(const QStringList& actions)
{
  for (auto action : actions)
    handleAction(action);
}

/////////////////////////////////////////////////////////////////////////////////////////
void InputComponent::registerHostCommand(const QString& command, QObject* receiver, const char* slot)
{
  auto  recvSlot = new ReceiverSlot;
  recvSlot->m_receiver = receiver;
  recvSlot->m_slot = QMetaObject::normalizedSignature(slot);
  recvSlot->m_hasArguments = false;

  QLOG_DEBUG() << "Adding host command:" << qPrintable(command) << "mapped to"
               << qPrintable(QString(receiver->metaObject()->className()) + "::" + recvSlot->m_slot);

  m_hostCommands.insert(command, recvSlot);

  auto slotWithArgs = QString("%1(QString)").arg(QString::fromLatin1(recvSlot->m_slot)).toLatin1();
  auto slotWithoutArgs = QString("%1()").arg(QString::fromLatin1(recvSlot->m_slot)).toLatin1();
  if (recvSlot->m_receiver->metaObject()->indexOfMethod(slotWithArgs.data()) != -1)
  {
    QLOG_DEBUG() << "Host command maps to method with an argument.";
    recvSlot->m_hasArguments = true;
  }
  else if (recvSlot->m_receiver->metaObject()->indexOfMethod(slotWithoutArgs.data()) != -1)
  {
    QLOG_DEBUG() << "Host command maps to method without arguments.";
  }
  else
  {
    QLOG_ERROR() << "Slot for host command missing, or has incorrect signature!";
  }
}

/////////////////////////////////////////////////////////////////////////////////////////
void InputComponent::registerHostCommand(const QString& command, std::function<void(void)> function)
{
  auto recvSlot = new ReceiverSlot;
  recvSlot->m_function = function;
  QLOG_DEBUG() << "Adding host command:" << qPrintable(command) << "mapped to anonymous function";
  m_hostCommands.insert(command, recvSlot);
}

/////////////////////////////////////////////////////////////////////////////////////////
void InputComponent::cancelAutoRepeat()
{
  m_autoRepeatTimer->stop();
  m_autoRepeatActions.clear();
}
