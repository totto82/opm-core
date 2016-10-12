/*
  Copyright 2012 SINTEF ICT, Applied Mathematics.

  This file is part of the Open Porous Media project (OPM).

  OPM is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  OPM is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with OPM.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef OPM_SIMULATORTIMER_HEADER_INCLUDED
#define OPM_SIMULATORTIMER_HEADER_INCLUDED

#include <opm/parser/eclipse/EclipseState/Schedule/TimeMap.hpp>
#include <opm/core/simulator/SimulatorTimerInterface.hpp>

#include <iosfwd>
#include <vector>

namespace Opm
{

    namespace parameter { class ParameterGroup; }

    class SimulatorTimer : public SimulatorTimerInterface
    {
    public:
        // use default implementation of these methods
        using SimulatorTimerInterface::currentDateTime;
        using SimulatorTimerInterface::currentPosixTime;

        /// Default constructor.
        SimulatorTimer();

        /// Initialize from parameters. Accepts the following:
        ///    num_psteps    (default 1)
        ///    stepsize_days (default 1)
        void init(const parameter::ParameterGroup& param);

        /// Use the SimulatorTimer as a shim around opm-parser's Opm::TimeMap
        void init(TimeMapConstPtr timeMap, size_t report_step = 0);

        /// Whether the current step is the first step.
        bool initialStep() const;

        /// Total number of steps.
        int numSteps() const;

        int numSubSteps() const;

        int numRestarts() const;

        /// Current step number. This is the number of timesteps that
        /// has been completed from the start of the run. The time
        /// after initialization but before the simulation has started
        /// is timestep number zero.
        int currentStepNum() const;

        /// Set current step number.
        void setCurrentStepNum(int step);

        /// Current step length. This is the length of the step
        /// the simulator will take in the next iteration.
        ///
        /// @note if done(), it is an error to call currentStepLength().
        double currentStepLength() const;

        /// Previous step length. This is the length of the step that
        /// was taken to arrive at this time.
        ///
        /// @note if no increments have been done (i.e. the timer is
        /// still in its constructed state and currentStepNum() == 0),
        /// it is an error to call stepLengthTaken().
        double stepLengthTaken () const;

        /// Time elapsed since the start of the simulation until the
        /// beginning of the current time step [s].
        double simulationTimeElapsed() const;

        /// Total time.
        double totalTime() const;

        /// Return start date of simulation
        boost::posix_time::ptime startDateTime() const;

        /// Return current date.
        boost::posix_time::ptime currentDateTime() const;

        /// Set total time.
        /// This is primarily intended for multi-epoch schedules,
        /// where a timer for a given epoch does not have
        /// access to later timesteps.
        void setTotalTime(double time);

        /// Print a report with current and total time etc.
        /// Note: if done(), it is an error to call report().
        void report(std::ostream& os) const;

        /// advance time by currentStepLength
        SimulatorTimer& operator++();

        /// advance time by currentStepLength
        void advance() { this->operator++(); }

        /// Return true if op++() has been called numSteps() times.
        bool done() const;

        /// return copy of object
        virtual std::unique_ptr< SimulatorTimerInterface > clone() const;

    private:
        std::vector<double> timesteps_;
        int current_step_;
        double current_time_;
        double total_time_;
        boost::gregorian::date start_date_;
    };


} // namespace Opm

#endif // OPM_SIMULATORTIMER_HEADER_INCLUDED
