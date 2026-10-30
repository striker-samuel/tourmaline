// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Jolt physics smoke test (Epoch 2 of the scene-tables migration —
// docs/scene_tables_migration.md): prove the vendored Jolt library
// simulates correctly STANDALONE, before any Tourmaline integration.
// Deliberately links ONLY the Jolt target — no engine, no renderer, no
// editor, no .pak, no Scene dependency (the acceptance gate). The
// lifecycle below is the exact one-Initializer-per-process contract the
// future PhysicsWorld subsystem (Epoch 11) will own: RegisterTypes once
// per process, PhysicsSystem/allocators per world.
//
// Hand-rolled runner (the house convention): each check prints
// PASS/FAIL, the process exits nonzero on any failure.

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <thread>

#include <Jolt/Jolt.h>

#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>

JPH_SUPPRESS_WARNINGS

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool cond, const char* name) {
	++g_checks;
	if (cond)
		std::printf("PASS %s\n", name);
	else {
		++g_failures;
		std::printf("FAIL %s\n", name);
	}
}

// Jolt trace sink (asserts + internal errors would be silent without
// one; the future PhysicsWorld will route this through the engine log).
void TraceImpl(const char* inFMT, ...)
{
	va_list list;
	va_start(list, inFMT);
	char buffer[1024];
	std::vsnprintf(buffer, sizeof(buffer), inFMT, list);
	va_end(list);
	std::printf("[Jolt] %s\n", buffer);
}

#ifdef JPH_ENABLE_ASSERTS
bool AssertFailedImpl(const char* inExpression, const char* inMessage,
	const char* inFile, JPH::uint inLine)
{
	std::printf("%s:%u: (%s) %s\n", inFile, inLine, inExpression,
		inMessage != nullptr ? inMessage : "");
	return true;
}
#endif

// Explicit layers: static level geometry vs dynamic bodies (the shape
// of the future Tourmaline collision layers — Epoch 10's RigidBodyRow
// layer field maps onto ObjectLayer).
namespace Layers {
	static constexpr JPH::ObjectLayer NON_MOVING = 0;
	static constexpr JPH::ObjectLayer MOVING = 1;
	static constexpr JPH::ObjectLayer NUM_LAYERS = 2;
}

class ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter {
public:
	bool ShouldCollide(JPH::ObjectLayer inObject1,
		JPH::ObjectLayer inObject2) const override
	{
		switch (inObject1) {
		case Layers::NON_MOVING:
			return inObject2 == Layers::MOVING;
		case Layers::MOVING:
			return true;
		default:
			return false;
		}
	}
};

namespace BroadPhaseLayers {
	static constexpr JPH::BroadPhaseLayer NON_MOVING(0);
	static constexpr JPH::BroadPhaseLayer MOVING(1);
	static constexpr JPH::uint NUM_LAYERS(2);
}

class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface {
public:
	BPLayerInterfaceImpl()
	{
		mObjectToBroadPhase[Layers::NON_MOVING] = BroadPhaseLayers::NON_MOVING;
		mObjectToBroadPhase[Layers::MOVING] = BroadPhaseLayers::MOVING;
	}
	JPH::uint GetNumBroadPhaseLayers() const override
	{
		return BroadPhaseLayers::NUM_LAYERS;
	}
	JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const override
	{
		JPH_ASSERT(inLayer < Layers::NUM_LAYERS);
		return mObjectToBroadPhase[inLayer];
	}
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
	const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const override
	{
		switch ((JPH::BroadPhaseLayer::Type)inLayer) {
		case (JPH::BroadPhaseLayer::Type)BroadPhaseLayers::NON_MOVING: return "NON_MOVING";
		case (JPH::BroadPhaseLayer::Type)BroadPhaseLayers::MOVING: return "MOVING";
		default: return "INVALID";
		}
	}
#endif
private:
	JPH::BroadPhaseLayer mObjectToBroadPhase[Layers::NUM_LAYERS];
};

class ObjectVsBroadPhaseLayerFilterImpl final
	: public JPH::ObjectVsBroadPhaseLayerFilter {
public:
	bool ShouldCollide(JPH::ObjectLayer inLayer1,
		JPH::BroadPhaseLayer inLayer2) const override
	{
		switch (inLayer1) {
		case Layers::NON_MOVING:
			return inLayer2 == BroadPhaseLayers::MOVING;
		case Layers::MOVING:
			return true;
		default:
			return false;
		}
	}
};

} // namespace

