/*
 * Software License Agreement (Apache License)
 *
 * Copyright (c) 2016, Jonathan Meyer
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "descartes_moveit/ikfast_moveit_state_adapter.h"

#include <eigen_conversions/eigen_msg.h>
#include <ros/node_handle.h>

const static std::string default_base_frame = "base_link";
const static std::string default_tool_frame = "tool0";

bool isEqual( const std::vector<double>& v1, const std::vector<double>& v2, const double tol = 1e-6 )
{
  return ( v1.size() == v2.size() ) 
  && std::equal( v1.begin(), v1.end(), v2.begin(), [&tol]( const double v1i, const double v2i ) { return std::fabs( v1i-v2i) < tol; } );
}

bool isInList( const std::vector<double>& v, const std::vector< std::vector<double>> & list,  const double tol = 1e-6 )
{
  if( list.size() > 0 )
  {
    for( auto const & v2 : list )
      if( isEqual(v,v2,tol) )
        return true;
  }
  return false;
}


// Compute the 'joint distance' between two poses
static double distance(const std::vector<double>& a, const std::vector<double>& b)
{
  double cost = 0.0;
  for (size_t i = 0; i < a.size(); ++i)
    cost += std::abs(b[i] - a[i]);
  return cost;
}

// Compute the index of the closest joint pose in 'candidates' from 'target'
static size_t closestJointPose(const std::vector<double>& target, const std::vector<std::vector<double>>& candidates)
{
  size_t closest = 0;  // index into candidates
  double lowest_cost = std::numeric_limits<double>::max();
  for (size_t i = 0; i < candidates.size(); ++i)
  {
    assert(target.size() == candidates[i].size());
    double c = distance(target, candidates[i]);
    if (c < lowest_cost)
    {
      closest = i;
      lowest_cost = c;
    }
  }
  return closest;
}

bool descartes_moveit::IkFastMoveitStateAdapter::initialize(const std::string& robot_description,
                                                            const std::string& group_name,
                                                            const std::string& world_frame,
                                                            const std::string& tcp_frame)
{
  if (!MoveitStateAdapter::initialize(robot_description, group_name, world_frame, tcp_frame))
  {
    return false;
  }

  return computeIKFastTransforms();
}

bool descartes_moveit::IkFastMoveitStateAdapter::getAllIK(const Eigen::Affine3d& pose,
                                                          std::vector<std::vector<double>>& joint_poses) const
{
  joint_poses.clear();
  const auto& solver = joint_group_->getSolverInstance();

  // Transform input pose
  Eigen::Affine3d tool_pose = world_to_base_.frame_inv * pose * tool0_to_tip_.frame;

  // convert to geometry_msgs ...
  geometry_msgs::Pose geometry_pose;
  tf::poseEigenToMsg(tool_pose, geometry_pose);
  std::vector<geometry_msgs::Pose> poses = { geometry_pose };

  std::vector<std::vector<double>> seed_states = seed_states_;
  if( seed_states.empty() )
  {
    std::vector<double> dummy_seed(getDOF(), 0.0);
    seed_states.push_back( dummy_seed );
  }
    
  for( const auto & seed_state : seed_states )
  { 
    std::vector<std::vector<double>> joint_results;
    kinematics::KinematicsResult result;
    kinematics::KinematicsQueryOptions options;  // defaults are reasonable as of Indigo
    if( solver->getPositionIK(poses, seed_state, joint_results, result, options) )
    {
      for (auto& sol : joint_results)
      {
        if (isValid(sol) && !isInList( sol, joint_poses, 1e-6 ) )
        {
          joint_poses.push_back(std::move(sol));
        }
      }
    }
  }

  return joint_poses.size() > 0;
}

bool descartes_moveit::IkFastMoveitStateAdapter::getIK(const Eigen::Affine3d& pose,
                                                       const std::vector<double>& seed_state,
                                                       std::vector<double>& joint_pose) const
{
  // Descartes Robot Model interface calls for 'closest' point to seed position
  std::vector<std::vector<double>> joint_poses;
  if (!getAllIK(pose, joint_poses))
    return false;
  // Find closest joint pose; getAllIK() does isValid checks already
  joint_pose = joint_poses[closestJointPose(seed_state, joint_poses)];
  return true;
}

bool descartes_moveit::IkFastMoveitStateAdapter::getFK(const std::vector<double>& joint_pose,
                                                       Eigen::Affine3d& pose) const
{
  const auto& solver = joint_group_->getSolverInstance();

  std::vector<std::string> tip_frame = { solver->getTipFrame() };
  std::vector<geometry_msgs::Pose> output;

  if (!isValid(joint_pose))
    return false;

  if (!solver->getPositionFK(tip_frame, joint_pose, output))
    return false;

  tf::poseMsgToEigen(output[0], pose);  // pose in frame of IkFast base
  pose = world_to_base_.frame * pose * tool0_to_tip_.frame_inv;
  return true;
}

void descartes_moveit::IkFastMoveitStateAdapter::setState(const moveit::core::RobotState& state)
{
  descartes_moveit::MoveitStateAdapter::setState(state);
  computeIKFastTransforms();
}

bool descartes_moveit::IkFastMoveitStateAdapter::computeIKFastTransforms()
{
  // look up the IKFast base and tool frame
  ros::NodeHandle nh;
  std::string ikfast_base_frame, ikfast_tool_frame;
  if (!nh.param<std::string>(group_name_+"/ikfast_base_frame", ikfast_base_frame, default_base_frame))
  {
    ROS_WARN("%s not defined, used %s TODO %s",(nh.getNamespace()+"/"+group_name_+"/ikfast_base_frame").c_str(),default_base_frame.c_str(),ikfast_base_frame.c_str());
  };
  if (!nh.param<std::string>(group_name_+"/ikfast_tool_frame", ikfast_tool_frame, default_tool_frame))
  {
    ROS_WARN("%s not defined, used %s TODO %s",(nh.getNamespace()+"/"+group_name_+"/ikfast_tool_frame").c_str(),default_tool_frame.c_str(),ikfast_tool_frame.c_str());
  };

  if (!robot_state_->knowsFrameTransform(ikfast_base_frame))
  {
    CONSOLE_BRIDGE_logError("IkFastMoveitStateAdapter: Cannot find transformation to frame '%s' in group '%s'.",
             ikfast_base_frame.c_str(), group_name_.c_str());
    return false;
  }

  if (!robot_state_->knowsFrameTransform(ikfast_tool_frame))
  {
    CONSOLE_BRIDGE_logError("IkFastMoveitStateAdapter: Cannot find transformation to frame '%s' in group '%s'.",
             ikfast_tool_frame.c_str(), group_name_.c_str());
    return false;
  }

  // calculate frames
  tool0_to_tip_ = descartes_core::Frame(robot_state_->getFrameTransform(tool_frame_).inverse() *
                                        robot_state_->getFrameTransform(ikfast_tool_frame));

  world_to_base_ = descartes_core::Frame(world_to_root_.frame * robot_state_->getFrameTransform(ikfast_base_frame));

  CONSOLE_BRIDGE_logInform("IkFastMoveitStateAdapter: initialized with IKFast tool frame '%s' and base frame '%s'.",
            ikfast_tool_frame.c_str(), ikfast_base_frame.c_str());
  return true;
}
