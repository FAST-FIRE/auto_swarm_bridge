#ifndef UDP_BRIDGE_HPP
#define UDP_BRIDGE_HPP

#include <thread>
#include <map>
#include <mutex>
#include <shared_mutex>

#include <ros/ros.h>

#include <swarm_bridge/idip.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#include <ifaddrs.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <linux/if_link.h>

class UDPBridge
{
public:
  typedef std::shared_ptr<UDPBridge> Ptr;
  UDPBridge(){};
  UDPBridge(const UDPBridge &rhs) = delete;
  UDPBridge &operator=(const UDPBridge &rhs) = delete;
  ~UDPBridge()
  {
    ROS_ERROR("[SwarmBridge] [UDPBridge] stopping");
    stop_flag_ = true;

    if (udp_recv_thread_.joinable())
      udp_recv_thread_.join();

    if (udp_send_thread_.joinable())
      udp_send_thread_.join();
    ROS_ERROR("[SwarmBridge] [UDPBridge] stopped");
  };

  void setSelfID(int id)
  {
    std::unique_lock<std::shared_mutex> lock(self_mutex_);
    self_id_ = id;

    udp_recv_thread_ = std::thread([this]
                                   { udp_recv_fun(); });
    udp_send_thread_ = std::thread([this]
                                   { udp_send_fun(); });
  };

  void setNetMode(const std::string &net_mode,
                  const std::string &self_ip = "127.0.0.1",
                  const std::string &broadcast_ip = "127.0.0.255")
  {
    net_mode_ = net_mode;
    self_ip_ = self_ip;
    broadcast_ip_ = broadcast_ip;

    // if (net_mode_ != "manual")
    // {
    //   return;
    // }

    // std::unique_lock<std::shared_mutex> lock(map_mutex_);
    // for (uint64_t i = 0; i < id_list.size(); i++)
    // {
    //   id_ip_map_[id_list[i]] = ip_list.at(i);
    //   id_time_map_[id_list[i]] = ros::Time::now().toSec();
    // }
  };

  void setTimeOut(const double &timeout)
  {
    time_out_ = timeout;
  };

  std::map<int32_t, std::string> getIDIP() const
  {
    std::shared_lock<std::shared_mutex> lock(map_mutex_);
    std::map<int32_t, std::string> map;
    for (auto it : id_ip_map_)
    {
      map[it.first] = it.second;
    }
    return map;
  };

private:
  std::map<int32_t, std::string> id_ip_map_;
  std::map<int32_t, double> id_time_map_;
  mutable std::shared_mutex map_mutex_;

  std::thread udp_recv_thread_;
  std::thread udp_send_thread_;

  bool stop_flag_ = false;

  std::string net_mode_ = "unset";
  double time_out_ = 1e6;

#define UDP_PORT 8858
#define BUF_LEN 1048576    // 1MB
#define BUF_LEN_SHORT 1024 // 1KB

  int self_id_ = -1;
  mutable std::shared_mutex self_mutex_;
  std::string self_ip_;
  std::string broadcast_ip_;

  int udp_server_fd_, udp_send_fd_;
  char udp_recv_buf_[BUF_LEN], udp_send_buf_[BUF_LEN];
  struct sockaddr_in addr_udp_send_;

  // recv msg
  swarm_bridge::idip udp_recv_idip_msg_;

  enum MESSAGE_TYPE
  {
    IDIP = 82
  } massage_type_;

