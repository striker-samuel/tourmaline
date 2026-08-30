#pragma once

#include <assert.h>
#include <algorithm>

#include "Engine/engine.h"

// InfiniteReversedZ projection. For better precission up close and no cutoff
glm::mat4 MakeInfReversedZProjRH(float fovY_radians, float aspectWbyH, float zNear);


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
	inline void lookAt(const glm::vec3& pos, const glm::vec3& target, const glm::vec3& up);
	glm::vec3 getForward() const;
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


