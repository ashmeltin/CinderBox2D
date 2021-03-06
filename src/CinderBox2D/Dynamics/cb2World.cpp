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

#include <CinderBox2D/Dynamics/cb2World.h>
#include <CinderBox2D/Dynamics/cb2Body.h>
#include <CinderBox2D/Dynamics/cb2Fixture.h>
#include <CinderBox2D/Dynamics/cb2Island.h>
#include <CinderBox2D/Dynamics/Joints/cb2PulleyJoint.h>
#include <CinderBox2D/Dynamics/Contacts/cb2Contact.h>
#include <CinderBox2D/Dynamics/Contacts/cb2ContactSolver.h>
#include <CinderBox2D/Collision/cb2Collision.h>
#include <CinderBox2D/Collision/cb2BroadPhase.h>
#include <CinderBox2D/Collision/Shapes/cb2CircleShape.h>
#include <CinderBox2D/Collision/Shapes/cb2EdgeShape.h>
#include <CinderBox2D/Collision/Shapes/cb2ChainShape.h>
#include <CinderBox2D/Collision/Shapes/cb2PolygonShape.h>
#include <CinderBox2D/Collision/cb2TimeOfImpact.h>
#include <CinderBox2D/Common/cb2Draw.h>
#include <CinderBox2D/Common/cb2Timer.h>
#include <new>

cb2World::cb2World(const ci::Vec2f& gravity)
{
	m_destructionListener = NULL;
	g_debugDraw = NULL;

	m_bodyList = NULL;
	m_jointList = NULL;

	m_bodyCount = 0;
	m_jointCount = 0;

	m_warmStarting = true;
	m_continuousPhysics = true;
	m_subStepping = false;

	m_stepComplete = true;

	m_allowSleep = true;
	m_gravity = gravity;

	m_flags = e_clearForces;

	m_inv_dt0 = 0.0f;

	m_contactManager.m_allocator = &m_blockAllocator;

	memset(&m_profile, 0, sizeof(cb2Profile));
}

cb2World::~cb2World()
{
	// Some shapes allocate using cb2Alloc.
	cb2Body* b = m_bodyList;
	while (b)
	{
		cb2Body* bNext = b->m_next;

		cb2Fixture* f = b->m_fixtureList;
		while (f)
		{
			cb2Fixture* fNext = f->m_next;
			f->m_proxyCount = 0;
			f->Destroy(&m_blockAllocator);
			f = fNext;
		}

		b = bNext;
	}
}

void cb2World::SetDestructionListener(cb2DestructionListener* listener)
{
	m_destructionListener = listener;
}

void cb2World::SetContactFilter(cb2ContactFilter* filter)
{
	m_contactManager.m_contactFilter = filter;
}

void cb2World::SetContactListener(cb2ContactListener* listener)
{
	m_contactManager.m_contactListener = listener;
}

void cb2World::SetDebugDraw(cb2Draw* debugDraw)
{
	g_debugDraw = debugDraw;
}

cb2Body* cb2World::CreateBody(const cb2BodyDef* def)
{
	cb2Assert(IsLocked() == false);
	if (IsLocked())
	{
		return NULL;
	}

	void* mem = m_blockAllocator.Allocate(sizeof(cb2Body));
	cb2Body* b = new (mem) cb2Body(def, this);

	// Add to world doubly linked list.
	b->m_prev = NULL;
	b->m_next = m_bodyList;
	if (m_bodyList)
	{
		m_bodyList->m_prev = b;
	}
	m_bodyList = b;
	++m_bodyCount;

	return b;
}

void cb2World::DestroyBody(cb2Body* b)
{
	cb2Assert(m_bodyCount > 0);
	cb2Assert(IsLocked() == false);
	if (IsLocked())
	{
		return;
	}

	// Delete the attached joints.
	cb2JointEdge* je = b->m_jointList;
	while (je)
	{
		cb2JointEdge* je0 = je;
		je = je->next;

		if (m_destructionListener)
		{
			m_destructionListener->SayGoodbye(je0->joint);
		}

		DestroyJoint(je0->joint);

		b->m_jointList = je;
	}
	b->m_jointList = NULL;

	// Delete the attached contacts.
	cb2ContactEdge* ce = b->m_contactList;
	while (ce)
	{
		cb2ContactEdge* ce0 = ce;
		ce = ce->next;
		m_contactManager.Destroy(ce0->contact);
	}
	b->m_contactList = NULL;

	// Delete the attached fixtures. This destroys broad-phase proxies.
	cb2Fixture* f = b->m_fixtureList;
	while (f)
	{
		cb2Fixture* f0 = f;
		f = f->m_next;

		if (m_destructionListener)
		{
			m_destructionListener->SayGoodbye(f0);
		}

		f0->DestroyProxies(&m_contactManager.m_broadPhase);
		f0->Destroy(&m_blockAllocator);
		f0->~cb2Fixture();
		m_blockAllocator.Free(f0, sizeof(cb2Fixture));

		b->m_fixtureList = f;
		b->m_fixtureCount -= 1;
	}
	b->m_fixtureList = NULL;
	b->m_fixtureCount = 0;

	// Remove world body list.
	if (b->m_prev)
	{
		b->m_prev->m_next = b->m_next;
	}

	if (b->m_next)
	{
		b->m_next->m_prev = b->m_prev;
	}

	if (b == m_bodyList)
	{
		m_bodyList = b->m_next;
	}

	--m_bodyCount;
	b->~cb2Body();
	m_blockAllocator.Free(b, sizeof(cb2Body));
}

