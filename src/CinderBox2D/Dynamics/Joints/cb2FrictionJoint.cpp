/*
* Copyright (c) 2006-2011 Erin Catto http://www.box2d.org
*
* This software is provided 'as-is', without any express or implied
* warranty.  In no event will the authors be held liable for any damages
* arising from the use of this software.
* Permission is granted to anyone to use this software for any purpose,
* including commercial applications, and to alter it and redistribute it
* freely, subject to the following restrictions:
* 1. The origin of this software must not be misrepresented; you must not
* claim that you wrote the original software. If you use this software
* in a product, an acknowledgment in the product documentation would be
* appreciated but is not required.
* 2. Altered source versions must be plainly marked as such, and must not be
* misrepresented as being the original software.
* 3. This notice may not be removed or altered from any source distribution.
*/

#include <CinderBox2D/Dynamics/Joints/cb2FrictionJoint.h>
#include <CinderBox2D/Dynamics/cb2Body.h>
#include <CinderBox2D/Dynamics/cb2TimeStep.h>

// Point-to-point constraint
// Cdot = v2 - v1
//      = v2 + cross(w2, r2) - v1 - cross(w1, r1)
// J = [-I -r1_skew I r2_skew ]
// Identity used:
// w k % (rx i + ry j) = w * (-ry i + rx j)

// Angle constraint
// Cdot = w2 - w1
// J = [0 0 -1 0 0 1]
// K = invI1 + invI2

void cb2FrictionJointDef::Initialize(cb2Body* bA, cb2Body* bB, const ci::Vec2f& anchor)
{
	bodyA = bA;
	bodyB = bB;
	localAnchorA = bodyA->GetLocalPoint(anchor);
	localAnchorB = bodyB->GetLocalPoint(anchor);
}

cb2FrictionJoint::cb2FrictionJoint(const cb2FrictionJointDef* def)
: cb2Joint(def)
{
	m_localAnchorA = def->localAnchorA;
	m_localAnchorB = def->localAnchorB;

	cb2::setZero(m_linearImpulse);
	m_angularImpulse = 0.0f;

	m_maxForce = def->maxForce;
	m_maxTorque = def->maxTorque;
}

void cb2FrictionJoint::InitVelocityConstraints(const cb2SolverData& data)
{
	m_indexA = m_bodyA->m_islandIndex;
	m_indexB = m_bodyB->m_islandIndex;
	m_localCenterA = m_bodyA->m_sweep.localCenter;
	m_localCenterB = m_bodyB->m_sweep.localCenter;
	m_invMassA = m_bodyA->m_invMass;
	m_invMassB = m_bodyB->m_invMass;
	m_invIA = m_bodyA->m_invI;
	m_invIB = m_bodyB->m_invI;

	float aA = data.positions[m_indexA].a;
	ci::Vec2f vA = data.velocities[m_indexA].v;
	float wA = data.velocities[m_indexA].w;

	float aB = data.positions[m_indexB].a;
	ci::Vec2f vB = data.velocities[m_indexB].v;
	float wB = data.velocities[m_indexB].w;

	cb2Rot qA(aA), qB(aB);

	// Compute the effective mass matrix.
	m_rA = cb2Mul(qA, m_localAnchorA - m_localCenterA);
	m_rB = cb2Mul(qB, m_localAnchorB - m_localCenterB);

	// J = [-I -r1_skew I r2_skew]
	//     [ 0       -1 0       1]
	// r_skew = [-ry; rx]

	// Matlab
	// K = [ mA+r1y^2*iA+mB+r2y^2*iB,  -r1y*iA*r1x-r2y*iB*r2x,          -r1y*iA-r2y*iB]
	//     [  -r1y*iA*r1x-r2y*iB*r2x, mA+r1x^2*iA+mB+r2x^2*iB,           r1x*iA+r2x*iB]
	//     [          -r1y*iA-r2y*iB,           r1x*iA+r2x*iB,                   iA+iB]

	float mA = m_invMassA, mB = m_invMassB;
	float iA = m_invIA, iB = m_invIB;

	ci::Matrix22f K;
	K.m00 = mA + mB + iA * m_rA.y * m_rA.y + iB * m_rB.y * m_rB.y;
	K.m01 = -iA * m_rA.x * m_rA.y - iB * m_rB.x * m_rB.y;
	K.m10 = K.m01;
	K.m11 = mA + mB + iA * m_rA.x * m_rA.x + iB * m_rB.x * m_rB.x;

	m_linearMass = K.inverted();

	m_angularMass = iA + iB;
	if (m_angularMass > 0.0f)
	{
		m_angularMass = 1.0f / m_angularMass;
	}

	if (data.step.warmStarting)
	{
		// Scale impulses to support a variable time step.
		m_linearImpulse *= data.step.dtRatio;
		m_angularImpulse *= data.step.dtRatio;

		ci::Vec2f P(m_linearImpulse.x, m_linearImpulse.y);
		vA -= mA * P;
		wA -= iA * (cb2Cross(m_rA, P) + m_angularImpulse);
		vB += mB * P;
		wB += iB * (cb2Cross(m_rB, P) + m_angularImpulse);
	}
	else
	{
		cb2::setZero(m_linearImpulse);
		m_angularImpulse = 0.0f;
	}

	data.velocities[m_indexA].v = vA;
	data.velocities[m_indexA].w = wA;
	data.velocities[m_indexB].v = vB;
	data.velocities[m_indexB].w = wB;
}

