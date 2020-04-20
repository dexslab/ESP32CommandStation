/**********************************************************************
ESP32 COMMAND STATION

COPYRIGHT (c) 2020 Mike Dunston

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

#include "LCCStackManager.h"
#include "CDIHelper.h"
#include "ConfigurationManager.h"
//#include <freertos_drivers/esp32/Esp32HardwareCanAdapter.hxx>
#include <openlcb/SimpleStack.hxx>
#include <utils/AutoSyncFileFlow.hxx>

namespace esp32cs
{

static constexpr const char LCC_NODE_ID_FILE[] = "lcc-node";
static constexpr const char LCC_RESET_MARKER_FILE[] = "lcc-rst";
static constexpr const char LCC_CAN_MARKER_FILE[] = "lcc-can";

LCCStackManager::LCCStackManager(const esp32cs::Esp32ConfigDef &cfg) : cfg_(cfg)
{
#if defined(CONFIG_LCC_FACTORY_RESET) || defined(CONFIG_ESP32CS_FORCE_FACTORY_RESET)
  bool lcc_factory_reset{true};
#else
  bool lcc_factory_reset{false};
#endif
  auto cfg_mgr = Singleton<ConfigurationManager>::instance();
  struct stat statbuf;

  // If we are not currently forcing a factory reset, verify if the LCC config
  // file is the correct size. If it is not the expected size force a factory
  // reset.
  if (!lcc_factory_reset &&
      stat(LCC_CONFIG_FILE, &statbuf) != 0 &&
      statbuf.st_size != openlcb::CONFIG_FILE_SIZE)
  {
    LOG(WARNING
      , "[LCC] Corrupt/missing configuration file detected, %s is not the "
        "expected size: %lu vs %zu bytes."
      , LCC_CONFIG_FILE, statbuf.st_size, openlcb::CONFIG_FILE_SIZE);
    lcc_factory_reset = true;
  }
  else
  {
    LOG(VERBOSE, "[LCC] node config file(%s) is expected size %lu bytes"
      , LCC_CONFIG_FILE, statbuf.st_size);
  }
  if (cfg_mgr->exists(LCC_RESET_MARKER_FILE) || lcc_factory_reset)
  {
    cfg_mgr->remove(LCC_RESET_MARKER_FILE);
    if (!stat(LCC_CONFIG_FILE, &statbuf))
    {
        LOG(WARNING, "[LCC] Forcing regeneration of %s", LCC_CONFIG_FILE);
        ERRNOCHECK(LCC_CONFIG_FILE, unlink(LCC_CONFIG_FILE));
    }
    if (!stat(LCC_CDI_XML, &statbuf))
    {
        LOG(WARNING, "[LCC] Forcing regeneration of %s", LCC_CDI_XML);
        ERRNOCHECK(LCC_CDI_XML, unlink(LCC_CDI_XML));
    }
  }

  LOG(INFO, "[LCC] Loading configuration");
  string node_id_str = uint64_to_string_hex(UINT64_C(CONFIG_LCC_NODE_ID));
  if (!cfg_mgr->exists(LCC_NODE_ID_FILE))
  {
    LOG(INFO, "[LCC] Initializing configuration data...");
    cfg_mgr->store(LCC_NODE_ID_FILE, node_id_str);
#if defined(CONFIG_LCC_CAN_ENABLED)
    cfg_mgr->store(LCC_CAN_MARKER_FILE, node_id_str);
#endif // CONFIG_LCC_CAN_ENABLED
  }
  else
  {
    node_id_str = cfg_mgr->load(LCC_NODE_ID_FILE);
  }

  nodeID_ = string_to_uint64(node_id_str);

  LOG(INFO, "[LCC] Initializing Stack (node-id: %s)"
    , uint64_to_string_hex(nodeID_).c_str());
#ifdef CONFIG_LCC_TCP_STACK
  stack_ = new openlcb::SimpleTcpStack(nodeID_);
#else
  stack_ = new openlcb::SimpleCanStack(nodeID_);
/*#if defined(CONFIG_LCC_CAN_ENABLED)
  if (cfg_mgr->exists(LCC_CAN_MARKER_FILE))
  {
    LOG(INFO, "[LCC] Enabling CAN interface (rx: %d, tx: %d)"
      , CONFIG_LCC_CAN_RX_PIN, CONFIG_LCC_CAN_TX_PIN);
    can_ = new Esp32HardwareCan("esp32can"
                              , (gpio_num_t)CONFIG_LCC_CAN_RX_PIN
                              , (gpio_num_t)CONFIG_LCC_CAN_TX_PIN
                              , false));
    canBridge_ = new CanBridge(can_, ((openlcb::SimpleCanStack *)stack_)->can_hub()));
    stack_->executor()->add(can_);
    stack_->executor()->add(canBridge_);
  }
#endif // CONFIG_LCC_CAN_ENABLED*/
#endif // CONFIG_LCC_TCP_STACK
}

