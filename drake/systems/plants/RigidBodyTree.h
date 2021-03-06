#ifndef DRAKE_RIGIDBODYTREE_H
#define DRAKE_RIGIDBODYTREE_H

#include <Eigen/Dense>
#include <Eigen/LU>
#include <set>
#include <unordered_map>
#include <Eigen/StdVector>

#include "collision/DrakeCollision.h"
#include "shapes/DrakeShapes.h"
#include "KinematicPath.h"
#include "ForceTorqueMeasurement.h"
#include "drakeUtil.h"
#include <stdexcept>


#undef DLLEXPORT_RBM
#if defined(WIN32) || defined(WIN64)
  #if defined(drakeRBM_EXPORTS)
    #define DLLEXPORT_RBM __declspec( dllexport )
  #else
    #define DLLEXPORT_RBM __declspec( dllimport )
  #endif
#else
  #define DLLEXPORT_RBM
#endif

#include "RigidBody.h"
#include "RigidBodyFrame.h"
#include "KinematicsCache.h"

#define BASIS_VECTOR_HALF_COUNT 2  //number of basis vectors over 2 (i.e. 4 basis vectors in this case)
#define EPSILON 10e-8
#define MIN_RADIUS 1e-7

typedef Eigen::Matrix<double, 3, BASIS_VECTOR_HALF_COUNT> Matrix3kd;

class DLLEXPORT_RBM RigidBodyActuator
{
public:
  RigidBodyActuator(std::string _name, std::shared_ptr<RigidBody> _body, double _reduction = 1.0) :
    name(_name), body(_body), reduction(_reduction) {};

  std::string name;
  std::shared_ptr<RigidBody> body;
  double reduction;
};

class DLLEXPORT_RBM RigidBodyLoop
{
public:
  RigidBodyLoop(const std::shared_ptr<RigidBodyFrame>& _frameA, const std::shared_ptr<RigidBodyFrame>& _frameB, const Eigen::Vector3d& _axis) :
    frameA(_frameA), frameB(_frameB), axis(_axis) {};

  std::shared_ptr<RigidBodyFrame> frameA, frameB;
  Eigen::Vector3d axis;

  friend std::ostream& operator<<(std::ostream& os, const RigidBodyLoop& obj);

public:
	EIGEN_MAKE_ALIGNED_OPERATOR_NEW
};

class DLLEXPORT_RBM RigidBodyTree
{
public:
  RigidBodyTree(const std::string &urdf_filename, const DrakeJoint::FloatingBaseType floating_base_type = DrakeJoint::ROLLPITCHYAW);
  RigidBodyTree(void);
  virtual ~RigidBodyTree(void);

  bool addRobotFromURDFString(const std::string &xml_string, const std::string &root_dir = ".", const DrakeJoint::FloatingBaseType floating_base_type = DrakeJoint::ROLLPITCHYAW);
  bool addRobotFromURDFString(const std::string &xml_string, std::map<std::string,std::string>& package_map, const std::string &root_dir = ".", const DrakeJoint::FloatingBaseType floating_base_type = DrakeJoint::ROLLPITCHYAW);
  bool addRobotFromURDF(const std::string &urdf_filename, const DrakeJoint::FloatingBaseType floating_base_type = DrakeJoint::ROLLPITCHYAW);
  bool addRobotFromURDF(const std::string &urdf_filename, std::map<std::string,std::string>& package_map, const DrakeJoint::FloatingBaseType floating_base_type = DrakeJoint::ROLLPITCHYAW);

  void addFrame(const std::shared_ptr<RigidBodyFrame>& frame);

  std::map<std::string, int> computePositionNameToIndexMap() const;

  void surfaceTangents(Eigen::Map<Eigen::Matrix3Xd> const & normals, std::vector< Eigen::Map<Eigen::Matrix3Xd> > & tangents) const;

  void compile(void);  // call me after the model is loaded

  void getRandomConfiguration(Eigen::VectorXd& q, std::default_random_engine& generator) const;

  // akin to the coordinateframe names in matlab
  std::string getPositionName(int position_num) const;
  std::string getVelocityName(int velocity_num) const;
  std::string getStateName(int state_num) const;

