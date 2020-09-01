// Copyright (c) 2020, Samsung Research America
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License. Reserved.

// benefits list:
//  - for tolerance, only search once (max iterations on appraoch meeting tolerance), if set tol low and iterations low then can use to actually compute to a scale before target
//      e.g. compute for the next ~N meters only on way towards something 
//  - Against NavFns: we have inflation + dynamic processing: cached gradiant map not used. reusibility, cannot be built on for nonholonomic, no looping or weird artifacts
//  - common building blocks for use in all planners to maximize reliability, stress test, and reduce likelihood of bugs
//  - not searching then backtracing with grad descent for 2x go through
//  - lower memory (?) and faster (?)
//  - modern data structures & carefully optimized & generic for use in other planning problems
//  - generic smoother that has applications to anything
//  - smoother costmap aware (vs bezier, splines, b-splines, etc) 
//  - caching paths rather than recomputing needlessly if they're still good
//  - network planner & arbitrary nonholonomic including ackermann
//  - non-circular footprints, diff/omni/ackermann, covering all classes of ground robots. circl diff/omni A*, ackerman hybrid, arbitrary diff/omni A* if relatively small, hybrid is large
//  - dials for Astar quality (can be quick and dirty or slow and smooth) then dials for the optimizer to suit (quick once over, or really smooth out a jazzed path)
//  - disable max iterations / tolerance with 0 / -1
//  - max time for soft gaurentees on planning and smoothing times, time tracking
//  - Do low potential field in all areas -- this should be the new defacto-default (really should have been already but ppl ignore it). Footprint + inflation important
//  - describe why and when on the 4 vs 8 connected
//  - plots of pts that violate over iterations (curve, dist > thresh, smooth > dist, cost > thresh)
// - Need to boil down statements about why I did this, clear benefits, and drawbacks of current approaches / solutions
//  - Identified 3 math errors of Thrun
//  - show and explain derivations on smoother / upsampler. Show and explain hybrid stuff
// Lets look at what we ahve here:
//   We have A* path smoothed to kinematic paramrters. Even without explicit modelling of ackermann or limited curvature kinematics, you can get it here. In fact, while a little hand wavey, if you plan in a full potential field with default settings, it steers intentionally in the center of spaces. If that space is built for a robot or vehicle (eg road, or aisle, or open space, or office) then you’re pseduo-promised that the curvature can be valid for your vehicle. Now the then the boundry conditions (initial and final state) are not. For alot of cases thats sufficient bc of an intelligent local planner based on dubin curves or something, but if not, we have a full hybrid A* as well. 
//   Ex of robot to limit curvature: industrial for max speed without dumping load, ackermann, legged to prop forward to minimize slow down for off acis motion, diff to not whip around
//  Show path, no map -- Show term smoothing, lovely, no map -- Then map, welp, thats useless

// astar timeout, max duration, optimizer gets rest or until its set maximum. Test time before/after A* but not in it, that would slow down. if over, send log warning like DWB

// if collision in smoothed path, anchor that point and then re-run until successful (helpful in narrow spaces).
// try vornoi from dynamic vornoi && if works, optimize it && put into vornoi layer in costmap 2d (how with struct / non char*?)

// NOTES 
// way to do collision checking on oriented footprint https://github.com/windelbouwman/move-base-ompl/blob/master/src/ompl_global_planner.cpp#L133 (but doesnt cache)
// https://github.com/ompl/ompl/blob/master/demos/GeometricCarPlanning.cpp for reeds/dubin hybrid. There's also a 2D point to point demo that could be helpful.
// optimization flags -03
// max iterations on approach only for 2D, not for SE2

// In fact, I use that smoother in the A* implementation to make it "smooth" so its not grid-blocky. 
// Its actually how I tested the smoother since that's the nuclear case with tons of sharp random angles.
// People are used to these smooth paths from Navigation Function approaches and I'm not sure anyone would be 
// happy if I just gave them a A* without it. Its stil quite fast but its much faster than NavFn without the smoother. 
// If you have a half decent controller though, its largely unneeded (I tested, its fine, its just not visually appealing).


