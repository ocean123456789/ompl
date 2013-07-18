/*********************************************************************
* Software License Agreement (BSD License)
*
*  Copyright (c) 2012, Rice University
*  All rights reserved.
*
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions
*  are met:
*
*   * Redistributions of source code must retain the above copyright
*     notice, this list of conditions and the following disclaimer.
*   * Redistributions in binary form must reproduce the above
*     copyright notice, this list of conditions and the following
*     disclaimer in the documentation and/or other materials provided
*     with the distribution.
*   * Neither the name of the Rice University nor the names of its
*     contributors may be used to endorse or promote products derived
*     from this software without specific prior written permission.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
*  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
*  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
*  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
*  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
*  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
*  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
*  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
*  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
*  POSSIBILITY OF SUCH DAMAGE.
*********************************************************************/

/* Author: Beck Chen, Mark Moll */

/**
\file Koules.cpp
\brief This file contains an elaborate demo to solve the game of
[Koules](http://www.ucw.cz/~hubicka/koules/English/).

This problem was used to illustrate the capabilities of the PDST planner to
find trajectories for underactuated systems with drift. The details can be
found in the references below [1,2]. The physics have been made significantly
harder compared to the original game. We have tried to recreate the problem as
closely as possible to the one described in [2]. The demo can solve just one
level of Koules, all levels, or run a number of planners on one level as a
benchmarking run.

This demo illustrates also many advanced OMPL concepts, such as classes for
a custom state space, a control sampler, a projection, a state propagator,
and a goal class. It also demonstrates how one could put a simple bang-bang
controller inside the StatePropagator. In this demo the
(Directed)ControlSampler simply samples a target velocity vector and inside
the StatePropagator the control is chosen to drive the ship to attain this
velocity.

[1] A. M. Ladd and L. E. Kavraki, “Motion planning in the presence of drift,
underactuation and discrete system changes,” in Robotics: Science and Systems
I, (Boston, MA), pp. 233–241, MIT Press, June 2005.

[2] A. M. Ladd, Motion Planning for Physical Simulation. PhD thesis, Dept. of
Computer Science, Rice University, Houston, TX, Dec. 2006.
*/

#include <ompl/base/spaces/SE2StateSpace.h>
#include <ompl/base/goals/GoalSampleableRegion.h>
#include <ompl/datastructures/NearestNeighborsSqrtApprox.h>
#include <ompl/control/spaces/RealVectorControlSpace.h>
#include <ompl/control/planners/rrt/RRT.h>
#include <ompl/control/planners/kpiece/KPIECE1.h>
#include <ompl/control/planners/est/EST.h>
#include <ompl/control/planners/pdst/PDST.h>
#include <ompl/tools/benchmark/Benchmark.h>
#include <ompl/config.h>
#include <boost/math/constants/constants.hpp>
#include <boost/program_options.hpp>
#include <boost/format.hpp>
#include <fstream>

// size of the square that defines workspace
const double sideLength = 1.;
// koule properties
const double kouleMass = .5;
const double kouleRadius = .015;
// ship properties
const double shipAcceleration = 1.;
const double shipRotVel = boost::math::constants::pi<double>();
const double shipMass = .75;
const double shipRadius = .03;
const double shipVmax = .5 / shipAcceleration;
const double shipVmin = .1 * shipVmax;
// dynamics, propagation, integration, control constants
const double lambda_c = 4.;
const double h = .05;
const double integrationStepSize = 1e-2;
const double propagationStepSize = .05;
const unsigned int propagationMinSteps = 1;
const unsigned int propagationMaxSteps = 100;
const double shipDelta = .5 * shipAcceleration * propagationStepSize;
const double shipEps = .5 * shipRotVel * propagationStepSize;
// number of attempts at each level when solving n-level koules
const unsigned int numAttempts = 1;

namespace ob = ompl::base;
namespace oc = ompl::control;
namespace og = ompl::geometric;
namespace ot = ompl::tools;
namespace po = boost::program_options;

// lightweight signed SO(2) distance; assumes x and y are in [-pi,pi]
double signedSO2Distance(double x, double y)
{
    double d0 = x - y;
    if (d0 < -boost::math::constants::pi<double>())
        return d0 + 2. * boost::math::constants::pi<double>();
    if (d0 > boost::math::constants::pi<double>())
        return d0 - 2. * boost::math::constants::pi<double>();
    return d0;
}

// A projection for the KoulesStateSpace
class KoulesProjection : public ob::ProjectionEvaluator
{
public:
    KoulesProjection(const ob::StateSpace* space, unsigned int numDimensions = 3)
        : ob::ProjectionEvaluator(space), numDimensions_(numDimensions)
    {
        unsigned int n = (space_->getDimension() - 1) / 2 + 1;
        if (numDimensions_ > n)
            numDimensions_ = n;
        else if (numDimensions_ < 3)
            numDimensions_ = 3;
    }