  template <typename DerivedQ>
  KinematicsCache<typename DerivedQ::Scalar> doKinematics(const Eigen::MatrixBase<DerivedQ>& q) {
    KinematicsCache<typename DerivedQ::Scalar> ret(bodies);
    ret.initialize(q);
    doKinematics(ret);
    return ret;
  }

  template <typename DerivedQ, typename DerivedV>
  KinematicsCache<typename DerivedQ::Scalar> doKinematics(const Eigen::MatrixBase<DerivedQ>& q, const Eigen::MatrixBase<DerivedV>& v, bool compute_JdotV = true) {
    KinematicsCache<typename DerivedQ::Scalar> ret(bodies);
    ret.initialize(q, v);
    doKinematics(ret, compute_JdotV);
    return ret;
  }

  template <typename Scalar>
  void doKinematics(KinematicsCache<Scalar>& cache, bool compute_JdotV = false) const {
    using namespace std;
    using namespace Eigen;

    const auto& q = cache.getQ();
    if (!initialized)
      throw runtime_error("RigidBodyTree::doKinematics: call compile first.");

    compute_JdotV = compute_JdotV && cache.hasV(); // no sense in computing Jdot times v if v is not passed in

    cache.setPositionKinematicsCached(); // doing this here because there is a geometricJacobian call within doKinematics below which checks for this

    for (int i = 0; i < bodies.size(); i++) {
      RigidBody& body = *bodies[i];
      KinematicsCacheElement<Scalar>& element = cache.getElement(body);

      if (body.hasParent()) {
        const KinematicsCacheElement<Scalar>& parent_element = cache.getElement(*body.parent);
        const DrakeJoint& joint = body.getJoint();
        auto q_body = q.middleRows(body.position_num_start, joint.getNumPositions());

        // transform
        auto T_body_to_parent = joint.getTransformToParentBody().cast<Scalar>() * joint.jointTransform(q_body);
        element.transform_to_world = parent_element.transform_to_world * T_body_to_parent;

        // motion subspace in body frame
        Matrix<Scalar, Dynamic, Dynamic>* dSdq = nullptr;
        joint.motionSubspace(q_body, element.motion_subspace_in_body, dSdq);

        // motion subspace in world frame
        element.motion_subspace_in_world = transformSpatialMotion(element.transform_to_world, element.motion_subspace_in_body);

        joint.qdot2v(q_body, element.qdot_to_v, nullptr);
        joint.v2qdot(q_body, element.v_to_qdot, nullptr);

        if (cache.hasV()) {
          const auto& v = cache.getV();
          if (joint.getNumVelocities()==0) { // for fixed joints
            element.twist_in_world = parent_element.twist_in_world;
            if (compute_JdotV) {
              element.motion_subspace_in_world_dot_times_v = parent_element.motion_subspace_in_world_dot_times_v;
            }
          } else {
            // twist
            auto v_body = v.middleRows(body.velocity_num_start, joint.getNumVelocities());

            Eigen::Matrix<Scalar, TWIST_SIZE, 1> joint_twist = element.motion_subspace_in_world * v_body;
            element.twist_in_world = parent_element.twist_in_world;
            element.twist_in_world.noalias() += joint_twist;

            if (compute_JdotV) {
              // Sdotv
              joint.motionSubspaceDotTimesV(q_body, v_body, element.motion_subspace_in_body_dot_times_v, nullptr, nullptr);

              // Jdotv
              auto joint_accel = crossSpatialMotion(element.twist_in_world, joint_twist);
              joint_accel += transformSpatialMotion(element.transform_to_world, element.motion_subspace_in_body_dot_times_v);
              element.motion_subspace_in_world_dot_times_v = parent_element.motion_subspace_in_world_dot_times_v + joint_accel;
            }
          }
        }
      }
      else {
        element.transform_to_world.setIdentity();
        // motion subspace in body frame is empty
        // motion subspace in world frame is empty
        // qdot to v is empty
        // v to qdot is empty

        if (cache.hasV()) {
          element.twist_in_world.setZero();
          element.motion_subspace_in_body.setZero();
          element.motion_subspace_in_world.setZero();
          element.qdot_to_v.setZero();
          element.v_to_qdot.setZero();

          if (compute_JdotV) {
            element.motion_subspace_in_body_dot_times_v.setZero();
            element.motion_subspace_in_world_dot_times_v.setZero();
          }
        }
      }
    }

    cache.setJdotVCached(compute_JdotV && cache.hasV());
  };

