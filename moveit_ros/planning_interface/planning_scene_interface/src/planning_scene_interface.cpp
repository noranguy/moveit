/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2012, Willow Garage, Inc.
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

/* Author: Ioan Sucan, Sachin Chitta */

#include <moveit/planning_scene_interface/planning_scene_interface.h>
#include <moveit/move_group/capability_names.h>
#include <moveit_msgs/GetPlanningScene.h>
#include <moveit_msgs/ApplyPlanningScene.h>
#include <tf2_geometry_msgs/tf2_geometry_msgs.h>
#include <ros/ros.h>
#include <algorithm>

namespace moveit
{
namespace planning_interface
{
static const std::string LOGNAME = "planning_scene_interface";

class PlanningSceneInterface::PlanningSceneInterfaceImpl
{
public:
  explicit PlanningSceneInterfaceImpl(const std::string& ns = "", bool wait = true, bool persistent_connections = false)
  {
    wait_ = wait;
    persistent_connections_ = persistent_connections;
    node_handle_ = ros::NodeHandle(ns);
    planning_scene_diff_publisher_ = node_handle_.advertise<moveit_msgs::PlanningScene>("planning_scene", 1);

    if (persistent_connections_ && !wait_)
    {
      ROS_WARN_NAMED(LOGNAME, "To create persistent service clients, wait is set to true");
      wait_ = true;
    }

    connectGetPlanningSceneService();
    connectApplyPlanningSceneService();
  }

  std::vector<std::string> getKnownObjectNames(bool with_type)
  {
    moveit_msgs::GetPlanningScene::Request request;
    moveit_msgs::GetPlanningScene::Response response;
    std::vector<std::string> result;
    request.components.components = request.components.WORLD_OBJECT_NAMES;

    if (!getPlanningSceneServiceCall(request, response))
      return result;
    if (with_type)
    {
      for (const moveit_msgs::CollisionObject& collision_object : response.scene.world.collision_objects)
        if (!collision_object.type.key.empty())
          result.push_back(collision_object.id);
    }
    else
    {
      for (const moveit_msgs::CollisionObject& collision_object : response.scene.world.collision_objects)
        result.push_back(collision_object.id);
    }
    return result;
  }

  std::vector<std::string> getKnownObjectNamesInROI(double minx, double miny, double minz, double maxx, double maxy,
                                                    double maxz, bool with_type, std::vector<std::string>& types)
  {
    moveit_msgs::GetPlanningScene::Request request;
    moveit_msgs::GetPlanningScene::Response response;
    std::vector<std::string> result;
    request.components.components = request.components.WORLD_OBJECT_GEOMETRY;

    if (!getPlanningSceneServiceCall(request, response))
    {
      ROS_WARN_NAMED(LOGNAME, "Could not call planning scene service to get object names");
      return result;
    }

    for (const moveit_msgs::CollisionObject& collision_object : response.scene.world.collision_objects)
    {
      if (with_type && collision_object.type.key.empty())
        continue;
      if (collision_object.mesh_poses.empty() && collision_object.primitive_poses.empty())
        continue;

      // Compose mesh and primitive poses with the object pose before checking inclusion in the ROI
      geometry_msgs::Pose pose_tf;
      geometry_msgs::TransformStamped tf;
      tf.header = collision_object.header;
      tf.transform.translation.x = collision_object.pose.position.x;
      tf.transform.translation.y = collision_object.pose.position.y;
      tf.transform.translation.z = collision_object.pose.position.z;
      tf.transform.rotation.x = collision_object.pose.orientation.x;
      tf.transform.rotation.y = collision_object.pose.orientation.y;
      tf.transform.rotation.z = collision_object.pose.orientation.z;
      tf.transform.rotation.w = collision_object.pose.orientation.w;

      bool good = true;
      for (const geometry_msgs::Pose& mesh_pose : collision_object.mesh_poses)
      {
        tf2::doTransform(mesh_pose, pose_tf, tf);
        if (!(pose_tf.position.x >= minx && pose_tf.position.x <= maxx && pose_tf.position.y >= miny &&
              pose_tf.position.y <= maxy && pose_tf.position.z >= minz && pose_tf.position.z <= maxz))
        {
          good = false;
          break;
        }
      }
      for (const geometry_msgs::Pose& primitive_pose : collision_object.primitive_poses)
      {
        tf2::doTransform(primitive_pose, pose_tf, tf);
        if (!(pose_tf.position.x >= minx && pose_tf.position.x <= maxx && pose_tf.position.y >= miny &&
              pose_tf.position.y <= maxy && pose_tf.position.z >= minz && pose_tf.position.z <= maxz))
        {
          good = false;
          break;
        }
      }
      if (good)
      {
        result.push_back(collision_object.id);
        if (with_type)
          types.push_back(collision_object.type.key);
      }
    }
    return result;
  }