// The scenario: a 20 m static floor box (top surface at y=0) and one
// 1 m dynamic cube dropped from y=10. 120 fixed steps at 1/60 s. The
// cube must fall, hit the floor, and come to rest on top of it (y ~
// 0.5 = half extent) with (near-)zero velocity.
int main() {
	using namespace JPH;

	// ---- one-time process init (the contract the engine will own) ----
	RegisterDefaultAllocator();
	Trace = TraceImpl;
#ifdef JPH_ENABLE_ASSERTS
	AssertFailed = AssertFailedImpl;
#endif
	Factory::sInstance = new Factory();
	RegisterTypes();

	int failuresBeforeInit = g_failures;
	{
		// ---- the world ----
		BPLayerInterfaceImpl broadPhaseLayerInterface;
		ObjectVsBroadPhaseLayerFilterImpl objectVsBroadPhaseLayerFilter;
		ObjectLayerPairFilterImpl objectVsObjectLayerFilter;

		constexpr uint cMaxBodies = 1024;
		constexpr uint cNumBodyMutexes = 0; // default
		constexpr uint cMaxBodyPairs = 1024;
		constexpr uint cMaxContactConstraints = 1024;

		PhysicsSystem physicsSystem;
		physicsSystem.Init(cMaxBodies, cNumBodyMutexes, cMaxBodyPairs,
			cMaxContactConstraints, broadPhaseLayerInterface,
			objectVsBroadPhaseLayerFilter, objectVsObjectLayerFilter);

		TempAllocatorImpl tempAllocator(10 * 1024 * 1024);
		JobSystemThreadPool jobSystem(cMaxPhysicsJobs, cMaxPhysicsBarriers,
			std::thread::hardware_concurrency() - 1);

		BodyInterface& bodyInterface = physicsSystem.GetBodyInterface();

		// ---- the floor: 20 m half-extent static box, top at y = 0 ----
		// (center at -20: half extent 20 -> spans [-40, 0] — the cube
		// must start ABOVE y=0 or it spawns inside the floor and the
		// contact solver ejects it upward.)
		RVec3 floorPos(0.0f, -20.0f, 0.0f);
		BodyCreationSettings floorSettings(
			new BoxShape(RVec3::sReplicate(20.0f)),
			floorPos,
			Quat::sIdentity(),
			EMotionType::Static,
			Layers::NON_MOVING);
		const BodyID floorId = bodyInterface.CreateAndAddBody(
			floorSettings, EActivation::DontActivate);

		// ---- the cube: 0.5 m half extents, dropped from y = 10 ----
		constexpr float cHalfExtent = 0.5f;
		constexpr float cStartY = 10.0f;
		RVec3 cubePos(0.0f, cStartY, 0.0f);
		BodyCreationSettings cubeSettings(
			new BoxShape(RVec3::sReplicate(cHalfExtent)),
			cubePos,
			Quat::sIdentity(),
			EMotionType::Dynamic,
			Layers::MOVING);
		const BodyID cubeId = bodyInterface.CreateAndAddBody(
			cubeSettings, EActivation::Activate);

		Check(physicsSystem.GetNumBodies() == 2, "two bodies added");
		Check(!floorId.IsInvalid() && !cubeId.IsInvalid(),
			"body ids valid");

		// ---- step 120 frames at 1/60 s ----
		constexpr float cDt = 1.0f / 60.0f;
		float lowestY = cStartY;
		for (int step = 0; step < 120; ++step) {
			physicsSystem.Update(cDt, /*collisionSteps=*/1,
				&tempAllocator, &jobSystem);
			// GetPosition = the body's creation position (box center);
			// GetCenterOfMassPosition carries the shape's COM offset.
			const float y = bodyInterface.GetPosition(cubeId).GetY();
			lowestY = y < lowestY ? y : lowestY;
		}

		const RVec3 finalPos = bodyInterface.GetPosition(cubeId);
		const RVec3 finalVel = bodyInterface.GetLinearVelocity(cubeId);
		const float expectedRestY = cHalfExtent; // floor top at y = 0
		const bool settledY = std::abs(finalPos.GetY() - expectedRestY)
			< 0.05f;
		const bool settledVel = finalVel.LengthSq() < 1e-4f;

		Check(finalPos.GetY() < cStartY - 1.0f, "cube fell");
		Check(lowestY >= expectedRestY - 0.01f, "cube never sank through the floor");
		Check(settledY, "cube came to rest on the floor surface");
		Check(settledVel, "cube velocity ~ zero at rest");

		// ---- teardown: remove + destroy, then the world ----
		bodyInterface.RemoveBody(floorId);
		bodyInterface.DestroyBody(floorId);
		bodyInterface.RemoveBody(cubeId);
		bodyInterface.DestroyBody(cubeId);
		Check(physicsSystem.GetNumBodies() == 0,
			"all bodies destroyed cleanly");
	}

	// ---- one-time process shutdown ----
	UnregisterTypes();
	delete Factory::sInstance;
	Factory::sInstance = nullptr;

	if (failuresBeforeInit != g_failures || g_failures > 0) {
		std::printf("physics_jolt_tests: %d/%d checks failed\n",
			g_failures, g_checks);
		return 1;
	}
	std::printf("physics_jolt_tests: all %d checks passed\n", g_checks);
	return 0;
}