  bool isBodyPartOfRobot(const RigidBody& body, const std::set<int>& robotnum) const;

  double getMass(const std::set<int>& robotnum = default_robot_num_set) const;

  template <typename Scalar>
  Eigen::Matrix<Scalar, SPACE_DIMENSION, 1> centerOfMass(KinematicsCache<Scalar> &cache, const std::set<int> &robotnum = default_robot_num_set) const;

  template <typename Scalar>
  Eigen::Matrix<Scalar, TWIST_SIZE, Eigen::Dynamic> worldMomentumMatrix(KinematicsCache<Scalar>& cache, const std::set<int>& robotnum = default_robot_num_set, bool in_terms_of_qdot = false) const;

  template <typename Scalar>
  Eigen::Matrix<Scalar, TWIST_SIZE, 1> worldMomentumMatrixDotTimesV(KinematicsCache<Scalar>& cache, const std::set<int>& robotnum = default_robot_num_set) const;

  template <typename Scalar>
  Eigen::Matrix<Scalar, TWIST_SIZE, Eigen::Dynamic> centroidalMomentumMatrix(KinematicsCache<Scalar>& cache, const std::set<int>& robotnum = default_robot_num_set, bool in_terms_of_qdot = false) const;

  template <typename Scalar>
  Eigen::Matrix<Scalar, TWIST_SIZE, 1> centroidalMomentumMatrixDotTimesV(KinematicsCache<Scalar>& cache, const std::set<int>& robotnum = default_robot_num_set) const;

  template <typename Scalar>
  Eigen::Matrix<Scalar, SPACE_DIMENSION, Eigen::Dynamic> centerOfMassJacobian(KinematicsCache<Scalar>& cache, const std::set<int>& robotnum = default_robot_num_set, bool in_terms_of_qdot = false) const;

  template <typename Scalar>
  Eigen::Matrix<Scalar, SPACE_DIMENSION, 1> centerOfMassJacobianDotTimesV(KinematicsCache<Scalar>& cache, const std::set<int>& robotnum = default_robot_num_set) const;

  template <typename DerivedA, typename DerivedB, typename DerivedC>
  void jointLimitConstraints(Eigen::MatrixBase<DerivedA> const & q, Eigen::MatrixBase<DerivedB> &phi, Eigen::MatrixBase<DerivedC> &J) const;

  size_t getNumJointLimitConstraints() const;

  int getNumContacts(const std::set<int> &body_idx) const;// = emptyIntSet);

  template <typename Derived>
  void getContactPositions(const KinematicsCache<typename Derived::Scalar>& cache, Eigen::MatrixBase<Derived> &pos, const std::set<int> &body_idx) const;// = emptyIntSet);

  template <typename Derived>
  void getContactPositionsJac(const KinematicsCache<typename Derived::Scalar>& cache, Eigen::MatrixBase<Derived> &J, const std::set<int> &body_idx) const;// = emptyIntSet);

//  template <typename Derived>
//  void getContactPositionsJacDot(MatrixBase<Derived> &Jdot, const std::set<int> &body_idx);// = emptyIntSet);
//

  /**
   * Computes CoP in world frame. Normal and point on contact plane should be in world frame too.
   */
  template <typename DerivedNormal, typename DerivedPoint>
  std::pair<Eigen::Vector3d, double> resolveCenterOfPressure(const KinematicsCache<double>& cache, const std::vector< ForceTorqueMeasurement > & force_torque_measurements, const Eigen::MatrixBase<DerivedNormal> & normal, const Eigen::MatrixBase<DerivedPoint> & point_on_contact_plane) const;

  void findAncestorBodies(std::vector<int>& ancestor_bodies, int body) const;

