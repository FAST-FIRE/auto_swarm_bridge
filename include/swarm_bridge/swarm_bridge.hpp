#ifndef _SWARM_BRIDGE_HPP
#define _SWARM_BRIDGE_HPP

#include <ros/ros.h>
#include <ros/callback_queue.h>

#include "swarm_bridge/udp_bridge.hpp"
#include "swarm_bridge/tcp_bridge.hpp"

#include <string>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <chrono>

#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

typedef float float32_t;
typedef double float64_t;

class SwarmBridge
{
public:
  SwarmBridge(int id);
  SwarmBridge(const SwarmBridge &rhs) = delete;
  SwarmBridge &operator=(const SwarmBridge &rhs) = delete;
  ~SwarmBridge();

  enum class State
  {
    Init,
    Running,
    Stop
  };

  typedef std::shared_ptr<SwarmBridge> Ptr;

  template <typename T>
  void publish(const T &msg);

  template <typename T>
  void publish(const std::string &topic_name, const T &msg);

  template <typename T>
  void subscribe(std::function<void(T)> func);

  template <typename T>
  void subscribe(const std::string &topic_name, std::function<void(T)> func);

  State getState() const;
  std::map<int32_t, std::string> getIDIP() const;

private:
  UDPBridge::Ptr udp_bridge_;
  TCPBridge::Ptr tcp_bridge_;

  int self_id_;

  std::thread swarm_bridge_thread_;
  void swarmBridgeThread();
};

SwarmBridge::SwarmBridge(int id) : self_id_(id)
{
  udp_bridge_.reset(new UDPBridge());
  tcp_bridge_.reset(new TCPBridge());

  udp_bridge_->setSelfID(self_id_);
  tcp_bridge_->setSelfID(self_id_);

  {
    ros::NodeHandle nh("~");
    std::string net_mode = "auto";
    double udp_timeout = 10;
    std::string self_ip = "127.0.0.1";
    std::string broadcast_ip = "127.0.0.255";

    if (!(nh.getParam("network/net_mode", net_mode) &&
          nh.getParam("network/udp_timeout", udp_timeout) &&
          nh.getParam("network/self_ip", self_ip) &&
          nh.getParam("network/broadcast_ip", broadcast_ip)))
    {
      ROS_FATAL("[SwarmBridge] get param failed");
      ros::shutdown();
    }

    udp_bridge_->setNetMode(net_mode, self_ip, broadcast_ip);
    udp_bridge_->setTimeOut(udp_timeout);
  }

  swarm_bridge_thread_ = std::thread([this]
                                     { swarmBridgeThread(); });
}

SwarmBridge::~SwarmBridge()
{
  printf("[SwarmBridge] stopping\n");
  if (swarm_bridge_thread_.joinable())
  {
    swarm_bridge_thread_.join();
  }
  printf("[SwarmBridge] stopped\n");
}

template <typename T>
void SwarmBridge::publish(const T &msg)
{
  this->publish(typeid(msg).name(), msg);
}

template <typename T>
void SwarmBridge::publish(const std::string &topic_name, const T &msg)
{
  tcp_bridge_->sendMsg(topic_name, msg);
}

template <typename T>
void SwarmBridge::subscribe(std::function<void(T)> func)
{
  tcp_bridge_->registerCallFunc(typeid(T).name(), func);
}

template <typename T>
void SwarmBridge::subscribe(const std::string &topic_name, std::function<void(T)> func)
{
  tcp_bridge_->registerCallFunc(topic_name, func);
}

std::map<int32_t, std::string> SwarmBridge::getIDIP() const
{
  return udp_bridge_->getIDIP();
}

void SwarmBridge::swarmBridgeThread()
{
  // spinner_.start();

  ros::Rate rate(10);
  while (ros::ok())
  {
    tcp_bridge_->updateIDIP(udp_bridge_->getIDIP());
    rate.sleep();
  }
}

#endif