cb2Joint* cb2World::CreateJoint(const cb2JointDef* def)
{
	cb2Assert(IsLocked() == false);
	if (IsLocked())
	{
		return NULL;
	}

	cb2Joint* j = cb2Joint::Create(def, &m_blockAllocator);

	// Connect to the world list.
	j->m_prev = NULL;
	j->m_next = m_jointList;
	if (m_jointList)
	{
		m_jointList->m_prev = j;
	}
	m_jointList = j;
	++m_jointCount;

	// Connect to the bodies' doubly linked lists.
	j->m_edgeA.joint = j;
	j->m_edgeA.other = j->m_bodyB;
	j->m_edgeA.prev = NULL;
	j->m_edgeA.next = j->m_bodyA->m_jointList;
	if (j->m_bodyA->m_jointList) j->m_bodyA->m_jointList->prev = &j->m_edgeA;
	j->m_bodyA->m_jointList = &j->m_edgeA;

	j->m_edgeB.joint = j;
	j->m_edgeB.other = j->m_bodyA;
	j->m_edgeB.prev = NULL;
	j->m_edgeB.next = j->m_bodyB->m_jointList;
	if (j->m_bodyB->m_jointList) j->m_bodyB->m_jointList->prev = &j->m_edgeB;
	j->m_bodyB->m_jointList = &j->m_edgeB;

	cb2Body* bodyA = def->bodyA;
	cb2Body* bodyB = def->bodyB;

	// If the joint prevents collisions, then flag any contacts for filtering.
	if (def->collideConnected == false)
	{
		cb2ContactEdge* edge = bodyB->GetContactList();
		while (edge)
		{
			if (edge->other == bodyA)
			{
				// Flag the contact for filtering at the next time step (where either
				// body is awake).
				edge->contact->FlagForFiltering();
			}

			edge = edge->next;
		}
	}

	// Note: creating a joint doesn't wake the bodies.

	return j;
}

void cb2World::DestroyJoint(cb2Joint* j)
{
	cb2Assert(IsLocked() == false);
	if (IsLocked())
	{
		return;
	}

	bool collideConnected = j->m_collideConnected;

	// Remove from the doubly linked list.
	if (j->m_prev)
	{
		j->m_prev->m_next = j->m_next;
	}

	if (j->m_next)
	{
		j->m_next->m_prev = j->m_prev;
	}

	if (j == m_jointList)
	{
		m_jointList = j->m_next;
	}

	// Disconnect from island graph.
	cb2Body* bodyA = j->m_bodyA;
	cb2Body* bodyB = j->m_bodyB;

	// Wake up connected bodies.
	bodyA->SetAwake(true);
	bodyB->SetAwake(true);

	// Remove from body 1.
	if (j->m_edgeA.prev)
	{
		j->m_edgeA.prev->next = j->m_edgeA.next;
	}

	if (j->m_edgeA.next)
	{
		j->m_edgeA.next->prev = j->m_edgeA.prev;
	}

	if (&j->m_edgeA == bodyA->m_jointList)
	{
		bodyA->m_jointList = j->m_edgeA.next;
	}

	j->m_edgeA.prev = NULL;
	j->m_edgeA.next = NULL;

	// Remove from body 2
	if (j->m_edgeB.prev)
	{
		j->m_edgeB.prev->next = j->m_edgeB.next;
	}

	if (j->m_edgeB.next)
	{
		j->m_edgeB.next->prev = j->m_edgeB.prev;
	}

	if (&j->m_edgeB == bodyB->m_jointList)
	{
		bodyB->m_jointList = j->m_edgeB.next;
	}

	j->m_edgeB.prev = NULL;
	j->m_edgeB.next = NULL;

	cb2Joint::Destroy(j, &m_blockAllocator);

	cb2Assert(m_jointCount > 0);
	--m_jointCount;

	// If the joint prevents collisions, then flag any contacts for filtering.
	if (collideConnected == false)
	{
		cb2ContactEdge* edge = bodyB->GetContactList();
		while (edge)
		{
			if (edge->other == bodyA)
			{
				// Flag the contact for filtering at the next time step (where either
				// body is awake).
				edge->contact->FlagForFiltering();
			}

			edge = edge->next;
		}
	}
}

