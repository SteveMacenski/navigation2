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

#include <math.h>
#include "smac_planner/node_se2.hpp"

namespace smac_planner
{

// defining static member for all instance to share
MotionTable NodeSE2::_motion_model;

// Each of these tables are the projected motion models through
// time and space applied to the search on the current node in
// continuous map-coordinates (e.g. not meters but partial map cells)
// Currently, these are set to project *at minimum* into a neighboring
// cell. Though this could be later modified to project a certain
// amount of time or particular distance forward.

// http://planning.cs.uiuc.edu/node821.html
// Model for ackermann style vehicle with minimum radius restriction
void MotionTable::initDubin(
  unsigned int & size_x_in,
  unsigned int & num_angle_quantization_in,
  float & min_turning_radius)
{
  size_x = size_x_in;
  num_angle_quantization = num_angle_quantization_in;

  // square root of two arc length used to ensure leaving current cell
  const float sqrt_2 = sqrt(2.0);
  // angle must meet 3 requirements:
  // 1) be increment of quantized bin size
  // 2) chord length must be greater than sqrt(2) to leave current cell 
  // 3) maximum curvature must be respected, represented by minimum turning angle
  // Thusly:
  // On circle of radius minimum turning angle, we need select motion primatives 
  // with chord length > sqrt(2) and be an increment of our bin size
  //
  // chord >= sqrt(2) >= 2 * R * sin (angle / 2); where angle / N = quantized bin size
  // Thusly: angle <= 2.0 * asin(sqrt(2) / (2 * R))
  float angle = 2.0 * asin(sqrt_2 / (2 * min_turning_radius));
  // Now make sure angle is an increment of the quantized bin size
  // And since its based on the minimum chord, we need to make sure its always larger
  const float bin_size =
    2.0f * static_cast<float>(M_PI) / static_cast<float>(num_angle_quantization);
  if (angle < bin_size) {
    angle = bin_size;
  } else if (angle > bin_size) {
    const int increments = ceil(angle / bin_size);
    angle = bin_size * increments;
  }

  // find deflections
  // If we make a right triangle out of the chord in circle of radius
  // min turning angle, we can see that delta X = R * sin (angle)
  float delta_x = min_turning_radius * sin(angle);
  // Using that same right triangle, we can see that the complement
  // to delta Y is R * cos (angle). If we subtract R, we get the actual value
  float delta_y = (min_turning_radius * cos(angle)) - min_turning_radius;

  projections.clear();
  projections.reserve(3);
  projections.emplace_back(sqrt_2, 0.0, 0.0);  // Forward
  projections.emplace_back(delta_x, delta_y, angle);  // Left
  projections.emplace_back(delta_x, -delta_y, -angle);  // Right
}

// http://planning.cs.uiuc.edu/node822.html
// Same as Dubin model but now reverse is valid
// See notes in Dubin for explanation
void MotionTable::initReedsShepp(
  unsigned int & size_x_in,
  unsigned int & num_angle_quantization_in,
  float & min_turning_radius)
{
  size_x = size_x_in;
  num_angle_quantization = num_angle_quantization_in;

  const float sqrt_2 = sqrt(2.0);
  float angle = 2.0 * asin(sqrt_2 / (2 * min_turning_radius));
  const float bin_size =
    2.0f * static_cast<float>(M_PI) / static_cast<float>(num_angle_quantization);
  if (angle < bin_size) {
    angle = bin_size;
  } else if (angle > bin_size) {
    const int increments = ceil(angle / bin_size);
    angle = bin_size * increments;
  }
  float delta_x = min_turning_radius * sin(angle);
  float delta_y = (min_turning_radius * cos(angle)) - min_turning_radius;

  projections.clear();
  projections.reserve(6);
  projections.emplace_back(sqrt_2, 0.0, 0.0);  // Forward
  projections.emplace_back(delta_x, delta_y, angle);  // Forward + Left
  projections.emplace_back(delta_x, -delta_y, -angle);  // Forward + Right
  projections.emplace_back(-sqrt_2, 0.0, 0.0);  // Backward
  projections.emplace_back(-delta_x, delta_y, angle);  // Backward + Left
  projections.emplace_back(-delta_x, -delta_y, -angle);  // Backward + Right
}

// http://planning.cs.uiuc.edu/node823.html
// Allows a differential drive robot to move in all the basic ways
// its base can allow: forward and back, spin in place, rotate while moving
// This isn't a 'pure' implementation, but in the right theme
void MotionTable::initBalkcomMason(
  unsigned int & size_x_in,
  unsigned int & num_angle_quantization_in)
{
  // square root of two arc length used to ensure leaving current cell
  const float sqrt_2 = sqrt(2.0);

  size_x = size_x_in;
  num_angle_quantization = num_angle_quantization_in;
  const float delta_angle =
    2.0f * static_cast<float>(M_PI) / static_cast<float>(num_angle_quantization);

  projections.clear();
  projections.reserve(8);
  projections.emplace_back(sqrt_2, 0.0, 0.0);  // Forward
  projections.emplace_back(-sqrt_2, 0.0, 0.0);  // Backward
  projections.emplace_back(0.0, 0.0, delta_angle);  // Spin left
  projections.emplace_back(0.0, 0.0, -delta_angle);  // Spin right
  projections.emplace_back(sqrt_2, 0.0, delta_angle);  // Spin left + Forward
  projections.emplace_back(-sqrt_2, 0.0, delta_angle);  // Spin left + Backward
  projections.emplace_back(sqrt_2, 0.0, -delta_angle);  // Spin right + Forward
  projections.emplace_back(-sqrt_2, 0.0, -delta_angle);  // Spin right + Backward
}

Poses MotionTable::getProjections(NodeSE2 * & node)
{
  Poses projection_list;
  for (uint i = 0; i != projections.size(); i++) {
    projection_list.push_back(getProjection(node, i));
  }

  return projection_list;
}

Pose MotionTable::getProjection(NodeSE2 * & node, const unsigned int & motion_index)
{
  return node->pose + projections[motion_index];
}

NodeSE2::NodeSE2(unsigned char & cost_in, const unsigned int index)
: parent(nullptr),
  _cell_cost(static_cast<float>(cost_in)),
  _accumulated_cost(std::numeric_limits<float>::max()),
  _index(index),
  _was_visited(false),
  _is_queued(false)
{
}

NodeSE2::~NodeSE2()
{
  parent = nullptr;
}

void NodeSE2::reset(const unsigned char & cost, const unsigned int index)
{
  parent = nullptr;
  _cell_cost = static_cast<float>(cost);
  _accumulated_cost = std::numeric_limits<float>::max();
  _index = index;
  _was_visited = false;
  _is_queued = false;
}

bool NodeSE2::isNodeValid(const bool & traverse_unknown) {
  // NOTE(stevemacenski): Right now, we do not check if the node has wrapped around
  // the regular grid (e.g. your node is on the edge of the costmap and i+1
  // goes to the other side). This check would add compute time and my assertion is
  // that if you do wrap around, the heuristic will be so high it'll be added far
  // in the queue that it will never be called if a valid path exists.
  // This is intentionally un-included to increase speed, but be aware. If this causes
  // trouble, please file a ticket and we can address it then.

  // TODO SE2 collision checking

  float & cost = this->getCost();

  // occupied node
  if (cost == OCCUPIED || cost == INSCRIBED) {
    return false;
  }

  // unknown node
  if (cost == UNKNOWN && !traverse_unknown) {
    return false;
  }

  return true;
}

float NodeSE2::getHeuristicCost(
  const Coordinates & node_coords,
  const Coordinates & goal_coordinates)
{
  return hypotf(
    goal_coordinates.x - node_coords.x,
    goal_coordinates.y - node_coords.y);
}

void NodeSE2::initMotionModel(
  const MotionModel & motion_model,
  unsigned int & size_x,
  unsigned int & num_angle_quantization,
  float min_turning_radius)
{
  // find the motion model selected
  switch(motion_model) {
    case MotionModel::DUBIN:
      _motion_model.initDubin(size_x, num_angle_quantization, min_turning_radius);
      break;
    case MotionModel::REEDS_SHEPP:
      _motion_model.initReedsShepp(size_x, num_angle_quantization, min_turning_radius);
      break;
    case MotionModel::BALKCOM_MASON:
      _motion_model.initBalkcomMason(size_x, num_angle_quantization);
      break;
    default:
      throw std::runtime_error("Invalid motion model for SE2 node. Please select between"
        " Dubin (Ackermann forward only),"
        " Reeds-Shepp (Ackermann forward and back),"
        " or Balkcom-Mason (Differential drive and omnidirectional) models.");
  }
}

void NodeSE2::getNeighbors(
  NodePtr & node,
  std::function<bool(const unsigned int&, smac_planner::NodeSE2*&)> & validityCheckerFunctor,
  NodeVector & neighbors)
{
  int index;
  NodePtr neighbor = nullptr;
  const Poses motion_projections = _motion_model.getProjections(node);

  for(unsigned int i = 0; i != motion_projections.size(); ++i) {
    index = NodeSE2::getIndex(
      static_cast<unsigned int>(motion_projections[i]._x),
      static_cast<unsigned int>(motion_projections[i]._y),
      static_cast<unsigned int>(motion_projections[i]._theta),
      _motion_model.size_x, _motion_model.num_angle_quantization);
    if (validityCheckerFunctor(index, neighbor)) {
      neighbor->setPose(motion_projections[i]);
      neighbors.push_back(neighbor);
    }
  }
}

}  // namespace smac_planner