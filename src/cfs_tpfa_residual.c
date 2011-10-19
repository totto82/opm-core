#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "blas_lapack.h"
#include "flow_bc.h"
#include "well.h"

#include "compr_quant_general.h"
#include "sparse_sys.h"
#include "trans_tpfa.h"

#include "cfs_tpfa_residual.h"

#if defined(MAX)
#undef MAX
#endif

#define MAX(a,b) (((a) > (b)) ? (a) : (b))



struct densrat_util {
    MAT_SIZE_T *ipiv;

    double      residual;
    double     *lu;
    double     *t1;
    double     *t2;
    double     *mat_row;
    double     *coeff;
    double     *linsolve_buffer;
};


struct cfs_tpfa_res_impl {
    int                  is_incomp

    /* One entry per component per face */
    double              *compflux_f;       /* A_{ij} v_{ij} */
    double              *compflux_deriv_f; /* A_{ij} \partial_{p} v_{ij} */

    double              *flux_work;

    /* Scratch array for face pressure calculation */
    double              *scratch_f;

    struct densrat_util *ratio;

    /* Linear storage */
    double *ddata;
};


/* ---------------------------------------------------------------------- */
static void
deallocate_densrat(struct densrat_util *ratio)
/* ---------------------------------------------------------------------- */
{
    if (ratio != NULL) {
        free(ratio->lu);
        free(ratio->ipiv);
    }

    free(ratio);
}


/* ---------------------------------------------------------------------- */
static struct densrat_util *
allocate_densrat(size_t max_conn, int np)
/* ---------------------------------------------------------------------- */
{
    size_t               alloc_sz, n_buffer_col;
    struct densrat_util *ratio;

    ratio = malloc(1 * sizeof *ratio);

    if (ratio != NULL) {
        n_buffer_col  = 1;            /* z */
        n_buffer_col += 1 * max_conn; /* A_{ij} v_{ij} */
        n_buffer_col += 2 * max_conn; /* A_{ij} \partial_{p} v_{ij} */

        alloc_sz  = np             * np; /* lu */
        alloc_sz += 2              * np; /* t1, t2 */
        alloc_sz += (max_conn + 1) * 1 ; /* mat_row */
        alloc_sz += (max_conn + 1) * 1 ; /* coeff */
        alloc_sz += n_buffer_col   * np; /* linsolve_buffer */

        ratio->ipiv = malloc(np       * sizeof *ratio->ipiv);
        ratio->lu   = malloc(alloc_sz * sizeof *ratio->lu  );

        if ((ratio->ipiv == NULL) || (ratio->lu == NULL)) {
            deallocate_densrat(ratio);
            ratio = NULL;
        } else {
            ratio->t1              = ratio->lu      + (np             * np);
            ratio->t2              = ratio->t1      + (1              * np);
            ratio->mat_row         = ratio->t2      + (1              * np);
            ratio->coeff           = ratio->mat_row + ((max_conn + 1) * 1 );
            ratio->linsolve_buffer = ratio->coeff   + ((max_conn + 1) * 1 );
        }
    }

    return ratio;
}


/* ---------------------------------------------------------------------- */
static void
impl_deallocate(struct cfs_tpfa_res_impl *pimpl)
/* ---------------------------------------------------------------------- */
{
    if (pimpl != NULL) {
        free              (pimpl->ddata);
        deallocate_densrat(pimpl->ratio);
    }

    free(pimpl);
}


/* ---------------------------------------------------------------------- */
static struct cfs_tpfa_res_impl *
impl_allocate(grid_t *G, size_t max_conn, int np)
/* ---------------------------------------------------------------------- */
{
    size_t                nnu, ngconn, nwperf;
    struct cfs_tpfa_res_impl *new;

    size_t ddata_sz;

    nnu    = G->number_of_cells;
    ngconn = G->cell_facepos[ G->number_of_cells ];
    nwperf = 0;

    /* Linear system */
    ddata_sz  = nnu;            /* b */

    /* Reservoir */
    ddata_sz += np *      G->number_of_faces ; /* compflux_f */
    ddata_sz += np * (2 * G->number_of_faces); /* compflux_deriv_f */

    ddata_sz += np * (1 + 2)                 ; /* flux_work */

    ddata_sz += 1  *      G->number_of_faces ; /* scratch_f */

    new = malloc(1 * sizeof *new);

    if (new != NULL) {
        new->ddata = malloc(ddata_sz * sizeof *new->ddata);
        new->ratio = allocate_densrat(max_conn, np);

        if (new->ddata == NULL || new->ratio == NULL) {
            impl_deallocate(new);
            new = NULL;
        }
    }

    return new;
}


