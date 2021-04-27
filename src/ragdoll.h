#pragma once

#include "physics.h"
#include "scene.h"

struct humanoid_ragdoll
{
	void initialize(struct scene& appScene, vec3 initialHipPosition);
	bool edit();

	scene_entity torso;
	scene_entity head;
	scene_entity leftUpperArm;
	scene_entity leftLowerArm;
	scene_entity rightUpperArm;
	scene_entity rightLowerArm;
	scene_entity leftUpperLeg;
	scene_entity leftLowerLeg;
	scene_entity leftFoot;
	scene_entity leftToes;
	scene_entity rightUpperLeg;
	scene_entity rightLowerLeg;
	scene_entity rightFoot;
	scene_entity rightToes;

	cone_twist_constraint_handle neckConstraint;
	cone_twist_constraint_handle leftShoulderConstraint;
	hinge_joint_constraint_handle leftElbowConstraint;
	cone_twist_constraint_handle rightShoulderConstraint;
	hinge_joint_constraint_handle rightElbowConstraint;
	cone_twist_constraint_handle leftHipConstraint;
	hinge_joint_constraint_handle leftKneeConstraint;
	cone_twist_constraint_handle leftAnkleConstraint;
	hinge_joint_constraint_handle leftToesConstraint;
	cone_twist_constraint_handle rightHipConstraint;
	hinge_joint_constraint_handle rightKneeConstraint;
	cone_twist_constraint_handle rightAnkleConstraint;
	hinge_joint_constraint_handle rightToesConstraint;
};