openlcb::SimpleStackBase *LCCStackManager::stack()
{
  return stack_;
}

#ifndef CONFIG_LCC_SD_FSYNC_SEC
#define CONFIG_LCC_SD_FSYNC_SEC 10
#endif

void LCCStackManager::start(bool is_sd)
{
  // Create the CDI.xml dynamically if it doesn't already exist.
  CDIHelper::create_config_descriptor_xml(cfg_, LCC_CDI_XML, stack_);

  // Create the default internal configuration file if it doesn't already exist.
  fd_ =
    stack_->create_config_file_if_needed(cfg_.seg().internal_config()
                                       , CONFIG_ESP32CS_CDI_VERSION
                                       , openlcb::CONFIG_FILE_SIZE);
  LOG(INFO, "[LCC] Config file FD: %d", fd_);

  if (is_sd)
  {
    // ESP32 FFat library uses a 512b cache in memory by default for the SD VFS
    // adding a periodic fsync call for the LCC configuration file ensures that
    // config changes are saved since the LCC config file is less than 512b.
    LOG(INFO, "[LCC] Creating automatic fsync(%d) calls every %d seconds."
      , fd_, CONFIG_LCC_SD_FSYNC_SEC);
    configAutoSync_ =
      new AutoSyncFileFlow(stack_->service(), fd_
                         , SEC_TO_USEC(CONFIG_LCC_SD_FSYNC_SEC));
  }
#if defined(CONFIG_LCC_PRINT_ALL_PACKETS)
  LOG(INFO, "[LCC] Configuring LCC packet printer");
  stack_->print_all_packets();
#endif
}

void LCCStackManager::shutdown()
{
  // Shutdown the auto-sync handler if it is running before unmounting the FS.
  if (configAutoSync_ != nullptr)
  {
    LOG(INFO, "[LCC] Disabling automatic fsync(%d) calls...", fd_);
    SyncNotifiable n;
    configAutoSync_->shutdown(&n);
    LOG(INFO, "[LCC] Waiting for sync to stop");
    n.wait_for_notification();
    configAutoSync_ = nullptr;
  }

  // shutdown the executor so that no more tasks will run
  LOG(INFO, "[LCC] Shutting down executor");
  stack_->executor()->shutdown();

  // close the config file if it is open
  if (fd_ >= 0)
  {
    LOG(INFO, "[LCC] Closing config file.");
    ::close(fd_);
  }
}

bool LCCStackManager::set_node_id(string node_id)
{
  LOG(VERBOSE, "[LCC] Current NodeID: %s, updated NodeID: %s"
    , uint64_to_string_hex(nodeID_).c_str(), node_id.c_str());
  uint64_t new_node_id = string_to_uint64(node_id);
  if (new_node_id != nodeID_)
  {
    auto cfg = Singleton<ConfigurationManager>::instance();
    string node_id_str = uint64_to_string_hex(new_node_id);
    LOG(INFO
      , "[LCC] Persisting NodeID: %s and enabling forced factory_reset."
      , node_id_str.c_str());
    cfg->store(LCC_NODE_ID_FILE, node_id_str);
    cfg->store(LCC_RESET_MARKER_FILE, node_id_str);
    // add callback to reboot the node
    stack()->executor()->add(new CallbackExecutable([](){ reboot(); }));
    return true;
  }
  return false;
}

bool LCCStackManager::reconfigure_can(bool enable)
{
#if defined(CONFIG_LCC_CAN_ENABLED)
  auto cfg = Singleton<ConfigurationManager>::instance();
  if (cfg->exists(LCC_CAN_MARKER_FILE) && !enable)
  {
    cfg->remove(LCC_CAN_MARKER_FILE);
    return true;
  }
  else if (!cfg->exists(LCC_CAN_MARKER_FILE) && enable)
  {
    string can_str = "true";
    cfg->store(LCC_CAN_MARKER_FILE, can_str);
    return true;
  }
#endif // CONFIG_LCC_CAN_ENABLED
  return false;
}

void LCCStackManager::factory_reset()
{
  auto cfg = Singleton<ConfigurationManager>::instance();
  string marker = uint64_to_string_hex(nodeID_);
  cfg->store(LCC_RESET_MARKER_FILE, marker);
}

std::string LCCStackManager::get_config_json()
{
  auto cfg = Singleton<ConfigurationManager>::instance();
  return StringPrintf("\"lcc\":{\"id\":\"%s\", \"can\":%s}"
                    , uint64_to_string_hex(nodeID_).c_str()
                    , cfg->exists(LCC_CAN_MARKER_FILE) ? "true" : "false");
}

} // namespace esp32cs