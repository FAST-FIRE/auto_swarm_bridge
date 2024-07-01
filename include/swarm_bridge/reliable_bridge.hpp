/**
 * @file reliable_bridge.hpp
 * @author Haojia Li (wisderek@stumail.neu.edu.cn)
 * @author Zhehan Li
 * @brief Reliable bridge for ros data transfer in unstable network.
 * It will estiblish the connections as peer to peer mode.
 * It will reconnect each other autonomously.
 * It can guarantee the data transfer to the device in the strict order.
 * It has a queue for sending data asynchronously.
 *
 * Note: This program relies on ZMQ amd ZMQPP.
 * sudo apt install libzmqpp-dev
 *
 * Core Idea: It would create the sending and receving thread for each devices and processing asynchronously.
 * The index number would correspond to the resource for one device.
 *
 * @version 0.2
 * @date 2023-07-04
 *
 * @copyright Copyright (c) 2021
 *
 */
#ifndef __RELIABLE_BRIDGE__
#define __RELIABLE_BRIDGE__
#include "zmq.hpp"
#include "stdio.h"
#include "stdlib.h"
#include "time.h"
#include "zmqpp/zmqpp.hpp"
#include "deque"
#include "map"
#include "ros/ros.h"
#include "thread"
#include "atomic"
#include "mutex"
#include "condition_variable"
#include <boost/shared_array.hpp>

#include "callback_function.hpp"

namespace ser = ros::serialization;

/**
 * @brief overload operator<< for add SerializedMessage to zmqpp::message
 */
inline zmqpp::message &operator<<(zmqpp::message &in, ros::SerializedMessage const &msg)
{
  in.add_raw(reinterpret_cast<void const *>(msg.buf.get()), msg.num_bytes);
  return in;
}

class ReliableBridge
{

private:
  /**
   * @brief sender thread, send data in queue asynchronously.
   *
   * @param index ：thread index number
   */
  void send_thread(int ind)
  {
    while (thread_flag[ind] && ros::ok())
    {
      std::unique_lock<std::mutex> locker(*sender_mutex[ind]);
      cond[ind]->wait(locker);
      auto &buffer = send_data[ind];
      locker.unlock(); // release in time

      while (!buffer->empty() && thread_flag[ind] && ros::ok())
      {
        auto &data = buffer->front();
        zmqpp::message send_array;

        send_array << data.first << data.second.num_bytes << data.second; //  add data
        if (senders[ind]->send(send_array, false))                        // block here until timeout, wait for sending
        {
          std::unique_lock<std::mutex> locker2(*sender_mutex[ind]);
          buffer->pop_front(); // delete the first data
          locker2.unlock();
        }
      }
      usleep(1);
    }
  }
  /**
   * @brief receiver thread
   * Receive data and call the callback function asynchronously.
   *
   * @param index ：thread index number
   */
  void recv_thread(int ind)
  {
    while (thread_flag[ind] && ros::ok())
    {
      zmqpp::message recv_array;

      if (receivers[ind]->receive(recv_array, false))
      {
        std::string topic_name;
        size_t data_len;

        // unpack meta data
        recv_array >> topic_name >> data_len;

        // unpack ros messages
        ros::SerializedMessage msg_ser;
        msg_ser.buf.reset(new uint8_t[data_len]);
        memcpy(msg_ser.buf.get(), static_cast<const uint8_t *>(recv_array.raw_data(recv_array.read_cursor())), data_len);
        recv_array.next(); // move read_cursor for next part.
        msg_ser.num_bytes = data_len;
        msg_ser.message_start = msg_ser.buf.get() + 4;

        auto &topic_cb = callback_list[ind];
        const auto &iter = topic_cb.find(topic_name); // find topic
        if (iter != topic_cb.end())
        {
          iter->second(ind, msg_ser); // go into callback;
        }
      }
      usleep(1);
    }
  }

  std::map<int, bool> thread_flag;
  zmqpp::context_t context;
  //  for communicating with each device,each device is allocted
  //  a sender, a receiver, a send_data queue, for data transfer
  //  a sender_mutex,a condition_variable for notificating the sender
  //  a send_thread,a recv_thread
  //  a callback map for finding the call back function

  std::map<int, std::unique_ptr<zmqpp::socket>> senders;   // index senders
  std::map<int, std::unique_ptr<zmqpp::socket>> receivers; // index receivers

  // data queue
  std::map<int, std::unique_ptr<std::deque<std::pair<std::string, ros::SerializedMessage>>>> send_data;

  // mutiple threads
  std::map<int, std::unique_ptr<std::mutex>> sender_mutex;
  std::map<int, std::unique_ptr<std::condition_variable>> cond;
  std::map<int, std::unique_ptr<std::thread>> send_threads;
  std::map<int, std::unique_ptr<std::thread>> recv_threads;