    virtual unsigned int getDimension(void) const
    {
        return numDimensions_;
    }
    virtual void defaultCellSizes(void)
    {
        cellSizes_.resize(numDimensions_, .05);
    }
    virtual void project(const ob::State *state, ob::EuclideanProjection &projection) const
    {
        const ob::CompoundStateSpace::StateType* cs = state->as<ob::CompoundStateSpace::StateType>();
        const double* xv = cs->as<ob::RealVectorStateSpace::StateType>(0)->values;
        const double theta = cs->as<ob::SO2StateSpace::StateType>(1)->value;
        unsigned int numKoules = (numDimensions_ - 3) / 2;
        // projection with coordinates in the same order as described in Andrew Ladd's thesis
        projection[0] = xv[4 * numKoules];
        projection[1] = xv[4 * numKoules + 1];
        projection[2] = theta;
        for (unsigned int i = 0; i < numKoules; ++i)
        {
            projection[2 * i + 3] = xv[4 * i];
            projection[2 * i + 4] = xv[4 * i + 1];
        }
    }
protected:
    unsigned int numDimensions_;
};

class KoulesStateSpace : public ob::CompoundStateSpace
{
public:
    KoulesStateSpace(unsigned int numKoules)
        : CompoundStateSpace(), mass_(numKoules + 1, kouleMass), radius_(numKoules + 1, kouleRadius)
    {
        mass_[numKoules] = shipMass;
        radius_[numKoules] = shipRadius;
        setName("Koules" + boost::lexical_cast<std::string>(numKoules) + getName());
        // layout: (... x_i y_i vx_i vy_i ... x_s y_s vx_s vy_s theta_s),
        // where (x_i, y_i) is the position of koule i (i=1,..,numKoules),
        // (vx_i, vy_i) its velocity, (x_s, y_s) the position of the ship,
        // (vx_s, vy_s) its velocity, and theta_s its orientation.
        addSubspace(ob::StateSpacePtr(new ob::RealVectorStateSpace(4 * (numKoules + 1))), 1.);
        addSubspace(ob::StateSpacePtr(new ob::SO2StateSpace), .5);
        lock();

        // create the bounds
        ob::RealVectorBounds bounds((numKoules + 1) * 4);
        unsigned int j = 0;
        for (unsigned int i = 0; i < numKoules; ++i)
        {
            // set the bounds for koule i's position
            bounds.setLow(j, -kouleRadius);
            bounds.setHigh(j++, sideLength + kouleRadius);
            bounds.setLow(j, -kouleRadius);
            bounds.setHigh(j++, sideLength + kouleRadius);
            // set the bounds for koule i's velocity
            bounds.setLow(j, -10);
            bounds.setHigh(j++, 10.);
            bounds.setLow(j, -10.);
            bounds.setHigh(j++, 10.);
        }
        // set the bounds for the ship's position
        bounds.setLow(j, shipRadius);
        bounds.setHigh(j++, sideLength - shipRadius);
        bounds.setLow(j, shipRadius);
        bounds.setHigh(j++, sideLength - shipRadius);
        // set the bounds for the ship's velocity
        bounds.setLow(j, -10.);
        bounds.setHigh(j++, 10.);
        bounds.setLow(j, -10.);
        bounds.setHigh(j++, 10.);
        as<ob::RealVectorStateSpace>(0)->setBounds(bounds);
    }
    virtual void registerProjections(void)
    {
        registerDefaultProjection(ob::ProjectionEvaluatorPtr(new KoulesProjection(this)));
        registerProjection("PDSTProjection", ob::ProjectionEvaluatorPtr(
            new KoulesProjection(this, (getDimension() - 1) / 2 + 1)));
    }
    double getMass(unsigned int i) const
    {
        return mass_[i];
    }
    double getRadius(unsigned int i) const
    {
        return radius_[i];
    }
protected:
    std::vector<double> mass_;
    std::vector<double> radius_;
};

