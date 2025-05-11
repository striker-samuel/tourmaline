#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <string>
#include <unordered_set>

class Camera {
 public:
  // Camera attributes
  glm::vec3 position;
  glm::vec3 front;
  glm::vec3 up;
  glm::vec3 right;
  glm::vec3 worldUp;

  // Euler angles
  float yaw;
  float pitch;

  // Movement physics
  glm::vec3 velocity = glm::vec3(0.0f);
  glm::vec3 acceleration = glm::vec3(0.0f);
  float maxSpeed = 0.25f;
  float accelerationRate = 0.001f;
  float damping = 5.0f;

  // Camera options
  float mouseSensitivity = 0.1f;
  float FOV = 45.0f;

  // Constructor
  Camera(glm::vec3 startPosition, glm::vec3 startUp, float startYaw,
         float startPitch)
      : front(glm::vec3(0.0f, 0.0f, -1.0f)),
        position(startPosition),
        worldUp(startUp),
        yaw(startYaw),
        pitch(startPitch) {
    updateCameraVectors();
  }

  // Returns the view matrix for rendering
  glm::mat4 getViewMatrix() const {
    return glm::lookAt(position, position + front, up);
  }

  // Input: a set of directions to apply acceleration
  void processKeyboardInput(const std::unordered_set<std::string>& directions) {
    acceleration = glm::vec3(0.0f);
    if (directions.count("FORWARD")) acceleration += front;
    if (directions.count("BACKWARD")) acceleration -= front;
    if (directions.count("LEFT")) acceleration -= right;
    if (directions.count("RIGHT")) acceleration += right;

    if (glm::length(acceleration) > 0.0f)
      acceleration = glm::normalize(acceleration) * accelerationRate;
  }

  // Call every frame to update position based on physics
  void update(float deltaTime) {
    // Apply acceleration
    velocity += acceleration * deltaTime;

    // Apply damping (ease-out) if no input
    if (glm::length(acceleration) < 0.0001f) {
      velocity -= velocity * glm::min(damping * deltaTime, 1.0f);
    }

    // Clamp speed
    if (glm::length(velocity) > maxSpeed) {
      velocity = glm::normalize(velocity) * maxSpeed;
    }

    // Move camera
    position += velocity * deltaTime;
  }

  // Processes mouse movement
  void processMouseMovement(float xOffset, float yOffset,
                            bool constrainPitch = true) {
    xOffset *= mouseSensitivity;
    yOffset *= mouseSensitivity;

    yaw += xOffset;
    pitch += yOffset;

    if (constrainPitch) {
      if (pitch > 89.0f) pitch = 89.0f;
      if (pitch < -89.0f) pitch = -89.0f;
    }

    updateCameraVectors();
  }

  // Processes mouse scroll for zooming
  void processMouseScroll(float yOffset) {
    FOV -= yOffset;
    if (FOV < 1.0f) FOV = 1.0f;
    if (FOV > 90.0f) FOV = 90.0f;
  }

 private:
  // Updates the front, right, and up vectors
  void updateCameraVectors() {
    glm::vec3 newFront;
    newFront.x = cos(glm::radians(yaw)) * cos(glm::radians(pitch));
    newFront.y = sin(glm::radians(pitch));
    newFront.z = sin(glm::radians(yaw)) * cos(glm::radians(pitch));
    front = glm::normalize(newFront);
    right = glm::normalize(glm::cross(front, worldUp));
    up = glm::normalize(glm::cross(right, front));
  }
};
