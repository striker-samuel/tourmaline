#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/constants.hpp>
#include <string>

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

    // Camera options
    float movementSpeed;
    float mouseSensitivity;
    float FOV;

    // Constructor
    Camera(glm::vec3 startPosition, glm::vec3 startUp, float startYaw, float startPitch)
        : front(glm::vec3(0.0f, 0.0f, -1.0f)),
        movementSpeed(0.01f),
        mouseSensitivity(0.1f),
        FOV(45.0f) {

        position = startPosition;
        worldUp = startUp;
        yaw = startYaw;
        pitch = startPitch;
        
        updateCameraVectors();
    }

    // Returns the view matrix for rendering
    glm::mat4 getViewMatrix() const {
        return glm::lookAt(position, position + front, up);
    }

    // Processes keyboard input
    void processKeyboard(const std::string& direction, float deltaTime) {
        float velocity = movementSpeed * deltaTime;
        if (direction == "FORWARD")
            position += front * velocity;
        if (direction == "BACKWARD")
            position -= front * velocity;
        if (direction == "LEFT")
            position -= right * velocity;
        if (direction == "RIGHT")
            position += right * velocity;
    }


    // Processes mouse movement
    void processMouseMovement(float xOffset, float yOffset, bool constrainPitch = true) {
        xOffset *= mouseSensitivity;
        yOffset *= mouseSensitivity;

        yaw += xOffset;
        pitch += yOffset;

        // Constrain the pitch to avoid screen flipping
        if (constrainPitch) {
            if (pitch > 89.0f) pitch = 89.0f;
            if (pitch < -89.0f) pitch = -89.0f;
        }

        // Update front, right, and up vectors
        updateCameraVectors();
    }

    // Processes mouse scroll for zooming
    void processMouseScroll(float yOffset) {
        // Adjust the field of view (simulating zoom)
        FOV -= yOffset;
        if (FOV < 1.0f) FOV = 1.0f;     // Clamp for "zoom in"
        //if (FOV > 45.0f) FOV = 45.0f;   // Clamp for "zoom out"
    }

private:
    // Updates the front, right, and up vectors based on the updated Euler angles
    void updateCameraVectors() {
        glm::vec3 newFront;
        newFront.x = cos(glm::radians(yaw)) * cos(glm::radians(pitch));
        newFront.y = sin(glm::radians(pitch));
        newFront.z = sin(glm::radians(yaw)) * cos(glm::radians(pitch));
        front = glm::normalize(newFront);

        // Recalculate right and up vectors
        right = glm::normalize(glm::cross(front, worldUp)); // Perpendicular to front and worldUp
        up = glm::normalize(glm::cross(right, front));      // Perpendicular to right and front
    }
};