// Control sampler for KouleStateSpace
class KoulesControlSampler : public oc::ControlSampler
{
public:
    KoulesControlSampler(const oc::ControlSpace *space) : oc::ControlSampler(space)
    {
    }
    // Sample random velocity with magnitude between vmin and vmax and
    // orientation uniformly random over [0, 2*pi].
    // (This method is not actually ever called.)
    void sample(oc::Control *control)
    {
        const ob::RealVectorBounds &bounds = space_->as<oc::RealVectorControlSpace>()->getBounds();
        oc::RealVectorControlSpace::ControlType *rcontrol =
            control->as<oc::RealVectorControlSpace::ControlType>();
        double r = rng_.uniformReal(bounds.low[0], bounds.high[0]);
        double theta = rng_.uniformReal(0., 2. * boost::math::constants::pi<double>());
        rcontrol->values[0] = r * cos(theta);
        rcontrol->values[1] = r * sin(theta);
    }
    // sample random velocity with magnitude between vmin and vmax and
    // direction given by the normalized vector from the current position
    // in state and a random point in the workspace
    virtual void sample(oc::Control *control, const ob::State *state)
    {
        steer(control, state, rng_.uniformReal(0., sideLength), rng_.uniformReal(0., sideLength));
    }
    virtual void sampleNext(oc::Control *control, const oc::Control * /* previous */, const ob::State *state)
    {
        sample(control, state);
    }
    virtual void steer(oc::Control *control, const ob::State *state, double x, double y)
    {
        const KoulesStateSpace::StateType* s = state->as<KoulesStateSpace::StateType>();
        const double* r = s->as<ob::RealVectorStateSpace::StateType>(0)->values;
        unsigned int dim = space_->getStateSpace()->getDimension();
        double dx = x - r[dim - 5];
        double dy = y - r[dim - 4];
        double xNrm2 = dx * dx + dy * dy;
        if (xNrm2 > std::numeric_limits<float>::epsilon())
        {
            const ob::RealVectorBounds &bounds = space_->as<oc::RealVectorControlSpace>()->getBounds();
            double v = rng_.uniformReal(bounds.low[0], bounds.high[0]) / sqrt(xNrm2);
            oc::RealVectorControlSpace::ControlType *rcontrol =
                control->as<oc::RealVectorControlSpace::ControlType>();
            rcontrol->values[0] = v * dx;
            rcontrol->values[1] = v * dy;
        }
        else
            sample(control);
    }

protected:
    ompl::RNG rng_;
};

// Directed control sampler
class KoulesDirectedControlSampler : public oc::DirectedControlSampler
{
public:
    KoulesDirectedControlSampler(const oc::SpaceInformation *si, const ob::GoalPtr &goal, bool propagateMax)
        : DirectedControlSampler(si), cs_(si->getControlSpace().get()),
        goal_(goal), statePropagator_(si->getStatePropagator()), propagateMax_(propagateMax)
    {
    }
    // This sampleTo implementation contains a modified version of the method
    // ompl::control::SpaceInformation::propagateWhileValid, with the key difference
    // that sampleTo also terminates when the goal is reached.
    virtual unsigned int sampleTo(oc::Control *control, const ob::State *source, ob::State *dest)
    {
        const KoulesStateSpace::StateType* dst = dest->as<KoulesStateSpace::StateType>();
        const double* dstPos = dst->as<ob::RealVectorStateSpace::StateType>(0)->values;
        double stepSize = si_->getPropagationStepSize();
        unsigned int steps = propagateMax_ ? si_->getMaxControlDuration() :
            cs_.sampleStepCount(si_->getMinControlDuration(), si_->getMaxControlDuration());
        unsigned int dim = si_->getStateSpace()->getDimension();

        cs_.steer(control, source, dstPos[dim - 5], dstPos[dim - 4]);
        // perform the first step of propagation
        statePropagator_->propagate(source, control, stepSize, dest);
        // if we reached the goal, we're done
        if (goal_->isSatisfied(dest))
            return 1;
        // if we found a valid state after one step, we can go on
        else if (si_->isValid(dest))
        {
            ob::State *temp1 = dest, *temp2 = si_->allocState(), *toDelete = temp2;
            unsigned int r = steps;
            for (unsigned int i = 1 ; i < steps ; ++i)
            {
                statePropagator_->propagate(temp1, control, stepSize, temp2);
                if (goal_->isSatisfied(dest))
                {
                    si_->copyState(dest, temp2);
                    si_->freeState(toDelete);
                    return i + 1;
                }
                else if (si_->isValid(temp2))
                    std::swap(temp1, temp2);
                else
                {
                    // the last valid state is temp1;
                    r = i;
                    break;
                }
            }
            // if we finished the for-loop without finding an invalid state, the last valid state is temp1
            // make sure dest contains that information
            if (dest != temp1)
                si_->copyState(dest, temp1);
            si_->freeState(toDelete);
            return r;
        }
        // if the first propagation step produced an invalid step, return 0 steps
        // the last valid state is the starting one (assumed to be valid)
        else
        {
            if (dest != source)
                si_->copyState(dest, source);
            return 0;
        }
    }
    virtual unsigned int sampleTo(oc::Control *control, const oc::Control * /* previous */,
        const ob::State *source, ob::State *dest)
    {
        return sampleTo(control, source, dest);
    }
protected:
    KoulesControlSampler          cs_;
    ompl::RNG                     rng_;
    const ob::GoalPtr             goal_;
    const oc::StatePropagatorPtr  statePropagator_;
    bool                          propagateMax_;
};

