#include <ros/ros.h>
#include <swarm_bridge/swarm_bridge.hpp>

#include <nav_msgs/Odometry.h>
#include <visualization_msgs/Marker.h>

int main(int argc, char **argv)
{
    ros::init(argc, argv, "swarm_bridge_node");
    ros::NodeHandle nh("~");

    int id;
    nh.getParam("self_id", id);

    // initialize
    SwarmBridge::Ptr swarm_bridge;
    swarm_bridge.reset(new SwarmBridge(id));

    // add a subscriber, 
    // which callback function implemented by lambda a function.
    // topic name using default.
    ros::Publisher test_pub = nh.advertise<nav_msgs::Odometry>("test", 10);
    swarm_bridge->subscribe<nav_msgs::Odometry>([&](nav_msgs::Odometry msg){test_pub.publish(msg);});

    // add a subscriber with a customized topic name, 
    // it will only subscribe to publishers with same topic name.
    ros::Publisher test2_pub = nh.advertise<visualization_msgs::Marker>("test2", 10);
    swarm_bridge->subscribe<visualization_msgs::Marker>("vis", [&](visualization_msgs::Marker msg){test2_pub.publish(msg);});

    ros::Rate r(100);
    while (ros::ok())
    {
        // publish data using the default topic name.
        nav_msgs::Odometry msg;
        swarm_bridge->publish<nav_msgs::Odometry>(msg);

        // publish data using customized topic names.
        visualization_msgs::Marker msg2;
        swarm_bridge->publish<visualization_msgs::Marker>("vis", msg2);
    
        r.sleep();
        ros::spinOnce();
    }

    ros::spin();
    return 0;
}