  KinematicPath findKinematicPath(int start_body_or_frame_idx, int end_body_or_frame_idx) const;

  template <typename Scalar>
  Eigen::Matrix<Scalar, Eigen::Dynamic, Eigen::Dynamic> massMatrix(KinematicsCache<Scalar>& cache) const;

  template <typename Scalar>
  Eigen::Matrix<Scalar, Eigen::Dynamic, 1> dynamicsBiasTerm(KinematicsCache<Scalar>& cache, const eigen_aligned_unordered_map<RigidBody const *, Eigen::Matrix<Scalar, TWIST_SIZE, 1> >& f_ext) const;

  template <typename Scalar>
  Eigen::Matrix<Scalar, Eigen::Dynamic, 1> inverseDynamics(KinematicsCache<Scalar>& cache, const eigen_aligned_unordered_map<RigidBody const *, Eigen::Matrix<Scalar, TWIST_SIZE, 1> >& f_ext, const Eigen::Matrix<Scalar, Eigen::Dynamic, 1>& vd) const;

  template <typename DerivedV>
  Eigen::Matrix<typename DerivedV::Scalar, Eigen::Dynamic, 1> frictionTorques(Eigen::MatrixBase<DerivedV> const & v) const;

  template <typename Scalar, typename DerivedPoints> // not necessarily any relation between the two; a major use case is having an AutoDiff KinematicsCache, but double points matrix
  Eigen::Matrix<Scalar, Eigen::Dynamic, DerivedPoints::ColsAtCompileTime> forwardKin(
      const KinematicsCache<Scalar> &cache, const Eigen::MatrixBase<DerivedPoints> &points, int current_body_or_frame_ind, int new_body_or_frame_ind, int rotation_type) const
  {
    cache.checkCachedKinematicsSettings(false, false, "forwardKin"); // rely on forwardJac for gradient cache check

    int npoints = static_cast<int>(points.cols());

    // compute rotation and translation
    auto T = relativeTransform(cache, new_body_or_frame_ind, current_body_or_frame_ind);

    // transform points to new frame
    Eigen::Matrix<Scalar, Eigen::Dynamic, DerivedPoints::ColsAtCompileTime> x(SPACE_DIMENSION + rotationRepresentationSize(rotation_type), npoints);
    x.template topRows<SPACE_DIMENSION>().noalias() = T * points.template cast<Scalar>();

    // convert rotation representation
    auto qrot = rotmat2Representation(T.linear(), rotation_type);
    x.bottomRows(qrot.rows()).colwise() = qrot;

    return x;
  };

  template <typename Scalar, typename DerivedPoints>
  Eigen::Matrix<Scalar, Eigen::Dynamic, Eigen::Dynamic> forwardKinJacobian(const KinematicsCache<Scalar>& cache, const Eigen::MatrixBase<DerivedPoints>& points, int current_body_or_frame_ind, int new_body_or_frame_ind, int rotation_type, bool in_terms_of_qdot) const;

  template <typename Scalar>
  Eigen::Matrix<Scalar, Eigen::Dynamic, Eigen::Dynamic> forwardKinPositionGradient(const KinematicsCache<Scalar>& cache, int npoints, int current_body_or_frame_ind, int new_body_or_frame_ind) const;

  template <typename Scalar, typename DerivedPoints>
  Eigen::Matrix<Scalar, Eigen::Dynamic, 1> forwardJacDotTimesV(const KinematicsCache<Scalar>& cache, const Eigen::MatrixBase<DerivedPoints>& points, int body_or_frame_ind, int base_or_frame_ind, int rotation_type) const;

  template<typename Scalar>
  Eigen::Matrix<Scalar, TWIST_SIZE, Eigen::Dynamic> geometricJacobian(const KinematicsCache<Scalar>& cache, int base_body_or_frame_ind, int end_effector_body_or_frame_ind, int expressed_in_body_or_frame_ind, bool in_terms_of_qdot = false, std::vector<int>* v_indices = nullptr) const;

  template <typename Scalar>
  Eigen::Matrix<Scalar, TWIST_SIZE, 1> geometricJacobianDotTimesV(const KinematicsCache<Scalar>& cache, int base_body_or_frame_ind, int end_effector_body_or_frame_ind, int expressed_in_body_or_frame_ind) const;