oc::ControlSamplerPtr KoulesControlSamplerAllocator(const oc::ControlSpace* cspace)
{
    return oc::ControlSamplerPtr(new KoulesControlSampler(cspace));
}
oc::DirectedControlSamplerPtr KoulesDirectedControlSamplerAllocator(
    const oc::SpaceInformation *si, const ob::GoalPtr &goal, bool propagateMax)
{
    return oc::DirectedControlSamplerPtr(new KoulesDirectedControlSampler(si, goal, propagateMax));
}

// State propagator for KouleModel.
class KoulesStatePropagator : public oc::StatePropagator
{
public:

    KoulesStatePropagator(const oc::SpaceInformationPtr &si) :
        oc::StatePropagator(si), timeStep_(integrationStepSize),
        numDimensions_(si->getStateSpace()->getDimension()),
        numKoules_((numDimensions_ - 5) / 4),
        q(numDimensions_), qdot(numDimensions_), hasCollision(numKoules_ + 1)
    {
    }

    virtual void propagate(const ob::State *start, const oc::Control* control,
        const double duration, ob::State *result) const
    {
        const double* cval = control->as<oc::RealVectorControlSpace::ControlType>()->values;
        unsigned int numSteps = ceil(duration / timeStep_), offset = 4 * numKoules_;
        double dt = duration / (double)numSteps, u[3] = {0., 0., 0.};

        si_->getStateSpace()->copyToReals(q, start);

        double v[2] = { cval[0] - q[offset + 2], cval[1] - q[offset + 3]};
        double deltaTheta = signedSO2Distance(atan2(v[1], v[0]), q[offset + 4]);
        if (v[0]*v[0] + v[1]*v[1] > shipDelta * shipDelta)
        {
            if (std::abs(deltaTheta) < shipEps)
            {
                u[0] = shipAcceleration * cos(q[offset + 4]);
                u[1] = shipAcceleration * sin(q[offset + 4]);
                u[2] = 0.;
            }
            else if (deltaTheta > 0)
                u[2] = shipRotVel;
            else
                u[2] = -shipRotVel;
        }
        for (unsigned int i = 0; i < numSteps; ++i)
        {
            ode(u);
            update(dt);
        }
        si_->getStateSpace()->copyFromReals(result, q);
        // Normalize orientation between -pi and pi
        si_->getStateSpace()->as<ob::CompoundStateSpace>()->as<ob::SO2StateSpace>(1)
            ->enforceBounds(result->as<ob::CompoundStateSpace::StateType>()
                ->as<ob::SO2StateSpace::StateType>(1));
    }

protected:

    void ode(double* u) const
    {
        // koules: qdot[4*i, 4*i + 1] is xdot, qdot[4*i + 2, 4*i + 3] is vdot
        unsigned int offset = 4 * numKoules_;
        for (unsigned int i = 0; i < offset; i += 4)
        {
            qdot[i    ] = q[i + 2];
            qdot[i + 1] = q[i + 3];
            qdot[i + 2] = (.5 * sideLength - q[i    ]) * lambda_c - q[i + 2] * h;
            qdot[i + 3] = (.5 * sideLength - q[i + 1]) * lambda_c - q[i + 3] * h;
        }
        // ship: qdot[offset, offset + 1] is xdot
        // ship: qdot[offset + 4] + ] is thetadot, qdot[offset + 2, offset + 3] is vdot
        qdot[offset    ] = q[offset + 2];
        qdot[offset + 1] = q[offset + 3];
        qdot[offset + 2] = u[0];
        qdot[offset + 3] = u[1];
        qdot[offset + 4] = u[2];
    }

    void update(double dt) const
    {
        // update collisions
        std::fill(hasCollision.begin(), hasCollision.end(), false);
        for (unsigned int i = 0; i < numKoules_; i++)
            for (unsigned int j = i + 1; j <= numKoules_; j++)
                if (checkCollision(i, j, dt))
                    hasCollision[i] = hasCollision[j] = true;

        // update objects with no collision according to qdot
        for (unsigned int i = 0; i < numKoules_; ++i)
            if (!hasCollision[i])
                for (unsigned int j = 0; j < 4; ++j)
                    q[4 * i + j] += qdot[4 * i + j] * dt;
        if (!hasCollision[numKoules_])
            for (unsigned int j = 0; j < 5; ++j)
                q[4 * numKoules_ + j] += qdot[4 * numKoules_ + j] * dt;
    }

