/**********************************************************************
ESP32 COMMAND STATION

COPYRIGHT (c) 2018-2019 NormHal, Mike Dunston

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.
  You should have received a copy of the GNU General Public License
  along with this program.  If not, see http://www.gnu.org/licenses
**********************************************************************/

#include "ESP32CommandStation.h"

HardwareSerial nextionSerial(NEXTION_UART_NUM);
Nextion nextion(nextionSerial);

BaseNextionPage *nextionPages[MAX_PAGES] =
{
  new NextionTitlePage(nextion),
  new NextionAddressPage(nextion),
  new NextionThrottlePage(nextion),
  new NextionTurnoutPage(nextion),
  new NextionSetupPage(nextion),
  /* new NextionRoutesPage(nextion) */ nullptr
};

NEXTION_DEVICE_TYPE nextionDeviceType{NEXTION_DEVICE_TYPE::UNKOWN_DISPLAY};

#if NEXTION_ENABLED

class NextionHMI : private StateFlowBase
{
public:
  /// Constructor.
  ///
  /// @param service is the @ref Service which will handle execution.
  NextionHMI(Service *service);

private:
  uint8_t detectAttempts_{0};
  const uint8_t maxDetectAttempts_{3};
  /// Detects the connected display and transitions to the default page.
  STATE_FLOW_STATE(initialize);

  /// Detects the connected display and transitions to the default page.
  STATE_FLOW_STATE(detect_display);

  /// Handler for data received from the connected display.
  STATE_FLOW_STATE(update);
};

NextionHMI::NextionHMI(Service *service)
  : StateFlowBase(service)
{
  start_flow(STATE(initialize));
}

StateFlowBase::Action NextionHMI::initialize()
{
  LOG(INFO, "[Nextion] Initializing UART(%d) at %u baud on RX %d, TX %d"
    , NEXTION_UART_NUM, config_nextion_uart_speed()
    , config_nextion_rx_pin(), config_nextion_tx_pin());
  nextionSerial.begin(config_nextion_uart_speed(), SERIAL_8N1
              , config_nextion_rx_pin(), config_nextion_tx_pin());
  return yield_and_call(STATE(detect_display));
}

StateFlowBase::Action NextionHMI::detect_display()
{
  StateFlowBase::Callback next_state = STATE(detect_display);
  static string const displayTypes[] =
  {
    "basic 3.2\"",
    "basic 3.5\"",
    "basic 5.0\"",
    "enhanced 3.2\"",
    "enhanced 3.5\"",
    "enhanced 5.0\"",
    "Unknown"
  };
  // flush the serial buffer before detection since it improves reliability
  // of detection.
  nextionSerial.flush();
  NextionTitlePage * titlePage =
    static_cast<NextionTitlePage *>(nextionPages[TITLE_PAGE]);

  // ensure the title page is displayed during detection
  titlePage->display();

  // attempt to identify the nextion display.
  if(detectAttempts_++ < maxDetectAttempts_ &&
      nextionDeviceType == NEXTION_DEVICE_TYPE::UNKOWN_DISPLAY)
  {
    LOG(INFO
      , "[Nextion] [%d/%d] Attempting to identify the attached Nextion display"
      , detectAttempts_, maxDetectAttempts_);
    nextion.sendCommand("DRAKJHSUYDGBNCJHGJKSHBDN");
    nextion.sendCommand("connect");
    String screenID = "";
    size_t res = nextion.receiveString(screenID, false);
    if(res && screenID.indexOf("comok") >= 0)
    {
      // break the returned string into its comma delimited chunks
      // start after the first space
      vector<string> parts;
      esp32cs::tokenize(screenID.substring(screenID.indexOf(' ') + 1).c_str()
                      , parts, ",");
      // attempt to parse device model
      if(!parts[2].compare(0, 7, "NX4024K"))
      {
        nextionDeviceType = NEXTION_DEVICE_TYPE::ENHANCED_3_2_DISPLAY;
      }
      else if(!parts[2].compare(0, 7, "NX4024T"))
      {
        nextionDeviceType = NEXTION_DEVICE_TYPE::BASIC_3_2_DISPLAY;
      }
      else if(!parts[2].compare(0, 7, "NX4832K"))
      {
        nextionDeviceType = NEXTION_DEVICE_TYPE::ENHANCED_3_5_DISPLAY;
      }
      else if(!parts[2].compare(0, 7, "NX4832T"))
      {
        nextionDeviceType = NEXTION_DEVICE_TYPE::BASIC_3_5_DISPLAY;
      }
      else if(!parts[2].compare(0, 7, "NX8048K"))
      {
        nextionDeviceType = NEXTION_DEVICE_TYPE::ENHANCED_5_0_DISPLAY;
      }
      else if(!parts[2].compare(0, 7, "NX8048T"))
      {
        nextionDeviceType = NEXTION_DEVICE_TYPE::BASIC_5_0_DISPLAY;
      }
      else
      {
        LOG(WARNING, "[Nextion] Unrecognized Nextion Device model: %s"
          , parts[2].c_str());
      }
      LOG(INFO, "[Nextion] Device type: %s"
        , displayTypes[nextionDeviceType].c_str());
      LOG(INFO, "[Nextion] Firmware Version: %s", parts[3].c_str());
      LOG(INFO, "[Nextion] MCU Code: %s", parts[4].c_str());
      LOG(INFO, "[Nextion] Serial #: %s", parts[5].c_str());
      LOG(INFO, "[Nextion] Flash size: %s bytes", parts[6].c_str());
      next_state = STATE(update);
    }
    else
    {
      LOG(WARNING
        , "[Nextion] Unrecognized response from Nextion display: %s"
        , screenID.c_str());
    }
  }
  else if(nextionDeviceType == NEXTION_DEVICE_TYPE::UNKOWN_DISPLAY)
  {
    LOG(WARNING
      , "[Nextion] Failed to identify the attached Nextion display, "
        "defaulting to 3.2\" basic display");
    nextionDeviceType = NEXTION_DEVICE_TYPE::BASIC_3_2_DISPLAY;
    next_state = STATE(update);
  }

  // flush the serial buffer after detection to discard any leftover data.
  nextionSerial.flush();
  return yield_and_call(next_state);
}

StateFlowBase::Action NextionHMI::update()
{
  nextion.poll();
  return yield_and_call(STATE(update));
}
uninitialized<NextionHMI> nextionHMI;
#endif // NEXTION_ENABLED

void nextionInterfaceInit()
{
#if NEXTION_ENABLED
  Singleton<InfoScreen>::instance()->replaceLine(
    INFO_SCREEN_ROTATING_STATUS_LINE, "Init Nextion");
  extern unique_ptr<OpenMRN> openmrn;
  nextionHMI.emplace(openmrn->stack()->service());
#endif // NEXTION_ENABLED
}