  template <typename Scalar>
  Eigen::Matrix<Scalar, TWIST_SIZE, 1> relativeTwist(const KinematicsCache<Scalar>& cache, int base_or_frame_ind, int body_or_frame_ind, int expressed_in_body_or_frame_ind) const;

  template <typename Scalar>
  Eigen::Matrix<Scalar, TWIST_SIZE, 1> transformSpatialAcceleration(const KinematicsCache<Scalar>& cache, const Eigen::Matrix<Scalar, TWIST_SIZE, 1>& spatial_acceleration, int base_or_frame_ind, int body_or_frame_ind, int old_body_or_frame_ind, int new_body_or_frame_ind) const;

  template<typename Scalar>
  Eigen::Transform<Scalar, SPACE_DIMENSION, Eigen::Isometry> relativeTransform(const KinematicsCache<Scalar>& cache, int base_or_frame_ind, int body_or_frame_ind) const;

  template <typename Scalar>
  void computeContactJacobians(const KinematicsCache<Scalar>& cache, Eigen::Ref<const Eigen::VectorXi> const & idxA, Eigen::Ref<const Eigen::VectorXi> const & idxB, Eigen::Ref<const Eigen::Matrix3Xd> const & xA, Eigen::Ref<const Eigen::Matrix3Xd> const & xB, Eigen::Matrix<Scalar, Eigen::Dynamic, Eigen::Dynamic> & J) const;

  DrakeCollision::ElementId addCollisionElement(const RigidBody::CollisionElement& element, const std::shared_ptr<RigidBody>& body, std::string group_name);

  void updateCollisionElements(const RigidBody& body, const Eigen::Transform<double, 3, Eigen::Isometry>& transform_to_world);

  void updateStaticCollisionElements();

  void updateDynamicCollisionElements(const KinematicsCache<double>& kin_cache);

  void getTerrainContactPoints(const RigidBody& body, Eigen::Matrix3Xd &terrain_points) const;

  bool collisionRaycast(const KinematicsCache<double>& cache, const Eigen::Matrix3Xd &origins, const Eigen::Matrix3Xd &ray_endpoints, Eigen::VectorXd &distances, bool use_margins=false);

  bool collisionDetect(const KinematicsCache<double>& cache,
                       Eigen::VectorXd& phi,
                       Eigen::Matrix3Xd& normal,
                       Eigen::Matrix3Xd& xA,
                       Eigen::Matrix3Xd& xB,
                       std::vector<int>& bodyA_idx,
                       std::vector<int>& bodyB_idx,
                       const std::vector<DrakeCollision::ElementId>& ids_to_check,
                       bool use_margins);

  bool collisionDetect(const KinematicsCache<double>& cache,
                       Eigen::VectorXd& phi,
                       Eigen::Matrix3Xd& normal,
                       Eigen::Matrix3Xd& xA, Eigen::Matrix3Xd& xB,
                       std::vector<int>& bodyA_idx,
                       std::vector<int>& bodyB_idx,
                       const std::vector<int>& bodies_idx,
                       const std::set<std::string>& active_element_groups,
                       bool use_margins = true);

  bool collisionDetect(const KinematicsCache<double>& cache,
                       Eigen::VectorXd& phi, Eigen::Matrix3Xd& normal,
                       Eigen::Matrix3Xd& xA, Eigen::Matrix3Xd& xB,
                       std::vector<int>& bodyA_idx,
                       std::vector<int>& bodyB_idx,
                       const std::vector<int>& bodies_idx,
                       bool use_margins = true);

  bool collisionDetect(const KinematicsCache<double>& cache,
                       Eigen::VectorXd& phi, Eigen::Matrix3Xd& normal,
                       Eigen::Matrix3Xd& xA, Eigen::Matrix3Xd& xB,
                       std::vector<int>& bodyA_idx,
                       std::vector<int>& bodyB_idx,
                       const std::set<std::string>& active_element_groups,
                       bool use_margins = true);