    // check collision among object i and j
    // compute elastic collision response if i and j collide
    // see http://en.wikipedia.org/wiki/Elastic_collision
    bool checkCollision(unsigned int i, unsigned int j, double dt) const
    {
        static const float delta = 1e-5;
        double *a = &q[4 * i], *b = &q[4 * j];
        double dx = a[0] - b[0], dy = a[1] - b[1];
        double dist = dx * dx + dy * dy;
        double minDist = si_->getStateSpace()->as<KoulesStateSpace>()->getRadius(i) +
            si_->getStateSpace()->as<KoulesStateSpace>()->getRadius(j) + delta;
        if (dist < minDist*minDist && ((b[2] - a[2]) * dx + (b[3] - a[3]) * dy > 0))
        // close enough and moving closer; elastic collision happens
        {
            dist = std::sqrt(dist);
            // compute unit normal and tangent vectors
            double normal[2] = {dx / dist, dy / dist};
            double tangent[2] = {-normal[1], normal[0]};

            // compute scalar projections of velocities onto normal and tangent vectors
            double aNormal = normal[0] * a[2] + normal[1] * a[3];
            double aTangentPrime = tangent[0] * a[2] + tangent[1] * a[3];
            double bNormal = normal[0] * b[2] + normal[1] * b[3];
            double bTangentPrime = tangent[0] * b[2] + tangent[1] * b[3];

            // compute new velocities using one-dimensional elastic collision in the normal direction
            double massA = si_->getStateSpace()->as<KoulesStateSpace>()->getMass(i);
            double massB = si_->getStateSpace()->as<KoulesStateSpace>()->getMass(j);
            double aNormalPrime = (aNormal * (massA - massB) + 2. * massB * bNormal) / (massA + massB);
            double bNormalPrime = (bNormal * (massB - massA) + 2. * massA * aNormal) / (massA + massB);

            // compute new normal and tangential velocity vectors
            double aNewNormalVel[2] = {normal[0] * aNormalPrime, normal[1] * aNormalPrime};
            double aNewTangentVel[2] = {tangent[0] * aTangentPrime, tangent[1] * aTangentPrime};
            double bNewNormalVel[2] = {normal[0] * bNormalPrime, normal[1] * bNormalPrime};
            double bNewTangentVel[2] = {tangent[0] * bTangentPrime, tangent[1] * bTangentPrime};

            // compute new velocities
            double bNewVel[2] = { bNewNormalVel[0] + bNewTangentVel[0], bNewNormalVel[1] + bNewTangentVel[1] };
            double aNewVel[2] = { aNewNormalVel[0] + aNewTangentVel[0], aNewNormalVel[1] + aNewTangentVel[1] };

            // preservation of momemtum
            assert(std::abs(massA * (a[2]-aNewVel[0]) + massB * (b[2]-bNewVel[0])) < 1e-6);
            assert(std::abs(massA * (a[3]-aNewVel[1]) + massB * (b[3]-bNewVel[1])) < 1e-6);
            // preservation of kinetic energy
            assert(std::abs(massA * (a[2]*a[2] + a[3]*a[3] - aNewVel[0]*aNewVel[0] - aNewVel[1]*aNewVel[1])
                + massB * (b[2]*b[2] + b[3]*b[3] - bNewVel[0]*bNewVel[0] - bNewVel[1]*bNewVel[1])) < 1e-6);

            // update state if collision happens
            a[0] += aNewVel[0] * dt;
            a[1] += aNewVel[1] * dt;
            a[2] = aNewVel[0];
            a[3] = aNewVel[1];
            b[0] += bNewVel[0] * dt;
            b[1] += bNewVel[1] * dt;
            b[2] = bNewVel[0];
            b[3] = bNewVel[1];

            return true;
        }
        else
            return false;
    }

    double timeStep_;
    unsigned int numDimensions_;
    unsigned int numKoules_;
    // The next three elements are scratch space. This is normally a very BAD
    // idea, since planners can be multi-threaded. However, none of the
    // planners used here are multi-threaded, so it's safe. This way the
    // propagate function doesn't have to allocate memory upon each call.
    mutable std::vector<double> q;
    mutable std::vector<double> qdot;
    mutable std::vector<bool> hasCollision;
};


// Sampleable goal region for KoulesModel.
class KoulesGoal : public ob::GoalSampleableRegion
{
public:
    KoulesGoal(const ob::SpaceInformationPtr &si)
        : ob::GoalSampleableRegion(si), stateSampler_(si->allocStateSampler())
    {
        threshold_ = 0.01;
        numKoules_ = (si->getStateDimension() - 5) / 4;
    }

