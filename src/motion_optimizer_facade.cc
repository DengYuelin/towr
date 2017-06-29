/**
 @file    motion_optimizer.cc
 @author  Alexander W. Winkler (winklera@ethz.ch)
 @date    Nov 20, 2016
 @brief   Brief description
 */

#include <xpp/opt/motion_optimizer_facade.h>

#include <Eigen/Dense>
#include <kindr/Core>

#include <xpp/endeffectors.h>

#include <xpp/ipopt_adapter.h>
#include <xpp/opt/angular_state_converter.h>
#include <xpp/opt/cost_constraint_factory.h>
#include <xpp/opt/polynomial_spline.h>
#include <xpp/opt/variables/contact_schedule.h>
#include <xpp/opt/variables/endeffectors_force.h>
#include <xpp/opt/variables/endeffectors_motion.h>
#include <xpp/opt/variables/variable_names.h>
#include <xpp/snopt_adapter.h>

namespace xpp {
namespace opt {

MotionOptimizerFacade::MotionOptimizerFacade ()
{
  opt_variables_ = std::make_shared<Composite>("nlp_variables", true);
}

MotionOptimizerFacade::~MotionOptimizerFacade ()
{
}

void
MotionOptimizerFacade::BuildDefaultStartStance ()
{
  State3d base;
  double offset_x = 0.0;
  inital_base_.lin.p_ << offset_x+0.000350114, -1.44379e-7, 0.573311;
  inital_base_.lin.v_ << 0.000137518, -4.14828e-07,  0.000554118;
  inital_base_.lin.a_ << 0.000197966, -5.72241e-07, -5.13328e-06;

  inital_base_.ang.p_ << 0.0, 0.0, 0.0; // euler (roll, pitch, yaw)

  initial_ee_W_ = motion_parameters_->GetNominalStanceInBase();
  for (auto ee : initial_ee_W_.GetEEsOrdered()) {
    initial_ee_W_.At(ee) += base.lin.p_;
    initial_ee_W_.At(ee).z() = 0.0;
  }
}

void
MotionOptimizerFacade::BuildVariables ()
{
  // initialize the contact schedule
  auto contact_schedule = std::make_shared<ContactSchedule>(
      motion_parameters_->GetContactSchedule());

  // initialize the ee_motion with the fixed parameters
  auto ee_motion = std::make_shared<EndeffectorsMotion>(initial_ee_W_,
                                                        *contact_schedule);

  double T = motion_parameters_->GetTotalTime();

  auto base_linear = std::make_shared<PolynomialSpline>(id::base_linear);
  base_linear->Init(T, motion_parameters_->duration_polynomial_,
                    inital_base_.lin.p_);

  // Represent roll, pitch and yaw by a spline each.
  // These angles are to be interpreted as mapping a vector expressed in world
  // frame to a base frame (not the other way around).
  auto base_angular = std::make_shared<PolynomialSpline>(id::base_angular);
  base_angular->Init(T, motion_parameters_->duration_polynomial_,
                     inital_base_.ang.p_);

  auto force = std::make_shared<EndeffectorsForce>(motion_parameters_->load_dt_,
                                                   *contact_schedule);
  opt_variables_->ClearComponents();
  opt_variables_->AddComponent(base_angular);
  opt_variables_->AddComponent(base_linear);
  opt_variables_->AddComponent(ee_motion);
  opt_variables_->AddComponent(force);
  opt_variables_->AddComponent(contact_schedule);
}

void
MotionOptimizerFacade::SolveProblem (NlpSolver solver)
{
  BuildVariables();

  CostConstraintFactory factory;
  factory.Init(opt_variables_, motion_parameters_,
               initial_ee_W_, inital_base_, final_base_);

  nlp.Init(opt_variables_);

  auto constraints = std::make_unique<Composite>("constraints", true);
  for (ConstraintName name : motion_parameters_->GetUsedConstraints()) {
    constraints->AddComponent(factory.GetConstraint(name));
  }
  constraints->Print();
  nlp.AddConstraint(std::move(constraints));


  auto costs = std::make_unique<Composite>("costs", false);
  for (const auto& pair : motion_parameters_->GetCostWeights()) {
    CostName name = pair.first;
    costs->AddComponent(factory.GetCost(name));
  }
  costs->Print();
  nlp.AddCost(std::move(costs));


  switch (solver) {
    case Ipopt:   IpoptAdapter::Solve(nlp); break;
    case Snopt:   SnoptAdapter::Solve(nlp); break;
    default: assert(false); // solver not implemented
  }
}

MotionOptimizerFacade::RobotStateVec
MotionOptimizerFacade::GetTrajectory (double dt) const
{
  RobotStateVec trajectory;

  auto base_lin         = std::dynamic_pointer_cast<PolynomialSpline>  (opt_variables_->GetComponent(id::base_linear));
  auto base_ang         = std::dynamic_pointer_cast<PolynomialSpline>  (opt_variables_->GetComponent(id::base_angular));
  auto ee_motion        = std::dynamic_pointer_cast<EndeffectorsMotion>(opt_variables_->GetComponent(id::endeffectors_motion));
  auto contact_schedule = std::dynamic_pointer_cast<ContactSchedule>   (opt_variables_->GetComponent(id::contact_schedule));
  auto ee_forces        = std::dynamic_pointer_cast<EndeffectorsForce> (opt_variables_->GetComponent(id::endeffector_force));

  AngularStateConverter converter(base_ang);

  double t=0.0;
  double T = motion_parameters_->GetTotalTime();
  while (t<=T+1e-5) {

    RobotStateCartesian state(initial_ee_W_.GetCount());

    // zmp_ move to own class
    State3d base; // positions and orientations set to zero
    base.lin = base_lin->GetPoint(t);
    StateLin3d rpy = base_ang->GetPoint(t);

//    std::cout << "rpy: " << rpy.p_.transpose() << std::endl;


    double yaw = 1.6;                                     // angle around z-axis
    double pitch = 0.0;                                   // angle around new y-axis
    double roll = 0.3;                                    // angle around new x-axis
//    kindr::EulerAnglesZyxD euler(yaw, pitch, roll);
    kindr::EulerAnglesZyxD euler(rpy.p_(Z), rpy.p_(Y), rpy.p_(X));
    kindr::RotationMatrixD mat(euler);
    kindr::RotationMatrixPD matP(euler);

//    std::cout << "mat_kindr_active:\n" << mat << std::endl;
//    std::cout << "mat_kindr_passive:\n" << matP << std::endl;

    // because optimized euler angles represent world->base mapping,
    // but quaternion expects mapping base->world

    // zmp_ roll and pitch are swapped! why?
    // because this is not world to base, but base to world
    Eigen::Matrix3d b_R_w = converter.GetRotationMatrixWorldToBase(t).toDense();

//    std::cout << "mat_own:\n" << b_R_w << std::endl;

    kindr::RotationQuaternionD quat(mat);



    Eigen::Matrix3d w_R_b = b_R_w.inverse();


//    Eigen::Quaterniond w_q_b2(0.2, 0.0, 1.0, 0.0);

    Eigen::Quaterniond w_q_b(b_R_w);

//    std::cout << "quat_own: " << w_q_b.w() << ", " << w_q_b.vec().transpose() << std::endl;
//    std::cout << "quat_kindr: " << quat << std::endl;

    base.ang.q = w_q_b;//quat.toImplementation();//w_q_b.inverse();//quat.toImplementation().inverse();
    base.ang.v = converter.GetAngularVelocity(t);
    base.ang.a = converter.GetAngularAcceleration(t);
    state.SetBase(base);

    state.SetEEStateInWorld(ee_motion->GetEndeffectors(t));
    state.SetEEForcesInWorld(ee_forces->GetForce(t));
    state.SetContactState(contact_schedule->IsInContact(t));

    state.SetTime(t);
    trajectory.push_back(state);
    t += dt;
  }



  return trajectory;
}

void
MotionOptimizerFacade::SetMotionParameters (const MotionParametersPtr& params)
{
  motion_parameters_ = params;
}

} /* namespace opt */
} /* namespace xpp */


