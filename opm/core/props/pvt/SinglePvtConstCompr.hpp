/*
  Copyright 2010, 2011, 2012 SINTEF ICT, Applied Mathematics.

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

#ifndef OPM_SINGLEPVTCONSTCOMPR_HEADER_INCLUDED
#define OPM_SINGLEPVTCONSTCOMPR_HEADER_INCLUDED


#include <opm/core/props/pvt/SinglePvtInterface.hpp>
#include <opm/core/utility/ErrorMacros.hpp>
#include <vector>
#include <algorithm>


namespace Opm
{

    /// Class for constant compressible phases (PVTW or PVCDO).
    /// The PVT properties can either be given as a function of pressure (p) and surface volume (z)
    /// or pressure (p) and gas resolution factor (r).
    /// For all the virtual methods, the following apply: p, r and z
    /// are expected to be of size n, size n and n*num_phases, respectively.
    /// Output arrays shall be of size n, and must be valid before
    /// calling the method.
    class SinglePvtConstCompr : public SinglePvtInterface
    {
    public:
        typedef std::vector<std::vector<double> > table_t;

        SinglePvtConstCompr(const table_t& pvtw)
        {
            const int region_number = 0;
            if (pvtw.size() != 1) {
                OPM_THROW(std::runtime_error, "More than one PVD-region");
            }
            ref_press_ = pvtw[region_number][0];
            ref_B_     = pvtw[region_number][1];
            comp_      = pvtw[region_number][2];
            viscosity_ = pvtw[region_number][3];
            visc_comp_ = pvtw[region_number][4];
        }

        SinglePvtConstCompr(double visc)
            : ref_press_(0.0),
              ref_B_(1.0),
              comp_(0.0),
              viscosity_(visc),
              visc_comp_(0.0)
        {
        }

        virtual ~SinglePvtConstCompr()
        {
        }

        virtual void mu(const int n,
                        const double* p,
                        const double* /*z*/,
                        double* output_mu) const
        {
            if (visc_comp_) {
// #pragma omp parallel for
                for (int i = 0; i < n; ++i) {
                    // Computing a polynomial approximation to the exponential.
                    double x = -visc_comp_*(p[i] - ref_press_);
                    output_mu[i] = viscosity_/(1.0 + x + 0.5*x*x);
                }
            } else {
                std::fill(output_mu, output_mu + n, viscosity_);
            }
        }

        virtual void mu(const int n,
                        const double* p,
                        const double* /*r*/,
                        double* output_mu,
                        double* output_dmudp,
                        double* output_dmudr) const
        {
            if (visc_comp_) {
                // #pragma omp parallel for
                for (int i = 0; i < n; ++i) {
                    // Computing a polynomial approximation to the exponential.
                    double x = -visc_comp_*(p[i] - ref_press_);
                    double d = (1.0 + x + 0.5*x*x);
                    output_mu[i] = viscosity_/d;
                    output_dmudp[i] = (viscosity_/(d*d))*(1+x) * visc_comp_;
                }
            } else {
                std::fill(output_mu, output_mu + n, viscosity_);
                std::fill(output_dmudp, output_dmudp + n, 0.0);
            }
            std::fill(output_dmudr, output_dmudr + n, 0.0);
        }

        virtual void mu(const int n,
                        const double* p,
                        const double* /*r*/,
                        const PhasePresence* /*cond*/,
                        double* output_mu,
                        double* output_dmudp,
                        double* output_dmudr) const
        {
            if (visc_comp_) {
                // #pragma omp parallel for
                for (int i = 0; i < n; ++i) {
                    // Computing a polynomial approximation to the exponential.
                    double x = -visc_comp_*(p[i] - ref_press_);
                    double d = (1.0 + x + 0.5*x*x);
                    output_mu[i] = viscosity_/d;
                    output_dmudp[i] = (viscosity_/(d*d))*(1+x) * visc_comp_;
                }
            } else {
                std::fill(output_mu, output_mu + n, viscosity_);
                std::fill(output_dmudp, output_dmudp + n, 0.0);
            }
            std::fill(output_dmudr, output_dmudr + n, 0.0);
        }

        virtual void B(const int n,
                       const double* p,
                       const double* /*z*/,
                       double* output_B) const
        {
            if (comp_) {
// #pragma omp parallel for
                for (int i = 0; i < n; ++i) {
                    // Computing a polynomial approximation to the exponential.
                    double x = comp_*(p[i] - ref_press_);
                    output_B[i] = ref_B_/(1.0 + x + 0.5*x*x);
                }
            } else {
                std::fill(output_B, output_B + n, ref_B_);
            }
        }

        virtual void dBdp(const int n,
                          const double* p,
                          const double* /*z*/,
                          double* output_B,
                          double* output_dBdp) const
        {
            if (comp_) {
// #pragma omp parallel for
                for (int i = 0; i < n; ++i) {
                    double x = comp_*(p[i] - ref_press_);
                    double d = (1.0 + x + 0.5*x*x);
                    output_B[i] = ref_B_/d;
                    output_dBdp[i] = (-ref_B_/(d*d))*(1 + x) * comp_;
                }
            } else {
                std::fill(output_B, output_B + n, ref_B_);
                std::fill(output_dBdp, output_dBdp + n, 0.0);
            }
        }

        virtual void b(const int n,
                               const double* p,
                               const double* /*r*/,
                               double* output_b,
                               double* output_dbdp,
                               double* output_dbdr) const
                {
                    if (comp_) {
        // #pragma omp parallel for
                        for (int i = 0; i < n; ++i) {
                            // Computing a polynomial approximation to the exponential.
                            double x = comp_*(p[i] - ref_press_);
                            double d = (1.0 + x + 0.5*x*x);

                            // b = 1/B = d/ref_B_B;
                            output_b[i] = d/ref_B_;
                            output_dbdp[i] = (1 + x) * comp_/ref_B_;
                        }
                    } else {

                        std::fill(output_b, output_b + n, 1/ref_B_);
                        std::fill(output_dbdp, output_dbdp + n, 0.0);
                    }
                    std::fill(output_dbdr, output_dbdr + n, 0.0);

                }

        virtual void b(const int n,
                               const double* p,
                               const double* /*r*/,
                               const PhasePresence* /*cond*/,
                               double* output_b,
                               double* output_dbdp,
                               double* output_dbdr) const
                {
                    if (comp_) {
        // #pragma omp parallel for
                        for (int i = 0; i < n; ++i) {
                            // Computing a polynomial approximation to the exponential.
                            double x = comp_*(p[i] - ref_press_);
                            double d = (1.0 + x + 0.5*x*x);

                            // b = 1/B = d/ref_B_B;
                            output_b[i] = d/ref_B_;
                            output_dbdp[i] = (1 + x) * comp_/ref_B_;
                        }
                    } else {

                        std::fill(output_b, output_b + n, 1/ref_B_);
                        std::fill(output_dbdp, output_dbdp + n, 0.0);
                    }
                    std::fill(output_dbdr, output_dbdr + n, 0.0);

                }

        virtual void rsSat(const int n,
                             const double* /*p*/,
                             double* output_rsSat,
                             double* output_drsSatdp) const

              {
                  std::fill(output_rsSat, output_rsSat + n, 0.0);
                  std::fill(output_drsSatdp, output_drsSatdp + n, 0.0);
              }

        virtual void rvSat(const int n,
                             const double* /*p*/,
                             double* output_rvSat,
                             double* output_drvSatdp) const

              {
                  std::fill(output_rvSat, output_rvSat + n, 0.0);
                  std::fill(output_drvSatdp, output_drvSatdp + n, 0.0);
              }

        virtual void R(const int n,
                       const double* /*p*/,
                       const double* /*z*/,
                       double* output_R) const
        {
            std::fill(output_R, output_R + n, 0.0);
        }

        virtual void dRdp(const int n,
                          const double* /*p*/,
                          const double* /*z*/,
                          double* output_R,
                          double* output_dRdp) const
        {
            std::fill(output_R, output_R + n, 0.0);
            std::fill(output_dRdp, output_dRdp + n, 0.0);
        }

    private:
        double ref_press_;
        double ref_B_;
        double comp_;
        double viscosity_;
        double visc_comp_;
    };

}


#endif // OPM_SINGLEPVTCONSTCOMPR_HEADER_INCLUDED