  // callback table   index->topic_name-> callback
  std::map<int, std::map<std::string, std::function<void(int, ros::SerializedMessage &)>>> callback_list;

  const int self_id; // self id
  const uint queue_size;

  void delete_bridge_thread(int id)
  {
    ROS_WARN("[SwarmBridge] [TCPBridge] [ReliableBridge] delete bridge %d", id);

    if (!check_id(id))
    {
      return;
    }

    // detect if the id is in the map
    if (id_ip_map.find(id) != id_ip_map.end())
    {
      std::unique_lock<std::mutex> locker(*sender_mutex[id]);
      locker.unlock();
      cond[id]->notify_all();

      thread_flag[id] = false;

      if (send_threads[id]->joinable())
      {
        send_threads[id]->join();
        send_threads.erase(id);
      }
      senders[id]->close();
      senders.erase(id);

      if (recv_threads[id]->joinable())
      {
        recv_threads[id]->join();
        recv_threads.erase(id);
      }
      receivers[id]->close();
      receivers.erase(id);

      id_ip_map.erase(id);
      callback_list.erase(id);

      send_data.erase(id);
      sender_mutex.erase(id);
      cond.erase(id);
    }

    ROS_WARN("[SwarmBridge] [TCPBridge] [ReliableBridge] delete bridge %d finished", id);
  }

public:
  std::map<int, std::string> id_ip_map; // id -> ip

  /**
   * @brief Construct a new Reliable Bridge object
   *
   * @param self_ID self ID (0-99)
   * @param IP_list other device IP
   * @param ID_list other device ID numbers (0-99)
   * @param Queue_size max send message queue for one device
   */
  ReliableBridge(const int self_ID, uint Queue_size = 10000) : self_id(self_ID), queue_size(Queue_size)
  {
  }
  ~ReliableBridge()
  {
    ROS_ERROR("[SwarmBridge] [TCPBridge] [ReliableBridge] stopping");
    StopALL();
    ROS_ERROR("[SwarmBridge] [TCPBridge] [ReliableBridge] stopped");
  }

  void StopALL()
  {
    std::vector<int> id_list;
    for (auto it : id_ip_map)
    {
      id_list.push_back(it.first);
    }
    for (auto id : id_list)
    {
      delete_bridge_thread(id);
    }
  }

  bool check_id(int id)
  {
    if (id_ip_map.size() > 100)
    {
      ROS_WARN("[SwarmBridge] [TCPBridge] [ReliableBridge] bridge only supports up to 100 devices!");
      assert(id_ip_map.size() <= 100);
      return false;
    }
    if (id > 100 || id < 0 || self_id > 100 || self_id < 0)
    {
      ROS_WARN("[SwarmBridge] [TCPBridge] [ReliableBridge] ID is invalid!");
      assert(id <= 100 && id > 0 && self_id <= 100 && self_id > 0);
      return false;
    }
    if (id == self_id) // remove self ID
    {
      ROS_WARN("[SwarmBridge] [TCPBridge] [ReliableBridge] ID is self ID!");
      assert(id == self_id);
      return false;
    }
    return true;
  }

  void delete_bridge(int id)
  {
    std::thread temp(&ReliableBridge::delete_bridge_thread, this, id);
    temp.detach();
  }

  void update_bridge(int id, std::string ip)
  {
    if (!check_id(id))
    {
      return;
    }

    if (id_ip_map.find(id) != id_ip_map.end())
    {
      if (id_ip_map[id] == ip)
      {
        return;
      }
      else
      {
        delete_bridge(id);
      }
    }

    int timeout = 1000;
    // The bind port number is 40000+self_id*100+other_device_id.
    std::string url = "tcp://*:" + std::to_string(40000 + self_id * 100 + id);
    ROS_WARN("[SwarmBridge] [TCPBridge] [ReliableBridge] bind(send): %s", url.c_str());
    std::unique_ptr<zmqpp::socket> sender(new zmqpp::socket(context, zmqpp::socket_type::push));
    sender->set(zmqpp::socket_option::send_timeout, timeout);
    sender->bind(url); // for others connection

    // The port number is 40000+other_device_id*100+self_ID. It will connect to other device.
    url = "tcp://" + ip + ":" + std::to_string(40000 + id * 100 + self_id);
    ROS_WARN("[SwarmBridge] [TCPBridge] [ReliableBridge] connect(receive): %s", url.c_str());
    std::unique_ptr<zmqpp::socket> receiver(new zmqpp::socket(context, zmqpp::socket_type::pull));
    receiver->set(zmqpp::socket_option::receive_timeout, timeout);
    receiver->connect(url); // connect to others

    thread_flag[id] = true;

    // Create sending and recving thread for every device
    id_ip_map[id] = ip;
    callback_list.emplace(id, std::map<std::string, std::function<void(int, ros::SerializedMessage &)>>());

    senders.emplace(id, std::move(sender));
    receivers.emplace(id, std::move(receiver));
    send_data.emplace(id, new std::deque<std::pair<std::string, ros::SerializedMessage>>());
    sender_mutex.emplace(id, new std::mutex());      // locker
    cond.emplace(id, new std::condition_variable()); // Notificate the sender to send data immediately.
    send_threads.emplace(id, new std::thread(&ReliableBridge::send_thread, this, id));
    recv_threads.emplace(id, new std::thread(&ReliableBridge::recv_thread, this, id));
  }

