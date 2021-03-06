#include "analog_controller.h"
#include "common/log.h"
#include "common/state_wrapper.h"
#include "common/string_util.h"
#include "host_interface.h"
#include "settings.h"
#include "system.h"
#include <cmath>
Log_SetChannel(AnalogController);

AnalogController::AnalogController(u32 index) : m_index(index)
{
  m_axis_state.fill(0x80);
  Reset();
}

AnalogController::~AnalogController() = default;

ControllerType AnalogController::GetType() const
{
  return ControllerType::AnalogController;
}

void AnalogController::Reset()
{
  m_state = State::Idle;
  m_analog_mode = false;
  m_configuration_mode = false;
  m_command_param = 0;
  m_motor_state.fill(0);

  ResetRumbleConfig();

  if (m_force_analog_on_reset)
  {
    if (g_settings.controller_disable_analog_mode_forcing)
    {
      g_host_interface->AddOSDMessage(
        g_host_interface->TranslateStdString(
          "OSDMessage", "Analog mode forcing is disabled by game settings. Controller will start in digital mode."),
        10.0f);
    }
    else
      SetAnalogMode(true);
  }
}

bool AnalogController::DoState(StateWrapper& sw, bool apply_input_state)
{
  if (!Controller::DoState(sw, apply_input_state))
    return false;

  const bool old_analog_mode = m_analog_mode;

  sw.Do(&m_analog_mode);
  sw.Do(&m_rumble_unlocked);
  sw.DoEx(&m_legacy_rumble_unlocked, 44, false);
  sw.Do(&m_configuration_mode);
  sw.Do(&m_command_param);

  u16 button_state = m_button_state;
  sw.DoEx(&button_state, 44, static_cast<u16>(0xFFFF));
  if (apply_input_state)
    m_button_state = button_state;

  sw.Do(&m_state);

  sw.DoEx(&m_rumble_config, 45, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
  sw.DoEx(&m_rumble_config_large_motor_index, 45, -1);
  sw.DoEx(&m_rumble_config_small_motor_index, 45, -1);
  sw.DoEx(&m_analog_toggle_queued, 45, false);

  MotorState motor_state = m_motor_state;
  sw.Do(&motor_state);

  if (sw.IsReading())
  {
    for (u8 i = 0; i < NUM_MOTORS; i++)
      SetMotorState(i, motor_state[i]);

    if (old_analog_mode != m_analog_mode)
    {
      g_host_interface->AddFormattedOSDMessage(
        5.0f,
        m_analog_mode ?
          g_host_interface->TranslateString("AnalogController", "Controller %u switched to analog mode.") :
          g_host_interface->TranslateString("AnalogController", "Controller %u switched to digital mode."),
        m_index + 1u);
    }
  }
  return true;
}

std::optional<s32> AnalogController::GetAxisCodeByName(std::string_view axis_name) const
{
  return StaticGetAxisCodeByName(axis_name);
}

std::optional<s32> AnalogController::GetButtonCodeByName(std::string_view button_name) const
{
  return StaticGetButtonCodeByName(button_name);
}

void AnalogController::SetAxisState(s32 axis_code, float value)
{
  if (axis_code < 0 || axis_code >= static_cast<s32>(Axis::Count))
    return;

  // -1..1 -> 0..255
  const float scaled_value = std::clamp(value * m_axis_scale, -1.0f, 1.0f);
  const u8 u8_value = static_cast<u8>(std::clamp(((scaled_value + 1.0f) / 2.0f) * 255.0f, 0.0f, 255.0f));

  SetAxisState(static_cast<Axis>(axis_code), u8_value);
}

void AnalogController::SetAxisState(Axis axis, u8 value)
{
  m_axis_state[static_cast<u8>(axis)] = value;
}

void AnalogController::SetButtonState(Button button, bool pressed)
{
  if (button == Button::Analog)
  {
    // analog toggle
    if (pressed)
      m_analog_toggle_queued = true;

    return;
  }

  if (pressed)
    m_button_state &= ~(u16(1) << static_cast<u8>(button));
  else
    m_button_state |= u16(1) << static_cast<u8>(button);
}

void AnalogController::SetButtonState(s32 button_code, bool pressed)
{
  if (button_code < 0 || button_code >= static_cast<s32>(Button::Count))
    return;

  SetButtonState(static_cast<Button>(button_code), pressed);
}

u32 AnalogController::GetButtonStateBits() const
{
  // flip bits, native data is active low
  return m_button_state ^ 0xFFFF;
}

u32 AnalogController::GetVibrationMotorCount() const
{
  return NUM_MOTORS;
}

float AnalogController::GetVibrationMotorStrength(u32 motor)
{
  DebugAssert(motor < NUM_MOTORS);
  if (m_motor_state[motor] == 0)
    return 0.0f;

  // Curve from https://github.com/KrossX/Pokopom/blob/master/Pokopom/Input_XInput.cpp#L210
  const double x =
    static_cast<double>(std::min<u32>(static_cast<u32>(m_motor_state[motor]) + static_cast<u32>(m_rumble_bias), 255));
  const double strength = 0.006474549734772402 * std::pow(x, 3.0) - 1.258165252213538 * std::pow(x, 2.0) +
                          156.82454281087692 * x + 3.637978807091713e-11;

  return static_cast<float>(strength / 65535.0);
}

void AnalogController::ResetTransferState()
{
  if (m_analog_toggle_queued)
  {
    if (m_analog_locked)
    {
      g_host_interface->AddFormattedOSDMessage(
        5.0f,
        m_analog_mode ?
          g_host_interface->TranslateString("AnalogController", "Controller %u is locked to analog mode by the game.") :
          g_host_interface->TranslateString("AnalogController", "Controller %u is locked to digital mode by the game."),
        m_index + 1u);
    }
    else
    {
      SetAnalogMode(!m_analog_mode);

      // Manually toggling controller mode resets and disables rumble configuration
      ResetRumbleConfig();

      // TODO: Mode switch detection (0x00 returned on certain commands instead of 0x5A)
    }

    m_analog_toggle_queued = false;
  }

  m_state = State::Idle;
}

u16 AnalogController::GetID() const
{
  static constexpr u16 DIGITAL_MODE_ID = 0x5A41;
  static constexpr u16 ANALOG_MODE_ID = 0x5A73;
  static constexpr u16 CONFIG_MODE_ID = 0x5AF3;

  if (m_configuration_mode)
    return CONFIG_MODE_ID;

  return m_analog_mode ? ANALOG_MODE_ID : DIGITAL_MODE_ID;
}

void AnalogController::SetAnalogMode(bool enabled)
{
  if (m_analog_mode == enabled)
    return;

  Log_InfoPrintf("Controller %u switched to %s mode.", m_index + 1u, enabled ? "analog" : "digital");
  g_host_interface->AddFormattedOSDMessage(
    5.0f,
    enabled ? g_host_interface->TranslateString("AnalogController", "Controller %u switched to analog mode.") :
              g_host_interface->TranslateString("AnalogController", "Controller %u switched to digital mode."),
    m_index + 1u);
  m_analog_mode = enabled;
}

void AnalogController::SetMotorState(u8 motor, u8 value)
{
  DebugAssert(motor < NUM_MOTORS);
  m_motor_state[motor] = value;
}

u8 AnalogController::GetExtraButtonMaskLSB() const
{
  if (!m_analog_dpad_in_digital_mode || m_analog_mode || m_configuration_mode)
    return 0xFF;

  static constexpr u8 NEG_THRESHOLD = static_cast<u8>(128.0f - (127.0 * 0.5f));
  static constexpr u8 POS_THRESHOLD = static_cast<u8>(128.0f + (127.0 * 0.5f));

  const bool left = (m_axis_state[static_cast<u8>(Axis::LeftX)] <= NEG_THRESHOLD);
  const bool right = (m_axis_state[static_cast<u8>(Axis::LeftX)] >= POS_THRESHOLD);
  const bool up = (m_axis_state[static_cast<u8>(Axis::LeftY)] <= NEG_THRESHOLD);
  const bool down = (m_axis_state[static_cast<u8>(Axis::LeftY)] >= POS_THRESHOLD);

  return ~((static_cast<u8>(left) << static_cast<u8>(Button::Left)) |
           (static_cast<u8>(right) << static_cast<u8>(Button::Right)) |
           (static_cast<u8>(up) << static_cast<u8>(Button::Up)) |
           (static_cast<u8>(down) << static_cast<u8>(Button::Down)));
}

void AnalogController::ResetRumbleConfig()
{
  m_legacy_rumble_unlocked = false;

  m_rumble_unlocked = false;
  m_rumble_config.fill(0xFF);

  m_rumble_config_large_motor_index = -1;
  m_rumble_config_small_motor_index = -1;

  SetMotorState(LargeMotor, 0);
  SetMotorState(SmallMotor, 0);
}

void AnalogController::SetMotorStateForConfigIndex(int index, u8 value)
{
  if (m_rumble_config_small_motor_index == index)
    SetMotorState(SmallMotor, ((value & 0x01) != 0) ? 255 : 0);
  else if (m_rumble_config_large_motor_index == index)
    SetMotorState(LargeMotor, value);
}

bool AnalogController::Transfer(const u8 data_in, u8* data_out)
{
  bool ack;
#ifdef _DEBUG
  u8 old_state = static_cast<u8>(m_state);
#endif

  switch (m_state)
  {
#define FIXED_REPLY_STATE(state, reply, ack_value, next_state)                                                         \
  case state:                                                                                                          \
  {                                                                                                                    \
    *data_out = reply;                                                                                                 \
    m_state = next_state;                                                                                              \
    ack = ack_value;                                                                                                   \
  }                                                                                                                    \
  break;

#define ID_STATE_MSB(state, next_state)                                                                                \
  case state:                                                                                                          \
  {                                                                                                                    \
    *data_out = Truncate8(GetID() >> 8);                                                                               \
    m_state = next_state;                                                                                              \
    ack = true;                                                                                                        \
  }                                                                                                                    \
  break;

#define REPLY_RUMBLE_CONFIG(state, index, ack_value, next_state)                                                       \
  case state:                                                                                                          \
  {                                                                                                                    \
    DebugAssert(index < m_rumble_config.size());                                                                       \
    *data_out = m_rumble_config[index];                                                                                \
    m_rumble_config[index] = data_in;                                                                                  \
                                                                                                                       \
    if (data_in == 0x00)                                                                                               \
      m_rumble_config_small_motor_index = index;                                                                       \
    else if (data_in == 0x01)                                                                                          \
      m_rumble_config_large_motor_index = index;                                                                       \
                                                                                                                       \
    m_state = next_state;                                                                                              \
    ack = ack_value;                                                                                                   \
  }                                                                                                                    \
  break;

    case State::Idle:
    {
      // ack when sent 0x01, send ID for 0x42
      if (data_in == 0x42)
      {
        *data_out = Truncate8(GetID());
        m_state = State::GetStateIDMSB;
        ack = true;
      }
      else if (data_in == 0x43)
      {
        *data_out = Truncate8(GetID());
        m_state = State::ConfigModeIDMSB;
        ack = true;
      }
      else if (m_configuration_mode && data_in == 0x44)
      {
        *data_out = Truncate8(GetID());
        m_state = State::SetAnalogModeIDMSB;
        ack = true;
      }
      else if (m_configuration_mode && data_in == 0x45)
      {
        *data_out = Truncate8(GetID());
        m_state = State::GetAnalogModeIDMSB;
        ack = true;
      }
      else if (m_configuration_mode && data_in == 0x46)
      {
        *data_out = Truncate8(GetID());
        m_state = State::Command46IDMSB;
        ack = true;
      }
      else if (m_configuration_mode && data_in == 0x47)
      {
        *data_out = Truncate8(GetID());
        m_state = State::Command47IDMSB;
        ack = true;
      }
      else if (m_configuration_mode && data_in == 0x4C)
      {
        *data_out = Truncate8(GetID());
        m_state = State::Command4CIDMSB;
        ack = true;
      }
      else if (m_configuration_mode && data_in == 0x4D)
      {
        m_rumble_unlocked = true;
        *data_out = Truncate8(GetID());
        m_state = State::UnlockRumbleIDMSB;
        m_rumble_config_large_motor_index = -1;
        m_rumble_config_small_motor_index = -1;
        ack = true;
      }
      else
      {
        Log_DebugPrintf("data_in = 0x%02X", data_in);
        *data_out = 0xFF;
        ack = (data_in == 0x01);
      }
    }
    break;

      ID_STATE_MSB(State::GetStateIDMSB, State::GetStateButtonsLSB);

    case State::GetStateButtonsLSB:
    {
      if (m_rumble_unlocked)
        SetMotorStateForConfigIndex(0, data_in);
      else if (data_in >= 0x40 && data_in <= 0x7F)
        m_legacy_rumble_unlocked = true;
      else
        SetMotorState(SmallMotor, 0);

      *data_out = Truncate8(m_button_state) & GetExtraButtonMaskLSB();
      m_state = State::GetStateButtonsMSB;
      ack = true;
    }
    break;

    case State::GetStateButtonsMSB:
    {
      if (m_rumble_unlocked)
      {
        SetMotorStateForConfigIndex(1, data_in);
      }
      else if (m_legacy_rumble_unlocked)
      {
        SetMotorState(SmallMotor, ((data_in & 0x01) != 0) ? 255 : 0);
        m_legacy_rumble_unlocked = false;
      }

      *data_out = Truncate8(m_button_state >> 8);
      m_state = (m_analog_mode || m_configuration_mode) ? State::GetStateRightAxisX : State::Idle;
      ack = m_analog_mode || m_configuration_mode;
    }
    break;

    case State::GetStateRightAxisX:
    {
      if (m_rumble_unlocked)
        SetMotorStateForConfigIndex(2, data_in);

      *data_out = Truncate8(m_axis_state[static_cast<u8>(Axis::RightX)]);
      m_state = State::GetStateRightAxisY;
      ack = true;
    }
    break;

    case State::GetStateRightAxisY:
    {
      if (m_rumble_unlocked)
        SetMotorStateForConfigIndex(3, data_in);

      *data_out = Truncate8(m_axis_state[static_cast<u8>(Axis::RightY)]);
      m_state = State::GetStateLeftAxisX;
      ack = true;
    }
    break;

    case State::GetStateLeftAxisX:
    {
      if (m_rumble_unlocked)
        SetMotorStateForConfigIndex(4, data_in);

      *data_out = Truncate8(m_axis_state[static_cast<u8>(Axis::LeftX)]);
      m_state = State::GetStateLeftAxisY;
      ack = true;
    }
    break;

    case State::GetStateLeftAxisY:
    {
      if (m_rumble_unlocked)
        SetMotorStateForConfigIndex(5, data_in);

      *data_out = Truncate8(m_axis_state[static_cast<u8>(Axis::LeftY)]);
      m_state = State::Idle;
      ack = false;
    }
    break;

      ID_STATE_MSB(State::ConfigModeIDMSB, State::ConfigModeSetMode);

    case State::ConfigModeSetMode:
    {
      // If 0x43 "enter/leave config mode" is called from within config mode, return all zeros
      Log_DebugPrintf("0x%02x(%s) config mode", data_in, data_in == 1 ? "enter" : "leave");
      bool prev_configuration_mode = m_configuration_mode;
      m_configuration_mode = (data_in == 1);
      *data_out = prev_configuration_mode ? 0x00 : Truncate8(m_button_state);
      m_state = prev_configuration_mode ? State::Pad5Bytes : State::GetStateButtonsMSB;
      ack = true;
    }
    break;

      ID_STATE_MSB(State::SetAnalogModeIDMSB, State::SetAnalogModeVal);

    case State::SetAnalogModeVal:
    {
      Log_DevPrintf("analog mode val 0x%02x", data_in);
      if (data_in == 0x00 || data_in == 0x01)
        SetAnalogMode((data_in == 0x01));

      *data_out = 0x00;
      m_state = State::SetAnalogModeSel;
      ack = true;
    }
    break;

    case State::SetAnalogModeSel:
    {
      Log_DevPrintf("analog mode lock 0x%02x", data_in);
      if (data_in == 0x02 || data_in == 0x03)
        m_analog_locked = (data_in == 0x03);

      *data_out = 0x00;
      m_state = State::Pad4Bytes;
      ack = true;
    }
    break;

      ID_STATE_MSB(State::GetAnalogModeIDMSB, State::GetAnalogMode1);
      FIXED_REPLY_STATE(State::GetAnalogMode1, 0x01, true, State::GetAnalogMode2);
      FIXED_REPLY_STATE(State::GetAnalogMode2, 0x02, true, State::GetAnalogMode3);
      FIXED_REPLY_STATE(State::GetAnalogMode3, BoolToUInt8(m_analog_mode), true, State::GetAnalogMode4);
      FIXED_REPLY_STATE(State::GetAnalogMode4, 0x02, true, State::GetAnalogMode5);
      FIXED_REPLY_STATE(State::GetAnalogMode5, 0x01, true, State::GetAnalogMode6);
      FIXED_REPLY_STATE(State::GetAnalogMode6, 0x00, false, State::Idle);

      ID_STATE_MSB(State::Command46IDMSB, State::Command461);

    case State::Command461:
    {
      Log_DebugPrintf("command 46 param 0x%02X", data_in);
      m_command_param = data_in;
      *data_out = 0x00;
      m_state = State::Command462;
      ack = true;
    }
    break;

      FIXED_REPLY_STATE(State::Command462, 0x00, true, State::Command463);
      FIXED_REPLY_STATE(State::Command463, 0x01, true, State::Command464);
      FIXED_REPLY_STATE(State::Command464, ((m_command_param == 1) ? 1 : 2), true, State::Command465);
      FIXED_REPLY_STATE(State::Command465, ((m_command_param == 1) ? 1 : 0), true, State::Command466);
      FIXED_REPLY_STATE(State::Command466, ((m_command_param == 1) ? 0x14 : 0x0A), false, State::Idle);

      ID_STATE_MSB(State::Command47IDMSB, State::Command471);
      FIXED_REPLY_STATE(State::Command471, 0x00, true, State::Command472);
      FIXED_REPLY_STATE(State::Command472, 0x00, true, State::Command473);
      FIXED_REPLY_STATE(State::Command473, 0x02, true, State::Command474);
      FIXED_REPLY_STATE(State::Command474, 0x00, true, State::Command475);
      FIXED_REPLY_STATE(State::Command475, 0x01, true, State::Command476);
      FIXED_REPLY_STATE(State::Command476, 0x00, false, State::Idle);

      ID_STATE_MSB(State::Command4CIDMSB, State::Command4CMode);

    case State::Command4CMode:
    {
      m_command_param = data_in;
      *data_out = 0x00;
      m_state = State::Command4C1;
      ack = true;
    }
    break;

      FIXED_REPLY_STATE(State::Command4C1, 0x00, true, State::Command4C2);
      FIXED_REPLY_STATE(State::Command4C2, 0x00, true, State::Command4C3);

    case State::Command4C3:
    {
      // Ape Escape sends both 0x00 and 0x01 sequences on startup and checks for correct response
      if (m_command_param == 0x00)
        *data_out = 0x04;
      else if (m_command_param == 0x01)
        *data_out = 0x07;
      else
        *data_out = 0x00;

      m_state = State::Command4C4;
      ack = true;
    }
    break;

      FIXED_REPLY_STATE(State::Command4C4, 0x00, true, State::Command4C5);
      FIXED_REPLY_STATE(State::Command4C5, 0x00, false, State::Idle);

      ID_STATE_MSB(State::UnlockRumbleIDMSB, State::GetSetRumble1);
      REPLY_RUMBLE_CONFIG(State::GetSetRumble1, 0, true, State::GetSetRumble2);
      REPLY_RUMBLE_CONFIG(State::GetSetRumble2, 1, true, State::GetSetRumble3);
      REPLY_RUMBLE_CONFIG(State::GetSetRumble3, 2, true, State::GetSetRumble4);
      REPLY_RUMBLE_CONFIG(State::GetSetRumble4, 3, true, State::GetSetRumble5);
      REPLY_RUMBLE_CONFIG(State::GetSetRumble5, 4, true, State::GetSetRumble6);

    case State::GetSetRumble6:
    {
      DebugAssert(5 < m_rumble_config.size());
      *data_out = m_rumble_config[5];
      m_rumble_config[5] = data_in;

      if (data_in == 0x00)
        m_rumble_config_small_motor_index = 5;
      else if (data_in == 0x01)
        m_rumble_config_large_motor_index = 5;

      if (m_rumble_config_large_motor_index == -1)
        SetMotorState(LargeMotor, 0);

      if (m_rumble_config_small_motor_index == -1)
        SetMotorState(SmallMotor, 0);

      if (m_rumble_config_large_motor_index == -1 && m_rumble_config_small_motor_index == -1)
        m_rumble_unlocked = false;

      // Unknown if motor config array forces 0xFF values if configured byte is not 0x00 or 0x01
      // Unknown under what circumstances rumble is locked and legacy rumble is re-enabled, if even possible
      // e.g. need all 0xFFs?

      m_state = State::Idle;
      ack = false;
    }
    break;

      FIXED_REPLY_STATE(State::Pad6Bytes, 0x00, true, State::Pad5Bytes);
      FIXED_REPLY_STATE(State::Pad5Bytes, 0x00, true, State::Pad4Bytes);
      FIXED_REPLY_STATE(State::Pad4Bytes, 0x00, true, State::Pad3Bytes);
      FIXED_REPLY_STATE(State::Pad3Bytes, 0x00, true, State::Pad2Bytes);
      FIXED_REPLY_STATE(State::Pad2Bytes, 0x00, true, State::Pad1Byte);
      FIXED_REPLY_STATE(State::Pad1Byte, 0x00, false, State::Idle);

    default:
    {
      UnreachableCode();
      return false;
    }
  }

  Log_DebugPrintf("Transfer, old_state=%u, new_state=%u, data_in=0x%02X, data_out=0x%02X, ack=%s",
                  static_cast<u32>(old_state), static_cast<u32>(m_state), data_in, *data_out, ack ? "true" : "false");
  return ack;
}

std::unique_ptr<AnalogController> AnalogController::Create(u32 index)
{
  return std::make_unique<AnalogController>(index);
}

std::optional<s32> AnalogController::StaticGetAxisCodeByName(std::string_view axis_name)
{
#define AXIS(name)                                                                                                     \
  if (axis_name == #name)                                                                                              \
  {                                                                                                                    \
    return static_cast<s32>(ZeroExtend32(static_cast<u8>(Axis::name)));                                                \
  }

  AXIS(LeftX);
  AXIS(LeftY);
  AXIS(RightX);
  AXIS(RightY);

  return std::nullopt;

#undef AXIS
}

std::optional<s32> AnalogController::StaticGetButtonCodeByName(std::string_view button_name)
{
#define BUTTON(name)                                                                                                   \
  if (button_name == #name)                                                                                            \
  {                                                                                                                    \
    return static_cast<s32>(ZeroExtend32(static_cast<u8>(Button::name)));                                              \
  }

  BUTTON(Select);
  BUTTON(L3);
  BUTTON(R3);
  BUTTON(Start);
  BUTTON(Up);
  BUTTON(Right);
  BUTTON(Down);
  BUTTON(Left);
  BUTTON(L2);
  BUTTON(R2);
  BUTTON(L1);
  BUTTON(R1);
  BUTTON(Triangle);
  BUTTON(Circle);
  BUTTON(Cross);
  BUTTON(Square);
  BUTTON(Analog);

  return std::nullopt;

#undef BUTTON
}

Controller::AxisList AnalogController::StaticGetAxisNames()
{
  return {{TRANSLATABLE("AnalogController", "LeftX"), static_cast<s32>(Axis::LeftX), AxisType::Full},
          {TRANSLATABLE("AnalogController", "LeftY"), static_cast<s32>(Axis::LeftY), AxisType::Full},
          {TRANSLATABLE("AnalogController", "RightX"), static_cast<s32>(Axis::RightX), AxisType::Full},
          {TRANSLATABLE("AnalogController", "RightY"), static_cast<s32>(Axis::RightY), AxisType::Full}};
}

Controller::ButtonList AnalogController::StaticGetButtonNames()
{
  return {{TRANSLATABLE("AnalogController", "Up"), static_cast<s32>(Button::Up)},
          {TRANSLATABLE("AnalogController", "Down"), static_cast<s32>(Button::Down)},
          {TRANSLATABLE("AnalogController", "Left"), static_cast<s32>(Button::Left)},
          {TRANSLATABLE("AnalogController", "Right"), static_cast<s32>(Button::Right)},
          {TRANSLATABLE("AnalogController", "Select"), static_cast<s32>(Button::Select)},
          {TRANSLATABLE("AnalogController", "Start"), static_cast<s32>(Button::Start)},
          {TRANSLATABLE("AnalogController", "Triangle"), static_cast<s32>(Button::Triangle)},
          {TRANSLATABLE("AnalogController", "Cross"), static_cast<s32>(Button::Cross)},
          {TRANSLATABLE("AnalogController", "Circle"), static_cast<s32>(Button::Circle)},
          {TRANSLATABLE("AnalogController", "Square"), static_cast<s32>(Button::Square)},
          {TRANSLATABLE("AnalogController", "L1"), static_cast<s32>(Button::L1)},
          {TRANSLATABLE("AnalogController", "L2"), static_cast<s32>(Button::L2)},
          {TRANSLATABLE("AnalogController", "R1"), static_cast<s32>(Button::R1)},
          {TRANSLATABLE("AnalogController", "R2"), static_cast<s32>(Button::R2)},
          {TRANSLATABLE("AnalogController", "L3"), static_cast<s32>(Button::L3)},
          {TRANSLATABLE("AnalogController", "R3"), static_cast<s32>(Button::R3)},
          {TRANSLATABLE("AnalogController", "Analog"), static_cast<s32>(Button::Analog)}};
}

u32 AnalogController::StaticGetVibrationMotorCount()
{
  return NUM_MOTORS;
}

Controller::SettingList AnalogController::StaticGetSettings()
{
  static constexpr std::array<SettingInfo, 4> settings = {
    {{SettingInfo::Type::Boolean, "ForceAnalogOnReset", TRANSLATABLE("AnalogController", "Force Analog Mode on Reset"),
      TRANSLATABLE("AnalogController", "Forces the controller to analog mode when the console is reset/powered on. May "
                                       "cause issues with games, so it is recommended to leave this option off."),
      "false"},
     {SettingInfo::Type::Boolean, "AnalogDPadInDigitalMode",
      TRANSLATABLE("AnalogController", "Use Analog Sticks for D-Pad in Digital Mode"),
      TRANSLATABLE("AnalogController",
                   "Allows you to use the analog sticks to control the d-pad in digital mode, as well as the buttons."),
      "false"},
     {SettingInfo::Type::Float, "AxisScale", TRANSLATABLE("AnalogController", "Analog Axis Scale"),
      TRANSLATABLE(
        "AnalogController",
        "Sets the analog stick axis scaling factor. A value between 1.30 and 1.40 is recommended when using recent "
        "controllers, e.g. DualShock 4, Xbox One Controller."),
      "1.00f", "0.01f", "1.50f", "0.01f"},
     {SettingInfo::Type::Integer, "VibrationBias", TRANSLATABLE("AnalogController", "Vibration Bias"),
      TRANSLATABLE("AnalogController", "Sets the rumble bias value. If rumble in some games is too weak or not "
                                       "functioning, try increasing this value."),
      "8", "0", "255", "1"}}};

  return SettingList(settings.begin(), settings.end());
}

void AnalogController::LoadSettings(const char* section)
{
  Controller::LoadSettings(section);
  m_force_analog_on_reset = g_host_interface->GetBoolSettingValue(section, "ForceAnalogOnReset", false);
  m_analog_dpad_in_digital_mode = g_host_interface->GetBoolSettingValue(section, "AnalogDPadInDigitalMode", false);
  m_axis_scale =
    std::clamp(std::abs(g_host_interface->GetFloatSettingValue(section, "AxisScale", 1.00f)), 0.01f, 1.50f);
  m_rumble_bias =
    static_cast<u8>(std::min<u32>(g_host_interface->GetIntSettingValue(section, "VibrationBias", 8), 255));
}