  bool collisionDetect(const KinematicsCache<double>& cache,
                       Eigen::VectorXd& phi, Eigen::Matrix3Xd& normal,
                       Eigen::Matrix3Xd& xA, Eigen::Matrix3Xd& xB,
                       std::vector<int>& bodyA_idx,
                       std::vector<int>& bodyB_idx,
                        bool use_margins = true);


  bool allCollisions(const KinematicsCache<double>& cache,
                     std::vector<int>& bodyA_idx, std::vector<int>& bodyB_idx,
                     Eigen::Matrix3Xd& ptsA, Eigen::Matrix3Xd& ptsB,
                     bool use_margins = true);

  void potentialCollisions(const KinematicsCache<double>& cache,
                           Eigen::VectorXd& phi,
                           Eigen::Matrix3Xd& normal,
                           Eigen::Matrix3Xd& xA,
                           Eigen::Matrix3Xd& xB,
                           std::vector<int>& bodyA_idx,
                           std::vector<int>& bodyB_idx,
                           bool use_margins = true);
  //bool closestDistanceAllBodies(VectorXd& distance, MatrixXd& Jd);

  virtual std::vector<size_t> collidingPoints(const KinematicsCache<double>& cache,
                                              const std::vector<Eigen::Vector3d>& points,
        double collision_threshold);

  void warnOnce(const std::string& id, const std::string& msg);

  std::shared_ptr<RigidBody> findLink(std::string linkname, int robot=-1) const;
  int findLinkId(const std::string& linkname, int robot = -1) const;
  std::shared_ptr<RigidBody> findJoint(std::string jointname, int robot=-1) const;
  int findJointId(const std::string& linkname, int robot = -1) const;
  //@param robot   the index of the robot. robot = -1 means to look at all the robots

  std::string getBodyOrFrameName(int body_or_frame_id) const;
  //@param body_or_frame_id   the index of the body or the id of the frame.

  // TODO: remove parseBodyOrFrameID methods
  template <typename Scalar>
  int parseBodyOrFrameID(const int body_or_frame_id, Eigen::Transform<Scalar, 3, Eigen::Isometry>* Tframe) const;
  int parseBodyOrFrameID(const int body_or_frame_id) const;

  template <typename Scalar>
  Eigen::Matrix<Scalar, Eigen::Dynamic, 1> positionConstraints(const KinematicsCache<Scalar>& cache) const;

  template<typename Scalar>
  Eigen::Matrix<Scalar, Eigen::Dynamic, Eigen::Dynamic> positionConstraintsJacobian(const KinematicsCache<Scalar> &cache) const;

  size_t getNumPositionConstraints() const;

  template <typename Derived>
  Eigen::Matrix<typename Derived::Scalar, Derived::RowsAtCompileTime, Eigen::Dynamic> transformVelocityMappingToPositionDotMapping(
      const KinematicsCache<typename Derived::Scalar>& cache, const Eigen::MatrixBase<Derived>& mat) const;
  
  /*
  template <typename Derived>
  Eigen::Matrix<typename Derived::Scalar, Derived::RowsAtCompileTime, Eigen::Dynamic> transformPositionDotMappingToVelocityMapping(
      const KinematicsCache<typename Derived::Scalar>& cache, const Eigen::MatrixBase<Derived>& mat) const;
  */
  
  template <typename Derived>
Eigen::Matrix<typename Derived::Scalar, Derived::RowsAtCompileTime, Eigen::Dynamic> transformPositionDotMappingToVelocityMapping(
    const KinematicsCache<typename Derived::Scalar>& cache, const Eigen::MatrixBase<Derived>& mat) const
{
  Eigen::Matrix<typename Derived::Scalar, Derived::RowsAtCompileTime, Eigen::Dynamic> ret(mat.rows(), num_velocities);
  int ret_col_start = 0;
  int mat_col_start = 0;
  for (auto it = bodies.begin(); it != bodies.end(); ++it) {
    RigidBody& body = **it;
    if (body.hasParent()) {
      const DrakeJoint& joint = body.getJoint();
      const auto& element = cache.getElement(body);
      ret.middleCols(ret_col_start, joint.getNumVelocities()).noalias() = mat.middleCols(mat_col_start, joint.getNumPositions()) * element.v_to_qdot;
      ret_col_start += joint.getNumVelocities();
      mat_col_start += joint.getNumPositions();
    }
  }
  return ret;
};