/* ---------------------------------------------------------------------- */
static struct CSRMatrix *
construct_matrix(grid_t *G)
/* ---------------------------------------------------------------------- */
{
    int    f, c1, c2, i, nc, nnu;
    size_t nnz;

    struct CSRMatrix *A;

    nc = nnu = G->number_of_cells;

    A = csrmatrix_new_count_nnz(nnu);

    if (A != NULL) {
        /* Self connections */
        for (i = 0; i < nnu; i++) {
            A->ia[ i + 1 ] = 1;
        }

        /* Other connections */
        for (f = 0; f < G->number_of_faces; f++) {
            c1 = G->face_cells[2*f + 0];
            c2 = G->face_cells[2*f + 1];

            if ((c1 >= 0) && (c2 >= 0)) {
                A->ia[ c1 + 1 ] += 1;
                A->ia[ c2 + 1 ] += 1;
            }
        }

        nnz = csrmatrix_new_elms_pushback(A);
        if (nnz == 0) {
            csrmatrix_delete(A);
            A = NULL;
        }
    }

    if (A != NULL) {
        /* Fill self connections */
        for (i = 0; i < nnu; i++) {
            A->ja[ A->ia[ i + 1 ] ++ ] = i;
        }

        /* Fill other connections */
        for (f = 0; f < G->number_of_faces; f++) {
            c1 = G->face_cells[2*f + 0];
            c2 = G->face_cells[2*f + 1];

            if ((c1 >= 0) && (c2 >= 0)) {
                A->ja[ A->ia[ c1 + 1 ] ++ ] = c2;
                A->ja[ A->ia[ c2 + 1 ] ++ ] = c1;
            }
        }

        assert ((size_t) A->ia[ nnu ] == nnz);

        /* Enforce sorted connection structure per row */
        csrmatrix_sortrows(A);
    }

    return A;
}


static void
factorise_fluid_matrix(int np, const double *A, struct densrat_util *ratio)
{
    int        np2;
    MAT_SIZE_T m, n, ld, info;

    m = n = ld = np;
    np2 = np * np;

    memcpy (ratio->lu, A, np2 * sizeof *ratio->lu);
    dgetrf_(&m, &n, ratio->lu, &ld, ratio->ipiv, &info);

    assert (info == 0);
}


static void
solve_linear_systems(int                  np   ,
                     MAT_SIZE_T           nrhs ,
                     struct densrat_util *ratio,
                     double              *b    )
{
    MAT_SIZE_T n, ldA, ldB, info;

    n = ldA = ldB = np;

    dgetrs_("No Transpose", &n,
            &nrhs, ratio->lu, &ldA, ratio->ipiv,
            b               , &ldB, &info);

    assert (info == 0);
}


static void
matvec(int nrow, int ncol, const double *A, const double *x, double *y)
{
    MAT_SIZE_T m, n, ld, incx, incy;
    double     a1, a2;

    m    = ld = nrow;
    n    = ncol;
    incx = incy = 1;
    a1   = 1.0;
    a2   = 0.0;

    dgemv_("No Transpose", &m, &n,
           &a1, A, &ld, x, &incx,
           &a2,         y, &incy);
}


static void
matmat(int np, int ncol, const double *A, const double *B, double *C)
{
    MAT_SIZE_T m, n, k, ldA, ldB, ldC;
    double     a1, a2;

    m  = k = ldA = ldB = ldC = np;
    n  = ncol;
    a1 = 1.0;
    a2 = 0.0;

    dgemm_("No Transpose", "No Transpose", &m, &n, &k,
           &a1, A, &ldA, B, &ldB, &a2, C, &ldC);
}