//
void cb2World::SetAllowSleeping(bool flag)
{
	if (flag == m_allowSleep)
	{
		return;
	}

	m_allowSleep = flag;
	if (m_allowSleep == false)
	{
		for (cb2Body* b = m_bodyList; b; b = b->m_next)
		{
			b->SetAwake(true);
		}
	}
}

// Find islands, integrate and solve constraints, solve position constraints
void cb2World::Solve(const cb2TimeStep& step)
{
	m_profile.solveInit = 0.0f;
	m_profile.solveVelocity = 0.0f;
	m_profile.solvePosition = 0.0f;

	// Size the island for the worst case.
	cb2Island island(m_bodyCount,
					m_contactManager.m_contactCount,
					m_jointCount,
					&m_stackAllocator,
					m_contactManager.m_contactListener);

	// Clear all the island flags.
	for (cb2Body* b = m_bodyList; b; b = b->m_next)
	{
		b->m_flags &= ~cb2Body::e_islandFlag;
	}
	for (cb2Contact* c = m_contactManager.m_contactList; c; c = c->m_next)
	{
		c->m_flags &= ~cb2Contact::e_islandFlag;
	}
	for (cb2Joint* j = m_jointList; j; j = j->m_next)
	{
		j->m_islandFlag = false;
	}

	// Build and simulate all awake islands.
	int stackSize = m_bodyCount;
	cb2Body** stack = (cb2Body**)m_stackAllocator.Allocate(stackSize * sizeof(cb2Body*));
	for (cb2Body* seed = m_bodyList; seed; seed = seed->m_next)
	{
		if (seed->m_flags & cb2Body::e_islandFlag)
		{
			continue;
		}

		if (seed->IsAwake() == false || seed->IsActive() == false)
		{
			continue;
		}

		// The seed can be dynamic or kinematic.
		if (seed->GetType() == cb2_staticBody)
		{
			continue;
		}

		// Reset island and stack.
		island.Clear();
		int stackCount = 0;
		stack[stackCount++] = seed;
		seed->m_flags |= cb2Body::e_islandFlag;

		// Perform a depth first search (DFS) on the constraint graph.
		while (stackCount > 0)
		{
			// Grab the next body off the stack and add it to the island.
			cb2Body* b = stack[--stackCount];
			cb2Assert(b->IsActive() == true);
			island.Add(b);

			// Make sure the body is awake.
			b->SetAwake(true);

			// To keep islands as small as possible, we don't
			// propagate islands across static bodies.
			if (b->GetType() == cb2_staticBody)
			{
				continue;
			}

			// Search all contacts connected to this body.
			for (cb2ContactEdge* ce = b->m_contactList; ce; ce = ce->next)
			{
				cb2Contact* contact = ce->contact;

				// Has this contact already been added to an island?
				if (contact->m_flags & cb2Contact::e_islandFlag)
				{
					continue;
				}

				// Is this contact solid and touching?
				if (contact->IsEnabled() == false ||
					contact->IsTouching() == false)
				{
					continue;
				}

				// Skip sensors.
				bool sensorA = contact->m_fixtureA->m_isSensor;
				bool sensorB = contact->m_fixtureB->m_isSensor;
				if (sensorA || sensorB)
				{
					continue;
				}

				island.Add(contact);
				contact->m_flags |= cb2Contact::e_islandFlag;

				cb2Body* other = ce->other;

				// Was the other body already added to this island?
				if (other->m_flags & cb2Body::e_islandFlag)
				{
					continue;
				}

				cb2Assert(stackCount < stackSize);
				stack[stackCount++] = other;
				other->m_flags |= cb2Body::e_islandFlag;
			}

			// Search all joints connect to this body.
			for (cb2JointEdge* je = b->m_jointList; je; je = je->next)
			{
				if (je->joint->m_islandFlag == true)
				{
					continue;
				}

				cb2Body* other = je->other;

				// Don't simulate joints connected to inactive bodies.
				if (other->IsActive() == false)
				{
					continue;
				}

				island.Add(je->joint);
				je->joint->m_islandFlag = true;

				if (other->m_flags & cb2Body::e_islandFlag)
				{
					continue;
				}

				cb2Assert(stackCount < stackSize);
				stack[stackCount++] = other;
				other->m_flags |= cb2Body::e_islandFlag;
			}
		}

		cb2Profile profile;
		island.Solve(&profile, step, m_gravity, m_allowSleep);
		m_profile.solveInit += profile.solveInit;
		m_profile.solveVelocity += profile.solveVelocity;
		m_profile.solvePosition += profile.solvePosition;

		// Post solve cleanup.
		for (int i = 0; i < island.m_bodyCount; ++i)
		{
			// Allow static bodies to participate in other islands.
			cb2Body* b = island.m_bodies[i];
			if (b->GetType() == cb2_staticBody)
			{
				b->m_flags &= ~cb2Body::e_islandFlag;
			}
		}
	}

	m_stackAllocator.Free(stack);

	{
		cb2Timer timer;
		// Synchronize fixtures, check for out of range bodies.
		for (cb2Body* b = m_bodyList; b; b = b->GetNext())
		{
			// If a body was not in an island then it did not move.
			if ((b->m_flags & cb2Body::e_islandFlag) == 0)
			{
				continue;
			}

			if (b->GetType() == cb2_staticBody)
			{
				continue;
			}

			// Update fixtures (for broad-phase).
			b->SynchronizeFixtures();
		}

		// Look for new contacts.
		m_contactManager.FindNewContacts();
		m_profile.broadphase = timer.GetMilliseconds();
	}
}

