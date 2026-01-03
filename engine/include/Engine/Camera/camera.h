#pragma once

#include <assert.h>
#include <algorithm>

#include "Engine/Shared/common.h"

glm::mat4 MakeInfReversedZProjRH(float fovY_radians, float aspectWbyH, float zNear)
{
	float f = 1.0f / tan(fovY_radians / 2.0f);
	return glm::mat4(
		f / aspectWbyH, 0.0f, 0.0f, 0.0f,
		0.0f, f, 0.0f, 0.0f,
		0.0f, 0.0f, 0.0f, -1.0f,
		0.0f, 0.0f, zNear, 0.0f);
}

class CameraPositionerInterface
{
public:
	virtual ~CameraPositionerInterface() = default;
	virtual glm::mat4 getViewMatrix() const = 0;
	virtual glm::vec3 getPosition() const = 0;
};

class Camera final
{
public:
	explicit Camera(CameraPositionerInterface& positioner)
		: positioner_(&positioner)
	{
	}

	Camera(const Camera&) = default;
	Camera& operator = (const Camera&) = default;

	glm::mat4 getViewMatrix() const { return positioner_->getViewMatrix(); }
	glm::vec3 getPosition() const { return positioner_->getPosition(); }
	glm::mat4 getProjMatrix() const { return proj_; }

	void setOrtho2D(float width, float height) {
		proj_ = glm::ortho(0.0f, width, 0.0f, height, -1.0f, 1.0f);
	}

	void setPerspective(float fovY, float aspect, float zNear, float zFar) {
		//proj_ = glm::perspective(glm::radians(fovY), aspect, zNear, zFar);
		proj_ = MakeInfReversedZProjRH(fovY, aspect, zNear);
	}

private:
	const CameraPositionerInterface* positioner_;
	glm::mat4 proj_;
};

class CameraPositioner_FirstPerson final : public CameraPositionerInterface
{
public:
	CameraPositioner_FirstPerson() = default;
	CameraPositioner_FirstPerson(const glm::vec3& pos, const glm::vec3& target, const glm::vec3& up)
		: cameraPosition_(pos)
		, cameraOrientation_(glm::lookAt(pos, target, up))
		, up_(up)
	{
		// store starting values for reset()
		starting_pos = cameraPosition_;
		starting_cameraOrientation = cameraOrientation_;
		starting_up = up_;
	}


	void update(double deltaSeconds, const glm::vec2& mousePos, bool mousePressed)
	{
		if (mousePressed)
		{
			const glm::vec2 delta = mousePos - mousePos_;
			glm::vec2 frameDelta = delta * static_cast<float>(deltaSeconds);
			const glm::quat deltaQuat = glm::quat(glm::vec3(
				-rotationSpeed_ * frameDelta.y,
				rotationSpeed_ * frameDelta.x,
				0.0f));

			cameraOrientation_ = deltaQuat * cameraOrientation_;
			cameraOrientation_ = glm::normalize(cameraOrientation_);
			setUpVector(up_);
		}
		mousePos_ = mousePos;

		const glm::mat4 v = glm::mat4_cast(cameraOrientation_);

		const glm::vec3 forward = -glm::vec3(v[0][2], v[1][2], v[2][2]);
		const glm::vec3 right = glm::vec3(v[0][0], v[1][0], v[2][0]);
		const glm::vec3 up = glm::cross(right, forward);

		glm::vec3 accel(0.0f);

		if (movement_.forward_) accel += forward;
		if (movement_.backward_) accel -= forward;

		if (movement_.left_) accel -= right;
		if (movement_.right_) accel += right;

		if (movement_.up_) accel += up;
		if (movement_.down_) accel -= up;

		if (movement_.fastSpeed_) accel *= fastCoef_;

		if (accel == glm::vec3(0))
		{
			// decelerate naturally according to the damping value
			moveSpeed_ -= moveSpeed_ * std::min((1.0f / damping_) * static_cast<float>(deltaSeconds), 1.0f);
		}
		else
		{
			// acceleration
			moveSpeed_ += accel * acceleration_ * static_cast<float>(deltaSeconds);
			const float maxSpeed = movement_.fastSpeed_ ? maxSpeed_ * fastCoef_ : maxSpeed_;
			if (glm::length(moveSpeed_) > maxSpeed) moveSpeed_ = glm::normalize(moveSpeed_) * maxSpeed;
		}

		cameraPosition_ += moveSpeed_ * static_cast<float>(deltaSeconds);
	}

	virtual glm::mat4 getViewMatrix() const override
	{
		const glm::mat4 t = glm::translate(glm::mat4(1.0f), -cameraPosition_);
		const glm::mat4 r = glm::mat4_cast(cameraOrientation_);
		return r * t;
	}


	virtual glm::vec3 getPosition() const override
	{
		return cameraPosition_;
	}

	void setPosition(const glm::vec3& pos)
	{
		cameraPosition_ = pos;
	}

	void setSpeed(const glm::vec3& speed) {
		moveSpeed_ = speed;
	}

	void resetMousePosition(const glm::vec2& p) {
		mousePos_ = p;
	};

	void setUpVector(const glm::vec3& up)
	{
		const glm::mat4 view = getViewMatrix();
		const glm::vec3 dir = -glm::vec3(view[0][2], view[1][2], view[2][2]);
		cameraOrientation_ = glm::lookAt(cameraPosition_, cameraPosition_ + dir, up);
	}

	inline void lookAt(const glm::vec3& pos, const glm::vec3& target, const glm::vec3& up) {
		cameraPosition_ = pos;
		cameraOrientation_ = glm::lookAt(pos, target, up);
	}
	glm::vec3 getForward() const {
		const glm::mat4 rot = glm::mat4_cast(cameraOrientation_);
		return -glm::vec3(rot[0][2], rot[1][2], rot[2][2]);
	}

	void reset() {
		cameraOrientation_ = starting_cameraOrientation;
		cameraPosition_ = starting_pos;
		up_ = starting_up;
	}


public:
	struct Movement
	{
		bool forward_ = false;
		bool backward_ = false;
		bool left_ = false;
		bool right_ = false;
		bool up_ = false;
		bool down_ = false;

		bool fastSpeed_ = false;

	} movement_;

	//rotationSpeed is in radians per second per NDC mouse unit
	float rotationSpeed_ = 50.0f;

	float damping_ = 0.5f; // seconds, time constant for velocity decay when no input
	float acceleration_ = 2.4f; // meters/sec², how fast speed builds while moving
	
	float maxSpeed_ = 2.0f; //m/s
	float fastCoef_ = 4.0f; // raw multiplier

	glm::vec3 cameraPosition_;
	glm::vec3 moveSpeed_ = glm::vec3(0.0f);
	glm::vec2 mousePos_ = glm::vec2(0);
	glm::quat cameraOrientation_ = glm::quat(glm::vec3(0));
	glm::vec3 up_ = glm::vec3(0.0f, 1.0f, 0.0f);

	glm::vec3 starting_pos;
	glm::quat starting_cameraOrientation;
	glm::vec3 starting_up;
};


