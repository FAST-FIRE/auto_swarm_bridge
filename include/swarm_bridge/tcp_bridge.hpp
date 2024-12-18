/*
 * @file tcp_bridge.hpp
 * @author Zhehan Li
 * @author Nanhe Chen (nanhe_chen@zju.edu.cn)
 * @brief Callback function wrapper for the bridge.
 * @copyright Copyright (c) 2023
 * All Rights Reserved
 * See LICENSE for the license information
 */
#ifndef _TCP_BRIDGE_HPP
#define _TCP_BRIDGE_HPP

#include <ros/ros.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <iostream>
#include <unistd.h>
#include <any>
#include <shared_mutex>

#include "reliable_bridge.hpp"
#include "callback_function.hpp"

#include <spdlog/spdlog.h>

class TCPBridge
{
public:
  typedef std::shared_ptr<TCPBridge> Ptr;
  TCPBridge(){
      // bridge_.reset(new ReliableBridge(self_id_, 100000));
      callback_list_.reset(new CallbackList());
  };
  TCPBridge(const TCPBridge &rhs) = delete;
  TCPBridge &operator=(const TCPBridge &rhs) = delete;
  ~TCPBridge()
  {
    spdlog::error("[SwarmBridge] [TCPBridge] stopped");
  };

  void setSelfID(int id)
  {
    std::unique_lock<std::shared_mutex> lock1(id_mutex_);
    std::unique_lock<std::shared_mutex> lock2(bridge_mutex_);

    if (self_id_ == id)
      return;
    self_id_ = id;

    // bridge_->StopALL();
    bridge_.reset(new ReliableBridge(self_id_, 100000));
  };

  template <typename T>
  void registerCallFunc(const std::string &topic_name, std::function<void(T)> func)
  {
    for (auto callback_name:callback_name_list_)
    {
      if (callback_name == topic_name)
      {
        spdlog::error("[SwarmBridge] [TCPBridge] register callback with same topic name ({})!", topic_name);
        return;
      }
    }
    callback_list_->inputWrapper(func);
    callback_name_list_.push_back(topic_name);
  }

  void updateIDIP(std::map<int32_t, std::string> map)
  {
    std::shared_lock<std::shared_mutex> lock1(id_mutex_);
    std::shared_lock<std::shared_mutex> lock2(map_mutex_);
    std::unique_lock<std::shared_mutex> lock3(bridge_mutex_);

    if (self_id_ == -1)
    {
      spdlog::warn("[SwarmBridge] [TCPBridge] self ID not set");
      return;
    }

    for (auto it : id_ip_map_)
    {
      auto iter = map.find(it.first);
      if (iter != map.end())
      {
        continue;
      }
      spdlog::warn("[SwarmBridge] [TCPBridge] delete ID IP map: {} {}", it.first, it.second);

      for (uint64_t i=0; i<callback_list_->size(); ++i)
        bridge_->register_callback(it.first,
                                   callback_name_list_[i], 
                                   [](int ID, ros::SerializedMessage &m){});

      bridge_->delete_bridge(it.first);
    }

    for (auto it : map)
    {
      auto iter = id_ip_map_.find(it.first);
      if (iter != id_ip_map_.end() && id_ip_map_[it.first] == it.second)
      {
        continue;
      }

      if (it.first == self_id_)
      {
        continue;
      }
      spdlog::warn("[SwarmBridge] [TCPBridge] update ID IP map: {} {}", it.first, it.second);
      bridge_->update_bridge(it.first, it.second);

      for (uint64_t i=0; i<callback_list_->size(); ++i)
      {
        bridge_->register_callback(it.first, callback_name_list_[i], 
            [this, i](int ID, ros::SerializedMessage &m)
            {callback_list_->getWrapper(i)->execute_other(ID, m);});
      }
    }

    id_ip_map_ = map;
  };
  
  template <typename T>
  int sendMsg(const std::string &topic_name, const T &msg, const int tgt_id = -1)
  {
    std::shared_lock<std::shared_mutex> lock1(map_mutex_);
    std::shared_lock<std::shared_mutex> lock2(id_mutex_);
    std::unique_lock<std::shared_mutex> lock3(bridge_mutex_);
    if (self_id_ <= -1)
    {
      // ROS_ERROR("[SwarmBridge] [TCPBridge] Invalid self ID %d", self_id_);
      return -1;
    }

    int err_code = 0;
    if (tgt_id >0)
    {
      err_code = bridge_->send_msg_to_one(tgt_id, topic_name, msg);
      if (err_code < 0)
      {
        spdlog::warn("[SwarmBridge] [TCPBridge] send error {} !!", typeid(T).name());
      }
      return err_code;
    }
    else if (tgt_id == -1)
    {
      for (auto it : id_ip_map_) // Only send to all devices.
      {
        if (it.first == self_id_) // skip myself
        {
          continue;
        }
        err_code = bridge_->send_msg_to_one(it.first, topic_name, msg);
        if (err_code < 0)
        {
          spdlog::warn("[SwarmBridge] [TCPBridge] send error {} !!", typeid(T).name());
        }
      }
      return err_code;
    }
    else
    {
      spdlog::warn("[SwarmBridge] [TCPBridge] illegal target id {}!!", tgt_id);
      return -1;
    }
  };

private:
  std::map<int, std::string> id_ip_map_; // id -> ip
  mutable std::shared_mutex map_mutex_;

  int self_id_ = -1;
  mutable std::shared_mutex id_mutex_;

  std::unique_ptr<ReliableBridge> bridge_;
  mutable std::shared_mutex bridge_mutex_;

  CallbackList::Ptr callback_list_;
  std::vector<std::string> callback_name_list_;

};

#endif