// Find TOI contacts and solve them.
void cb2World::SolveTOI(const cb2TimeStep& step)
{
	cb2Island island(2 * cb2_maxTOIContacts, cb2_maxTOIContacts, 0, &m_stackAllocator, m_contactManager.m_contactListener);

	if (m_stepComplete)
	{
		for (cb2Body* b = m_bodyList; b; b = b->m_next)
		{
			b->m_flags &= ~cb2Body::e_islandFlag;
			b->m_sweep.alpha0 = 0.0f;
		}

		for (cb2Contact* c = m_contactManager.m_contactList; c; c = c->m_next)
		{
			// Invalidate TOI
			c->m_flags &= ~(cb2Contact::e_toiFlag | cb2Contact::e_islandFlag);
			c->m_toiCount = 0;
			c->m_toi = 1.0f;
		}
	}

	// Find TOI events and solve them.
	for (;;)
	{
		// Find the first TOI.
		cb2Contact* minContact = NULL;
		float minAlpha = 1.0f;

		for (cb2Contact* c = m_contactManager.m_contactList; c; c = c->m_next)
		{
			// Is this contact disabled?
			if (c->IsEnabled() == false)
			{
				continue;
			}

			// Prevent excessive sub-stepping.
			if (c->m_toiCount > cb2_maxSubSteps)
			{
				continue;
			}

			float alpha = 1.0f;
			if (c->m_flags & cb2Contact::e_toiFlag)
			{
				// This contact has a valid cached TOI.
				alpha = c->m_toi;
			}
			else
			{
				cb2Fixture* fA = c->GetFixtureA();
				cb2Fixture* fB = c->GetFixtureB();

				// Is there a sensor?
				if (fA->IsSensor() || fB->IsSensor())
				{
					continue;
				}

				cb2Body* bA = fA->GetBody();
				cb2Body* bB = fB->GetBody();

				cb2BodyType typeA = bA->m_type;
				cb2BodyType typeB = bB->m_type;
				cb2Assert(typeA == cb2_dynamicBody || typeB == cb2_dynamicBody);

				bool activeA = bA->IsAwake() && typeA != cb2_staticBody;
				bool activeB = bB->IsAwake() && typeB != cb2_staticBody;

				// Is at least one body active (awake and dynamic or kinematic)?
				if (activeA == false && activeB == false)
				{
					continue;
				}

				bool collideA = bA->IsBullet() || typeA != cb2_dynamicBody;
				bool collideB = bB->IsBullet() || typeB != cb2_dynamicBody;

				// Are these two non-bullet dynamic bodies?
				if (collideA == false && collideB == false)
				{
					continue;
				}

				// Compute the TOI for this contact.
				// Put the sweeps onto the same time interval.
				float alpha0 = bA->m_sweep.alpha0;

				if (bA->m_sweep.alpha0 < bB->m_sweep.alpha0)
				{
					alpha0 = bB->m_sweep.alpha0;
					bA->m_sweep.Advance(alpha0);
				}
				else if (bB->m_sweep.alpha0 < bA->m_sweep.alpha0)
				{
					alpha0 = bA->m_sweep.alpha0;
					bB->m_sweep.Advance(alpha0);
				}

				cb2Assert(alpha0 < 1.0f);

				int indexA = c->GetChildIndexA();
				int indexB = c->GetChildIndexB();

				// Compute the time of impact in interval [0, minTOI]
				cb2TOIInput input;
				input.proxyA.set(fA->GetShape(), indexA);
				input.proxyB.set(fB->GetShape(), indexB);
				input.sweepA = bA->m_sweep;
				input.sweepB = bB->m_sweep;
				input.tMax = 1.0f;

				cb2TOIOutput output;
				cb2TimeOfImpact(&output, &input);

				// Beta is the fraction of the remaining portion of the .
				float beta = output.t;
				if (output.state == cb2TOIOutput::e_touching)
				{
					alpha = cb2Min(alpha0 + (1.0f - alpha0) * beta, 1.0f);
				}
				else
				{
					alpha = 1.0f;
				}

				c->m_toi = alpha;
				c->m_flags |= cb2Contact::e_toiFlag;
			}

			if (alpha < minAlpha)
			{
				// This is the minimum TOI found so far.
				minContact = c;
				minAlpha = alpha;
			}
		}

		if (minContact == NULL || 1.0f - 10.0f * cb2_epsilon < minAlpha)
		{
			// No more TOI events. Done!
			m_stepComplete = true;
			break;
		}

		// Advance the bodies to the TOI.
		cb2Fixture* fA = minContact->GetFixtureA();
		cb2Fixture* fB = minContact->GetFixtureB();
		cb2Body* bA = fA->GetBody();
		cb2Body* bB = fB->GetBody();

		cb2Sweep backup1 = bA->m_sweep;
		cb2Sweep backup2 = bB->m_sweep;

		bA->Advance(minAlpha);
		bB->Advance(minAlpha);

		// The TOI contact likely has some new contact points.
		minContact->Update(m_contactManager.m_contactListener);
		minContact->m_flags &= ~cb2Contact::e_toiFlag;
		++minContact->m_toiCount;

		// Is the contact solid?
		if (minContact->IsEnabled() == false || minContact->IsTouching() == false)
		{
			// Restore the sweeps.
			minContact->SetEnabled(false);
			bA->m_sweep = backup1;
			bB->m_sweep = backup2;
			bA->SynchronizeTransform();
			bB->SynchronizeTransform();
			continue;
		}

		bA->SetAwake(true);
		bB->SetAwake(true);

		// Build the island
		island.Clear();
		island.Add(bA);
		island.Add(bB);
		island.Add(minContact);

		bA->m_flags |= cb2Body::e_islandFlag;
		bB->m_flags |= cb2Body::e_islandFlag;
		minContact->m_flags |= cb2Contact::e_islandFlag;

		// Get contacts on bodyA and bodyB.
		cb2Body* bodies[2] = {bA, bB};
		for (int i = 0; i < 2; ++i)
		{
			cb2Body* body = bodies[i];
			if (body->m_type == cb2_dynamicBody)
			{
				for (cb2ContactEdge* ce = body->m_contactList; ce; ce = ce->next)
				{
					if (island.m_bodyCount == island.m_bodyCapacity)
					{
						break;
					}

					if (island.m_contactCount == island.m_contactCapacity)
					{
						break;
					}

					cb2Contact* contact = ce->contact;

					// Has this contact already been added to the island?
					if (contact->m_flags & cb2Contact::e_islandFlag)
					{
						continue;
					}

					// Only add static, kinematic, or bullet bodies.
					cb2Body* other = ce->other;
					if (other->m_type == cb2_dynamicBody &&
						body->IsBullet() == false && other->IsBullet() == false)
					{
						continue;
					}

					// Skip sensors.
					bool sensorA = contact->m_fixtureA->m_isSensor;
					bool sensorB = contact->m_fixtureB->m_isSensor;
					if (sensorA || sensorB)
					{
						continue;
					}

					// Tentatively advance the body to the TOI.
					cb2Sweep backup = other->m_sweep;
					if ((other->m_flags & cb2Body::e_islandFlag) == 0)
					{
						other->Advance(minAlpha);
					}

					// Update the contact points
					contact->Update(m_contactManager.m_contactListener);

					// Was the contact disabled by the user?
					if (contact->IsEnabled() == false)
					{
						other->m_sweep = backup;
						other->SynchronizeTransform();
						continue;
					}

					// Are there contact points?
					if (contact->IsTouching() == false)
					{
						other->m_sweep = backup;
						other->SynchronizeTransform();
						continue;
					}

					// Add the contact to the island
					contact->m_flags |= cb2Contact::e_islandFlag;
					island.Add(contact);

					// Has the other body already been added to the island?
					if (other->m_flags & cb2Body::e_islandFlag)
					{
						continue;
					}
					
					// Add the other body to the island.
					other->m_flags |= cb2Body::e_islandFlag;

					if (other->m_type != cb2_staticBody)
					{
						other->SetAwake(true);
					}

					island.Add(other);
				}
			}
		}

		cb2TimeStep subStep;
		subStep.dt = (1.0f - minAlpha) * step.dt;
		subStep.inv_dt = 1.0f / subStep.dt;
		subStep.dtRatio = 1.0f;
		subStep.positionIterations = 20;
		subStep.velocityIterations = step.velocityIterations;
		subStep.warmStarting = false;
		island.SolveTOI(subStep, bA->m_islandIndex, bB->m_islandIndex);

		// Reset island flags and synchronize broad-phase proxies.
		for (int i = 0; i < island.m_bodyCount; ++i)
		{
			cb2Body* body = island.m_bodies[i];
			body->m_flags &= ~cb2Body::e_islandFlag;

			if (body->m_type != cb2_dynamicBody)
			{
				continue;
			}

			body->SynchronizeFixtures();

			// Invalidate all contact TOIs on this displaced body.
			for (cb2ContactEdge* ce = body->m_contactList; ce; ce = ce->next)
			{
				ce->contact->m_flags &= ~(cb2Contact::e_toiFlag | cb2Contact::e_islandFlag);
			}
		}

		// Commit fixture proxy movements to the broad-phase so that new contacts are created.
		// Also, some contacts can be destroyed.
		m_contactManager.FindNewContacts();

		if (m_subStepping)
		{
			m_stepComplete = false;
			break;
		}
	}
}