static void
compute_darcyflux_and_deriv(int           np,
                            double        trans,
                            double        dp,
                            const double *pmobf,
                            const double *gcapf,
                            double       *dflux,
                            double       *dflux_deriv)
{
    int    p;
    double a;

    for (p = 0; p < np; p++) {
        a = trans * pmobf[p];

        dflux      [       p] =   a * (dp + gcapf[p]);
        dflux_deriv[0*np + p] =   a; /* ignore gravity... */
        dflux_deriv[1*np + p] = - a;
    }
}


static void
compute_compflux_and_deriv(grid_t               *G     ,
                           int                   np    ,
                           const double         *cpress,
                           const double         *trans ,
                           const double         *pmobf ,
                           const double         *gcapf ,
                           const double         *Af    ,
                           struct cfs_tpfa_res_impl *pimpl )
{
    int     c1, c2, f, np2;
    double  dp;
    double *cflux, *dcflux;

    np2    = np * np;

    cflux  = pimpl->compflux_f;
    dcflux = pimpl->compflux_deriv_f;

    for (f = 0; f < G->number_of_faces;
         f++, pmobf += np, gcapf += np, Af += np2,
             cflux += np, dcflux += 2 * np) {

        c1 = G->face_cells[2*f + 0];
        c2 = G->face_cells[2*f + 1];

        if ((c1 >= 0) && (c2 >= 0)) {
            dp = cpress[c1] - cpress[c2];

            compute_darcyflux_and_deriv(np, trans[f], dp, pmobf, gcapf,
                                        pimpl->flux_work,
                                        pimpl->flux_work + np);

            /* Component flux = Af * v*/
            matvec(np, np, Af, pimpl->flux_work     , cflux );

            /* Derivative = Af * (dv/dp) */
            matmat(np, 2 , Af, pimpl->flux_work + np, dcflux);
        }

        /* Boundary connections excluded */
    }
}


static int
count_internal_conn(grid_t *G, int c)
{
    int c1, c2, f, i, nconn;

    for (i = G->cell_facepos[c]; i < G->cell_facepos[c + 1]; i++) {
        f  = G->cell_faces[i];
        c1 = G->face_cells[2*f + 0];
        c2 = G->face_cells[2*f + 1];

        nconn += (c1 >= 0) && (c2 >= 0);
    }

    return nconn;
}


static int
init_cell_contrib(grid_t               *G    ,
                  int                   c    ,
                  int                   np   ,
                  double                pvol ,
                  double                dt   ,
                  const double         *z    ,
                  struct cfs_tpfa_res_impl *pimpl)
{
    int     c1, c2, f, i, conn, nconn;
    double *cflx, *dcflx;

    nconn = count_internal_conn(G, c);

    memcpy(pimpl->ratio->linsolve_buffer, z, np * sizeof *z);

    pimpl->ratio->coeff[0] = -pvol;
    conn = 1;

    cflx  = pimpl->ratio->linsolve_buffer + (1 * np);
    dcflx = cflx + (nconn * np);

    for (i = G->cell_facepos[c]; i < G->cell_facepos[c + 1]; i++) {
        f  = G->cell_faces[i];
        c1 = G->face_cells[2*f + 0];
        c2 = G->face_cells[2*f + 1];

        if ((c1 >= 0) && (c2 >= 0)) {
            memcpy(cflx, pimpl->compflux_f + (f*np + 0),
                   np * sizeof *cflx);

            memcpy(dcflx, pimpl->compflux_deriv_f + (f*(2 * np) + 0),
                   2 * np * sizeof *dcflx);

            cflx  += 1 * np;
            dcflx += 2 * np;

            pimpl->ratio->coeff[ conn++ ] = dt;
        }
    }

    assert (conn == nconn + 1);
    assert (cflx == pimpl->ratio->linsolve_buffer + (nconn + 1)*np);

    return nconn;
}


static void
compute_cell_contrib(grid_t               *G    ,
                     int                   c    ,
                     int                   np   ,
                     double                pvol ,
                     double                dt   ,
                     const double         *z    ,
                     const double         *Ac   ,
                     const double         *dAc  ,
                     struct cfs_tpfa_res_impl *pimpl)
{
    int        c1, c2, f, i, off, nconn, np2, p;
    MAT_SIZE_T nrhs;
    double     s, dF1, dF2, *dv, *dv1, *dv2;

