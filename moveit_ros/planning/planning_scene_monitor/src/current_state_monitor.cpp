/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2011, Willow Garage, Inc.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of Willow Garage nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************/

/* Author: Ioan Sucan */

#include <moveit/planning_scene_monitor/current_state_monitor.h>
#include <moveit/planning_scene_monitor/current_state_monitor_middleware_handle.hpp>

#include <tf2_eigen/tf2_eigen.h>
#include <tf2_geometry_msgs/tf2_geometry_msgs.h>

#include <memory>
#include <limits>

namespace planning_scene_monitor
{
namespace
{
static const rclcpp::Logger LOGGER = rclcpp::get_logger("moveit_ros.current_state_monitor");
}

CurrentStateMonitor::CurrentStateMonitor(std::unique_ptr<CurrentStateMonitor::MiddlewareHandle> middleware_handle,
                                         const moveit::core::RobotModelConstPtr& robot_model,
                                         const std::shared_ptr<tf2_ros::Buffer>& tf_buffer)
  : middleware_handle_(std::move(middleware_handle))
  , tf_buffer_(tf_buffer)
  , robot_model_(robot_model)
  , robot_state_(robot_model)
  , state_monitor_started_(false)
  , copy_dynamics_(false)
  , error_(std::numeric_limits<double>::epsilon())
{
  robot_state_.setToDefaultValues();
}

CurrentStateMonitor::CurrentStateMonitor(const rclcpp::Node::SharedPtr& node,
                                         const moveit::core::RobotModelConstPtr& robot_model,
                                         const std::shared_ptr<tf2_ros::Buffer>& tf_buffer)
  : CurrentStateMonitor(std::make_unique<CurrentStateMonitorMiddlewareHandle>(node), robot_model, tf_buffer)
{
}

CurrentStateMonitor::~CurrentStateMonitor()
{
  stopStateMonitor();
}

moveit::core::RobotStatePtr CurrentStateMonitor::getCurrentState() const
{
  std::unique_lock<std::mutex> slock(state_update_lock_);
  moveit::core::RobotState* result = new moveit::core::RobotState(robot_state_);
  return moveit::core::RobotStatePtr(result);
}

rclcpp::Time CurrentStateMonitor::getCurrentStateTime() const
{
  std::unique_lock<std::mutex> slock(state_update_lock_);
  return current_state_time_;
}

std::pair<moveit::core::RobotStatePtr, rclcpp::Time> CurrentStateMonitor::getCurrentStateAndTime() const
{
  std::unique_lock<std::mutex> slock(state_update_lock_);
  moveit::core::RobotState* result = new moveit::core::RobotState(robot_state_);
  return std::make_pair(moveit::core::RobotStatePtr(result), current_state_time_);
}

std::map<std::string, double> CurrentStateMonitor::getCurrentStateValues() const
{
  std::map<std::string, double> m;
  std::unique_lock<std::mutex> slock(state_update_lock_);
  const double* pos = robot_state_.getVariablePositions();
  const std::vector<std::string>& names = robot_state_.getVariableNames();
  for (std::size_t i = 0; i < names.size(); ++i)
    m[names[i]] = pos[i];
  return m;
}

void CurrentStateMonitor::setToCurrentState(moveit::core::RobotState& upd) const
{
  std::unique_lock<std::mutex> slock(state_update_lock_);
  const double* pos = robot_state_.getVariablePositions();
  upd.setVariablePositions(pos);
  if (copy_dynamics_)
  {
    if (robot_state_.hasVelocities())
    {
      const double* vel = robot_state_.getVariableVelocities();
      upd.setVariableVelocities(vel);
    }
    if (robot_state_.hasAccelerations())
    {
      const double* acc = robot_state_.getVariableAccelerations();
      upd.setVariableAccelerations(acc);
    }
    if (robot_state_.hasEffort())
    {
      const double* eff = robot_state_.getVariableEffort();
      upd.setVariableEffort(eff);
    }
  }
}

void CurrentStateMonitor::addUpdateCallback(const JointStateUpdateCallback& fn)
{
  if (fn)
    update_callbacks_.push_back(fn);
}

void CurrentStateMonitor::clearUpdateCallbacks()
{
  update_callbacks_.clear();
}

void CurrentStateMonitor::startStateMonitor(const std::string& joint_states_topic)
{
  if (!state_monitor_started_ && robot_model_)
  {
    joint_time_.clear();
    if (joint_states_topic.empty())
    {
      RCLCPP_ERROR(LOGGER, "The joint states topic cannot be an empty string");
    }
    else
    {
      middleware_handle_->createJointStateSubscription(
          joint_states_topic, std::bind(&CurrentStateMonitor::jointStateCallback, this, std::placeholders::_1));
    }
    if (tf_buffer_ && !robot_model_->getMultiDOFJointModels().empty())
    {
      // TODO (anasarrak): replace this for the appropiate function, there is no similar
      // function in ros2/geometry2.
      // tf_connection_.reset(new TFConnection(
      //     tf_buffer_->_addTransformsChangedListener(std::bind(&CurrentStateMonitor::tfCallback, this))));
    }
    state_monitor_started_ = true;
    monitor_start_time_ = middleware_handle_->now();
    RCLCPP_INFO(LOGGER, "Listening to joint states on topic '%s'", joint_states_topic.c_str());
  }
}

bool CurrentStateMonitor::isActive() const
{
  return state_monitor_started_;
}

void CurrentStateMonitor::stopStateMonitor()
{
  if (state_monitor_started_)
  {
    middleware_handle_->resetJointStateSubscription();
    if (tf_buffer_ && tf_connection_)
    {
      // TODO (anasarrak): replace this for the appropiate function, there is no similar
      // function in ros2/geometry2.
      // tf_buffer_->_removeTransformsChangedListener(*tf_connection_);
      tf_connection_.reset();
    }
    RCLCPP_DEBUG(LOGGER, "No longer listening for joint states");
    state_monitor_started_ = false;
  }
}

std::string CurrentStateMonitor::getMonitoredTopic() const
{
  return middleware_handle_->getJointStateTopicName();
}

bool CurrentStateMonitor::haveCompleteStateHelper(const rclcpp::Time& oldest_allowed_update_time,
                                                  std::vector<std::string>* missing_joints) const
{
  const std::vector<const moveit::core::JointModel*>& active_joints = robot_model_->getActiveJointModels();
  std::unique_lock<std::mutex> slock(state_update_lock_);
  for (const moveit::core::JointModel* joint : active_joints)
  {
    std::map<const moveit::core::JointModel*, rclcpp::Time>::const_iterator it = joint_time_.find(joint);
    if (it == joint_time_.end())
    {
      RCLCPP_DEBUG(LOGGER, "Joint '%s' has never been updated", joint->getName().c_str());
    }
    else if (it->second < oldest_allowed_update_time)
    {
      RCLCPP_DEBUG(LOGGER, "Joint '%s' was last updated %0.3lf seconds before requested time", joint->getName().c_str(),
                   (oldest_allowed_update_time - it->second).seconds());
    }
    else
      continue;

    if (missing_joints)
      missing_joints->push_back(joint->getName());
    else
      return false;
  }
  return (missing_joints == nullptr) || missing_joints->empty();
}

bool CurrentStateMonitor::waitForCurrentState(const rclcpp::Time& t, double wait_time_s) const
{
  rclcpp::Time start = middleware_handle_->now();
  rclcpp::Duration elapsed(0, 0);
  rclcpp::Duration timeout = rclcpp::Duration::from_seconds(wait_time_s);

  std::unique_lock<std::mutex> lock(state_update_lock_);
  while (current_state_time_ < t)
  {
    state_update_condition_.wait_for(lock, (timeout - elapsed).to_chrono<std::chrono::duration<double>>());
    elapsed = middleware_handle_->now() - start;
    if (elapsed > timeout)
    {
      RCLCPP_INFO(LOGGER,
                  "Didn't received robot state (joint angles) with recent timestamp within "
                  "%f seconds.\n"
                  "Check clock synchronization if your are running ROS across multiple machines!",
                  wait_time_s);
      return false;
    }
  }
  return true;
}

bool CurrentStateMonitor::waitForCompleteState(double wait_time_s) const
{
  const double sleep_step_s = std::min(0.05, wait_time_s / 10.0);
  const auto sleep_step_duration = rclcpp::Duration::from_seconds(sleep_step_s);
  double slept_time_s = 0.0;
  while (!haveCompleteState() && slept_time_s < wait_time_s)
  {
    middleware_handle_->sleepFor(sleep_step_duration.to_chrono<std::chrono::nanoseconds>());
    slept_time_s += sleep_step_s;
  }
  return haveCompleteState();
}

bool CurrentStateMonitor::waitForCompleteState(const std::string& group, double wait_time_s) const
{
  if (waitForCompleteState(wait_time_s))
    return true;
  bool ok = true;

  // check to see if we have a fully known state for the joints we want to record
  std::vector<std::string> missing_joints;
  if (!haveCompleteState(missing_joints))
  {
    const moveit::core::JointModelGroup* jmg = robot_model_->getJointModelGroup(group);
    if (jmg)
    {
      std::set<std::string> mj;
      mj.insert(missing_joints.begin(), missing_joints.end());
      const std::vector<std::string>& names = jmg->getJointModelNames();
      bool ok = true;
      for (std::size_t i = 0; ok && i < names.size(); ++i)
        if (mj.find(names[i]) != mj.end())
          ok = false;
    }
    else
      ok = false;
  }
  return ok;
}

void CurrentStateMonitor::jointStateCallback(sensor_msgs::msg::JointState::ConstSharedPtr joint_state)
{
  if (joint_state->name.size() != joint_state->position.size())
  {
    rclcpp::Clock steady_clock(RCL_STEADY_TIME);
    RCLCPP_ERROR_THROTTLE(LOGGER, steady_clock, 1000,
                          "State monitor received invalid joint state (number of joint names does not match number of "
                          "positions)");
    return;
  }
  bool update = false;

  {
    std::unique_lock<std::mutex> _(state_update_lock_);
    // read the received values, and update their time stamps
    std::size_t n = joint_state->name.size();
    current_state_time_ = joint_state->header.stamp;
    for (std::size_t i = 0; i < n; ++i)
    {
      const moveit::core::JointModel* jm = robot_model_->getJointModel(joint_state->name[i]);
      if (!jm)
        continue;
      // ignore fixed joints, multi-dof joints (they should not even be in the message)
      if (jm->getVariableCount() != 1)
        continue;

      joint_time_[jm] = joint_state->header.stamp;

      if (robot_state_.getJointPositions(jm)[0] != joint_state->position[i])
      {
        update = true;
        robot_state_.setJointPositions(jm, &(joint_state->position[i]));

        // continuous joints wrap, so we don't modify them (even if they are outside bounds!)
        if (jm->getType() == moveit::core::JointModel::REVOLUTE)
          if (static_cast<const moveit::core::RevoluteJointModel*>(jm)->isContinuous())
            continue;

        const moveit::core::VariableBounds& b =
            jm->getVariableBounds()[0];  // only one variable in the joint, so we get its bounds

        // if the read variable is 'almost' within bounds (up to error_ difference), then consider it to be within
        // bounds
        if (joint_state->position[i] < b.min_position_ && joint_state->position[i] >= b.min_position_ - error_)
          robot_state_.setJointPositions(jm, &b.min_position_);
        else if (joint_state->position[i] > b.max_position_ && joint_state->position[i] <= b.max_position_ + error_)
          robot_state_.setJointPositions(jm, &b.max_position_);
      }

      // optionally copy velocities and effort
      if (copy_dynamics_)
      {
        // update joint velocities
        if (joint_state->name.size() == joint_state->velocity.size() &&
            (!robot_state_.hasVelocities() || robot_state_.getJointVelocities(jm)[0] != joint_state->velocity[i]))
        {
          update = true;
          robot_state_.setJointVelocities(jm, &(joint_state->velocity[i]));
        }

        // update joint efforts
        if (joint_state->name.size() == joint_state->effort.size() &&
            (!robot_state_.hasEffort() || robot_state_.getJointEffort(jm)[0] != joint_state->effort[i]))
        {
          update = true;
          robot_state_.setJointEfforts(jm, &(joint_state->effort[i]));
        }
      }
    }
  }

  // callbacks, if needed
  if (update)
    for (JointStateUpdateCallback& update_callback : update_callbacks_)
      update_callback(joint_state);

  // notify waitForCurrentState() *after* potential update callbacks
  state_update_condition_.notify_all();
}

void CurrentStateMonitor::tfCallback()
{
  // read multi-dof joint states from TF, if needed
  const std::vector<const moveit::core::JointModel*>& multi_dof_joints = robot_model_->getMultiDOFJointModels();

  bool update = false;
  bool changes = false;
  {
    std::unique_lock<std::mutex> _(state_update_lock_);
    for (const moveit::core::JointModel* joint : multi_dof_joints)
    {
      const std::string& child_frame = joint->getChildLinkModel()->getName();
      const std::string& parent_frame =
          joint->getParentLinkModel() ? joint->getParentLinkModel()->getName() : robot_model_->getModelFrame();

      rclcpp::Time latest_common_time;
      geometry_msgs::msg::TransformStamped transf;
      try
      {
        transf = tf_buffer_->lookupTransform(parent_frame, child_frame, tf2::TimePointZero);
        latest_common_time = transf.header.stamp;
      }
      catch (tf2::TransformException& ex)
      {
        RCLCPP_WARN_ONCE(LOGGER,
                         "Unable to update multi-DOF joint '%s':"
                         "Failure to lookup transform between '%s'"
                         "and '%s' with TF exception: %s",
                         joint->getName().c_str(), parent_frame.c_str(), child_frame.c_str(), ex.what());
        continue;
      }

      // allow update if time is more recent or if it is a static transform (time = 0)
      if (latest_common_time <= joint_time_[joint] && latest_common_time > rclcpp::Time(0))
        continue;
      joint_time_[joint] = latest_common_time;

      std::vector<double> new_values(joint->getStateSpaceDimension());
      const moveit::core::LinkModel* link = joint->getChildLinkModel();
      if (link->jointOriginTransformIsIdentity())
        joint->computeVariablePositions(tf2::transformToEigen(transf), new_values.data());
      else
        joint->computeVariablePositions(link->getJointOriginTransform().inverse() * tf2::transformToEigen(transf),
                                        new_values.data());

      if (joint->distance(new_values.data(), robot_state_.getJointPositions(joint)) > 1e-5)
      {
        changes = true;
      }

      robot_state_.setJointPositions(joint, new_values.data());
      update = true;
    }
  }

  // callbacks, if needed
  if (changes)
  {
    // stub joint state: multi-dof joints are not modelled in the message,
    // but we should still trigger the update callbacks
    auto joint_state = std::make_shared<sensor_msgs::msg::JointState>();
    for (JointStateUpdateCallback& update_callback : update_callbacks_)
      update_callback(joint_state);
  }

  if (update)
  {
    // notify waitForCurrentState() *after* potential update callbacks
    state_update_condition_.notify_all();
  }
}

}  // namespace planning_scene_monitor