  std::map<std::string, geometry_msgs::Pose> getObjectPoses(const std::vector<std::string>& object_ids)
  {
    moveit_msgs::GetPlanningScene::Request request;
    moveit_msgs::GetPlanningScene::Response response;
    std::map<std::string, geometry_msgs::Pose> result;
    request.components.components = request.components.WORLD_OBJECT_GEOMETRY;

    if (!getPlanningSceneServiceCall(request, response))
    {
      ROS_WARN_NAMED(LOGNAME, "Could not call planning scene service to get object names");
      return result;
    }

    for (const moveit_msgs::CollisionObject& collision_object : response.scene.world.collision_objects)
    {
      if (std::find(object_ids.begin(), object_ids.end(), collision_object.id) != object_ids.end())
      {
        result[collision_object.id] = collision_object.pose;
      }
    }
    return result;
  }

  std::map<std::string, moveit_msgs::CollisionObject> getObjects(const std::vector<std::string>& object_ids)
  {
    moveit_msgs::GetPlanningScene::Request request;
    moveit_msgs::GetPlanningScene::Response response;
    std::map<std::string, moveit_msgs::CollisionObject> result;
    request.components.components = request.components.WORLD_OBJECT_GEOMETRY;

    if (!getPlanningSceneServiceCall(request, response))
    {
      ROS_WARN_NAMED(LOGNAME, "Could not call planning scene service to get object geometries");
      return result;
    }

    for (const moveit_msgs::CollisionObject& collision_object : response.scene.world.collision_objects)
    {
      if (object_ids.empty() || std::find(object_ids.begin(), object_ids.end(), collision_object.id) != object_ids.end())
      {
        result[collision_object.id] = collision_object;
      }
    }
    return result;
  }

  std::map<std::string, moveit_msgs::AttachedCollisionObject>
  getAttachedObjects(const std::vector<std::string>& object_ids)
  {
    moveit_msgs::GetPlanningScene::Request request;
    moveit_msgs::GetPlanningScene::Response response;
    std::map<std::string, moveit_msgs::AttachedCollisionObject> result;
    request.components.components = request.components.ROBOT_STATE_ATTACHED_OBJECTS;

    if (!getPlanningSceneServiceCall(request, response))
    {
      ROS_WARN_NAMED(LOGNAME, "Could not call planning scene service to get attached object geometries");
      return result;
    }

    for (const moveit_msgs::AttachedCollisionObject& attached_collision_object :
         response.scene.robot_state.attached_collision_objects)
    {
      if (object_ids.empty() ||
          std::find(object_ids.begin(), object_ids.end(), attached_collision_object.object.id) != object_ids.end())
      {
        result[attached_collision_object.object.id] = attached_collision_object;
      }
    }
    return result;
  }

  moveit_msgs::PlanningScene getPlanningSceneMsg(uint32_t components)
  {
    moveit_msgs::GetPlanningScene::Request request;
    moveit_msgs::GetPlanningScene::Response response;
    request.components.components = components;

    if (!getPlanningSceneServiceCall(request, response))
    {
      ROS_WARN_NAMED(LOGNAME, "Could not call planning scene service");
      return moveit_msgs::PlanningScene();
    }
    return response.scene;
  }

  bool applyPlanningScene(const moveit_msgs::PlanningScene& planning_scene)
  {
    moveit_msgs::ApplyPlanningScene::Request request;
    moveit_msgs::ApplyPlanningScene::Response response;
    request.scene = planning_scene;

    if (!applyPlanningSceneServiceCall(request, response))
    {
      ROS_WARN_NAMED(LOGNAME, "Failed to call ApplyPlanningScene service");
      return false;
    }
    return response.success;
  }

  void addCollisionObjects(const std::vector<moveit_msgs::CollisionObject>& collision_objects,
                           const std::vector<moveit_msgs::ObjectColor>& object_colors) const
  {
    moveit_msgs::PlanningScene planning_scene;
    planning_scene.world.collision_objects = collision_objects;
    planning_scene.object_colors = object_colors;

    for (size_t i = 0; i < planning_scene.object_colors.size(); ++i)
    {
      if (planning_scene.object_colors[i].id.empty() && i < collision_objects.size())
        planning_scene.object_colors[i].id = collision_objects[i].id;
      else
        break;
    }

    planning_scene.is_diff = true;
    planning_scene_diff_publisher_.publish(planning_scene);
  }

