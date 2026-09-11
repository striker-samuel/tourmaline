#pragma once

#include <assert.h>
#include <algorithm>

#include "Engine/core.h"
#include <glm/gtc/quaternion.hpp>

// InfiniteReversedZ projection. For better precission up close and no cutoff
glm::mat4 MakeInfReversedZProjRH(float fovY_radians, float aspectWbyH, float zNear);

// Finite reversed-Z projection (Vulkan ZO, right-handed): maps
// [zNear, zFar] to depth [1, 0] (near = 1, far = 0, like the infinite
// form, whose limit it recovers as zFar -> infinity). Prefer this for the
// main view: unprojecting sky/far depth yields finite positions instead of
// infinities (no NaN class downstream), and the frustum near/far planes
// are functional instead of degenerate. Requires zFar > zNear.
glm::mat4 MakeReversedZProjRH(float fovY_radians, float aspectWbyH, float zNear, float zFar);

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

	explicit Camera(CameraPositionerInterface& positioner);

	Camera(const Camera&) = default;
	Camera& operator = (const Camera&) = default;

	glm::mat4 getViewMatrix() const;
	glm::vec3 getPosition() const;
	glm::mat4 getProjMatrix() const;

	void setOrtho2D(float width, float height);
	void setPerspective(float fovY, float aspect, float zNear, float zFar);

private:
	const CameraPositionerInterface* positioner_;
	glm::mat4 proj_;
};

class CameraPositioner_FirstPerson final : public CameraPositionerInterface
{
public:
	CameraPositioner_FirstPerson() = default;
	CameraPositioner_FirstPerson(const glm::vec3& pos, const glm::vec3& target, const glm::vec3& up);
	void update(double deltaSeconds, const glm::vec2& mousePos, bool mousePressed);

	virtual glm::mat4 getViewMatrix() const override;
	virtual glm::vec3 getPosition() const override;
	void setPosition(const glm::vec3& pos);
	void setSpeed(const glm::vec3& speed);
	void resetMousePosition(const glm::vec2& p);
	void setUpVector(const glm::vec3& up);
	void lookAt(const glm::vec3& pos, const glm::vec3& target, const glm::vec3& up);
	glm::vec3 getForward() const;
	glm::quat getOrientation() const;
	// x = pitch, y = yaw, z = roll, in degrees (glm eulerAngles convention).
	glm::vec3 getEulerDegrees() const;
	void reset();

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

		// Analog gamepad input (-1..1, applied alongside the digital keys):
		// stickMove.x = strafe right+, .y = forward+ (left stick),
		// stickLook.x = yaw right+, .y = pitch up+ (right stick),
		// vertical = up+ / down- (triggers).
		glm::vec2 stickMove_{ 0.0f };
		glm::vec2 stickLook_{ 0.0f };
		float vertical_ = 0.0f;
	} movement_;

	//rotationSpeed is in radians per second per NDC mouse unit
	float rotationSpeed_ = 50.0f;

	// Right-stick look rate (rad/s at full deflection).
	float stickLookSpeed_ = 2.8f;

	float damping_ = 0.5f; // seconds, time constant for velocity decay when no input
	float acceleration_ = 2.4f; // meters/sec�, how fast speed builds while moving

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


