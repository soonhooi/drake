#ifndef DRAKE_KINEMATICSCACHE_H
#define DRAKE_KINEMATICSCACHE_H

#include <Eigen/Core>
#include <Eigen/Geometry>
#include <unordered_map>
#include <vector>
#include <cassert>
#include <numeric>
#include <type_traits>
#include <stdexcept>
#include <utility>
#include "GradientVar.h"
#include "drakeGeometryUtil.h"
#include "RigidBody.h"


template <typename Scalar>
class KinematicsCacheElement
{
public:
  /*
   * Configuration dependent
   */
  Eigen::Transform<Scalar, SPACE_DIMENSION, Eigen::Isometry> transform_to_world;
  Eigen::Matrix<Scalar, TWIST_SIZE, Eigen::Dynamic, 0, TWIST_SIZE, DrakeJoint::MAX_NUM_VELOCITIES> motion_subspace_in_body; // gradient w.r.t. q_i only
  Eigen::Matrix<Scalar, TWIST_SIZE, Eigen::Dynamic, 0, TWIST_SIZE, DrakeJoint::MAX_NUM_VELOCITIES> motion_subspace_in_world; // gradient w.r.t. q
  Eigen::Matrix<Scalar, Eigen::Dynamic, Eigen::Dynamic, 0, DrakeJoint::MAX_NUM_VELOCITIES, DrakeJoint::MAX_NUM_POSITIONS> qdot_to_v; // gradient w.r.t. q
  Eigen::Matrix<Scalar, Eigen::Dynamic, Eigen::Dynamic, 0, DrakeJoint::MAX_NUM_POSITIONS, DrakeJoint::MAX_NUM_VELOCITIES> v_to_qdot; // gradient w.r.t. q
  Eigen::Matrix<Scalar, TWIST_SIZE, TWIST_SIZE> inertia_in_world;
  Eigen::Matrix<Scalar, TWIST_SIZE, TWIST_SIZE> crb_in_world;

  /*
   * Configuration and velocity dependent
   */
  Eigen::Matrix<Scalar, TWIST_SIZE, 1> twist_in_world; // gradient w.r.t. q only; gradient w.r.t. v is motion_subspace_in_world
  Eigen::Matrix<Scalar, TWIST_SIZE, 1> motion_subspace_in_body_dot_times_v; // gradient w.r.t. q_i and v_i only
  Eigen::Matrix<Scalar, TWIST_SIZE, 1> motion_subspace_in_world_dot_times_v; // gradient w.r.t. q and v

public:
  KinematicsCacheElement(int num_positions_joint, int num_velocities_joint) :
      motion_subspace_in_body(TWIST_SIZE, num_velocities_joint),
      motion_subspace_in_world(TWIST_SIZE, num_velocities_joint),
      qdot_to_v(num_velocities_joint, num_positions_joint),
      v_to_qdot(num_positions_joint, num_velocities_joint)
  {
    // empty
  }

public:
	EIGEN_MAKE_ALIGNED_OPERATOR_NEW
};

template <typename Scalar>
class KinematicsCache
{
private:
  std::unordered_map<RigidBody const *, KinematicsCacheElement<Scalar>, std::hash<RigidBody const *>, std::equal_to<RigidBody const *>, Eigen::aligned_allocator<std::pair<RigidBody const* const, KinematicsCacheElement<Scalar> > > > elements;
  Eigen::Matrix<Scalar, Eigen::Dynamic, 1> q;
  Eigen::Matrix<Scalar, Eigen::Dynamic, 1> v;
  bool velocity_vector_valid;
  bool position_kinematics_cached;
  bool jdotV_cached;
  bool inertias_cached;

public:
  KinematicsCache(const std::vector<std::shared_ptr<RigidBody>>& bodies) :
      q(Eigen::Matrix<Scalar, Eigen::Dynamic, 1>::Zero(getNumPositions(bodies), 1)),
      v(Eigen::Matrix<Scalar, Eigen::Dynamic, 1>::Zero(getNumVelocities(bodies), 1)),
      velocity_vector_valid(false)
  {
    for (const auto& body_shared_ptr : bodies) {
      const RigidBody& body = *body_shared_ptr;
      int num_positions_joint = body.hasParent() ? body.getJoint().getNumPositions() : 0;
      int num_velocities_joint = body.hasParent() ? body.getJoint().getNumVelocities() : 0;
      elements.insert({&body, KinematicsCacheElement<Scalar>(num_positions_joint, num_velocities_joint)});
    }
    invalidate();
  }