    virtual double distanceGoal(const ob::State *st) const
    {
        // the shortest distance between any koule and an edge
        double minDist = sideLength;
        double minX, minY;
        const double* v = st->as<ob::CompoundStateSpace::StateType>()
            ->as<ob::RealVectorStateSpace::StateType>(0)->values;
        for (unsigned int i = 0; i < numKoules_; ++i)
        {
            minX = std::min(v[4 * i    ], sideLength - v[4 * i    ]);
            minY = std::min(v[4 * i + 1], sideLength - v[4 * i + 1]);
            minDist = std::min(minDist, std::min(minX, minY) - kouleRadius + threshold_);
        }
        if (minDist < 0)
            minDist = 0;
        return minDist;
    }

    virtual unsigned int maxSampleCount(void) const
    {
        return 100;
    }

    virtual void sampleGoal(ob::State *st) const
    {
        double* v = st->as<ob::CompoundStateSpace::StateType>()
            ->as<ob::RealVectorStateSpace::StateType>(0)->values;
        stateSampler_->sampleUniform(st);
        for (unsigned i = 0; i < numKoules_; ++i)
        {
            // randomly pick an edge for each koule to collide
            if (rng_.uniformBool())
            {
                v[4 * i    ] = rng_.uniformBool() ? 0. : sideLength;
                v[4 * i + 1] = rng_.uniformReal(0., sideLength);
            }
            else
            {
                v[4 * i    ] = rng_.uniformReal(0., sideLength);
                v[4 * i + 1] = rng_.uniformBool() ? 0. : sideLength;
            }
        }
    }

private:
    mutable ompl::RNG rng_;
    ob::StateSamplerPtr stateSampler_;
    unsigned int numKoules_;
};

bool isStateValid(const oc::SpaceInformation *si, const ob::State *state)
{
    return si->satisfiesBounds(state);
}

ob::PlannerPtr getPlanner(const std::string& plannerName, const oc::SpaceInformationPtr& si)
{
    if (plannerName == "rrt")
    {
        ob::PlannerPtr rrtplanner(new oc::RRT(si));
        rrtplanner->as<oc::RRT>()->setIntermediateStates(true);
        return rrtplanner;
    }
    else if (plannerName == "est")
        return ob::PlannerPtr(new oc::EST(si));
    else if (plannerName == "kpiece")
        return ob::PlannerPtr(new oc::KPIECE1(si));
    else
    {
        ob::PlannerPtr pdstplanner(new oc::PDST(si));
        pdstplanner->as<oc::PDST>()->setProjectionEvaluator(
            si->getStateSpace()->getProjection("PDSTProjection"));
        return pdstplanner;
    }
}

oc::SimpleSetup* koulesSetup(unsigned int numKoules, const std::string& plannerName, const std::vector<double>& stateVec = std::vector<double>())
{
    // construct state space
    ob::StateSpacePtr space(new KoulesStateSpace(numKoules));
    space->setup();
    // construct control space
    oc::ControlSpacePtr cspace(new oc::RealVectorControlSpace(space, 2));
    ob::RealVectorBounds cbounds(2);
    cbounds.setLow(shipVmin);
    cbounds.setHigh(shipVmax);
    cspace->as<oc::RealVectorControlSpace>()->setBounds(cbounds);
    // set control sampler allocator
    cspace->setControlSamplerAllocator(KoulesControlSamplerAllocator);

    // define a simple setup class
    oc::SimpleSetup* ss = new oc::SimpleSetup(cspace);
    oc::SpaceInformationPtr si = ss->getSpaceInformation();
    // setup start state
    ob::ScopedState<> start(space);
    if (stateVec.size() == space->getDimension())
        space->copyFromReals(start.get(), stateVec);
    else
    {
        // Pick koule positions evenly radially distributed, but at a linearly
        // increasing distance from the center. The ship's initial position is
        // at the center. Initial velocities are 0.
        std::vector<double> startVec(space->getDimension(), 0.);
        double r, theta = boost::math::constants::pi<double>(), delta = 2.*theta / numKoules;
        for (unsigned int i = 0; i < numKoules; ++i, theta += delta)
        {
            r = .1 + i * .1 / numKoules;
            startVec[4 * i    ] = .5 * sideLength + r * cos(theta);
            startVec[4 * i + 1] = .5 * sideLength + r * sin(theta);
        }
        startVec[4 * numKoules    ] = .5 * sideLength;
        startVec[4 * numKoules + 1] = .5 * sideLength;
        startVec[4 * numKoules + 4] = .5 * delta;
        space->copyFromReals(start.get(), startVec);
    }
    ss->setStartState(start);
    // set goal
    ss->setGoal(ob::GoalPtr(new KoulesGoal(si)));
    // set propagation step size
    si->setPropagationStepSize(propagationStepSize);
    // set min/max propagation steps
    si->setMinMaxControlDuration(propagationMinSteps, propagationMaxSteps);
    // set directed control sampler; when using the PDST planner, propagate as long as possible
    si->setDirectedControlSamplerAllocator(
        boost::bind(&KoulesDirectedControlSamplerAllocator, _1, ss->getGoal(), plannerName == "pdst"));
    // set planner
    ss->setPlanner(getPlanner(plannerName, si));
    // set validity checker
    ss->setStateValidityChecker(boost::bind(&isStateValid, si.get(), _1));
    // set state propagator
    ss->setStatePropagator(oc::StatePropagatorPtr(new KoulesStatePropagator(si)));
    return ss;
}
oc::SimpleSetup* koulesSetup(unsigned int numKoules, const std::string& plannerName, double kouleVel)
{
    oc::SimpleSetup* ss = koulesSetup(numKoules, plannerName);
    double* state = ss->getProblemDefinition()->getStartState(0)->as<KoulesStateSpace::StateType>()
        ->as<ob::RealVectorStateSpace::StateType>(0)->values;
    double theta;
    ompl::RNG rng;
    for (unsigned int i = 0; i < numKoules; ++i)
    {
        theta = rng.uniformReal(0., 2. * boost::math::constants::pi<double>());
        state[4 * i + 2] = kouleVel * cos(theta);
        state[4 * i + 3] = kouleVel * sin(theta);
    }
    return ss;
}