void cb2World::Step(float dt, int velocityIterations, int positionIterations)
{
	cb2Timer stepTimer;

	// If new fixtures were added, we need to find the new contacts.
	if (m_flags & e_newFixture)
	{
		m_contactManager.FindNewContacts();
		m_flags &= ~e_newFixture;
	}

	m_flags |= e_locked;

	cb2TimeStep step;
	step.dt = dt;
	step.velocityIterations	= velocityIterations;
	step.positionIterations = positionIterations;
	if (dt > 0.0f)
	{
		step.inv_dt = 1.0f / dt;
	}
	else
	{
		step.inv_dt = 0.0f;
	}

	step.dtRatio = m_inv_dt0 * dt;

	step.warmStarting = m_warmStarting;
	
	// Update contacts. This is where some contacts are destroyed.
	{
		cb2Timer timer;
		m_contactManager.Collide();
		m_profile.collide = timer.GetMilliseconds();
	}

	// Integrate velocities, solve velocity constraints, and integrate positions.
	if (m_stepComplete && step.dt > 0.0f)
	{
		cb2Timer timer;
		Solve(step);
		m_profile.solve = timer.GetMilliseconds();
	}

	// Handle TOI events.
	if (m_continuousPhysics && step.dt > 0.0f)
	{
		cb2Timer timer;
		SolveTOI(step);
		m_profile.solveTOI = timer.GetMilliseconds();
	}

	if (step.dt > 0.0f)
	{
		m_inv_dt0 = step.inv_dt;
	}

	if (m_flags & e_clearForces)
	{
		ClearForces();
	}

	m_flags &= ~e_locked;

	m_profile.step = stepTimer.GetMilliseconds();
}

