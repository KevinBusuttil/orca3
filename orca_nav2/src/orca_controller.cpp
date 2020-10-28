// MIT License
//
// Copyright (c) 2020 Clyde McQueen
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

// Inspired by
// https://navigation.ros.org/plugin_tutorials/docs/writing_new_nav2controller_plugin.html

#include <algorithm>
#include <string>
#include <vector>

#include "orca_nav2/param_macro.hpp"
#include "orca_nav2/util.hpp"
#include "orca_shared/mw/pose_stamped.hpp"
#include "orca_shared/util.hpp"
#include "nav2_core/controller.hpp"
#include "nav2_core/exceptions.hpp"
#include "rclcpp/rclcpp.hpp"
#include "pluginlib/class_loader.hpp"

namespace orca_nav2 {

constexpr bool sign(const double & v) { return v > 0; }

class Limiter
{
  double max_a_{};
  double max_dv_{};

public:
  Limiter() = default;

  Limiter(const double & max_a, const double & dt)
  : max_a_{max_a}, max_dv_{max_a * dt}
  {
    assert(max_a > 0);
    assert(dt > 0);
  }

  // Ramp down velocity as the sub approaches the goal
  // Particularly important for linear.x, as momentum will carry the sub a good distance
  // Less important for linear.z, as drag is higher and buoyancy tends to dominate
  void decelerate(double & v, const double & goal_dist) const
  {
    assert(sign(v) == sign(goal_dist));
    auto decel_v = std::sqrt(2 * std::abs(goal_dist) * max_a_);
    auto result_v = std::min(std::abs(v), decel_v);
    v = (sign(v) ? result_v : -result_v);
  }

  // Limit acceleration
  void limit(double & v, const double & prev_v) const
  {
    auto dv = v - prev_v;
    if (dv > max_dv_) {
      v = prev_v + max_dv_;
    } else if (dv < -max_dv_) {
      v = prev_v - max_dv_;
    }
  }
};

class OrcaController: public nav2_core::Controller
{
  rclcpp::Logger logger_{rclcpp::get_logger("placeholder_will_be_set_in_configure")};
  std::shared_ptr<tf2_ros::Buffer> tf_;
  std::string base_frame_id_;

  // Parameters
  double xy_vel_{};
  double xy_accel_{};
  double z_vel_{};
  double z_accel_{};
  double yaw_vel_{};
  double yaw_accel_{};
  double lookahead_dist_{};
  double transform_tolerance_{};
  double move_threshold_{};       // Stop motion when we're very close to the goal
  double tick_rate_{};            // Tick rate, used to compute dt

  Limiter xy_limiter_;
  Limiter z_limiter_;
  Limiter yaw_limiter_;

  rclcpp::Duration transform_tolerance_d_{0, 0};

  // Plan from OrcaPlanner
  nav_msgs::msg::Path plan_;

  // Keep track of the previous cmd_vel to limit acceleration
  geometry_msgs::msg::Twist prev_vel_{};

  // Return the first pose in the plan > lookahead distance away, or the last pose in the plan
  geometry_msgs::msg::PoseStamped
  find_goal(const geometry_msgs::msg::PoseStamped & pose_f_map) const
  {
    // Walk the plan calculating distance. The plan may be stale, so distances may be be
    // decreasing for a while. When they start to increase we've found the closest pose. Then look
    // for the first pose > lookahead_dist_. Return the last pose if we run out of poses.
    auto min_dist = std::numeric_limits<double>::max();
    bool dist_decreasing = true;

    for (const auto & item : plan_.poses) {
      auto item_dist = dist(
        item.pose.position.x - pose_f_map.pose.position.x,
        item.pose.position.y - pose_f_map.pose.position.y,
        item.pose.position.z - pose_f_map.pose.position.z);

      if (dist_decreasing) {
        if (item_dist < min_dist) {
          min_dist = item_dist;
        } else {
          dist_decreasing = false;
        }
      }

      if (!dist_decreasing) {
        if (item_dist > lookahead_dist_) {
          return item;
        }
      }
    }

    return plan_.poses[plan_.poses.size() - 1];
  }