void planOneLevel(oc::SimpleSetup& ss, double maxTime, const std::string& plannerName,
    const std::string& outputFile)
{
    if (ss.solve(maxTime))
    {
        std::ofstream out(outputFile.c_str());
        oc::PathControl path(ss.getSolutionPath());
        path.interpolate();
        if (!path.check())
            OMPL_ERROR("Path is invalid");
        path.printAsMatrix(out);
        if (!ss.haveExactSolutionPath())
            OMPL_INFORM("Solution is approximate. Distance to actual goal is %g",
                ss.getProblemDefinition()->getSolutionDifference());
        OMPL_INFORM("Output saved in %s", outputFile.c_str());
    }

#if 0
    // Get the planner data, save the ship's (x,y) coordinates to one file and
    // the edge information to another file. This can be used for debugging
    // purposes; plotting the tree of states might give you some idea of
    // a planner's strategy.
    ob::PlannerData pd(ss.getSpaceInformation());
    ss.getPlannerData(pd);
    std::ofstream vertexFile((outputFile + "-vertices").c_str()), edgeFile((outputFile + "-edges").c_str());
    double* coords;
    unsigned numVerts = pd.numVertices(), offset = ss.getStateSpace()->getDimension() - 5;
    std::vector<unsigned int> edgeList;

    for (unsigned int i = 0; i < numVerts; ++i)
    {
        coords = pd.getVertex(i).getState()->as<KoulesStateSpace::StateType>()
            ->as<ob::RealVectorStateSpace::StateType>(0)->values;
        vertexFile << coords[offset] << ' ' << coords[offset + 1] << '\n';

        pd.getEdges(i, edgeList);
        for (unsigned int j = 0; j < edgeList.size(); ++j)
            edgeFile << i << ' ' << edgeList[j] << '\n';
    }
#endif
}

void planAllLevelsRecursive(oc::SimpleSetup* ss, double maxTime, const std::string& plannerName,
    std::vector<ob::PathPtr>& solution)
{
    double timeAttempt = maxTime / numAttempts;
    ob::PlannerStatus status;
    for (unsigned int i = 0; i < numAttempts; ++i)
    {
        ompl::time::point startTime = ompl::time::now();
        solution.clear();
        ss->clear();
        OMPL_INFORM("Attempt %d of %d to solve for %d koules",
            i + 1, numAttempts, (ss->getStateSpace()->getDimension() - 5)/4);
        status = ss->solve(timeAttempt);
        if (status != ob::PlannerStatus::EXACT_SOLUTION && numAttempts > 1)
            continue;

        ob::PathPtr path(ss->getProblemDefinition()->getSolutionPath());
        oc::PathControl* cpath = static_cast<oc::PathControl*>(path.get());
        const ob::State* goalState = cpath->getStates().back();
        std::vector<double> s, nextStart;

        if (status == ob::PlannerStatus::APPROXIMATE_SOLUTION)
        {
            cpath->interpolate();
            solution.push_back(path);
            OMPL_INFORM("Approximate solution found for %d koules",
                (ss->getStateSpace()->getDimension() - 5)/4);
            return;
        }
        ss->getStateSpace()->copyToReals(s, goalState);
        nextStart.reserve(s.size() - 4);
        for (unsigned int j = 0; j < s.size() - 5; j += 4)
            // include koule in next state if it is within workspace
            if (std::min(s[j], s[j+1]) > kouleRadius && std::max(s[j], s[j+1]) < sideLength - kouleRadius)
                for (unsigned k = 0; k < 4; ++k)
                    nextStart.push_back(s[j + k]);
        // add ship's state
        for (unsigned int j = s.size() - 5; j < s.size(); ++j)
            nextStart.push_back(s[j]);
        // make sure the problem size decreases as we recurse
        assert(nextStart.size() < s.size());

        unsigned int numKoules = (nextStart.size() - 5) / 4;
        if (numKoules > 0)
        {
            double timeElapsed = (ompl::time::now() - startTime).total_microseconds() * 1e-6;
            oc::SimpleSetup* ssNext = koulesSetup(numKoules, plannerName, nextStart);
            planAllLevelsRecursive(ssNext, timeAttempt - timeElapsed, plannerName, solution);
            if (solution.size() == 0)
                delete ssNext;
        }
        if (numKoules == 0 || solution.size())
        {
            cpath->interpolate();
            solution.push_back(path);
            OMPL_INFORM("Solution found for %d koules", (s.size() - 5) / 4);
            return;
        }
    }
}