void cb2World::ClearForces()
{
	for (cb2Body* body = m_bodyList; body; body = body->GetNext())
	{
		cb2::setZero(body->m_force);
		body->m_torque = 0.0f;
	}
}

struct cb2WorldQueryWrapper
{
	bool QueryCallback(int proxyId)
	{
		cb2FixtureProxy* proxy = (cb2FixtureProxy*)broadPhase->GetUserData(proxyId);
		return callback->ReportFixture(proxy->fixture);
	}

	const cb2BroadPhase* broadPhase;
	cb2QueryCallback* callback;
};

void cb2World::QueryAABB(cb2QueryCallback* callback, const cb2AABB& aabb) const
{
	cb2WorldQueryWrapper wrapper;
	wrapper.broadPhase = &m_contactManager.m_broadPhase;
	wrapper.callback = callback;
	m_contactManager.m_broadPhase.Query(&wrapper, aabb);
}

struct cb2WorldRayCastWrapper
{
	float RayCastCallback(const cb2RayCastInput& input, int proxyId)
	{
		void* userData = broadPhase->GetUserData(proxyId);
		cb2FixtureProxy* proxy = (cb2FixtureProxy*)userData;
		cb2Fixture* fixture = proxy->fixture;
		int index = proxy->childIndex;
		cb2RayCastOutput output;
		bool hit = fixture->RayCast(&output, input, index);

		if (hit)
		{
			float fraction = output.fraction;
			ci::Vec2f point = (1.0f - fraction) * input.p1 + fraction * input.p2;
			return callback->ReportFixture(fixture, point, output.normal, fraction);
		}

		return input.maxFraction;
	}

	const cb2BroadPhase* broadPhase;
	cb2RayCastCallback* callback;
};

void cb2World::RayCast(cb2RayCastCallback* callback, const ci::Vec2f& point1, const ci::Vec2f& point2) const
{
	cb2WorldRayCastWrapper wrapper;
	wrapper.broadPhase = &m_contactManager.m_broadPhase;
	wrapper.callback = callback;
	cb2RayCastInput input;
	input.maxFraction = 1.0f;
	input.p1 = point1;
	input.p2 = point2;
	m_contactManager.m_broadPhase.RayCast(&wrapper, input);
}