  // Modified pure pursuit path tracking algorithm: works in 3D and supports deceleration
  // Reference "Implementation of the Pure Pursuit Path Tracking Algorithm" by R. Craig Coulter
  geometry_msgs::msg::Twist
  pure_pursuit_3d(const geometry_msgs::msg::PoseStamped & pose_f_odom) const
  {
    // Transform pose odom -> map
    geometry_msgs::msg::PoseStamped pose_f_map;
    if (!orca::transform_with_tolerance(logger_, tf_, plan_.header.frame_id,
      pose_f_odom, pose_f_map,
      transform_tolerance_d_)) {
      return geometry_msgs::msg::Twist{};
    }

    // Find goal
    auto goal_f_map = find_goal(pose_f_map);

    // Plan poses are stale, update the timestamp to get recent map -> odom -> base transforms
    goal_f_map.header.stamp = pose_f_map.header.stamp;

    // Transform goal map -> base
    geometry_msgs::msg::PoseStamped goal_f_base;
    if (!orca::transform_with_tolerance(logger_, tf_, base_frame_id_,
      goal_f_map, goal_f_base,
      transform_tolerance_d_)) {
      return geometry_msgs::msg::Twist{};
    }

    auto xy_dist_sq = dist_sq(goal_f_base.pose.position.x, goal_f_base.pose.position.y);
    auto xy_dist = std::sqrt(xy_dist_sq);
    auto z_dist = std::abs(goal_f_base.pose.position.z);

#if 0
    // Useful for debugging, but happens frequently as the sub decelerates
    if (z_dist < move_threshold_ && xy_dist < move_threshold_) {
      std::cout << "Decelerating / coasting" << std::endl;
      std::cout << "pose_f_odom: " << mw::PoseStamped(pose_f_odom) << std::endl;
      std::cout << "pose_f_map: " << mw::PoseStamped(pose_f_map) << std::endl;
      std::cout << "goal_f_map: " << mw::PoseStamped(goal_f_map) << std::endl;
      std::cout << "goal_f_base: " << mw::PoseStamped(goal_f_base) << std::endl;
      std::cout << "prev_vel: " << mw::Twist(prev_vel_) << std::endl;
    }
#endif

    geometry_msgs::msg::Twist cmd_vel;

    // Calc linear.z
    if (z_dist > move_threshold_) {
      cmd_vel.linear.z = goal_f_base.pose.position.z > 0 ? z_vel_ : -z_vel_;

      // Decelerate
      z_limiter_.decelerate(cmd_vel.linear.z, goal_f_base.pose.position.z);
    }

    // Calc linear.x and angular.z using pure pursuit algorithm
    if (xy_dist > move_threshold_) {
      if (goal_f_base.pose.position.x > 0) {
        // Goal is ahead of the sub: move forward along the shortest curve
        auto curvature = 2.0 * goal_f_base.pose.position.y / xy_dist_sq;
        if (std::abs(curvature) * xy_vel_ <= yaw_vel_) {
          // Move at constant velocity
          cmd_vel.linear.x = xy_vel_;
          cmd_vel.angular.z = curvature * xy_vel_;
        } else {
          // Tight curve... don't exceed angular velocity limit
          cmd_vel.linear.x = yaw_vel_ / std::abs(curvature);
          cmd_vel.angular.z = curvature > 0 ? yaw_vel_ : -yaw_vel_;
        }

        // Decelerate
        xy_limiter_.decelerate(cmd_vel.linear.x, xy_dist);
      } else {
        // Goal is behind the sub: rotate to face it
        cmd_vel.angular.z = goal_f_base.pose.position.y > 0 ? yaw_vel_ : -yaw_vel_;
      }
    }

    return cmd_vel;
  }

public:
  OrcaController() = default;
  ~OrcaController() override = default;

  void configure(
    const rclcpp_lifecycle::LifecycleNode::SharedPtr & parent,
    std::string name,
    const std::shared_ptr<tf2_ros::Buffer> & tf,
    const std::shared_ptr<nav2_costmap_2d::Costmap2DROS> & costmap_ros) override
  {
    // Do not keep a ptr to the parent, this may cause a circular ref
    // Discussion: https://github.com/ros-planning/navigation2/pull/1900

    logger_ = parent->get_logger();
    tf_ = tf;
    base_frame_id_ = costmap_ros->getBaseFrameID();

    PARAMETER(parent, name, xy_vel, 0.4)
    PARAMETER(parent, name, xy_accel, 0.4)
    PARAMETER(parent, name, z_vel, 0.2)
    PARAMETER(parent, name, z_accel, 0.2)
    PARAMETER(parent, name, yaw_vel, 0.4)
    PARAMETER(parent, name, yaw_accel, 0.4)
    PARAMETER(parent, name, lookahead_dist, 1.0)
    PARAMETER(parent, name, transform_tolerance, 1.0)
    PARAMETER(parent, name, move_threshold, 0.1)
    PARAMETER(parent, name, tick_rate, 20.0)

    xy_limiter_ = Limiter(xy_accel_, 1. / tick_rate_);
    z_limiter_ = Limiter(z_accel_, 1. / tick_rate_);
    yaw_limiter_ = Limiter(yaw_accel_, 1. / tick_rate_);

    transform_tolerance_d_ = rclcpp::Duration::from_seconds(transform_tolerance_);

    RCLCPP_INFO(logger_, "OrcaController configured");
  }

  void cleanup() override {}

  void activate() override {}

  void deactivate() override {}

  geometry_msgs::msg::TwistStamped computeVelocityCommands(
    const geometry_msgs::msg::PoseStamped & pose,
    const geometry_msgs::msg::Twist &) override
  {
    geometry_msgs::msg::TwistStamped cmd_vel;
    cmd_vel.header = pose.header;

    // Track the plan
    cmd_vel.twist = pure_pursuit_3d(pose);

    // Limit acceleration
    xy_limiter_.limit(cmd_vel.twist.linear.x, prev_vel_.linear.x);
    z_limiter_.limit(cmd_vel.twist.linear.z, prev_vel_.linear.z);
    yaw_limiter_.limit(cmd_vel.twist.angular.z, prev_vel_.angular.z);

    // Twist parameter from nav2_controller is generated from a Twist2D, so linear.z is always 0
    // Keep a copy of the previous cmd_vel instead
    prev_vel_ = cmd_vel.twist;

    return cmd_vel;
  }

  void setPlan(const nav_msgs::msg::Path & plan) override
  {
    if (plan.poses.empty()) {
      throw nav2_core::PlannerException("Received plan with zero length");
    }
    plan_ = plan;
  }

};

}  // namespace orca_nav2

#include "pluginlib/class_list_macros.hpp"

// Register this controller as a nav2_core plugin
PLUGINLIB_EXPORT_CLASS(orca_nav2::OrcaController, nav2_core::Controller)
