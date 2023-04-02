// Jolt Physics Library (https://github.com/jrouwe/JoltPhysics)
// SPDX-FileCopyrightText: 2023 Jorrit Rouwe
// SPDX-License-Identifier: MIT

#include <Jolt/Jolt.h>

#include <Jolt/Physics/Vehicle/MotorcycleController.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/ObjectStream/TypeDeclarations.h>
#include <Jolt/Core/StreamIn.h>
#include <Jolt/Core/StreamOut.h>
#ifdef JPH_DEBUG_RENDERER
	#include <Jolt/Renderer/DebugRenderer.h>
#endif // JPH_DEBUG_RENDERER

JPH_NAMESPACE_BEGIN

JPH_IMPLEMENT_SERIALIZABLE_VIRTUAL(MotorcycleControllerSettings)
{
	JPH_ADD_BASE_CLASS(MotorcycleControllerSettings, VehicleControllerSettings)

	JPH_ADD_ATTRIBUTE(MotorcycleControllerSettings, mMaxLeanAngle)
	JPH_ADD_ATTRIBUTE(MotorcycleControllerSettings, mLeanSpringConstant)
	JPH_ADD_ATTRIBUTE(MotorcycleControllerSettings, mLeanSpringDamping)
	JPH_ADD_ATTRIBUTE(MotorcycleControllerSettings, mLeanSmoothingFactor)
}

VehicleController *MotorcycleControllerSettings::ConstructController(VehicleConstraint &inConstraint) const
{
	return new MotorcycleController(*this, inConstraint);
}

void MotorcycleControllerSettings::SaveBinaryState(StreamOut &inStream) const
{
	WheeledVehicleControllerSettings::SaveBinaryState(inStream);

	inStream.Write(mMaxLeanAngle);
	inStream.Write(mLeanSpringConstant);
	inStream.Write(mLeanSpringDamping);
	inStream.Write(mLeanSmoothingFactor);
}

void MotorcycleControllerSettings::RestoreBinaryState(StreamIn &inStream)
{
	WheeledVehicleControllerSettings::RestoreBinaryState(inStream);

	inStream.Read(mMaxLeanAngle);
	inStream.Read(mLeanSpringConstant);
	inStream.Read(mLeanSpringDamping);
	inStream.Read(mLeanSmoothingFactor);
}

MotorcycleController::MotorcycleController(const MotorcycleControllerSettings &inSettings, VehicleConstraint &inConstraint) :
	WheeledVehicleController(inSettings, inConstraint),
	mMaxLeanAngle(inSettings.mMaxLeanAngle),
	mLeanSpringConstant(inSettings.mLeanSpringConstant),
	mLeanSpringDamping(inSettings.mLeanSpringDamping),
	mLeanSmoothingFactor(inSettings.mLeanSmoothingFactor)
{
}

float MotorcycleController::GetWheelBase() const
{
	float low = FLT_MAX, high = -FLT_MAX;

	for (const Wheel *w : mConstraint.GetWheels())
	{
		const WheelSettings *s = w->GetSettings();

		// Measure distance along the forward axis by looking at the fully extended suspension
		float value = (s->mPosition + s->mSuspensionDirection * s->mSuspensionMaxLength).Dot(mConstraint.GetLocalForward());

		// Update min and max
		low = min(low, value);
		high = max(high, value);
	}

	return high - low;
}

void MotorcycleController::PreCollide(float inDeltaTime, PhysicsSystem &inPhysicsSystem)
{
	WheeledVehicleController::PreCollide(inDeltaTime, inPhysicsSystem);

	Vec3 gravity = inPhysicsSystem.GetGravity();
	float gravity_len = gravity.Length();
	Vec3 world_up = gravity_len > 0.0f? -gravity / gravity_len : mConstraint.GetLocalUp();

	Body *body = mConstraint.GetVehicleBody();
	Vec3 forward = body->GetRotation() * mConstraint.GetLocalForward();
	float wheel_base = GetWheelBase();
	float velocity = body->GetLinearVelocity().Dot(forward);
	float velocity_sq = Square(velocity);

	// Calculate the target lean vector, this is in the direction of the total applied impulse by the ground on the wheels
	Vec3 target_lean = Vec3::sZero();
	for (const Wheel *w : mConstraint.GetWheels())
		if (w->HasContact())
			target_lean += w->GetContactNormal() * w->GetSuspensionLambda() + w->GetContactLateral() * w->GetLateralLambda();

	// Normalize the impulse
	target_lean = target_lean.NormalizedOr(world_up);

	// Smooth the impulse to avoid jittery behavior
	mTargetLean = mLeanSmoothingFactor * mTargetLean + (1.0f - mLeanSmoothingFactor) * target_lean;

	// Remove forward component, we can only lean sideways
	mTargetLean -= mTargetLean * mTargetLean.Dot(forward);
	mTargetLean = mTargetLean.NormalizedOr(world_up);

	// Calculate max steering angle based on the max lean angle we're willing to take
	// See: https://en.wikipedia.org/wiki/Bicycle_and_motorcycle_dynamics#Leaning
	// LeanAngle = Atan(Velocity^2 / (Gravity * TurnRadius))
	// And: https://en.wikipedia.org/wiki/Turning_radius (we're ignoring the tire width)
	// The CasterAngle is the added according to https://en.wikipedia.org/wiki/Bicycle_and_motorcycle_dynamics#Turning (this is the same formula but without small angle approximation)
	// TurnRadius = WheelBase / (Sin(SteerAngle) * Cos(CasterAngle))
	// => SteerAngle = ASin(WheelBase * Tan(LeanAngle) * Gravity / (Velocity^2 * Cos(CasterAngle))
	// The caster angle is different for each wheel so we can only calculate part of the equation here
	float max_steer_angle_factor = wheel_base * Tan(mMaxLeanAngle) * gravity_len;

	// Decompose steering into sign and direction
	float steer_strength = abs(mRightInput);
	float steer_sign = -Sign(mRightInput);

	for (Wheel *w_base : mConstraint.GetWheels())
	{
		WheelWV *w = static_cast<WheelWV *>(w_base);
		const WheelSettingsWV *s = w->GetSettings();

		// Check if this wheel can steer
		if (s->mMaxSteerAngle != 0.0f)
		{
			// Calculate cos(caster angle), the angle between the steering axis and the up vector
			float cos_caster_angle = s->mSteeringAxis.Dot(mConstraint.GetLocalUp());

			// Calculate steer angle
			float steer_angle = steer_strength * w->GetSettings()->mMaxSteerAngle;

			// Clamp to max steering angle
			if (velocity_sq > 1.0e-6f)
			{
				float max_steer_angle = ASin(max_steer_angle_factor / (velocity_sq * cos_caster_angle));
				steer_angle = min(steer_angle, max_steer_angle);
			}

			// Set steering angle
			w->SetSteerAngle(steer_sign * steer_angle);
		}
	}

	// Reset applied impulse
	mAppliedImpulse = 0;
}