  template <typename Derived>
Eigen::Matrix<typename Derived::Scalar, Derived::RowsAtCompileTime, Eigen::Dynamic> compactToFull(
    const Eigen::MatrixBase<Derived>& compact, const std::vector<int>& joint_path, bool in_terms_of_qdot) const {
  /*
   * This method is used after calling geometric Jacobian, where compact is the Jacobian on the joints that are on the kinematic path; if we want to reconstruct the full Jacobian on all joints, then we should call this method.
   */
  int ncols = in_terms_of_qdot ? num_positions : num_velocities;
  Eigen::Matrix<typename Derived::Scalar, Derived::RowsAtCompileTime, Eigen::Dynamic> full(compact.rows(), ncols);
  full.setZero();
  int compact_col_start = 0;
  for (std::vector<int>::const_iterator it = joint_path.begin(); it != joint_path.end(); ++it) {
    RigidBody& body = *bodies[*it];
    int ncols_joint = in_terms_of_qdot ? body.getJoint().getNumPositions() : body.getJoint().getNumVelocities();
    int col_start = in_terms_of_qdot ? body.position_num_start : body.velocity_num_start;
    full.middleCols(col_start, ncols_joint) = compact.middleCols(compact_col_start, ncols_joint);
    compact_col_start += ncols_joint;
  }
  return full;
};
public:
  static const std::set<int> default_robot_num_set;

  std::vector<std::string> robot_name;

  int num_positions;
  int num_velocities;
  Eigen::VectorXd joint_limit_min;
  Eigen::VectorXd joint_limit_max;

  // Rigid body objects
  std::vector<std::shared_ptr<RigidBody> > bodies;

  // Rigid body frames
  std::vector<std::shared_ptr<RigidBodyFrame> > frames;

  // Rigid body actuators
  std::vector<RigidBodyActuator,Eigen::aligned_allocator<RigidBodyActuator> > actuators;

  // Rigid body loops
  std::vector<RigidBodyLoop,Eigen::aligned_allocator<RigidBodyLoop> > loops;

  Eigen::Matrix<double,TWIST_SIZE,1> a_grav;
  Eigen::MatrixXd B;  // the B matrix maps inputs into joint-space forces

private:
  //helper functions for contactConstraints
  template <typename Scalar>
  void accumulateContactJacobian(const KinematicsCache<Scalar> &cache, const int bodyInd, Eigen::Matrix3Xd const &bodyPoints, std::vector<size_t> const &cindA, std::vector<size_t> const &cindB,
                                 Eigen::Matrix<Scalar, Eigen::Dynamic, Eigen::Dynamic> &J) const;

  template <typename Scalar>
  void updateCompositeRigidBodyInertias(KinematicsCache<Scalar>& cache) const;

  bool initialized;


  // collision_model and collision_model_no_margins both maintain
  // a collection of the collision geometry in the RBM for use in
  // collision detection of different kinds. collision_model has
  // small margins applied to all collision geometry when that
  // geometry is added, to improve the numerical stability of
  // contact gradients taken using the model. collision_model_no_margins
  // does not apply these margins, such that it can be used for
  // precise raycasting, e.g. for simulating a laser scanner
  // These models are switched between with the use_margins flag
  // to collision-relevant methods of the RBM.
  std::unique_ptr< DrakeCollision::Model > collision_model;
  //std::shared_ptr< DrakeCollision::Model > collision_model_no_margins;
public:
  EIGEN_MAKE_ALIGNED_OPERATOR_NEW

// The following was required for building w/ DLLEXPORT_RBM on windows (due to the unique_ptrs).  See
// http://stackoverflow.com/questions/8716824/cannot-access-private-member-error-only-when-class-has-export-linkage
private:
  RigidBodyTree(const RigidBodyTree &);
  RigidBodyTree & operator=(const RigidBodyTree &) { return *this; }

  std::set<std::string> already_printed_warnings;
};


#endif