  void removeCollisionObjects(const std::vector<std::string>& object_ids) const
  {
    moveit_msgs::PlanningScene planning_scene;
    moveit_msgs::CollisionObject object;
    for (const std::string& object_id : object_ids)
    {
      object.id = object_id;
      object.operation = object.REMOVE;
      planning_scene.world.collision_objects.push_back(object);
    }
    planning_scene.is_diff = true;
    planning_scene_diff_publisher_.publish(planning_scene);
  }

  bool clear()
  {
    moveit_msgs::PlanningScene clear_scene;
    clear_scene.is_diff = true;
    clear_scene.robot_state.is_diff = true;
    clear_scene.robot_state.attached_collision_objects.resize(1);
    clear_scene.robot_state.attached_collision_objects[0].object.operation = moveit_msgs::CollisionObject::REMOVE;
    clear_scene.world.collision_objects.resize(1);
    clear_scene.world.collision_objects[0].operation = moveit_msgs::CollisionObject::REMOVE;

    return applyPlanningScene(clear_scene);
  }

private:
  void waitForService(ros::ServiceClient& srv)
  {
    ros::Duration time_before_warning(5.0);
    srv.waitForExistence(time_before_warning);
    if (!srv.exists())
    {
      ROS_WARN_STREAM_NAMED(LOGNAME, "service '" << srv.getService() << "' not advertised yet. Continue waiting...");
      srv.waitForExistence();
    }
  }

  void connectGetPlanningSceneService()
  {
    if (persistent_connections_)
    {
      ros::service::waitForService(move_group::GET_PLANNING_SCENE_SERVICE_NAME);
    }

    planning_scene_service_ = node_handle_.serviceClient<moveit_msgs::GetPlanningScene>(
        move_group::GET_PLANNING_SCENE_SERVICE_NAME, persistent_connections_);

    if (wait_)
    {
      waitForService(planning_scene_service_);
    }
    else
    {
      if (!planning_scene_service_.exists())
      {
        throw std::runtime_error("ROS service not available");
      }
    }
  }

  void connectApplyPlanningSceneService()
  {
    if (persistent_connections_)
    {
      ros::service::waitForService(move_group::APPLY_PLANNING_SCENE_SERVICE_NAME);
    }

    apply_planning_scene_service_ = node_handle_.serviceClient<moveit_msgs::ApplyPlanningScene>(
        move_group::APPLY_PLANNING_SCENE_SERVICE_NAME, persistent_connections_);

    if (wait_)
    {
      waitForService(apply_planning_scene_service_);
    }
    else
    {
      if (!apply_planning_scene_service_.exists())
      {
        throw std::runtime_error("ROS service not available");
      }
    }
  }

  bool getPlanningSceneServiceCall(const moveit_msgs::GetPlanningScene::Request& request,
                                   moveit_msgs::GetPlanningScene::Response& response)
  {
    if (!planning_scene_service_.isValid())
      connectGetPlanningSceneService();

    return planning_scene_service_.call(request, response);
  }

  bool applyPlanningSceneServiceCall(const moveit_msgs::ApplyPlanningScene::Request& request,
                                     moveit_msgs::ApplyPlanningScene::Response& response)
  {
    if (!apply_planning_scene_service_.isValid())
      connectApplyPlanningSceneService();

    return apply_planning_scene_service_.call(request, response);
  }

  bool wait_;
  bool persistent_connections_;