void cb2FrictionJoint::SolveVelocityConstraints(const cb2SolverData& data)
{
	ci::Vec2f vA = data.velocities[m_indexA].v;
	float wA = data.velocities[m_indexA].w;
	ci::Vec2f vB = data.velocities[m_indexB].v;
	float wB = data.velocities[m_indexB].w;

	float mA = m_invMassA, mB = m_invMassB;
	float iA = m_invIA, iB = m_invIB;

	float h = data.step.dt;

	// Solve angular friction
	{
		float Cdot = wB - wA;
		float impulse = -m_angularMass * Cdot;

		float oldImpulse = m_angularImpulse;
		float maxImpulse = h * m_maxTorque;
		m_angularImpulse = cb2Clamp(m_angularImpulse + impulse, -maxImpulse, maxImpulse);
		impulse = m_angularImpulse - oldImpulse;

		wA -= iA * impulse;
		wB += iB * impulse;
	}

	// Solve linear friction
	{
		ci::Vec2f Cdot = vB + cb2Cross(wB, m_rB) - vA - cb2Cross(wA, m_rA);

		ci::Vec2f impulse = -cb2Mul(m_linearMass, Cdot);
		ci::Vec2f oldImpulse = m_linearImpulse;
		m_linearImpulse += impulse;

		float maxImpulse = h * m_maxForce;

		if (m_linearImpulse.lengthSquared() > maxImpulse * maxImpulse)
		{
			m_linearImpulse.normalize();
			m_linearImpulse *= maxImpulse;
		}

		impulse = m_linearImpulse - oldImpulse;

		vA -= mA * impulse;
		wA -= iA * cb2Cross(m_rA, impulse);

		vB += mB * impulse;
		wB += iB * cb2Cross(m_rB, impulse);
	}

	data.velocities[m_indexA].v = vA;
	data.velocities[m_indexA].w = wA;
	data.velocities[m_indexB].v = vB;
	data.velocities[m_indexB].w = wB;
}

bool cb2FrictionJoint::SolvePositionConstraints(const cb2SolverData& data)
{
	CB2_NOT_USED(data);

	return true;
}

ci::Vec2f cb2FrictionJoint::GetAnchorA() const
{
	return m_bodyA->GetWorldPoint(m_localAnchorA);
}

ci::Vec2f cb2FrictionJoint::GetAnchorB() const
{
	return m_bodyB->GetWorldPoint(m_localAnchorB);
}

ci::Vec2f cb2FrictionJoint::GetReactionForce(float inv_dt) const
{
	return inv_dt * m_linearImpulse;
}

float cb2FrictionJoint::GetReactionTorque(float inv_dt) const
{
	return inv_dt * m_angularImpulse;
}

void cb2FrictionJoint::SetMaxForce(float force)
{
	cb2Assert(cb2::isValid(force) && force >= 0.0f);
	m_maxForce = force;
}

float cb2FrictionJoint::GetMaxForce() const
{
	return m_maxForce;
}

void cb2FrictionJoint::SetMaxTorque(float torque)
{
	cb2Assert(cb2::isValid(torque) && torque >= 0.0f);
	m_maxTorque = torque;
}

float cb2FrictionJoint::GetMaxTorque() const
{
	return m_maxTorque;
}

void cb2FrictionJoint::Dump()
{
	int indexA = m_bodyA->m_islandIndex;
	int indexB = m_bodyB->m_islandIndex;

	cb2Log("  cb2FrictionJointDef jd;\n");
	cb2Log("  jd.bodyA = bodies[%d];\n", indexA);
	cb2Log("  jd.bodyB = bodies[%d];\n", indexB);
	cb2Log("  jd.collideConnected = bool(%d);\n", m_collideConnected);
	cb2Log("  jd.localAnchorA.set(%.15lef, %.15lef);\n", m_localAnchorA.x, m_localAnchorA.y);
	cb2Log("  jd.localAnchorB.set(%.15lef, %.15lef);\n", m_localAnchorB.x, m_localAnchorB.y);
	cb2Log("  jd.maxForce = %.15lef;\n", m_maxForce);
	cb2Log("  jd.maxTorque = %.15lef;\n", m_maxTorque);
	cb2Log("  joints[%d] = m_world->CreateJoint(&jd);\n", m_index);
}