  int init_broadcast(const char *ip, const int port)
  {
    int fd;

    if ((fd = socket(AF_INET, SOCK_DGRAM, 0)) <= 0)
    {
      perror("socket sender creation error!");
      exit(EXIT_FAILURE);
    }

    int so_broadcast = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &so_broadcast, sizeof(so_broadcast)) < 0)
    {
      perror("error in setting Broadcast option");
      exit(EXIT_FAILURE);
    }

    addr_udp_send_.sin_family = AF_INET;
    addr_udp_send_.sin_port = htons(port);

    if (inet_pton(AF_INET, ip, &addr_udp_send_.sin_addr) <= 0)
    {
      perror("invalid address/ Address not supported");
      exit(EXIT_FAILURE);
    }

    return fd;
  }

  int udp_bind_to_port(const int port, int &server_fd)
  {
    struct sockaddr_in address;
    int opt = 1;

    // Creating socket file descriptor
    if ((server_fd = socket(AF_INET, SOCK_DGRAM, 0)) == 0)
    {
      perror("socket failed");
      exit(EXIT_FAILURE);
    }

    // Set timeout
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    if (setsockopt(server_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)
    {
      perror("setsockopt");
      exit(EXIT_FAILURE);
    }

    // Forcefully attaching socket to the port
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT,
                   &opt, sizeof(opt)))
    {
      perror("setsockopt");
      exit(EXIT_FAILURE);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    // Forcefully attaching socket to the port
    if (bind(server_fd, (struct sockaddr *)&address,
             sizeof(address)) < 0)
    {
      perror("bind failed");
      exit(EXIT_FAILURE);
    }

    return server_fd;
  }

  template <typename T>
  int serializeTopic(const MESSAGE_TYPE msg_type, const T &msg)
  {
    // message stored order is type, size, msg (xun)
    auto ptr = (uint8_t *)(udp_send_buf_);

    *((int *)ptr) = self_id_;
    ptr += sizeof(int);
    *((MESSAGE_TYPE *)ptr) = msg_type;
    ptr += sizeof(MESSAGE_TYPE);

    namespace ser = ros::serialization;
    uint32_t msg_size = ser::serializationLength(msg);

    *((uint32_t *)ptr) = msg_size;
    ptr += sizeof(uint32_t);

    ser::OStream stream(ptr, msg_size);
    ser::serialize(stream, msg);

    return msg_size + sizeof(MESSAGE_TYPE) + sizeof(uint32_t) + sizeof(int);
  }

  template <typename T>
  int deserializeTopic(T &msg)
  {
    auto ptr = (uint8_t *)(udp_recv_buf_ + sizeof(MESSAGE_TYPE) + sizeof(int));

    uint32_t msg_size = *((uint32_t *)ptr);
    ptr += sizeof(uint32_t);

    namespace ser = ros::serialization;
    ser::IStream stream(ptr, msg_size);
    ser::deserialize(stream, msg);

    return msg_size + sizeof(MESSAGE_TYPE) + sizeof(uint32_t) + sizeof(int);
  }

  void idip_udp_callback(const std::pair<int32_t, std::string> &idip_pair)
  {
    swarm_bridge::idip msg;
    msg.id = idip_pair.first;
    sscanf((idip_pair.second).c_str(), "%hhu.%hhu.%hhu.%hhu", &(msg.ip0), &(msg.ip1), &(msg.ip2), &(msg.ip3));
    int len = serializeTopic(MESSAGE_TYPE::IDIP, msg);
    if (sendto(udp_send_fd_, udp_send_buf_, len, 0, (struct sockaddr *)&addr_udp_send_, sizeof(addr_udp_send_)) <= 0)
    {
      ROS_WARN("[SwarmBridge] [UDPBridge] udp send error");
    }
  }

  void udp_recv_fun()
  {
    int valread;
    struct sockaddr_in addr_client;
    socklen_t addr_len;

    // Connect
    if (udp_bind_to_port(UDP_PORT, udp_server_fd_) < 0)
    {
      perror("socket recever creation error!");
      exit(EXIT_FAILURE);
    }

    ros::Rate rate(10);
    while (ros::ok())
    {
      if (stop_flag_)
        break;
      rate.sleep();

      if ((valread = recvfrom(udp_server_fd_, udp_recv_buf_, BUF_LEN, 0, (struct sockaddr *)&addr_client, (socklen_t *)&addr_len)) < 0)
      {
        // perror("recvfrom() < 0, error:");
        // exit(EXIT_FAILURE);
        continue;
      }

      char *ptr = udp_recv_buf_;
      ptr += sizeof(int);
      switch (*((MESSAGE_TYPE *)ptr))
      {
      case MESSAGE_TYPE::IDIP:
      {
        if (valread == deserializeTopic(udp_recv_idip_msg_))
        {
          std::unique_lock<std::shared_mutex> lock(map_mutex_);
          id_ip_map_[udp_recv_idip_msg_.id] = std::to_string((int32_t)(udp_recv_idip_msg_.ip0)) + "." + std::to_string((int32_t)(udp_recv_idip_msg_.ip1)) + "." + std::to_string((int32_t)(udp_recv_idip_msg_.ip2)) + "." + std::to_string((int32_t)(udp_recv_idip_msg_.ip3));
          id_time_map_[udp_recv_idip_msg_.id] = ros::Time::now().toSec();
        }
        else
        {
          ROS_WARN("[SwarmBridge] [UDPBridge] received message length not matches the sent one !!!");
          continue;
        }
        break;
      }
      default:
      {
        ROS_WARN("[SwarmBridge] [UDPBridge] unknown received message type ???");
        break;
      }
      }
    }
  }

  void get_self_ip()
  {
    struct ifaddrs *ifap, *ifa;
    if (getifaddrs(&ifap) == -1)
    {
      std::cerr << "Error getting interface addresses." << std::endl;
      return;
    }

    for (ifa = ifap; ifa != nullptr; ifa = ifa->ifa_next)
    {
      if (ifa->ifa_addr == nullptr)
      {
        continue;
      }

      if (ifa->ifa_addr->sa_family == AF_INET)
      {
        // get self ip and broadcast ip
        struct sockaddr_in *pAddr = (struct sockaddr_in *)ifa->ifa_addr;
        struct sockaddr_in *pBroadAddr = (struct sockaddr_in *)ifa->ifa_broadaddr;

        if (std::string(inet_ntoa(pAddr->sin_addr)) == std::string("127.0.0.1"))
        {
          continue;
        }

        if (net_mode_ == "manual")
          break;

        self_ip_ = inet_ntoa(pAddr->sin_addr);
        broadcast_ip_ = inet_ntoa(pBroadAddr->sin_addr);
        break;
      }
    }

    freeifaddrs(ifap);
  }

  void udp_send_fun()
  {
    // if (net_mode_ != "auto")
    // {
    //   return;
    // }

    get_self_ip();
    udp_send_fd_ = init_broadcast(broadcast_ip_.c_str(), UDP_PORT);

    ros::Rate rate(1);
    while (ros::ok())
    {
      if (stop_flag_)
        break;
      rate.sleep();

      {
        std::shared_lock<std::shared_mutex> lock(self_mutex_);
        if (self_id_ == -1)
        {
          continue;
        }
      }

      std::pair<int32_t, std::string> idip_pair;
      {
        std::shared_lock<std::shared_mutex> lock1(self_mutex_);
        idip_pair.first = self_id_;
        idip_pair.second = self_ip_;
        std::shared_lock<std::shared_mutex> lock2(map_mutex_);
        id_ip_map_[self_id_] = self_ip_;
        id_time_map_[self_id_] = ros::Time::now().toSec();
      }
      idip_udp_callback(idip_pair);

      std::unique_lock<std::shared_mutex> lock(map_mutex_);
      double now_time = ros::Time::now().toSec();
      std::vector<int> old_ids;
      for (auto it : id_time_map_)
      {
        if (now_time - it.second > time_out_)
        {
          old_ids.push_back(it.first);
        }
      }

      for (auto id : old_ids)
      {
        ROS_WARN("[SwarmBridge] [UDPBridge] delete old id: %d", id);
        id_ip_map_.erase(id);
        id_time_map_.erase(id);
      }
    }

    close(udp_server_fd_);
    close(udp_send_fd_);

    return;
  }
};

#endif