void cb2World::DrawShape(cb2Fixture* fixture, const cb2Transform& xf, const cb2Color& color)
{
	switch (fixture->GetType())
	{
	case cb2Shape::e_circle:
		{
			cb2CircleShape* circle = (cb2CircleShape*)fixture->GetShape();

			ci::Vec2f center = cb2Mul(xf, circle->m_p);
			float radius = circle->m_radius;
			ci::Vec2f axis = cb2Mul(xf.q, ci::Vec2f(1.0f, 0.0f));

			g_debugDraw->DrawSolidCircle(center, radius, axis, color);
		}
		break;

	case cb2Shape::e_edge:
		{
			cb2EdgeShape* edge = (cb2EdgeShape*)fixture->GetShape();
			ci::Vec2f v1 = cb2Mul(xf, edge->m_vertex1);
			ci::Vec2f v2 = cb2Mul(xf, edge->m_vertex2);
			g_debugDraw->DrawSegment(v1, v2, color);
		}
		break;

	case cb2Shape::e_chain:
		{
			cb2ChainShape* chain = (cb2ChainShape*)fixture->GetShape();
			int count = chain->m_count;
			const ci::Vec2f* vertices = chain->m_vertices;

			ci::Vec2f v1 = cb2Mul(xf, vertices[0]);
			for (int i = 1; i < count; ++i)
			{
				ci::Vec2f v2 = cb2Mul(xf, vertices[i]);
				g_debugDraw->DrawSegment(v1, v2, color);
				g_debugDraw->DrawCircle(v1, 0.05f, color);
				v1 = v2;
			}
		}
		break;

	case cb2Shape::e_polygon:
		{
			cb2PolygonShape* poly = (cb2PolygonShape*)fixture->GetShape();
			int vertexCount = poly->m_count;
			cb2Assert(vertexCount <= cb2_maxPolygonVertices);
			ci::Vec2f vertices[cb2_maxPolygonVertices];

			for (int i = 0; i < vertexCount; ++i)
			{
				vertices[i] = cb2Mul(xf, poly->m_vertices[i]);
			}

			g_debugDraw->DrawSolidPolygon(vertices, vertexCount, color);
		}
		break;
            
    default:
        break;
	}
}

void cb2World::DrawJoint(cb2Joint* joint)
{
	cb2Body* bodyA = joint->GetBodyA();
	cb2Body* bodyB = joint->GetBodyB();
	const cb2Transform& xf1 = bodyA->GetTransform();
	const cb2Transform& xf2 = bodyB->GetTransform();
	ci::Vec2f x1 = xf1.p;
	ci::Vec2f x2 = xf2.p;
	ci::Vec2f p1 = joint->GetAnchorA();
	ci::Vec2f p2 = joint->GetAnchorB();

	cb2Color color(0.5f, 0.8f, 0.8f);

	switch (joint->GetType())
	{
	case e_distanceJoint:
		g_debugDraw->DrawSegment(p1, p2, color);
		break;

	case e_pulleyJoint:
		{
			cb2PulleyJoint* pulley = (cb2PulleyJoint*)joint;
			ci::Vec2f s1 = pulley->GetGroundAnchorA();
			ci::Vec2f s2 = pulley->GetGroundAnchorB();
			g_debugDraw->DrawSegment(s1, p1, color);
			g_debugDraw->DrawSegment(s2, p2, color);
			g_debugDraw->DrawSegment(s1, s2, color);
		}
		break;

	case e_mouseJoint:
		// don't draw this
		break;

	default:
		g_debugDraw->DrawSegment(x1, p1, color);
		g_debugDraw->DrawSegment(p1, p2, color);
		g_debugDraw->DrawSegment(x2, p2, color);
	}
}