  KinematicsCacheElement<Scalar>& getElement(const RigidBody& body)
  {
    return elements.at(&body);
  }

  const KinematicsCacheElement<Scalar>& getElement(const RigidBody& body) const
  {
    return elements.at(&body);
  }

  template <typename Derived>
  void initialize(const Eigen::MatrixBase<Derived>& q) {
    static_assert(Derived::ColsAtCompileTime == 1, "q must be a vector");
    static_assert(std::is_same<typename Derived::Scalar, Scalar>::value, "scalar type of q must match scalar type of KinematicsCache");
    assert(this->q.rows() == q.rows());
    this->q = q;
    invalidate();
    velocity_vector_valid = false;
  }

  template <typename DerivedQ, typename DerivedV>
  void initialize(const Eigen::MatrixBase<DerivedQ>& q, const Eigen::MatrixBase<DerivedV>& v) {
    initialize(q); // also invalidates
    static_assert(DerivedV::ColsAtCompileTime == 1, "v must be a vector");
    static_assert(std::is_same<typename DerivedV::Scalar, Scalar>::value, "scalar type of v must match scalar type of KinematicsCache");
    assert(this->v.rows() == v.rows());
    this->v = v;
    velocity_vector_valid = true;
  }

  void checkCachedKinematicsSettings(bool velocity_kinematics_required, bool jdot_times_v_required, const std::string& method_name) const
  {
    if (!position_kinematics_cached) {
      throw std::runtime_error(method_name + " requires position kinematics, which have not been cached. Please call doKinematics.");
    }
    if (velocity_kinematics_required && !hasV()) {
      throw std::runtime_error(method_name + " requires velocity kinematics, which have not been cached. Please call doKinematics with a velocity vector.");
    }
    if (jdot_times_v_required && !jdotV_cached) {
      throw std::runtime_error(method_name + " requires Jdot times v, which has not been cached. Please call doKinematics with a velocity vector and compute_JdotV set to true.");
    }
  }


  const Eigen::Matrix<Scalar, Eigen::Dynamic, 1>& getQ() const
  {
    return q;
  }

  const Eigen::Matrix<Scalar, Eigen::Dynamic, 1>& getV() const
  {
    if (hasV())
      return v;
    else
      throw std::runtime_error("Kinematics cache has no valid velocity vector.");
  }

  bool hasV() const
  {
    return velocity_vector_valid;
  }

  void setInertiasCached() {
    inertias_cached = true;
  }

  bool areInertiasCached() {
    return inertias_cached;
  }

  void setPositionKinematicsCached() {
    position_kinematics_cached = true;
  }

  void setJdotVCached(bool jdotV_cached) {
    this->jdotV_cached = jdotV_cached;
  }

private:
  void invalidate()
  {
    position_kinematics_cached = false;
    jdotV_cached = false;
    inertias_cached = false;
  }

  static int getNumPositions(const std::vector<std::shared_ptr<RigidBody>>& bodies) {
    auto add_num_positions = [] (int result, std::shared_ptr<RigidBody> body_ptr) -> int {
      return body_ptr->hasParent() ? result + body_ptr->getJoint().getNumPositions() : result;
    };
    return std::accumulate(bodies.begin(), bodies.end(), 0, add_num_positions);
  }

  static int getNumVelocities(const std::vector<std::shared_ptr<RigidBody>>& bodies) {
    auto add_num_velocities = [] (int result, std::shared_ptr<RigidBody> body_ptr) -> int {
      return body_ptr->hasParent() ? result + body_ptr->getJoint().getNumVelocities() : result;
    };
    return std::accumulate(bodies.begin(), bodies.end(), 0, add_num_velocities);
  }

public:
	EIGEN_MAKE_ALIGNED_OPERATOR_NEW
};


#endif //DRAKE_KINEMATICSCACHE_H