  ros::NodeHandle node_handle_;
  ros::ServiceClient planning_scene_service_;
  ros::ServiceClient apply_planning_scene_service_;
  ros::Publisher planning_scene_diff_publisher_;
  moveit::core::RobotModelConstPtr robot_model_;
};

PlanningSceneInterface::PlanningSceneInterface(const std::string& ns, bool wait, bool persistent_connections)
{
  impl_ = new PlanningSceneInterfaceImpl(ns, wait, persistent_connections);
}

PlanningSceneInterface::~PlanningSceneInterface()
{
  delete impl_;
}

std::vector<std::string> PlanningSceneInterface::getKnownObjectNames(bool with_type)
{
  return impl_->getKnownObjectNames(with_type);
}

std::vector<std::string> PlanningSceneInterface::getKnownObjectNamesInROI(double minx, double miny, double minz,
                                                                          double maxx, double maxy, double maxz,
                                                                          bool with_type,
                                                                          std::vector<std::string>& types)
{
  return impl_->getKnownObjectNamesInROI(minx, miny, minz, maxx, maxy, maxz, with_type, types);
}

std::map<std::string, geometry_msgs::Pose>
PlanningSceneInterface::getObjectPoses(const std::vector<std::string>& object_ids)
{
  return impl_->getObjectPoses(object_ids);
}

std::map<std::string, moveit_msgs::CollisionObject>
PlanningSceneInterface::getObjects(const std::vector<std::string>& object_ids)
{
  return impl_->getObjects(object_ids);
}

std::map<std::string, moveit_msgs::AttachedCollisionObject>
PlanningSceneInterface::getAttachedObjects(const std::vector<std::string>& object_ids)
{
  return impl_->getAttachedObjects(object_ids);
}

bool PlanningSceneInterface::applyCollisionObject(const moveit_msgs::CollisionObject& collision_object)
{
  moveit_msgs::PlanningScene ps;
  ps.robot_state.is_diff = true;
  ps.is_diff = true;
  ps.world.collision_objects.reserve(1);
  ps.world.collision_objects.push_back(collision_object);
  return applyPlanningScene(ps);
}

bool PlanningSceneInterface::applyCollisionObject(const moveit_msgs::CollisionObject& collision_object,
                                                  const std_msgs::ColorRGBA& object_color)
{
  moveit_msgs::PlanningScene ps;
  ps.robot_state.is_diff = true;
  ps.is_diff = true;
  ps.world.collision_objects.reserve(1);
  ps.world.collision_objects.push_back(collision_object);
  moveit_msgs::ObjectColor oc;
  oc.id = collision_object.id;
  oc.color = object_color;
  ps.object_colors.push_back(oc);
  return applyPlanningScene(ps);
}

bool PlanningSceneInterface::applyCollisionObjects(const std::vector<moveit_msgs::CollisionObject>& collision_objects,
                                                   const std::vector<moveit_msgs::ObjectColor>& object_colors)
{
  moveit_msgs::PlanningScene ps;
  ps.robot_state.is_diff = true;
  ps.is_diff = true;
  ps.world.collision_objects = collision_objects;
  ps.object_colors = object_colors;

  for (size_t i = 0; i < ps.object_colors.size(); ++i)
  {
    if (ps.object_colors[i].id.empty() && i < collision_objects.size())
      ps.object_colors[i].id = collision_objects[i].id;
    else
      break;
  }

  return applyPlanningScene(ps);
}
//confusion on two different functions that basically do the same thing?
bool PlanningSceneInterface::applyAttachedCollisionObject(const moveit_msgs::AttachedCollisionObject& collision_object)
{
  //service to clear octomap for attached object
  ros::NodeHandle _Alloc_noderos::ServiceClient clear_OM = nh.serviceClient<std_srvs::Empty>("/clear_octomap");
  moveit_msgs::PlanningScene ps;
  ps.robot_state.is_diff = true;
  ps.is_diff = true;
  ps.robot_state.attached_collision_objects.reserve(1);
  ps.robot_state.attached_collision_objects.push_back(collision_object);
  //clear octomap
  return applyPlanningScene(ps);
}
bool PlanningSceneInterface::applyAttachedCollisionObjects(
    const std::vector<moveit_msgs::AttachedCollisionObject>& collision_objects)
{
  moveit_msgs::PlanningScene ps;
  ps.robot_state.is_diff = true;
  ps.is_diff = true;
  ps.robot_state.attached_collision_objects.reserve(1);
  ps.robot_state.attached_collision_objects = collision_objects;
  return applyPlanningScene(ps);
}

moveit_msgs::PlanningScene PlanningSceneInterface::getPlanningSceneMsg(uint32_t components)
{
  return impl_->getPlanningSceneMsg(components);
}

bool PlanningSceneInterface::applyPlanningScene(const moveit_msgs::PlanningScene& ps)
{
  return impl_->applyPlanningScene(ps);
}

void PlanningSceneInterface::addCollisionObjects(const std::vector<moveit_msgs::CollisionObject>& collision_objects,
                                                 const std::vector<moveit_msgs::ObjectColor>& object_colors) const
{
  impl_->addCollisionObjects(collision_objects, object_colors);
}

void PlanningSceneInterface::removeCollisionObjects(const std::vector<std::string>& object_ids) const
{
  impl_->removeCollisionObjects(object_ids);
}

bool PlanningSceneInterface::clear()
{
  return impl_->clear();
}

}  // namespace planning_interface
}  // namespace moveit