void cb2World::DrawDebugData()
{
	if (g_debugDraw == NULL)
	{
		return;
	}

	unsigned int flags = g_debugDraw->GetFlags();

	if (flags & cb2Draw::e_shapeBit)
	{
		for (cb2Body* b = m_bodyList; b; b = b->GetNext())
		{
			const cb2Transform& xf = b->GetTransform();
			for (cb2Fixture* f = b->GetFixtureList(); f; f = f->GetNext())
			{
				if (b->IsActive() == false)
				{
					DrawShape(f, xf, cb2Color(0.5f, 0.5f, 0.3f));
				}
				else if (b->GetType() == cb2_staticBody)
				{
					DrawShape(f, xf, cb2Color(0.5f, 0.9f, 0.5f));
				}
				else if (b->GetType() == cb2_kinematicBody)
				{
					DrawShape(f, xf, cb2Color(0.5f, 0.5f, 0.9f));
				}
				else if (b->IsAwake() == false)
				{
					DrawShape(f, xf, cb2Color(0.6f, 0.6f, 0.6f));
				}
				else
				{
					DrawShape(f, xf, cb2Color(0.9f, 0.7f, 0.7f));
				}
			}
		}
	}

	if (flags & cb2Draw::e_jointBit)
	{
		for (cb2Joint* j = m_jointList; j; j = j->GetNext())
		{
			DrawJoint(j);
		}
	}

	if (flags & cb2Draw::e_pairBit)
	{
		cb2Color color(0.3f, 0.9f, 0.9f);
		for (cb2Contact* c = m_contactManager.m_contactList; c; c = c->GetNext())
		{
			//cb2Fixture* fixtureA = c->GetFixtureA();
			//cb2Fixture* fixtureB = c->GetFixtureB();

			//ci::Vec2f cA = fixtureA->GetAABB().GetCenter();
			//ci::Vec2f cB = fixtureB->GetAABB().GetCenter();

			//g_debugDraw->DrawSegment(cA, cB, color);
		}
	}

	if (flags & cb2Draw::e_aabbBit)
	{
		cb2Color color(0.9f, 0.3f, 0.9f);
		cb2BroadPhase* bp = &m_contactManager.m_broadPhase;

		for (cb2Body* b = m_bodyList; b; b = b->GetNext())
		{
			if (b->IsActive() == false)
			{
				continue;
			}

			for (cb2Fixture* f = b->GetFixtureList(); f; f = f->GetNext())
			{
				for (int i = 0; i < f->m_proxyCount; ++i)
				{
					cb2FixtureProxy* proxy = f->m_proxies + i;
					cb2AABB aabb = bp->GetFatAABB(proxy->proxyId);
					ci::Vec2f vs[4];
					vs[0].set(aabb.lowerBound.x, aabb.lowerBound.y);
					vs[1].set(aabb.upperBound.x, aabb.lowerBound.y);
					vs[2].set(aabb.upperBound.x, aabb.upperBound.y);
					vs[3].set(aabb.lowerBound.x, aabb.upperBound.y);

					g_debugDraw->DrawPolygon(vs, 4, color);
				}
			}
		}
	}

	if (flags & cb2Draw::e_centerOfMassBit)
	{
		for (cb2Body* b = m_bodyList; b; b = b->GetNext())
		{
			cb2Transform xf = b->GetTransform();
			xf.p = b->GetWorldCenter();
			g_debugDraw->DrawTransform(xf);
		}
	}
}

int cb2World::GetProxyCount() const
{
	return m_contactManager.m_broadPhase.GetProxyCount();
}

int cb2World::GetTreeHeight() const
{
	return m_contactManager.m_broadPhase.GetTreeHeight();
}

int cb2World::GetTreeBalance() const
{
	return m_contactManager.m_broadPhase.GetTreeBalance();
}

float cb2World::GetTreeQuality() const
{
	return m_contactManager.m_broadPhase.GetTreeQuality();
}

void cb2World::ShiftOrigin(const ci::Vec2f& newOrigin)
{
	cb2Assert((m_flags & e_locked) == 0);
	if ((m_flags & e_locked) == e_locked)
	{
		return;
	}

	for (cb2Body* b = m_bodyList; b; b = b->m_next)
	{
		b->m_xf.p -= newOrigin;
		b->m_sweep.c0 -= newOrigin;
		b->m_sweep.c -= newOrigin;
	}

	for (cb2Joint* j = m_jointList; j; j = j->m_next)
	{
		j->ShiftOrigin(newOrigin);
	}

	m_contactManager.m_broadPhase.ShiftOrigin(newOrigin);
}

void cb2World::Dump()
{
	if ((m_flags & e_locked) == e_locked)
	{
		return;
	}

	cb2Log("ci::Vec2f g(%.15lef, %.15lef);\n", m_gravity.x, m_gravity.y);
	cb2Log("m_world->SetGravity(g);\n");

	cb2Log("cb2Body** bodies = (cb2Body**)cb2Alloc(%d * sizeof(cb2Body*));\n", m_bodyCount);
	cb2Log("cb2Joint** joints = (cb2Joint**)cb2Alloc(%d * sizeof(cb2Joint*));\n", m_jointCount);
	int i = 0;
	for (cb2Body* b = m_bodyList; b; b = b->m_next)
	{
		b->m_islandIndex = i;
		b->Dump();
		++i;
	}

	i = 0;
	for (cb2Joint* j = m_jointList; j; j = j->m_next)
	{
		j->m_index = i;
		++i;
	}

	// First pass on joints, skip gear joints.
	for (cb2Joint* j = m_jointList; j; j = j->m_next)
	{
		if (j->m_type == e_gearJoint)
		{
			continue;
		}

		cb2Log("{\n");
		j->Dump();
		cb2Log("}\n");
	}

	// Second pass on joints, only gear joints.
	for (cb2Joint* j = m_jointList; j; j = j->m_next)
	{
		if (j->m_type != e_gearJoint)
		{
			continue;
		}

		cb2Log("{\n");
		j->Dump();
		cb2Log("}\n");
	}

	cb2Log("cb2Free(joints);\n");
	cb2Log("cb2Free(bodies);\n");
	cb2Log("joints = NULL;\n");
	cb2Log("bodies = NULL;\n");
}