  /**
   * @brief register a callback for receiving data from a specific device.
   * For processing data, you should register a callback at the begining.
   * Cation: The callback will be used in other threads, so ensure the callback can be reentrant!!
   * Please use local variables.
   * When you want to write into global variables, it is better to use locker to protect them.
   * @param ID :receive data from specific device (MUST IN ID LIST)
   * @param topic_name :specific topic
   * @param callback :callback function, like `void callback_sample(int ID, ros::SerializedMessage& m)`
   * @example
   * void callback_sample(int ID, ros::SerializedMessage& m)
   *      {
   *          geometry_msgs::Point msg;
   *          ros::serialization::deserializeMessage(m,msg);
   *          //ID is which device send this message.
   *          //msg is the ros message.
   *      }
   */
  void register_callback(int ind, std::string topic_name, std::function<void(int, ros::SerializedMessage &)> callback)
  {
    callback_list[ind][topic_name] = callback;
  }

  /**
   * @brief register a callback for receiving data from all devices.
   * For processing data, you should register a callback at the begining.
   * Cation: The callback will be used in other threads, so ensure the callback can be reentrant!!
   * Please use local variables.
   * When you want to write into global variables, it is better to use locker to protect them.
   * @param topic_name :specific topic
   * @param callback :callback function, like `void callback_sample(int ID, ros::SerializedMessage& m)`
   * @example
   * void callback_sample(int ID, ros::SerializedMessage& m)
   *      {
   *          geometry_msgs::Point msg;
   *          ros::serialization::deserializeMessage(m,msg);
   *          //ID is which device send this message.
   *          //msg is the ros message.
   *      }
   */
  void register_callback_for_all(std::string topic_name, std::function<void(int, ros::SerializedMessage &)> callback)
  {
    for (auto it : id_ip_map)
    {
      register_callback(it.first, topic_name, callback);
    }
  }

  /**
   * @brief send message to a specific device asynchronously
   * This function will push the data in queue and the send thread will send the data immediately.
   * @tparam T <message type>
   * @param ID :Which device do you want to send (MUST IN ID LIST).
   * @param topic_name :topic name
   * @param msg :ros message you want to send
   * @return int :0 for no error, -1 for queue is full
   * @example
   * std_msgs::String text;
   * send_msg_to_one(1,"hello",text);
   */
  template <typename T>
  int send_msg_to_one(int ind, std::string topic_name, T &msg)
  {
    auto iter = id_ip_map.find(ind);
    if (iter == id_ip_map.end())
    {
      ROS_WARN("[SwarmBridge] [TCPBridge] [ReliableBridge] ID:%d is not in the list", ind);
      return -1;
    }

    auto &buffer = send_data[ind];
    if (buffer->size() > queue_size)
    {
      ROS_WARN("[SwarmBridge] [TCPBridge] [ReliableBridge] ID:%d Send buffer is full", ind);
      return -1; // buffer is full
    }
    {
      std::unique_lock<std::mutex> locker(*sender_mutex[ind]);
      buffer->emplace_back(make_pair(topic_name, ser::serializeMessage<T>(msg)));
      locker.unlock();
      cond[ind]->notify_all();
    }
    return 0;
  }
  /**
   * @brief send message to a all device asynchronously
   * This function will push the data in queue and the send thread will send the data immediately.
   * @tparam T <message type>
   * @param topic_name :topic name
   * @param msg :ros message you want to send
   * @return int :0 for no error, <0 for queue is full
   * @example
   * std_msgs::String text;
   * send_msg_to_all(1,"hello",text);
   */
  template <typename T>
  int send_msg_to_all(std::string topic_name, T &msg)
  {
    int err_code = 0;
    for (auto it : id_ip_map)
    {
      err_code += send_msg_to_one<T>(it.first, topic_name, msg);
    }
    return err_code;
  }
};

#endif