    np2   = np * np;

    nconn = init_cell_contrib(G, c, np, pvol, dt, z, pimpl);
    nrhs  = 1 + (1 + 2)*nconn;  /* [z, Af*v, Af*dv] */

    factorise_fluid_matrix(np, Ac, pimpl->ratio);
    solve_linear_systems  (np, nrhs, pimpl->ratio,
                           pimpl->ratio->linsolve_buffer);

    /* Sum residual contributions over the connections (+ accumulation):
     *   t1 <- (Ac \ [z, Af*v]) * [-pvol; repmat(dt, [nconn, 1])] */
    matvec(np, nconn + 1, pimpl->ratio->linsolve_buffer,
           pimpl->ratio->coeff, pimpl->ratio->t1);

    /* Compute residual in cell 'c' */
    pimpl->ratio->residual = pvol;
    for (p = 0; p < np; p++) {
        pimpl->ratio->residual += pimpl->ratio->t1[ p ];
    }

    /* Jacobian row */

    vector_zero(1 + nconn, pimpl->ratio->mat_row);

    /* t2 <- A \ ((dA/dp) * t1) */
    matvec(np, np, dAc, pimpl->ratio->t1, pimpl->ratio->t2);
    solve_linear_systems(np, 1, pimpl->ratio, pimpl->ratio->t2);

    dF1 = dF2 = 0.0;
    for (p = 0; p < np; p++) {
        dF1 += pimpl->ratio->t1[ p ];
        dF2 += pimpl->ratio->t2[ p ];
    }

    pimpl->is_incomp           = pimpl->is_incomp && (! (fabs(dF2) > 0));
    pimpl->ratio->mat_row[ 0 ] = dF1 - dF2;

    /* Accumulate inter-cell Jacobian contributions */
    dv  = pimpl->ratio->linsolve_buffer + (1 + nconn)*np;
    off = 1;
    for (i = G->cell_facepos[c]; i < G->cell_facepos[c + 1]; i++, off++) {

        f  = G->cell_faces[i];
        c1 = G->face_cells[2*f + 0];
        c2 = G->face_cells[2*f + 1];

        if ((c1 >= 0) && (c2 >= 0)) {
            if (c1 == c) { s =  1.0; dv1 = dv + 0 ; dv2 = dv + np; }
            else         { s = -1.0; dv1 = dv + np; dv2 = dv + 0 ; }

            dF1 = dF2 = 0.0;
            for (p = 0; p < np; p++) {
                dF1 += dv1[ p ];
                dF2 += dv2[ p ];
            }

            pimpl->ratio->mat_row[  0  ] += s * dt * dF1;
            pimpl->ratio->mat_row[ off ] += s * dt * dF2;
        }
    }
}


/* ---------------------------------------------------------------------- */
static int
assemble_cell_contrib(grid_t               *G,
                      int                   c,
                      struct cfs_tpfa_res_data *h)
/* ---------------------------------------------------------------------- */
{
    int c1, c2, i, f, j1, j2, off;
    int is_neumann;

    is_neumann = 1;

    j1 = csrmatrix_elm_index(c, c, h->J);

    h->J->sa[j1] += h->pimpl->ratio->mat_row[ 0 ];

    off = 1;
    for (i = G->cell_facepos[c]; i < G->cell_facepos[c + 1]; i++, off++) {
        f = G->cell_faces[i];

        c1 = G->face_cells[2*f + 0];
        c2 = G->face_cells[2*f + 1];

        c2 = (c1 == c) ? c2 : c1;

        if (c2 >= 0) {
            j2 = csrmatrix_elm_index(c, c2, h->J);

            h->J->sa[j2] += h->pimpl->ratio->mat_row[ off ];
        }
    }

    h->F[ c ] = h->pimpl->ratio->residual;

    return 0;
}


/* ---------------------------------------------------------------------- */
static void
compute_fpress(grid_t       *G,
               flowbc_t     *bc,
               int           np,
               const double *htrans,
               const double *pmobf,
               const double *gravcap_f,
               const double *cpress,
               const double *fflux,
               double       *fpress,
               double       *scratch_f)