void planAllLevels(oc::SimpleSetup& ss, double maxTime,
    const std::string& plannerName, const std::string& outputFile)
{
    std::vector<ob::PathPtr> solution;
    planAllLevelsRecursive(&ss, maxTime, plannerName, solution);
    if (solution.size())
    {
        std::ofstream out(outputFile.c_str());
        for (std::vector<ob::PathPtr>::reverse_iterator p = solution.rbegin(); p != solution.rend(); p++)
            static_cast<oc::PathControl*>(p->get())->printAsMatrix(out);
        OMPL_INFORM("Output saved in %s", outputFile.c_str());
    }
}

void benchmarkOneLevel(oc::SimpleSetup& ss, ot::Benchmark::Request request,
    const std::string& plannerName, const std::string& outputFile)
{
    // Create a benchmark class
    ompl::tools::Benchmark b(ss, "Koules experiment");
    // Add the planner to evaluate
    b.addPlanner(getPlanner(plannerName, ss.getSpaceInformation()));
    // Start benchmark
    b.benchmark(request);
    // Save the results
    b.saveResultsToFile(outputFile.c_str());
    OMPL_INFORM("Output saved in %s", outputFile.c_str());
}

int main(int argc, char **argv)
{
    try
    {
        unsigned int numKoules, numRuns;
        double maxTime, kouleVel;
        std::string plannerName, outputFile;
        po::options_description desc("Options");
        desc.add_options()
            ("help", "show help message")
            ("plan", "plan one level of koules")
            ("planall", "plan all levels of koules")
            ("benchmark", "benchmark one level")
            ("numkoules", po::value<unsigned int>(&numKoules)->default_value(3),
                "start from <numkoules> koules")
            ("maxtime", po::value<double>(&maxTime)->default_value(10.),
                "time limit in seconds")
            ("output", po::value<std::string>(&outputFile), "output file name")
            ("numruns", po::value<unsigned int>(&numRuns)->default_value(10),
                "number of runs for each planner in benchmarking mode")
            ("planner", po::value<std::string>(&plannerName)->default_value("kpiece"),
                "planning algorithm to use (pdst, kpiece, rrt, or est)")
            ("velocity", po::value<double>(&kouleVel)->default_value(0.),
                "initial velocity of each koule")
        ;

        po::variables_map vm;
        po::store(po::parse_command_line(argc, argv, desc,
            po::command_line_style::unix_style ^ po::command_line_style::allow_short), vm);
        po::notify(vm);

        oc::SimpleSetup* ss = koulesSetup(numKoules, plannerName, kouleVel);
        if (vm.count("help") || argc == 1)
        {
            std::cout << "Solve the games of Koules.\nSelect one of these three options:\n"
                      << "\"--plan\", \"--planall\", or \"--benchmark\"\n\n" << desc << "\n";
            return 1;
        }

        if (outputFile.size() == 0)
        {
            std::string prefix(vm.count("plan") ? "koules_"
                : (vm.count("planall") ? "koules_1-" : "koulesBenchmark_"));
            outputFile = boost::str(boost::format("%1%%2%_%3%_%4%.dat")
                % prefix % numKoules % plannerName % maxTime);
        }
        if (vm.count("plan"))
            planOneLevel(*ss, maxTime, plannerName, outputFile);
        else if (vm.count("planall"))
            planAllLevels(*ss, maxTime, plannerName, outputFile);
        else if (vm.count("benchmark"))
            benchmarkOneLevel(*ss, ot::Benchmark::Request(maxTime, 10000.0, numRuns),
                plannerName, outputFile);
        delete ss;
    }
    catch(std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    catch(...) {
        std::cerr << "Exception of unknown type!\n";
    }

    return 0;
}