// TODO seperate createPlan into a few functions

#include <string>
#include <memory>
#include <vector>
#include "Eigen/Core"
#include "smac_planner/smac_planner.hpp"

#define BENCHMARK_TESTING

namespace smac_planner
{
using namespace std::chrono;
using namespace std;

SmacPlanner::SmacPlanner()
: _a_star(nullptr),
  _smoother(nullptr),
  _upsampler(nullptr),
  _node(nullptr),
  _costmap(nullptr),
  _costmap_downsampler(nullptr)
{
}

SmacPlanner::~SmacPlanner()
{
  RCLCPP_INFO(
    _node->get_logger(), "Destroying plugin %s of type SmacPlanner",
    _name.c_str());
}

void SmacPlanner::configure(
  rclcpp_lifecycle::LifecycleNode::SharedPtr parent,
  std::string name, std::shared_ptr<tf2_ros::Buffer> tf,
  std::shared_ptr<nav2_costmap_2d::Costmap2DROS> costmap_ros)
{
  _node = parent;
  _costmap = costmap_ros->getCostmap();
  _name = name;
  _global_frame = costmap_ros->getGlobalFrameID();

  bool allow_unknown;
  int max_iterations;
  int max_on_approach_iterations = std::numeric_limits<int>::max();
  int angle_quantizations;
  float travel_cost_scale;
  float minimum_turning_radius;
  bool smooth_path;
  bool upsample_path;
  std::string motion_model_for_search;

  // General planner params
  nav2_util::declare_parameter_if_not_declared(
    _node, name + ".tolerance", rclcpp::ParameterValue(0.125));
  _tolerance = static_cast<float>(_node->get_parameter(name + ".tolerance").as_double());
  nav2_util::declare_parameter_if_not_declared(
          _node, name + ".downsample_costmap", rclcpp::ParameterValue(true));
  _node->get_parameter(name + ".downsample_costmap", _downsample_costmap);
  nav2_util::declare_parameter_if_not_declared(
          _node, name + ".downsampling_factor", rclcpp::ParameterValue(1));
  _node->get_parameter(name + ".downsampling_factor", _downsampling_factor);
  
  nav2_util::declare_parameter_if_not_declared(
          _node, name + ".angle_quantization_bins", rclcpp::ParameterValue(1));
  _node->get_parameter(name + ".angle_quantization_bins", angle_quantizations);
  _angle_bin_size = 2.0 * M_PI / angle_quantizations;
  _angle_quantizations = static_cast<unsigned int>(angle_quantizations);

  nav2_util::declare_parameter_if_not_declared(
    _node, name + ".allow_unknown", rclcpp::ParameterValue(true));
  _node->get_parameter(name + ".allow_unknown", allow_unknown);
  nav2_util::declare_parameter_if_not_declared(
    _node, name + ".max_iterations", rclcpp::ParameterValue(-1));
  _node->get_parameter(name + ".max_iterations", max_iterations);
  nav2_util::declare_parameter_if_not_declared(
    _node, name + ".travel_cost_scale", rclcpp::ParameterValue(0.8));
  _node->get_parameter(name + ".travel_cost_scale", travel_cost_scale);
  nav2_util::declare_parameter_if_not_declared(
    _node, name + ".smooth_path", rclcpp::ParameterValue(true));
  _node->get_parameter(name + ".smooth_path", smooth_path);
  nav2_util::declare_parameter_if_not_declared(
    _node, name + ".upsample_path", rclcpp::ParameterValue(false));
  _node->get_parameter(name + ".upsample_path", upsample_path);
  nav2_util::declare_parameter_if_not_declared(
    _node, name + ".smoother.upsampling_ratio", rclcpp::ParameterValue(2));
  _node->get_parameter(name + ".smoother.upsampling_ratio", _upsampling_ratio);

  nav2_util::declare_parameter_if_not_declared(
    _node, name + ".minimum_turning_radius", rclcpp::ParameterValue(1.0));
  _node->get_parameter(name + ".minimum_turning_radius", minimum_turning_radius);

  nav2_util::declare_parameter_if_not_declared(
    _node, name + ".motion_model_for_search", rclcpp::ParameterValue(std::string("MOORE")));
  _node->get_parameter(name + ".motion_model_for_search", motion_model_for_search);
  MotionModel motion_model = fromString(motion_model_for_search);
  if (motion_model == MotionModel::UNKNOWN) {
    RCLCPP_WARN(_node->get_logger(),
      "Unable to get MotionModel search type. Given '%s', "
      "valid options are MOORE, VON_NEUMANN, DUBIN, REEDS_SHEPP, BALKCOM_MASON.",
      motion_model_for_search.c_str());
  }

  if (max_on_approach_iterations <= 0) {
    RCLCPP_INFO(_node->get_logger(), "On approach iteration selected as <= 0, "
      "disabling tolerance and on approach iterations.");
    max_on_approach_iterations = std::numeric_limits<int>::max();
  }

  if (max_iterations <= 0) {
    RCLCPP_INFO(_node->get_logger(), "maximum iteration selected as <= 0, "
      "disabling maximum iterations.");
    max_iterations = std::numeric_limits<int>::max();
  }

  if (travel_cost_scale > 1.0 || travel_cost_scale < 0.0) {
    RCLCPP_FATAL(_node->get_logger(), "Travel cost scale must be between 0 and 1, exiting.");
    exit(-1);
  }

  if (_upsampling_ratio != 2 && _upsampling_ratio != 4) {
    RCLCPP_WARN(_node->get_logger(),
      "Upsample ratio set to %i, only 2 and 4 are valid. Defaulting to 2.", _upsampling_ratio);
    _upsampling_ratio = 2;
  }

  float grid_coord_min_turning_rad =
    minimum_turning_radius / (_costmap->getResolution() * _downsampling_factor);
  _a_star = std::make_unique<AStarAlgorithm<NodeSE2>>(motion_model, grid_coord_min_turning_rad);
  _a_star->initialize(
    travel_cost_scale,
    allow_unknown,
    max_iterations,
    max_on_approach_iterations);

  if (smooth_path) {
    _smoother = std::make_unique<Smoother>();
    _optimizer_params.get(_node.get(), name);  // Get optimizer params TODO per-run with time left over
    _smoother_params.get(_node.get(), name);  // Get weights
    _smoother->initialize(_optimizer_params);

    if (upsample_path && _upsampling_ratio > 0) {
      _upsampler = std::make_unique<Upsampler>();
      _upsampler->initialize(_optimizer_params);
    }
  }

  if (_downsample_costmap && _downsampling_factor > 1) {
    std::string topic_name = "downsampled_costmap";
    _costmap_downsampler = std::make_unique<CostmapDownsampler>(_node);
    _costmap_downsampler->initialize(_global_frame, topic_name, _costmap, _downsampling_factor);
  }

  _raw_plan_publisher = _node->create_publisher<nav_msgs::msg::Path>("unsmoothed_plan", 1);
  _smoothed_plan_publisher = _node->create_publisher<nav_msgs::msg::Path>("smoothed_plan", 1);

  RCLCPP_INFO(
    _node->get_logger(), "Configured plugin %s of type SmacPlanner with "
    "travel cost %.2f, tolerance %.2f, maximum iterations %i, "
    "max on approach iterations %i, and %s. Using motion model: %s.",
    _name.c_str(), travel_cost_scale, _tolerance, max_iterations, max_on_approach_iterations,
    allow_unknown ? "allowing unknown traversal" : "not allowing unknown traversal",
    toString(motion_model).c_str());
}

void SmacPlanner::activate()
{
  RCLCPP_INFO(
    _node->get_logger(), "Activating plugin %s of type SmacPlanner",
    _name.c_str());
  _raw_plan_publisher->on_activate();
  _smoothed_plan_publisher->on_activate();
  if (_costmap_downsampler) {
    _costmap_downsampler->activatePublisher();
  }
}

void SmacPlanner::deactivate()
{
  RCLCPP_INFO(
    _node->get_logger(), "Deactivating plugin %s of type SmacPlanner",
    _name.c_str());
  _raw_plan_publisher->on_deactivate();
  _smoothed_plan_publisher->on_deactivate();
  if (_costmap_downsampler) {
    _costmap_downsampler->deactivatePublisher();
  }
}

void SmacPlanner::cleanup()
{
  RCLCPP_INFO(
    _node->get_logger(), "Cleaning up plugin %s of type SmacPlanner",
    _name.c_str());
  _a_star.reset();
  _smoother.reset();
  _upsampler.reset();
  _costmap_downsampler.reset();
  _raw_plan_publisher.reset();
  _smoothed_plan_publisher.reset();
}

nav_msgs::msg::Path SmacPlanner::createPlan(
  const geometry_msgs::msg::PoseStamped & start,
  const geometry_msgs::msg::PoseStamped & goal)
{
#ifdef BENCHMARK_TESTING
  steady_clock::time_point a = steady_clock::now();
#endif

  std::unique_lock<nav2_costmap_2d::Costmap2D::mutex_t> lock(*(_costmap->getMutex()));

  // Choose which costmap to use for the planning
  nav2_costmap_2d::Costmap2D * costmap = _costmap;
  if (_costmap_downsampler) {
    costmap = _costmap_downsampler->downsample(_downsampling_factor);
  }

  // Set Costmap
  unsigned char * char_costmap = costmap->getCharMap();
  _a_star->createGraph(
    costmap->getSizeInCellsX(),
    costmap->getSizeInCellsY(),
    _angle_quantizations,
    char_costmap);

  // Set starting point
  unsigned int mx, my;
  costmap->worldToMap(start.pose.position.x, start.pose.position.y, mx, my);
  double orientation = tf2::getYaw(start.pose.orientation);
  _a_star->setStart(mx, my, static_cast<unsigned int>(orientation / _angle_bin_size));

  // Set goal point
  costmap->worldToMap(goal.pose.position.x, goal.pose.position.y, mx, my);
  orientation = tf2::getYaw(start.pose.orientation);
  _a_star->setGoal(mx, my, static_cast<unsigned int>(orientation / _angle_bin_size));

  // Setup message
  nav_msgs::msg::Path plan;
  plan.header.stamp = _node->now();
  plan.header.frame_id = _global_frame;
  geometry_msgs::msg::PoseStamped pose;
  pose.header = plan.header;
  pose.pose.position.z = 0.0;
  pose.pose.orientation.x = 0.0;
  pose.pose.orientation.y = 0.0;
  pose.pose.orientation.z = 0.0;
  pose.pose.orientation.w = 1.0;

  // Compute plan
  IndexPath path;
  int num_iterations = 0;
  std::string error;
  try {
    if (!_a_star->createPath(
      path, num_iterations, _tolerance / static_cast<float>(costmap->getResolution())))
    {
      if (num_iterations < _a_star->getMaxIterations()) {
        error = std::string("no valid path found");
      } else {
        error = std::string("exceeded maximum iterations");
      }
    }
  } catch (const std::runtime_error & e) {
    error = "invalid use: ";
    error += e.what();
  }

  if (!error.empty()) {
    RCLCPP_WARN(
      _node->get_logger(),
      "%s: failed to create plan, %s.",
      _name.c_str(), error.c_str());
    return plan;
  }

  // Convert to world coordinates and downsample path for smoothing if necesssary
  // We're going to downsample by 4x to give terms room to move.
  const int downsample_ratio = 4;
  std::vector<Eigen::Vector2d> path_world;
  path_world.reserve(_smoother ? path.size() / downsample_ratio : path.size());
  plan.poses.reserve(_smoother ? path.size() / downsample_ratio : path.size());

  for (int i = path.size() - 1; i >= 0; --i) {
    if (_smoother && i % downsample_ratio != 0) {
      continue;
    }

    path_world.push_back(getWorldCoords(path[i].first, path[i].second, costmap));
    pose.pose.position.x = path_world.back().x();
    pose.pose.position.y = path_world.back().y();
    plan.poses.push_back(pose);
  }

  // Publish raw path for debug
  if (_node->count_subscribers(_raw_plan_publisher->get_topic_name()) > 0) {
    _raw_plan_publisher->publish(plan);
  }

  if (!_smoother) {
#ifdef BENCHMARK_TESTING
    steady_clock::time_point b = steady_clock::now();
    duration<double> time_span = duration_cast<duration<double> >(b-a);
    cout << "It took " << time_span.count() * 1000 <<
      " milliseconds with " << num_iterations << " iterations." <<  endl;
#endif
    return plan;
  }

  // if too small, return path
  if (path_world.size() < 4) {
    return plan;
  }

  // Smooth plan
  MinimalCostmap mcmap(char_costmap, costmap->getSizeInCellsX(),
    costmap->getSizeInCellsY(), costmap->getOriginX(), costmap->getOriginY(),
    costmap->getResolution());
  if (!_smoother->smooth(path_world, & mcmap, _smoother_params)) {
    RCLCPP_WARN(
      _node->get_logger(),
      "%s: failed to smooth plan, Ceres could not find a usable solution to optimize.",
      _name.c_str());
    return plan;
  }

  removeHook(path_world);

  // Publish smoothed path for debug
  if (_node->count_subscribers(_smoothed_plan_publisher->get_topic_name()) > 0) {
    for (int i = 0; i != path_world.size(); i++) {
      pose.pose.position.x = path_world[i][0];
      pose.pose.position.y = path_world[i][1];
      plan.poses[i] = pose;
    }
    _smoothed_plan_publisher->publish(plan);
  }

  // Upsample path
  if (_upsampler) {
    if(!_upsampler->upsample(path_world, _smoother_params, _upsampling_ratio))
    {
      RCLCPP_WARN(
        _node->get_logger(),
        "%s: failed to upsample plan, Ceres could not find a usable solution to optimize.",
        _name.c_str());
    } else {
      plan.poses.resize(path_world.size());
    }
  }

  for (int i = 0; i != plan.poses.size(); i++) {
    pose.pose.position.x = path_world[i][0];
    pose.pose.position.y = path_world[i][1];
    plan.poses[i] = pose;
  }

  return plan;
}

void SmacPlanner::removeHook(std::vector<Eigen::Vector2d> & path)
{
  // Removes the end "hooking" since goal is locked in place
  Eigen::Vector2d interpolated_second_to_last_point;
  interpolated_second_to_last_point = (path.end()[-3] + path.end()[-1]) / 2.0;
  if (
    squaredDistance(path.end()[-2], path.end()[-1]) >
    squaredDistance(interpolated_second_to_last_point, path.end()[-1]))
  {
    path.end()[-2] = interpolated_second_to_last_point;
  }
}

Eigen::Vector2d SmacPlanner::getWorldCoords(
  const float & mx, const float & my, const nav2_costmap_2d::Costmap2D * costmap)
{
  double world_x = 
    static_cast<double>(costmap->getOriginX()) + (mx + 0.5) * costmap->getResolution();
  double world_y =
    static_cast<double>(costmap->getOriginY()) + (my + 0.5) * costmap->getResolution();
  return Eigen::Vector2d(world_x, world_y);
}

}  // namespace smac_planner

#include "pluginlib/class_list_macros.hpp"
PLUGINLIB_EXPORT_CLASS(smac_planner::SmacPlanner, nav2_core::GlobalPlanner)