/* ---------------------------------------------------------------------- */
{
    int    c, i, f, c1, c2;

    /* Suppress warning about unused parameters. */
    (void) np;  (void) pmobf;  (void) gravcap_f;  (void) fflux;

    /*
     * Define face pressures as weighted average of connecting cell
     * pressures.  Specifically, we define
     *
     *     pf = (t1 p1 + t2 p2) / (t1 + t2)
     *
     * in which t1 and t2 are the one-sided transmissibilities and p1
     * and p2 are the associated cell pressures.
     *
     * NOTE: The formula does not account for effects of gravity or
     * flux boundary conditions.
     */
    for (f = 0; f < G->number_of_faces; f++) {
        scratch_f[f] = fpress[f] = 0.0;
    }

    for (c = i = 0; c < G->number_of_cells; c++) {
        for (; i < G->cell_facepos[c + 1]; i++) {
            f = G->cell_faces[i];
            scratch_f[f] += htrans[i];
            fpress[f]    += htrans[i] * cpress[c];
        }
    }

    for (f = 0; f < G->number_of_faces; f++) {
        fpress[f] /= scratch_f[f];

        c1 = G->face_cells[2*f + 0];
        c2 = G->face_cells[2*f + 1];

        if (((c1 < 0) || (c2 < 0)) && (bc->type[f] == PRESSURE)) {
            fpress[f] = bc->bcval[f];
        }
    }
}


/* ---------------------------------------------------------------------- */
static void
compute_flux(grid_t       *G,
             flowbc_t     *bc,
             int           np,
             const double *trans,
             const double *pmobf,
             const double *gravcap_f,
             const double *cpress,
             double       *fflux)
/* ---------------------------------------------------------------------- */
{
    int    f, c1, c2, p;
    double t, dp, g;

    for (f = 0; f < G->number_of_faces; f++) {
        c1 = G->face_cells[2*f + 0];
        c2 = G->face_cells[2*f + 1];

        if (((c1 < 0) || (c2 < 0)) && (bc->type[f] == FLUX)) {
            fflux[f] = bc->bcval[f];
            continue;
        }

        t = g = 0.0;
        for (p = 0; p < np; p++) {
            t += pmobf[f*np + p];
            g += pmobf[f*np + p] * gravcap_f[f*np + p];
        }
        /* t *= trans[f]; */

        if ((c1 >= 0) && (c2 >= 0)) {
            dp  = cpress[c1] - cpress[c2];
        } else if (bc->type[f] == PRESSURE) {
            if (c1 < 0) {
                dp = bc->bcval[f] - cpress[c2];
                /* g  = -g; */
            } else {
                dp = cpress[c1] - bc->bcval[f];
            }
        } else {
            /* No BC -> no-flow (== pressure drop offsets gravity) */
            dp = -g / t;
        }

        fflux[f] = trans[f] * (t*dp + g);
    }
}


static size_t
maxconn(grid_t *G)
{
    int    c;
    size_t m, n;

    for (c = 0, m = 0; c < G->number_of_cells; c++) {
        n = G->cell_facepos[c + 1] - G->cell_facepos[c];

        if (n > m) { m = n; }
    }

    return m;
}


/* ======================================================================
 * Public interface below separator.
 * ====================================================================== */


/* ---------------------------------------------------------------------- */
void
cfs_tpfa_res_destroy(struct cfs_tpfa_res_data *h)
/* ---------------------------------------------------------------------- */
{
    if (h != NULL) {
        csrmatrix_delete(h->J);
        impl_deallocate (h->pimpl);
    }

    free(h);
}