bool MotorcycleController::SolveLongitudinalAndLateralConstraints(float inDeltaTime) 
{
	bool impulse = WheeledVehicleController::SolveLongitudinalAndLateralConstraints(inDeltaTime);

	// Only apply a lean impulse if all wheels are in contact, otherwise we can easily spin out
	bool all_in_contact = true;
	for (Wheel *w : mConstraint.GetWheels())
		if (!w->HasContact() || w->GetSuspensionLambda() <= 0.0f)
		{
			all_in_contact = false;
			break;
		}

	if (all_in_contact)
	{
		Body *body = mConstraint.GetVehicleBody();
		MotionProperties *mp = body->GetMotionProperties();

		Vec3 forward = body->GetRotation() * mConstraint.GetLocalForward();
		Vec3 up = body->GetRotation() * mConstraint.GetLocalUp();

		// Calculate delta to target angle and derivative
		float d_angle = -Sign(mTargetLean.Cross(up).Dot(forward)) * ACos(mTargetLean.Dot(up));
		float ddt_angle = body->GetAngularVelocity().Dot(forward);

		// Calculate impulse to apply to get to target lean angle
		float total_impulse = (mLeanSpringConstant * d_angle - mLeanSpringDamping * ddt_angle) * inDeltaTime;

		// Remember angular velocity pre angular impulse
		Vec3 old_w = mp->GetAngularVelocity();

		// Apply impulse taking into account the impulse we've applied earlier
		float delta_impulse = total_impulse - mAppliedImpulse;
		body->AddAngularImpulse(delta_impulse * forward);
		mAppliedImpulse = total_impulse;

		// Calculate delta angular velocity due to angular impulse
		Vec3 dw = mp->GetAngularVelocity() - old_w;
		Vec3 linear_acceleration = Vec3::sZero();
		float total_lambda = 0.0f;
		for (Wheel *w_base : mConstraint.GetWheels())
		{
			WheelWV *w = static_cast<WheelWV *>(w_base);

			// We weigh the importance of each contact point according to the contact force
			float lambda = w->GetSuspensionLambda();
			total_lambda += lambda;

			// Linear acceleration of contact point is dw x com_to_contact
			Vec3 r = Vec3(w->GetContactPosition() - body->GetCenterOfMassPosition());
			linear_acceleration += lambda * dw.Cross(r);
		}

		// Apply linear impulse to COM to cancel the average velocity change on the wheels due to the angular impulse
		Vec3 linear_impulse = -linear_acceleration / (total_lambda * mp->GetInverseMass());
		body->AddImpulse(linear_impulse);

		// Return true if we applied an impulse
		impulse |= delta_impulse != 0.0f;
	}

	return impulse;
}

#ifdef JPH_DEBUG_RENDERER

void MotorcycleController::Draw(DebugRenderer *inRenderer) const 
{
	WheeledVehicleController::Draw(inRenderer);

	// Draw current and desired lean angle
	Body *body = mConstraint.GetVehicleBody();
	RVec3 center_of_mass = body->GetCenterOfMassPosition();
	Vec3 up = body->GetRotation() * mConstraint.GetLocalUp();
	inRenderer->DrawArrow(center_of_mass, center_of_mass + up, Color::sYellow, 0.1f);
	inRenderer->DrawArrow(center_of_mass, center_of_mass + mTargetLean, Color::sRed, 0.1f);
}

#endif // JPH_DEBUG_RENDERER

JPH_NAMESPACE_END