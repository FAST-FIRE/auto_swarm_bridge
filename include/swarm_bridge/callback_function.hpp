#ifndef _CALLBACK_FUNCTION_WRAPPER_HPP
#define _CALLBACK_FUNCTION_WRAPPER_HPP

#include <iostream>
#include <vector>
#include <functional>
#include <any>
#include <memory>

#include <ros/ros.h>

class BaseWrapper
{
public:
  typedef std::shared_ptr<BaseWrapper> Ptr;
  // virtual void execute() = 0;
  virtual void execute_other(int ID, ros::SerializedMessage &m) = 0;
};

template <typename T>
class CallbackFunctionWrapper : public BaseWrapper
{
public:
  typedef std::shared_ptr<CallbackFunctionWrapper> Ptr;
  CallbackFunctionWrapper() = delete;
  CallbackFunctionWrapper(std::function<void(T)> &func, T &arg) : func_(func), arg_(arg){};
  CallbackFunctionWrapper(std::function<void(T)> &func) : func_(func){};
  ~CallbackFunctionWrapper(){};

public:
  // void execute() override
  // {
  //     func_(arg_);
  // }
  void execute_other(int ID, ros::SerializedMessage &m) override
  {
    T msg;
    ros::serialization::deserializeMessage(m, msg);
    if (func_)
    {
      func_(msg);
    }
  }

public:
  std::function<void(T)> func_;
  T arg_;
};

class CallbackList
{
public:
  typedef std::shared_ptr<CallbackList> Ptr;

public:
  CallbackList() { wrappers.clear(); };
  ~CallbackList(){};

public:
  template <typename T>
  void inputWrapper(std::function<void(T)> &func, T &arg)
  {
    std::shared_ptr<CallbackFunctionWrapper<T>> wrapper;
    wrapper.reset(new CallbackFunctionWrapper<T>(func, arg));
    wrappers.push_back(wrapper);
  }

  template <typename T>
  void inputWrapper(std::function<void(T)> &func)
  {
    std::shared_ptr<CallbackFunctionWrapper<T>> wrapper;
    wrapper.reset(new CallbackFunctionWrapper<T>(func));
    wrappers.push_back(wrapper);
  }

  auto getWrapper(const uint64_t &idx)
  {
    return wrappers[idx];
  }

  auto size() { return wrappers.size(); }

private:
  std::vector<BaseWrapper::Ptr> wrappers;
};

#endif