/* ---------------------------------------------------------------------- */
struct cfs_tpfa_res_data *
cfs_tpfa_res_construct(grid_t *G, int nphases)
/* ---------------------------------------------------------------------- */
{
    size_t                    nc, nf, ngconn;
    struct cfs_tpfa_res_data *h;

    h = malloc(1 * sizeof *h);

    if (h != NULL) {
        h->pimpl = impl_allocate(G, maxconn(G), nphases);
        h->J     = construct_matrix(G);

        if ((h->pimpl == NULL) || (h->J == NULL)) {
            cfs_tpfa_res_destroy(h);
            h = NULL;
        }
    }

    if (h != NULL) {
        nc     = G->number_of_cells;
        nf     = G->number_of_faces;
        ngconn = G->cell_facepos[nc];

        /* Allocate linear system components */
        h->F                       = h->pimpl->ddata + 0;

        h->pimpl->compflux_f       = h->F            + h->J->m;
        h->pimpl->compflux_deriv_f =
            h->pimpl->compflux_f                     + (nphases * nf);

        h->pimpl->flux_work        =
            h->pimpl->compflux_deriv_f               + (nphases * 2 * nf);

        h->pimpl->scratch_f        =
            h->pimpl->flux_work                      + (nphases * (1 + 2));
    }

    return h;
}


/* ---------------------------------------------------------------------- */
void
cfs_tpfa_res_assemble(grid_t                      *G,
                      double                       dt,
                      flowbc_t                    *bc,
                      const double                *src,
                      const double                *zc,
                      struct compr_quantities_gen *cq,
                      const double                *trans,
                      const double                *gravcap_f,
                      const double                *cpress,
                      const double                *porevol,
                      struct cfs_tpfa_res_data    *h)
/* ---------------------------------------------------------------------- */
{
    int res_is_neumann, c, np2;

    csrmatrix_zero(         h->J);
    vector_zero   (h->J->m, h->F);

    h->pimpl->is_incomp = 1;

    compute_compflux_and_deriv(G, cq->nphases, cpress, trans,
                               cq->phasemobf, gravcap_f, cq->Af, h->pimpl);

    np2 = cq->nphases * cq->nphases;
    for (c = 0; c < G->number_of_cells;
         c++, zc += cq->nphases) {

        compute_cell_contrib(G, c, cq->nphases, porevol[c], dt, zc,
                             cq->Ac + (c * np2), cq->dAc + (c * np2),
                             h->pimpl);

        assemble_cell_contrib(G, c, h);
    }

    res_is_neumann = 1;

    if (res_is_neumann && h->pimpl->is_incomp) {
        h->J->sa[0] *= 2;
    }
}


/* ---------------------------------------------------------------------- */
void
cfs_tpfa_res_flux(grid_t       *G,
                  flowbc_t     *bc,
                  int           np,
                  const double *trans,
                  const double *pmobf,
                  const double *gravcap_f,
                  const double *cpress,
                  double       *fflux)
/* ---------------------------------------------------------------------- */
{
    compute_flux(G, bc, np, trans, pmobf, gravcap_f, cpress, fflux);
}


/* ---------------------------------------------------------------------- */
void
cfs_tpfa_res_fpress(grid_t                   *G,
                    flowbc_t                 *bc,
                    int                       np,
                    const double             *htrans,
                    const double             *pmobf,
                    const double             *gravcap_f,
                    struct cfs_tpfa_res_data *h,
                    const double             *cpress,
                    const double             *fflux,
                    double                   *fpress)
/* ---------------------------------------------------------------------- */
{
    compute_fpress(G, bc, np, htrans, pmobf, gravcap_f,
                   cpress, fflux, fpress, h->pimpl->scratch_f);
}


#if 0
/* ---------------------------------------------------------------------- */
void
cfs_tpfa_res_retrieve_masstrans(grid_t               *G,
                            int                   np,
                            struct cfs_tpfa_res_data *h,
                            double               *masstrans_f)
/* ---------------------------------------------------------------------- */
{
    memcpy(masstrans_f, h->pimpl->masstrans_f,
           np * G->number_of_faces * sizeof *masstrans_f);
}


/* ---------------------------------------------------------------------- */
void
cfs_tpfa_res_retrieve_gravtrans(grid_t               *G,
                            int                   np,
                            struct cfs_tpfa_res_data *h,
                            double               *gravtrans_f)
/* ---------------------------------------------------------------------- */
{
    memcpy(gravtrans_f, h->pimpl->gravtrans_f,
           np * G->number_of_faces * sizeof *gravtrans_f);
}


