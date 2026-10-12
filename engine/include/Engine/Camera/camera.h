#pragma once

#include <assert.h>
#include <algorithm>

#include "Engine/core.h"
#include <glm/gtc/matrix_transform.hpp>
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

// Degenerate-lookAt resolution (the SINGLE shared convention — every
// camera framing that can look straight down/up must go through these so
// all consumers of one (eye, target) pair agree on the screen basis:
// the editor's overlay camera, the camera drive paths and the
// locked look-through each used to pick their own fallback (a sideways
// target nudge here, an arbitrary up-axis there), which rotated the
// editor's wire boxes 90 degrees against the scene in the straight-down
// top view).
//
// ResolveLookAtUp: the preferred up while it is usable, else a
// deterministic horizontal fallback — straight DOWN looks with -Z as
// screen-up ("north up"), straight UP with +Z. No target nudging: a
// nudge pans the view and disagrees with direct-lookAt users.
[[nodiscard]] inline glm::vec3 ResolveLookAtUp(const glm::vec3& eye,
	const glm::vec3& target,
	const glm::vec3& preferredUp = glm::vec3(0.0f, 1.0f, 0.0f)) {
	glm::vec3 f = target - eye;
	const float len = glm::length(f);
	if (len < 1e-6f)
		return preferredUp; // eye == target: nothing to resolve against
	f /= len;
	if (glm::abs(glm::dot(f, preferredUp)) <= 0.9995f)
		return preferredUp;
	// (Nearly) parallel to the preferred up: pick the fallback by the
	// dominant view direction sign so the basis never flips discontinuously.
	return (f.y < 0.0f) ? glm::vec3(0.0f, 0.0f, -1.0f)
		: glm::vec3(0.0f, 0.0f, 1.0f);
}

// lookAt with the shared degenerate resolution (see ResolveLookAtUp):
// straight-down/up framings get the deterministic horizontal basis
// instead of NaNs (glm::lookAt with a parallel up crosses zero normals).
[[nodiscard]] inline glm::mat4 SafeLookAt(const glm::vec3& eye,
	const glm::vec3& target,
	const glm::vec3& preferredUp = glm::vec3(0.0f, 1.0f, 0.0f)) {
	return glm::lookAt(eye, target,
		ResolveLookAtUp(eye, target, preferredUp));
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

	explicit Camera(CameraPositionerInterface& positioner);

	Camera(const Camera&) = default;
	Camera& operator = (const Camera&) = default;

	glm::mat4 getViewMatrix() const;
	glm::vec3 getPosition() const;
	glm::mat4 getProjMatrix() const;

	void setOrtho2D(float width, float height);
	void setPerspective(float fovY, float aspect, float zNear, float zFar);
	// TRUE 3D orthographic lens (reversed-Z, GL-convention Y-up like
	// setPerspective — the frame-uniform layer applies the Vulkan
	// Y-flip): halfH = the vertical half-extent in world METERS, the
	// horizontal extent = halfH * aspect. Swapped glm near/far args map
	// [zNear, zFar] -> depth [1, 0], matching MakeReversedZProjRH.
	void setOrtho3D(float halfH, float aspect, float zNear, float zFar);

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

	// rotationSpeed is in radians per NDC mouse unit (mouse deltas are
	// displacements, not rates - see update()). Default preserves the old
	// 60fps feel exactly (the old 50 rad/s/NDC times a 1/60s frame).
	float rotationSpeed_ = 50.0f / 60.0f;

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