/* ---------------------------------------------------------------------- */
static double
cfs_tpfa_res_impes_maxtime_cell(int                      c,
                            grid_t                  *G,
                            struct compr_quantities *cq,
                            const double            *trans,
                            const double            *porevol,
                            struct cfs_tpfa_res_data    *h,
                            const double            *dpmobf,
                            const double            *surf_dens,
                            const double            *gravity)
/* ---------------------------------------------------------------------- */
{
    /* Reference:
       K. H. Coats, "IMPES Stability: The Stable Step", SPE 69225

       Capillary pressure parts not included.
    */

    int i, j, k, f, c2;
    double f11, f12, f21, f22;
    double dp, dzg, tr, tmob, detF, eqv_flux;
    const double *pmob;
    const double *A;
    /* This is intended to be compatible with the dune-porsol blackoil
       code. This library is otherwise not depending on particular
       orderings of phases or components, so at some point this
       function should be generalized. */
    enum { Water = 0, Oil = 1, Gas = 2 };
    enum { num_phases = 3 };
    double rho[num_phases];
    double pot[num_phases];
    /* Notation: dpmob[Oil][Water] is d/ds_w(lambda_o) */
    double dpmob[num_phases][num_phases]
        = { {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0} };

    assert (cq->nphases == 3);

    f11 = f12 = f21 = f22 = 0.0;

    /* Loop over neighbour faces to accumulate f11, f12 etc. */
    for (i = G->cell_facepos[c]; i < G->cell_facepos[c + 1]; ++i) {
        f  = G->cell_faces[i];
        if ((c2 = G->face_cells[2*f + 0]) == c) {
            c2  = G->face_cells[2*f + 1];
        }

        /* Initially only interior faces */
        if (c2 < 0) {
            continue;
        }

        /* Computing density */
        A = cq->Af + f*(cq->nphases)*(cq->nphases);
        for (j = 0; j < cq->nphases; ++j) {
            rho[j] = 0.0;
            for (k = 0; k < cq->nphases; ++k) {
                rho[j] += A[cq->nphases*j + k]*surf_dens[k];
            }
        }
        /* Computing gravity potentials */
        dp = h->x[c] - h->x[c2];
        dzg = 0.0;
        for (j = 0; j < G->dimensions; ++j) {
            dzg += (G->cell_centroids[G->dimensions*c + j] - G->cell_centroids[G->dimensions*c2 + j])*gravity[j];
        }
        for (j = 0; j < cq->nphases; ++j) {
            pot[j] = fabs(dp - rho[j]*dzg);
        }
        /* Filling the dpmob array from available data.
           Note that we only need the following combinations:
           (Water, Water)
           (Water, Gas)
           (Oil, Water)
           (Oil, Gas)
           (Gas, Gas)

           No derivatives w.r.t. Oil is needed, since there are only two
           independent saturation variables.

           The lack of (Gas, Water) may be due to assumptions on the
           three-phase model used (should be checked to be compatible
           with our choices).
        */
        dpmob[Water][Water] = dpmobf[9*f];
        dpmob[Water][Gas] = dpmobf[9*f + 2];
        dpmob[Oil][Water] = dpmobf[9*f + 3];
        dpmob[Oil][Gas] = dpmobf[9*f + 5];
        dpmob[Gas][Gas] = dpmobf[9*f + 8];
        /* Computing the flux parts f_ij */
        pmob = cq->phasemobf + f*cq->nphases;
        tr = trans[f];
        tmob = pmob[Water] + pmob[Oil] + pmob[Gas];
        f11 += tr*((pmob[Oil] + pmob[Gas])*dpmob[Water][Water]*pot[Water]
                   - pmob[Water]*dpmob[Oil][Water]*pot[Oil])/tmob;
        f12 += -tr*(pmob[Water]*dpmob[Oil][Gas]*pot[Oil]
                    + pmob[Water]*dpmob[Gas][Gas]*pot[Gas]
                    - (pmob[Oil] + pmob[Gas])*dpmob[Water][Gas]*pot[Water])/tmob;
        f21 += -tr*(pmob[Gas]*dpmob[Water][Water]*pot[Water]
                    + pmob[Gas]*dpmob[Oil][Water]*pot[Oil])/tmob;
        f22 += tr*(-pmob[Gas]*dpmob[Oil][Gas]*pot[Oil]
                   + (pmob[Water] + pmob[Oil])*dpmob[Gas][Gas]*pot[Gas]
                   - pmob[Gas]*dpmob[Water][Gas]*pot[Water])/tmob;
    }

    /* (from eq. 3, 4a-e, 5a-c)
       F_i = 1/2 |f11_i + f22_i + \sqrt{G}|
       G = (f11_i + f22_i)^2 - 4 det(F_i)
       fXX_i = \sum_j fXX_ij     (j runs over the neighbours)
       det(F_i) = f11_i f22_i - f12_i f21_i
    */
    detF = f11*f22 - f12*f21;
    eqv_flux = 0.5*fabs(f11 + f22 + sqrt((f11 + f22)*(f11 + f22) - 4*detF));
    /* delta_t < porevol/eqv_flux */
    return porevol[c]/eqv_flux;
}

/* ---------------------------------------------------------------------- */
double
cfs_tpfa_res_impes_maxtime(grid_t                  *G,
                       struct compr_quantities *cq,
                       const double            *trans,
                       const double            *porevol,
                       struct cfs_tpfa_res_data    *h,
                       const double            *dpmobf,
                       const double            *surf_dens,
                       const double            *gravity)
/* ---------------------------------------------------------------------- */
{
    int c;
    double max_dt, cell_dt;
    max_dt = 1e100;
    for (c = 0; c < G->number_of_cells; ++c) {
        cell_dt = cfs_tpfa_res_impes_maxtime_cell(c, G, cq, trans, porevol, h,
                                              dpmobf, surf_dens, gravity);
        if (cell_dt < max_dt) {
            max_dt = cell_dt;
        }
    }
    return max_dt;
}



/* ---------------------------------------------------------------------- */
void
cfs_tpfa_res_expl_mass_transport(grid_t                 *G,
                             well_t                 *W,
                             struct completion_data *wdata,
                             int                     np,
                             double                  dt,
                             const double           *porevol,
                             struct cfs_tpfa_res_data   *h,
                             double                 *surf_vol)
/* ---------------------------------------------------------------------- */
{
    int    c, i, f, c2, p, w;
    double dp, dz, gsgn;
    const double *masstrans_f, *gravtrans_f, *masstrans_p, *gravtrans_p;
    const double *cpress, *wpress;

    /* Suppress warning about unused parameter. */
    (void) wdata;

    masstrans_f = h->pimpl->masstrans_f;
    gravtrans_f = h->pimpl->gravtrans_f;
    masstrans_p = h->pimpl->masstrans_p;
    gravtrans_p = h->pimpl->gravtrans_p;
    cpress = h->x;
    wpress = h->x + G->number_of_cells;

    /* Transport through interior faces */
    for (c = i = 0; c < G->number_of_cells; c++) {
        for (; i < G->cell_facepos[c + 1]; i++) {
            f  = G->cell_faces[i];

            if ((c2 = G->face_cells[2*f + 0]) == c) {
              gsgn = 1.0;
                c2  = G->face_cells[2*f + 1];
            } else {
              gsgn = -1.0;
            }

            if (c2 >= 0) {
                dp = cpress[c] - cpress[c2];

                for (p = 0; p < np; p++) {
                    dz  = masstrans_f[f*np + p] * dp;
                    dz += gravtrans_f[f*np + p] * gsgn;

                    /* dz > 0 => flow from c into c2. */
                    surf_vol[c*np + p] -= dz * dt / porevol[c];
                }
            }
        }
    }

    /* Transport through well perforations */
    if (W != NULL) {
        for (w = i = 0; w < W->number_of_wells; w++) {
            for (; i < W->well_connpos[w + 1]; i++) {
                c = W->well_cells[i];
                dp = wpress[w] - cpress[c];

                for (p = 0; p < np; p++) {
                    dz = masstrans_p[i*np + p] * dp;
                    dz += gravtrans_p[i*np + p];

                    /* dz > 0 => flow from perforation into c. */
                    surf_vol[c*np + p] += dz * dt / porevol[c];
                }
            }
        }
    }
}
#endif
